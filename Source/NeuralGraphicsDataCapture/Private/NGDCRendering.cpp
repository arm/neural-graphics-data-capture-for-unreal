// SPDX-FileCopyrightText: Copyright 2025-2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: Apache-2.0

#include "NGDCRendering.h"

#include "UNeuralGraphicsDataCaptureSubsystem.h"
#include "NeuralGraphicsDataCaptureModule.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "TemporalUpscaler.h"
#include "DynamicResolutionState.h"
#include "Async/TaskGraphInterfaces.h"
#include "Async/Async.h"
#include "Math/Halton.h"
#include "Math/UnrealMathUtility.h"
#include "CanvasItem.h"
#include "Engine/GameViewportClient.h"

#if NGDC_DEBUGGING_ENABLED
UE_DISABLE_OPTIMIZATION
#endif

bool FNGDCRenderingSettings::Validate(TArray<FString>& OutErrors) const
{
	bool bValid = true;
	if (SupersamplingRatio < 1)
	{
		OutErrors.Add(TEXT("SupersamplingRatio must be at least 1"));
		bValid = false;
	}
	if (UpscalingRatio < 1.0f)
	{
		OutErrors.Add(TEXT("UpscalingRatio must be at least 1"));
		bValid = false;
	}
	if (FixedFrameRate < 0.0f)
	{
		OutErrors.Add(TEXT("FixedFrameRate must be >= 0"));
		bValid = false;
	}
	if (CameraCutTranslationThreshold < 0.0f)
	{
		OutErrors.Add(TEXT("CameraCutTranslationThreshold cannot be negative"));
		bValid = false;
	}
	if (CameraCutRotationThresholdDegrees < 0.0f)
	{
		OutErrors.Add(TEXT("CameraCutRotationThresholdDegrees cannot be negative"));
		bValid = false;
	}
	return bValid;
}

// Compute shader for generating ground truth textures from the super-high-res rendered textures.
class FNGDCGroundTruthCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNGDCGroundTruthCS);
	SHADER_USE_PARAMETER_STRUCT(FNGDCGroundTruthCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, InSceneColor)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, InSceneVelocity)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, InSceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutGroundTruthColor)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutGroundTruthVelocity)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FNGDCGroundTruthCS, "/Plugin/NeuralGraphicsDataCapture/Private/NGDCGroundTruthCS.usf", "GroundTruthCS", SF_Compute);

// Compute shader for generating low-res jittered textures from the super-high-res rendered textures.
class FNGDCJitteredDecimateCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNGDCJitteredDecimateCS);
	SHADER_USE_PARAMETER_STRUCT(FNGDCJitteredDecimateCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)

		SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, InSceneColor)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, InSceneVelocity)
		SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, InSceneDepth)
		SHADER_PARAMETER(FVector2f, JitterPixelsLowRes)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutJitteredColor)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutJitteredDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutJitteredVelocity)
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FNGDCJitteredDecimateCS, "/Plugin/NeuralGraphicsDataCapture/Private/NGDCJitteredDecimateCS.usf", "JitteredDecimateCS", SF_Compute);

// This ITemporalUpscaler implementation allows us to get access to the super high-res color, depth and velocity
// buffers that Unreal Engine renders. We then run our compute shaders to downsample/decimate these textures.
// We create an instance of this every frame from the FNGDCSceneViewExtension.
// The object is created on the game thread, but then Fork_GameThread is called where we create a new instance 
// which is passed to the render thread.
class FNGDCTemporalUpscaler : public UE::Renderer::Private::ITemporalUpscaler
{
public:
	FNGDCTemporalUpscaler(
		TFunction<void(TVariant<FNGDCRendering::FCapturedFrame, FString>)> OnCompletionCallback_RenderThread, 
		uint32 FrameNumber,
		int SupersamplingRatio,
		float InUpscalingRatio,
		FVector2f JitterPixelsLowRes,
		bool bInCameraCut
	)
		: OnCompletionCallback_RenderThread(OnCompletionCallback_RenderThread)
		, FrameNumber(FrameNumber)
		, SupersamplingRatio(SupersamplingRatio)
		, UpscalingRatio(InUpscalingRatio)
		, JitterPixelsLowRes(JitterPixelsLowRes)
		, bCameraCut(bInCameraCut)
	{
	}

private:	
	static const TCHAR* const DebugName; // Needs to be same raw pointer value used for both places where this is used.

	// Dummy IHistory implementation. We don't need to store any history but must provide an implementation anyway.
	class FHistory : public IHistory, FRefCountBase
	{
	public:
		uint32 AddRef() const final
		{
			return FRefCountBase::AddRef();
		}

		uint32 Release() const final
		{
			return FRefCountBase::Release();
		}

		uint32 GetRefCount() const final
		{
			return FRefCountBase::GetRefCount();
		}

		// Note this must point to the same TCHAR array as FNGDCTemporalUpscaler::GetDebugName().
		const TCHAR* GetDebugName() const override { return DebugName; }
		uint64 GetGPUSizeBytes() const override { return 0; }
	};

	// Note this must point to the same TCHAR array as FHistory::GetDebugName().
	const TCHAR* GetDebugName() const override { return DebugName; }

	ITemporalUpscaler* Fork_GameThread(const class FSceneViewFamily& ViewFamily) const override
	{
		// Note that the render thread's copy of the scene view family will take ownership of this raw pointer and will free it at the end of the frame.
		return new FNGDCTemporalUpscaler(OnCompletionCallback_RenderThread, FrameNumber, SupersamplingRatio, UpscalingRatio, JitterPixelsLowRes, bCameraCut);
	}

	FOutputs AddPasses(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FInputs& Inputs) const override
	{
		UE_LOG(LogNGDC, VeryVerbose, TEXT("FNGDCTemporalUpscaler::AddPasses"));

		if (bHasAddedPassesToView)
		{
			// For anything other than the first view, simply pass the input through to the output and do not
			// capture anything (we only support capturing from one view at a time).
			FOutputs Outputs;
			FRDGTextureDesc DummyOutputDesc(ETextureDimension::Texture2D, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable, EPixelFormat::PF_FloatRGBA, FClearValueBinding(), Inputs.OutputViewRect.Size(), 1, 1, 1, 1, 0);
			FRDGTextureRef DummyOutput = GraphBuilder.CreateTexture(DummyOutputDesc, TEXT("DummyOutput"));
			AddDrawTexturePass(GraphBuilder, FScreenPassViewInfo(View), Inputs.SceneColor, FScreenPassRenderTarget(DummyOutput, ERenderTargetLoadAction::ENoAction));
			Outputs.FullRes = FScreenPassTexture(DummyOutput);
			Outputs.NewHistory = MakeRefCount<FHistory>();
			return Outputs;
		}
		bHasAddedPassesToView = true;

		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());

		FInt32Point RenderedSize = Inputs.SceneColor.ViewRect.Size(); // Super high-res, e.g. 8k
		FInt32Point GroundTruthSize = Inputs.OutputViewRect.Size(); // e.g. 1080p
		auto CalculateJitteredAxis = [this](int32 AxisValue)
		{
			float Scaled = AxisValue / UpscalingRatio;
			return FMath::Max(1, FMath::RoundToInt(Scaled));
		};
		FInt32Point JitteredInputsSize(CalculateJitteredAxis(GroundTruthSize.X), CalculateJitteredAxis(GroundTruthSize.Y)); // e.g. 540p

		// Check that the rendered size is as we expect. This should have been set up by our screen percentage interface,
		// but may have been affected by other Unreal Engine settings which would mess us up.
		FInt32Point ExpectedRenderedSize = GroundTruthSize * SupersamplingRatio;
		if (RenderedSize != ExpectedRenderedSize)
		{
			// Raise an error
			OnCompletionCallback_RenderThread(
				TVariant<FNGDCRendering::FCapturedFrame, FString>(TInPlaceType<FString>{}, 
					FString::Format(TEXT("Rendered size ({0}x{1}) was not as expected ({2}x{3}). This could be due to a secondary screen percentage."),
						{ RenderedSize.X, RenderedSize.Y, ExpectedRenderedSize.X, ExpectedRenderedSize.Y })));

			// We still need to return something for Unreal Engine to use in the rest of the rendering pipeline, 
			// so just copy the input to the output (resizing as necessary).
			FOutputs Outputs;
			FRDGTextureDesc DummyOutputDesc(ETextureDimension::Texture2D, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable, EPixelFormat::PF_FloatRGBA, FClearValueBinding(), GroundTruthSize, 1, 1, 1, 1, 0);
			FRDGTextureRef DummyOutput = GraphBuilder.CreateTexture(DummyOutputDesc, TEXT("DummyOutput"));
			AddDrawTexturePass(GraphBuilder, FScreenPassViewInfo(View), Inputs.SceneColor, FScreenPassRenderTarget(DummyOutput, ERenderTargetLoadAction::ENoAction));
			Outputs.FullRes = FScreenPassTexture(DummyOutput);
			Outputs.NewHistory = MakeRefCount<FHistory>();
			return Outputs;
		}

		FNGDCRendering::FCapturedFrame CapturedFrame{ GraphBuilder };
		CapturedFrame.FrameNumber = FrameNumber;
		CapturedFrame.bCameraCut = bCameraCut;
		CapturedFrame.JitterPixelsLowRes = JitterPixelsLowRes;
		CapturedFrame.ViewProjectionMatrix = View.ViewMatrices.GetViewProjectionMatrix();
		CapturedFrame.FovXRadians = View.ViewMatrices.ComputeHalfFieldOfViewPerAxis().X * 2.0f;
		CapturedFrame.FovYRadians = View.ViewMatrices.ComputeHalfFieldOfViewPerAxis().Y * 2.0f;
		CapturedFrame.Exposure = View.GetLastEyeAdaptationExposure();
		CapturedFrame.NearClippingDistance = View.NearClippingDistance;

		// Create output textures
		FRDGTextureDesc GroundTruthColorDesc(ETextureDimension::Texture2D, ETextureCreateFlags::UAV | ETextureCreateFlags::ShaderResource, EPixelFormat::PF_FloatRGBA, FClearValueBinding(), GroundTruthSize, 1, 1, 1, 1, 0);
		CapturedFrame.GroundTruthColor = GraphBuilder.CreateTexture(GroundTruthColorDesc, TEXT("GroundTruthColor"));

		FRDGTextureDesc GroundTruthVelocityDesc(ETextureDimension::Texture2D, ETextureCreateFlags::UAV | ETextureCreateFlags::ShaderResource, EPixelFormat::PF_G16R16F, FClearValueBinding(), GroundTruthSize, 1, 1, 1, 1, 0);
		CapturedFrame.GroundTruthVelocity = GraphBuilder.CreateTexture(GroundTruthVelocityDesc, TEXT("GroundTruthVelocity"));

		FRDGTextureDesc JitteredColorDesc(ETextureDimension::Texture2D, ETextureCreateFlags::UAV | ETextureCreateFlags::ShaderResource, EPixelFormat::PF_FloatRGBA, FClearValueBinding(), JitteredInputsSize, 1, 1, 1, 1, 0);
		CapturedFrame.JitteredColor = GraphBuilder.CreateTexture(JitteredColorDesc, TEXT("JitteredColor"));

		FRDGTextureDesc JitteredDepthDesc(ETextureDimension::Texture2D, ETextureCreateFlags::UAV | ETextureCreateFlags::ShaderResource, EPixelFormat::PF_R32_FLOAT, FClearValueBinding(), JitteredInputsSize, 1, 1, 1, 1, 0);
		CapturedFrame.JitteredDepth = GraphBuilder.CreateTexture(JitteredDepthDesc, TEXT("JitteredDepth"));

		FRDGTextureDesc JitteredVelocityDesc(ETextureDimension::Texture2D, ETextureCreateFlags::UAV | ETextureCreateFlags::ShaderResource, EPixelFormat::PF_G16R16F, FClearValueBinding(), JitteredInputsSize, 1, 1, 1, 1, 0);
		CapturedFrame.JitteredVelocity = GraphBuilder.CreateTexture(JitteredVelocityDesc, TEXT("JitteredVelocity"));

		// Run the 'ground truth' shader - this will downsample the super-high res color and velocity to the ground truth resolution.
		FNGDCGroundTruthCS::FParameters* GroundTruthParameters = GraphBuilder.AllocParameters<FNGDCGroundTruthCS::FParameters>();
		GroundTruthParameters->View = View.ViewUniformBuffer;
		GroundTruthParameters->InSceneColor = GetScreenPassTextureInput(Inputs.SceneColor, TStaticSamplerState<ESamplerFilter::SF_Point>::GetRHI());
		GroundTruthParameters->InSceneDepth = GetScreenPassTextureInput(Inputs.SceneDepth, TStaticSamplerState<ESamplerFilter::SF_Point>::GetRHI());
		GroundTruthParameters->InSceneVelocity = GetScreenPassTextureInput(Inputs.SceneVelocity, TStaticSamplerState<ESamplerFilter::SF_Point>::GetRHI());
		GroundTruthParameters->OutGroundTruthColor = GraphBuilder.CreateUAV(CapturedFrame.GroundTruthColor);
		GroundTruthParameters->OutGroundTruthVelocity = GraphBuilder.CreateUAV(CapturedFrame.GroundTruthVelocity);
		TShaderMapRef<FNGDCGroundTruthCS> GroundTruthShader(ShaderMap);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("NGDC GroundTruth"),
			GroundTruthShader,
			GroundTruthParameters,
			FComputeShaderUtils::GetGroupCount(GroundTruthSize, 8));

		// Run the 'jittered decimate' shader - this will decimate the super-high res color/velocity/depth to the 'jittered inputs' resolution
		FNGDCJitteredDecimateCS::FParameters* JitteredDecimateParameters = GraphBuilder.AllocParameters<FNGDCJitteredDecimateCS::FParameters>();
		JitteredDecimateParameters->View = View.ViewUniformBuffer;
		JitteredDecimateParameters->InSceneColor = GetScreenPassTextureInput(Inputs.SceneColor, TStaticSamplerState<ESamplerFilter::SF_Point>::GetRHI());
		JitteredDecimateParameters->InSceneDepth = GetScreenPassTextureInput(Inputs.SceneDepth, TStaticSamplerState<ESamplerFilter::SF_Point>::GetRHI());
		JitteredDecimateParameters->InSceneVelocity = GetScreenPassTextureInput(Inputs.SceneVelocity, TStaticSamplerState<ESamplerFilter::SF_Point>::GetRHI());
		JitteredDecimateParameters->JitterPixelsLowRes = JitterPixelsLowRes;
		JitteredDecimateParameters->OutJitteredColor = GraphBuilder.CreateUAV(CapturedFrame.JitteredColor);
		JitteredDecimateParameters->OutJitteredDepth = GraphBuilder.CreateUAV(CapturedFrame.JitteredDepth);
		JitteredDecimateParameters->OutJitteredVelocity = GraphBuilder.CreateUAV(CapturedFrame.JitteredVelocity);
		TShaderMapRef<FNGDCJitteredDecimateCS> JitteredDecimateShader(ShaderMap);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("NGDC JitteredDecimate"),
			JitteredDecimateShader,
			JitteredDecimateParameters,
			FComputeShaderUtils::GetGroupCount(JitteredInputsSize, 8));


		// It doesn't really matter what we return as the output from our upscaler as we've already captured the data 
		// we need. However we want something that looks reasonable in the viewport so we return the input
		// along with a dummy (empty) history object as we don't need any history to be stored.
		// We also draw a red box around the output to show the user which window is being captured.
		// If there are multiple windows and views (e.g. splitscreen) then this will make it clear exactly what is being captured,
		// as we only capture from one view.
		FRDGTextureDesc OutputDesc(ETextureDimension::Texture2D, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::RenderTargetable, EPixelFormat::PF_FloatRGBA, FClearValueBinding(), Inputs.OutputViewRect.Size(), 1, 1, 1, 1, 0);
		FRDGTextureRef Output = GraphBuilder.CreateTexture(OutputDesc, TEXT("NGDCOutput"));
		AddDrawTexturePass(GraphBuilder, FScreenPassViewInfo(View), Inputs.SceneColor, FScreenPassRenderTarget(Output, ERenderTargetLoadAction::ENoAction));
		AddDrawCanvasPass(GraphBuilder,
			FRDGEventName(TEXT("NGDC Text Overlay")),
			View,
			FScreenPassRenderTarget(Output, ERenderTargetLoadAction::ELoad),
			[&](FCanvas& Canvas)
			{
				auto DrawLine = [&](int X1, int Y1, int X2, int Y2) {
					FCanvasLineItem Line(FVector(X1, Y1, 0), FVector(X2, Y2, 0));
					Line.LineThickness = 5.0f;
					Line.SetColor(FLinearColor::Red);
					Canvas.DrawItem(Line);
				};
				FIntPoint Size = Inputs.OutputViewRect.Size();
				DrawLine(0, 0, Size.X, 0);
				DrawLine(Size.X, 0, Size.X, Size.Y);
				DrawLine(Size.X, Size.Y, 0, Size.Y);
				DrawLine(0, Size.Y, 0, 0);
			});

		FOutputs Outputs;	
		Outputs.FullRes = FScreenPassTexture(Output);
		Outputs.NewHistory = MakeRefCount<FHistory>();

		// Report success!
		OnCompletionCallback_RenderThread(
			TVariant<FNGDCRendering::FCapturedFrame, FString>(
				TInPlaceType<FNGDCRendering::FCapturedFrame>{}, CapturedFrame));

		return Outputs;
	}

	// These min/max functions are used for dynamic resolution and shouldn't matter too much.
	float GetMinUpsampleResolutionFraction() const override { return 0.0f; }
	float GetMaxUpsampleResolutionFraction() const override { return 999.0f; }

	// We only support capturing from one view (e.g. in a split-screen case, we only capture the first player's view),
	// and we ignore all other views.
	mutable bool bHasAddedPassesToView = false;

	// This callback will be called (on the render thread) when this temporal upscaler's 
	// AddPasses function gets called by Unreal Engine. This is where the captured textures are made available.
	// If an error occured, an FString will be provided instead which describes the error.
	TFunction<void(TVariant<FNGDCRendering::FCapturedFrame, FString>)> OnCompletionCallback_RenderThread;

	// Monotonic per-capture frame number reported for the frame being captured.
	uint32 FrameNumber;

	// The ratio of the super-high resolution rendered frames to the downsampled ground truth.
	// If this was 4 and the target resolution was 1920x1080, then we would render at 7680x4320 and then
	// do a 4x4 downsample to get the 1920x1080 ground truth frames, as well as decimate the 7680x4320
	// texture to get the even lower resolution jittered textures.
	int SupersamplingRatio;

	// The ratio of the ground truth resolution to the decimated jittered input resolution.
	// This would be 2 for generating training data for 2x upscaling.
	float UpscalingRatio;

	// The jitter (in pixels of the low-resolution jittered input resolution, between -0.5 and +0.5)
	// to be applied when decimating the super-high res frame to produce the jittered input frame.
	FVector2f JitterPixelsLowRes;

	// Indicates whether this frame coincides with a camera cut.
	bool bCameraCut = false;
};

const TCHAR* const FNGDCTemporalUpscaler::DebugName = TEXT("FNGDCTemporalUpscaler");

// This ISceneViewFamilyScreenPercentage implementation allows us to set the rendering resolution
// to much larger than the window size, which we need for capturing frames.
// We create an instance of this every frame from the FNGDCSceneViewExtension.
// The object is created on the game thread, but then Fork_GameThread is called where we create a new instance 
// which is passed to the render thread.
class FNGDCScreenPercentageDriver : public ISceneViewFamilyScreenPercentage
{
public:
	FNGDCScreenPercentageDriver(int SupersamplingRatio) : SupersamplingRatio(SupersamplingRatio) {}

private:
	ISceneViewFamilyScreenPercentage* Fork_GameThread(const class FSceneViewFamily& ViewFamily) const override 
	{
		// Note that the render thread's copy of the scene view family will take ownership of this raw pointer and will free it at the end of the frame.
		return new FNGDCScreenPercentageDriver(SupersamplingRatio);
	}

	DynamicRenderScaling::TMap<float> GetResolutionFractionsUpperBound() const override 
	{
		return GetResolutionFractions_RenderThread();
	}

	DynamicRenderScaling::TMap<float> GetResolutionFractions_RenderThread() const override 
	{
		DynamicRenderScaling::TMap<float> Result;
		Result.SetAll(1.0f);
		Result[GDynamicPrimaryResolutionFraction] = SupersamplingRatio;
		return Result;
	}

	int SupersamplingRatio;
};

// This ISceneViewExtension allows us to modify Unreal Enines's rendering settings
// as well as intercept the rendered outputs at the correct point in the pipeline.
// 
// This class has various methods which are called by Unreal Engine at points in the rendering flow.
// Some are called on the game thread and some are called on the render thread, and we can't
// assume too much about the synchronisation between these two sets.
//
// Error handling strategy:
// 
//		There are lots of ways that our attempts to modify and intercept
//		the scene rendering can be thwarted, including conflicting Unreal Engine settings. Although we could attempt
//		to force these settings to values that work for us by overriding them in the various overridden 
//		methods here, this is quite tricky (and not always possible) because the engine code has lots of logic
//		between the overridden methods that change things and other scene view extensions or plugins could be messing
//		around too. This could lead to quite a mess!
//		Instead we adopt a more passive approach where we check for incompatible settings and report them as errors
//		through a callback which can be handled centrally.
//
//      We can't check for all the potential errors up-front (as things we need to check won't be determined until later)
//      and we can't stop this scene view extension from being used once it's started (as we will have already declared 
//		ourselves as active), so we have our own 'error' flag to disable further processing once we hit an error, to prevent
//		getting into weird states where some things worked and some things didn't.
class FNGDCSceneViewExtension : public FSceneViewExtensionBase
{
public:
	FNGDCSceneViewExtension(
		const FAutoRegister& AutoRegister, 
		TFunction<void(FString)> ErrorCallback_GameThread, 
		TFunction<void(FNGDCRendering::FCapturedFrame)> CapturedCallback_RenderThread
	)
		: FSceneViewExtensionBase(AutoRegister)
		, ErrorCallback_GameThread(ErrorCallback_GameThread) 
		, CapturedCallback_RenderThread(CapturedCallback_RenderThread)
	{
	}

	void Enable(FNGDCRenderingSettings InSettings, FName InWorldContextHandle)
	{
		bEnabled = true;
		Settings = InSettings;
		WorldContextHandle = InWorldContextHandle;
		CaptureFrameIndex = 0;
		bHasPreviousViewTransform = false;
		bCameraCutThisFrame = false;
		PreviousViewLocation = FVector::ZeroVector;
		PreviousViewRotation = FQuat::Identity;
	}

	void Disable()
	{
		bEnabled = false;
		CaptureFrameIndex = 0;
		bHasPreviousViewTransform = false;
		bCameraCutThisFrame = false;
		PreviousViewLocation = FVector::ZeroVector;
		PreviousViewRotation = FQuat::Identity;
	}

private:
	// Called by Unreal Engine (on the game thread) at the beginning of each frame to determine if it should
	// use this scene view extension for the frame.
	bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override 
	{
		if (!bEnabled)
		{
			return false;
		}

		if (Context.Viewport == nullptr)
		{
			// Ignore anything not connected to a viewport, for example scene captures
			return false;
		}

		// Only intercept frames from the World Context that we are capturing
		FWorldContext* WorldContext = GEngine->GetWorldContextFromHandle(WorldContextHandle);
		if (!WorldContext || !WorldContext->GameViewport)
		{
			return false; // Invalid world context, e.g. PIE session was ended during capture.
		}
		if (WorldContext->GameViewport->Viewport != Context.Viewport)
		{
			return false; // Wrong world (e.g. a different PIE session)
		}

		return true;
	}

	// Called by Unreal Engine (on the game thread).
	void SetupViewFamily(FSceneViewFamily& InViewFamily) override 
	{
		// Reset error flag each frame (it might work this time!)
		bSkipThisViewFamily = false;
		bHasSetupViewThisViewFamily = false;
		bCameraCutThisFrame = false;
	}

	// Called by Unreal Engine (on the game thread).
	void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override
	{
		if (bSkipThisViewFamily || bHasSetupViewThisViewFamily)
		{
			return;
		}
		bHasSetupViewThisViewFamily = true;

		// We need screen percentage to be enabled as this is how we get Unreal Engine to render at super-high res.
		// Even though this is a property of the view *family*, there is some logic in UGameViewportClient::Draw which can modify
		// this propery inbetween SetupViewFamily and SetupView, so we check the final value here.
		if (!InViewFamily.EngineShowFlags.ScreenPercentage)
		{
			ErrorCallback_GameThread(TEXT("ScreenPercentage Show Flag is disabled"));
			bSkipThisViewFamily = true;
			return;
		}

		// We need Unreal Engine to call into our temporal upscaler, so it needs to have selected temporal upscaling.
		// This field is set when constructing the FSceneView, before calling SetupView.
		if (InView.PrimaryScreenPercentageMethod != EPrimaryScreenPercentageMethod::TemporalUpscale)
		{
			// There are a few things that can cause this to fail.
			FString PossibleCauses;
			if (!InViewFamily.EngineShowFlags.AntiAliasing)
			{
				PossibleCauses += "AntiAliasing Show Flag must be enabled. ";
			}
			if (!InViewFamily.EngineShowFlags.PostProcessing)
			{
				PossibleCauses += "PostProcessing Show Flag must be enabled. ";
			}
			if (InView.AntiAliasingMethod != EAntiAliasingMethod::AAM_TemporalAA && InView.AntiAliasingMethod != EAntiAliasingMethod::AAM_TSR)
			{
				PossibleCauses += "AntiAliasingMethod must be set to TAA or TSR. ";
			}
			if (InView.AntiAliasingMethod == EAntiAliasingMethod::AAM_TemporalAA)
			{
				IConsoleVariable* CVarEnableTemporalUpsample = IConsoleManager::Get().FindConsoleVariable(TEXT("r.TemporalAA.Upsampling"));
				if (CVarEnableTemporalUpsample && CVarEnableTemporalUpsample->GetInt() != 1)
				{
					PossibleCauses += "When AntiAliasingMethod is TAA, r.TemporalAA.Upsampling must be 1. ";
				}
			}

			ErrorCallback_GameThread(FString::Format(TEXT("PrimaryScreenPercentageMethod is not TemporalUpscale (possible causes: {0})"), { PossibleCauses }));
			bSkipThisViewFamily = true;
			return;
		}

		// Even if PrimaryScreenPercentageMethod is set to TemporalUpscale, Unreal Engine won't use our temporal upscaler
		// interface if it's disabled via this console variable
		IConsoleVariable* CVarUseTemporalAAUpscaler = IConsoleManager::Get().FindConsoleVariable(TEXT("r.TemporalAA.Upscaler"));
		if (CVarUseTemporalAAUpscaler && CVarUseTemporalAAUpscaler->GetInt() != 1)
		{
			ErrorCallback_GameThread(TEXT("r.TemporalAA.Upscaler must be 1"));
			bSkipThisViewFamily = true;
			return;
		}

		if (InViewFamily.GetScreenPercentageInterface() != nullptr)
		{
			// It's not allowed to set a screen percentage interface if one is already set.
			ErrorCallback_GameThread(TEXT("Another screen percentage interface is being used"));
			bSkipThisViewFamily = true;
			return;
		}

		// We don't want the high-res rendering to be jittered (which it would be by default as we have TAA/TSR enabled).
		// (We apply our own jitter in our compute shaders.)
		InView.bAllowTemporalJitter = false;

		// Set our screen percentage interface so that we can render the scene at super-high resolution
		// Note that the docs for SetScreenPercentageInterface say that it should be called in BeginRenderViewFamily(), but this isn't possible
		// because Unreal Engine will already have set it by then. We also can't set it in SetupViewFamily() because that causes an error for being set too early!
		// Note that the scene view family takes ownership of the raw pointer and will free it at the end of the frame.
		InViewFamily.SetScreenPercentageInterface(new FNGDCScreenPercentageDriver(Settings.SupersamplingRatio));

		UpdateCameraCutState(InViewFamily, InView);
	}

	void UpdateCameraCutState(const FSceneViewFamily& InViewFamily, const FSceneView& InView)
	{
		if (bSkipThisViewFamily)
		{
			return;
		}

		bCameraCutThisFrame = bCameraCutThisFrame || InView.bCameraCut;

		const bool bTranslationCheckEnabled = Settings.CameraCutTranslationThreshold > 0.0f;
		const bool bRotationCheckEnabled = Settings.CameraCutRotationThresholdDegrees > 0.0f;
		if (!bCameraCutThisFrame && bHasPreviousViewTransform && (bTranslationCheckEnabled || bRotationCheckEnabled))
		{
			const FVector CurrentLocation = InView.ViewLocation;
			const FQuat CurrentRotation = InView.ViewRotation.Quaternion();
			const float TranslationDelta = bTranslationCheckEnabled ? FVector::Dist(PreviousViewLocation, CurrentLocation) : 0.0f;
			const float RotationDeltaDegrees = bRotationCheckEnabled ? FMath::RadiansToDegrees(PreviousViewRotation.AngularDistance(CurrentRotation)) : 0.0f;

			if ((bTranslationCheckEnabled && TranslationDelta >= Settings.CameraCutTranslationThreshold) ||
				(bRotationCheckEnabled && RotationDeltaDegrees >= Settings.CameraCutRotationThresholdDegrees))
			{
				bCameraCutThisFrame = true;
				UE_LOG(LogNGDC, Verbose, TEXT("Camera cut detected heuristically (translation delta %.2f cm, rotation delta %.2f deg)."), TranslationDelta, RotationDeltaDegrees);
			}
		}

		PreviousViewLocation = InView.ViewLocation;
		PreviousViewRotation = InView.ViewRotation.Quaternion();
		bHasPreviousViewTransform = true;
	}

	// Called by Unreal Engine (on the game thread).
	void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override
	{
		if (bSkipThisViewFamily)
		{
			return;
		}

		if (InViewFamily.GetTemporalUpscalerInterface() != nullptr)
		{
			// It's not allowed to set a temporal upscaler interface if one is already set.
			ErrorCallback_GameThread(FString::Format(TEXT("Another temporal upscaler is being used ({0})"), { InViewFamily.GetTemporalUpscalerInterface()->GetDebugName() }));
			bSkipThisViewFamily = true;
			return;
		}

		// Choose the next jitter sample using Halton sequences
		// Note sequence lengths are one less than a power of the base, to avoid bias
		// Also we start at index 1, which is the first valid index
		float JitterX = Halton((CaptureFrameIndex % 31) + 1, 2);
		float JitterY = Halton((CaptureFrameIndex % 26) + 1, 3);
		FVector2f JitterPixelsLowRes(JitterX, JitterY);

		const FSceneView* PrimaryView = InViewFamily.Views.Num() > 0 ? InViewFamily.Views[0] : nullptr;
		if (PrimaryView)
		{
			// Quantize the jitter on each axis, grid size should be ratio of super-high res to input res e.g. (7680,4320)/(960,540).
			const FIntPoint GroundTruthSize = PrimaryView->UnscaledViewRect.Size();
			const FIntPoint JitteredInputsSize(
				FMath::Max(1, FMath::RoundToInt(GroundTruthSize.X / Settings.UpscalingRatio)),
				FMath::Max(1, FMath::RoundToInt(GroundTruthSize.Y / Settings.UpscalingRatio))
			);
			const FIntPoint RenderedSize = GroundTruthSize * Settings.SupersamplingRatio;
			const FVector2f QuantGridSize(
				RenderedSize.X / static_cast<float>(JitteredInputsSize.X),
				RenderedSize.Y / static_cast<float>(JitteredInputsSize.Y)
			);
			auto QuantizeJitterValue = [](float JitterValue, float GridSize)
			{
				if (GridSize <= 1.0f)
				{
					return JitterValue;
				}
				const float Quantized = (FMath::Floor(JitterValue * GridSize) + 0.5f) / GridSize;
				return Quantized;
			};
			JitterPixelsLowRes.X = QuantizeJitterValue(JitterPixelsLowRes.X, QuantGridSize.X);
			JitterPixelsLowRes.Y = QuantizeJitterValue(JitterPixelsLowRes.Y, QuantGridSize.Y);
		}

		// We want jitter between -0.5 and +0.5 so it is from the centre of the pixel.
		JitterPixelsLowRes.X -= 0.5;
		JitterPixelsLowRes.Y -= 0.5;

		// Set our temporal upscaler interface so that we can get access to the rendered color/velocity/depth buffers
		// Note that the scene view family takes ownership of the raw pointer and will free it at the end of the frame.
		InViewFamily.SetTemporalUpscalerInterface(new FNGDCTemporalUpscaler(
			[&](auto Result) { this->TemporalUpscalerOnCompletion_RenderThread(Result); },
			CaptureFrameIndex, Settings.SupersamplingRatio, Settings.UpscalingRatio, JitterPixelsLowRes, bCameraCutThisFrame)
		);

		++CaptureFrameIndex;
	}

	// Called by our FNGDCTemporalUpscaler (on the render thread) when it has captured a frame (or encountered an error).
	void TemporalUpscalerOnCompletion_RenderThread(TVariant<FNGDCRendering::FCapturedFrame, FString> Result)
	{
		if (FNGDCRendering::FCapturedFrame* CapturedFrame = Result.TryGet<FNGDCRendering::FCapturedFrame>())
		{
			// Capturing succeeded - call our callback to let the parent subsystem handle the captured frames.
			CapturedCallback_RenderThread(*CapturedFrame);
		}
		else if (FString* Error = Result.TryGet<FString>())
		{
			// We're on the render thread here, but the error callback needs to be called on the game thread
			AsyncTask(ENamedThreads::GameThread, [this, Error = *Error]() { this->ErrorCallback_GameThread(Error); });
		}
		bTemporalUpscalerUsedThisViewFamily_RenderThread = true;
	}

	// Called by Unreal Engine (on the render thread).
	void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override
	{
		// Reset error flag
		bTemporalUpscalerUsedThisViewFamily_RenderThread = false;
	}

	// Called by Unreal Engine (on the render thread).
	void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override
	{
		// Detect the case where rendering finished but our temporal upscaler was never called 
		// (e.g. a setting that causes Unreal Engine to ignore it).
		// This means that we won't have captured any data so we report an error. 
		if (!bTemporalUpscalerUsedThisViewFamily_RenderThread)
		{
			// We're on the render thread here, but the error callback needs to be called on the game thread
			AsyncTask(ENamedThreads::GameThread, [&]() { this->ErrorCallback_GameThread("Temporal upscaler was never called"); });
		}
	}

	// As this object is always registered as a scene view extension (even when not capturing),
	// we need this flag to indicate whether we should be intercepting and capturing frames or not.
	bool bEnabled = false;
	FNGDCRenderingSettings Settings; // Settings for the current capture.	
	// Handle for the World Context which we are capturing frames for. We will only capture frames that are rendered
	// for this world.
	FName WorldContextHandle;
	uint32 CaptureFrameIndex = 0; // Monotonic per-capture index used for jitter and output frame numbering.

	// We want to gracefully handle cases where we can't capture frames due to Unreal Engine settings
	// preventing our code from running (e.g. if anti aliasing is disabled). 
	// Unfortunately we can't detect all these up-front and so 
	// may have already started setting things up for capturing this frame before we realize that we can't
	// continue. This flag is used to remember if this happens so that later steps can be skipped.
	bool bSkipThisViewFamily = false;

	// We only support capturing from one view (e.g. in a split-screen case, we only capture the first player's view),
	// and we ignore all other views.
	bool bHasSetupViewThisViewFamily = false;

	// Camera cut detection state.
	bool bCameraCutThisFrame = false;
	bool bHasPreviousViewTransform = false;
	FVector PreviousViewLocation = FVector::ZeroVector;
	FQuat PreviousViewRotation = FQuat::Identity;

	// Although most of the setup logic here occurs on the game thread, some decisions are made by Unreal Engine on 
	// the render thread after our temporal upscaler and screen percentage driver have been forked.
	// There can still be problems after this point if Unreal Engine decides not to use our interfaces, so 
	// we keep track of whether our temporal upscaler was actually used.
	// Note the render thread is not synchronised with the game thread so this won't refer to the same frame
	// as the bSkipThisViewFamily flag!
	bool bTemporalUpscalerUsedThisViewFamily_RenderThread = false;

	// Callback which this object calls (on the game thread) when an error occurs, used to report errors to the parent subsystem.
	TFunction<void(FString)> ErrorCallback_GameThread;

	// We will call this callback (on the render thread) once a frame has been captured.
	// (Or at least, once the commands to capture it have been enqueued into the render graph).
	TFunction<void(FNGDCRendering::FCapturedFrame)> CapturedCallback_RenderThread;
};

FNGDCRendering::FNGDCRendering(TFunction<void(FString)> ErrorCallback_GameThread, TFunction<void(FNGDCRendering::FCapturedFrame)> CapturedCallback_RenderThread)
	: SceneViewExtension(FSceneViewExtensions::NewExtension<FNGDCSceneViewExtension>(ErrorCallback_GameThread, CapturedCallback_RenderThread))
{
}

FNGDCRendering::~FNGDCRendering()
{
	// In case this object is destroyed while capturing is still in progress, end it cleanly.
	EndCapture();
}

void FNGDCRendering::BeginCapture(FNGDCRenderingSettings Settings, FName WorldContextHandle)
{
	SceneViewExtension->Enable(Settings, WorldContextHandle);
}

void FNGDCRendering::EndCapture()
{
	// Wait for any in-flight rendering to finish, so that we get a clean end to the capture.
	FlushRenderingCommands();

	SceneViewExtension->Disable();
}

#if NGDC_DEBUGGING_ENABLED
UE_ENABLE_OPTIMIZATION
#endif
