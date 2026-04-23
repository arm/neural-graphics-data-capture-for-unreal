// SPDX-FileCopyrightText: Copyright 2025-2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Templates/SharedPointer.h"
#include "Templates/Function.h"
#include "RenderGraphFwd.h"

#include "NGDCRendering.generated.h"

// Settings for the 'rendering' part of the data capture plugin.
USTRUCT(BlueprintType)
struct FNGDCRenderingSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings", meta = (ToolTip = "Supersampling ratio for rendering high-quality frames before downsampling/decimation. Lowering this value can save GPU time and memory but will reduce quality of captured frames. Be very careful with high values!", ClampMin = 1, ClampMax = 8))
	int SupersamplingRatio = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings", meta = (ToolTip = "Upscaling ratio between the jittered inputs and the ground truth outputs.", ClampMin = "1.0", ClampMax = "8.0"))
	float UpscalingRatio = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Timing", meta = (ToolTip = "When > 0, fixes the frame rate during capture and writes it to the output JSON (EmulatedFramerate). Set to 0 to leave the current frame rate unchanged.", ClampMin = "0.0"))
	float FixedFrameRate = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Cuts", meta = (ToolTip = "World-space distance (cm) that triggers a camera cut heuristic when the automatic flag is missing. Set to 0 to disable the translation check.", ClampMin = "0.0"))
	float CameraCutTranslationThreshold = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Cuts", meta = (ToolTip = "Angular delta (degrees) that triggers a camera cut heuristic when the automatic flag is missing. Set to 0 to disable the rotation check.", ClampMin = "0.0", ClampMax = "180.0"))
	float CameraCutRotationThresholdDegrees = 30.0f;

	bool Validate(TArray<FString>& OutErrors) const;
};

// Responsible for the high-resolution scene rendering and downsampling/decimation to produce
// the textures that we want to capture.
// Capturing is enabled/disabled with the BeginCapture() and EndCapture() methods, and captured
// frames are reported via. the callback provided at construction time.
class FNGDCRendering 
{ 
public:
	// Details of a frame that has been captured. Note that the textures here are RDG textures
	// so won't actually have been rendered yet (just recorded into the render graph).
	struct FCapturedFrame
	{
		FRDGBuilder& GraphBuilder; // The graph builder that owns the textures below
		uint32 FrameNumber;
		bool bCameraCut = false;

		FVector2f JitterPixelsLowRes; // The jitter (in pixels in the low resolution space, so between -0.5 and +0.5) applied to the jittered textures below
		FMatrix ViewProjectionMatrix;
		float FovXRadians = 0.0f;
		float FovYRadians = 0.0f;
		float Exposure = 0.0f;
		float NearClippingDistance;
		FIntPoint GroundTruthExtent;

		FRDGTextureRef GroundTruthColor;
		FRDGTextureRef GroundTruthVelocity;
		FRDGTextureRef JitteredColor;
		FRDGTextureRef JitteredDepth;
		FRDGTextureRef JitteredVelocity;
	};

	FNGDCRendering(TFunction<void(FString)> ErrorCallback_GameThread, TFunction<void(FNGDCRendering::FCapturedFrame)> CapturedCallback_RenderThread);
	~FNGDCRendering();

	void BeginCapture(FNGDCRenderingSettings Settings, FName WorldContextHandle);
	void EndCapture();

private:
	// We use an ISceneViewExtension to hook into Unreal Engine's rendering which needs to be kept alive,
	// so we keep it in a member variable here.
	// Note that once this extension is registered, it's not reliably possible to unregister it 
	// (even though Unreal Engine stores these as weak ptrs) because there are strong references stored
	// by the render thread, so it depends on the threading behaviour as to whether it would actually
	// get cleaned up or not. Therefore we register this once when our plugin loads and keep it forever,
	// turning it on and off as required.
	TSharedRef<class FNGDCSceneViewExtension> SceneViewExtension;
};
