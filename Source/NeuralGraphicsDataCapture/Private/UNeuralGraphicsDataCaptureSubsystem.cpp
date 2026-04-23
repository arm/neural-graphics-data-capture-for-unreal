// SPDX-FileCopyrightText: Copyright 2025-2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: Apache-2.0

#include "UNeuralGraphicsDataCaptureSubsystem.h"
#include "NeuralGraphicsDataCaptureModule.h"
#include "GameDelegates.h"
#include "RenderGraphResources.h"
#include "Engine/Engine.h"

#if NGDC_DEBUGGING_ENABLED
UE_DISABLE_OPTIMIZATION
#endif

bool FNGDCCaptureSettings::Validate(TArray<FString>& OutErrors) const
{
	bool bValid = true;
	bValid &= Rendering.Validate(OutErrors);
	bValid &= Export.Validate(OutErrors);
	return bValid;
}

void UNeuralGraphicsDataCaptureSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	UE_LOG(LogNGDC, Verbose, TEXT("UNeuralGraphicsDataCaptureSubsystem::Initialize"));

	// Create rendering sub-object, hooking up callbacks to ourself
	Rendering = MakeUnique<FNGDCRendering>(
		[&](FString Error) { OnErrorDuringCapture_GameThread(Error); },
		[&](FNGDCRendering::FCapturedFrame CapturedFrame) { OnFrameCaptured_RenderThread(CapturedFrame); });
}

void UNeuralGraphicsDataCaptureSubsystem::Deinitialize()
{
	UE_LOG(LogNGDC, Verbose, TEXT("UNeuralGraphicsDataCaptureSubsystem::Deinitialize"));

	// In case the engine shuts down whilst still capturing, end it now to make sure it finishes cleanly
	if (IsCapturing())
	{
		EndCapture();
	}
}

void UNeuralGraphicsDataCaptureSubsystem::BeginCapture(FNGDCCaptureSettings Settings)
{
	UE_LOG(LogNGDC, Verbose, TEXT("UNeuralGraphicsDataCaptureSubsystem::BeginCapture"));

	if (IsCapturing())
	{
		OnErrorOutsideCapture(TEXT("Can't begin capture - already capturing!"));
		return;
	}

	// Check if the settings are valid. 
	// Even though Unreal Engine will automatically validate in some cases (e.g. using the metadata on 
	// the fields (e.g. ClampMin/Max) when the property editor), this doesn't catch all cases 
	// and an invalid value could still be set by C++ code (for example). 
	// Therefore we provide our own validation check as well.
	TArray<FString> Errors;
	if (!Settings.Validate(Errors))
	{
		OnErrorOutsideCapture(TEXT("Settings are invalid:\n") + FString::Join(Errors, TEXT("\n")));
		return;
	}

	// Decide which WorldContext we are going to capture. There can be many worlds (and thus many viewports)
	// alive at the same time in the editor (e.g. the editor window, multiple PIE instances) and for now we 
	// only support capturing one of these.
	FName WorldContextHandle;
	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		if (WorldContext.WorldType == EWorldType::Game || WorldContext.WorldType == EWorldType::PIE)
		{
			// For now we simply pick the first Game or PIE world, with no way for the user to override this.
			// In future this could be improved by checking which window is focused, for example.
			// Store the *handle* of the world context, not the pointer (as this can be persisted, the pointers shouldn't be stored)
			WorldContextHandle = WorldContext.ContextHandle;
			break;
		}
	}
	if (WorldContextHandle.IsNone())
	{
		OnErrorOutsideCapture(TEXT("Unable to find a valid World Context"));
		return;
	}

	// Create capture context object to hold state for this capture
	CurrentCapture = MakeUnique<FNGDCCaptureContext>();
	CurrentCapture->Export = MakeUnique<FNGDCExport>(Settings.Export, Settings.Rendering.UpscalingRatio, Settings.Rendering.FixedFrameRate, [&](FString Error) { OnErrorDuringCapture_GameThread(Error); });
	FString Error;
	if (!CurrentCapture->Export->Initialize(Error))
	{
		OnErrorOutsideCapture(Error);
		CurrentCapture.Reset();
		return;
	}

	CurrentCapture->WorldContextHandle = WorldContextHandle;
	CurrentCapture->Settings = Settings;

	if (Settings.Rendering.FixedFrameRate > 0.0f)
	{
		CurrentCapture->bAppliedFixedFrameRate = true;
		CurrentCapture->bPrevUseFixedFrameRate = GEngine->bUseFixedFrameRate;
		CurrentCapture->PrevFixedFrameRate = GEngine->FixedFrameRate;
		GEngine->bUseFixedFrameRate = true;
		GEngine->FixedFrameRate = Settings.Rendering.FixedFrameRate;
	}

	// Register a callback to be told when a world gets destroyed, so we can stop capturing
	// if the world we were capturing gets destroyed.
	FGameDelegates::Get().GetEndPlayMapDelegate().AddUObject(this, &UNeuralGraphicsDataCaptureSubsystem::OnEndPlayMap);

	// Show a notification in the corner of the editor to indicate that capturing is ongoing
	FAsyncTaskNotificationConfig NotificationConfig;
	NotificationConfig.TitleText = FText::AsCultureInvariant("Neural Graphics Data Capture");
	NotificationConfig.ProgressText = FText::AsCultureInvariant("Capturing...");
	NotificationConfig.bKeepOpenOnFailure = true; // To make it obvious to the user when something has gone wrong
	NotificationConfig.bCanCancel = true; // A 'Cancel' button 
	NotificationConfig.ExpireDuration = 5.0f; // Increase the expire duration so the user has time to click the hyperlink if they want
	CurrentCapture->Notification = MakeUnique<FAsyncTaskNotification>(NotificationConfig);
	// Add a hyperlink to open the dataset folder in Explorer
	CurrentCapture->Notification->SetHyperlink(
		FSimpleDelegate::CreateLambda([Path = CurrentCapture->Export->GetAbsJsonPath()]() { FPlatformProcess::ExploreFolder(*Path); }),
		FText::AsCultureInvariant(TEXT("Open dataset folder")));

	// The rendering object isn't part of the capture context, so start it manually
	Rendering->BeginCapture(Settings.Rendering, WorldContextHandle);
	
	// Start timer for periodically checking if the user clicked 'Cancel' on the notification
	CurrentCapture->TimerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UNeuralGraphicsDataCaptureSubsystem::OnTimer), 0.1f);

	TRACE_BEGIN_REGION(TEXT("NeuralGraphicsDataCapture")); // Mark regions where we're capturing frames so they appear in Unreal Insights
}

void UNeuralGraphicsDataCaptureSubsystem::EndCapture()
{
	UE_LOG(LogNGDC, Verbose, TEXT("UNeuralGraphicsDataCaptureSubsystem::EndCapture"));

	if (!IsCapturing())
	{
		OnErrorOutsideCapture(TEXT("Can't end capture - not capturing"));
		return;
	}

	EndCapture_Internal(FString()); // Call internal version with no error reason
}

void UNeuralGraphicsDataCaptureSubsystem::EndCapture_Internal(FString ErrorReason)
{
	UE_LOG(LogNGDC, Verbose, TEXT("UNeuralGraphicsDataCaptureSubsystem::EndCapture_Internal"));
	check(CurrentCapture);

	TRACE_END_REGION(TEXT("NeuralGraphicsDataCapture")); // Mark regions where we're capturing frames so they appear in Unreal Insights

	// Stop timer for periodically checking if the user clicked 'Cancel' on the notification
	FTSTicker::GetCoreTicker().RemoveTicker(CurrentCapture->TimerHandle);

	if (CurrentCapture->bAppliedFixedFrameRate)
	{
		GEngine->bUseFixedFrameRate = CurrentCapture->bPrevUseFixedFrameRate;
		GEngine->FixedFrameRate = CurrentCapture->PrevFixedFrameRate;
	}

	// The rendering object isn't part of the capture context, so stop it manually.
	// Note that it's important we stop this before stopping the export, so that in-flight frames get
	// queued up for export and then we wait for those frames to be exported.
	Rendering->EndCapture();

	// Destroy capture context object to clean up state for this capture
	TUniquePtr<FAsyncTaskNotification> Notification = MoveTemp(CurrentCapture->Notification);
	int32 NumFramesExported = CurrentCapture->Export->GetNumFramesExported();
	CurrentCapture.Reset();

	FGameDelegates::Get().GetEndPlayMapDelegate().RemoveAll(this);

	// Update notification to show the final state. Note we do this as the final step after we've
	// cleaned up the current capture state, to make sure everything has stopped/finished so that no more
	// errors can be reported.
	bool bSuccess = ErrorReason.IsEmpty();
	FText Message = bSuccess ?
		FText::AsCultureInvariant(FString::Printf(TEXT("Captured %d frames"), NumFramesExported)) :
		FText::AsCultureInvariant(FString::Printf(TEXT("%s after capturing %d frames"), *ErrorReason, NumFramesExported));

	Notification->SetComplete(bSuccess);
	Notification->SetProgressText(Message);
}

bool UNeuralGraphicsDataCaptureSubsystem::IsCapturing() const
{
	return CurrentCapture.IsValid();
}

void UNeuralGraphicsDataCaptureSubsystem::OnErrorDuringCapture_GameThread(FString Error)
{
	UE_LOG(LogNGDC, Error, TEXT("Error whilst capturing frames: %s"), *Error);

	// Even though we expect there will always be a current capture at this point (given the name of this function!),
	// because errors may get queued up on the game thread from other threads, it's possible this might get called
	// after the current capture has finished, in which case we'll just log it but not do any of the more advanced stuff
	// below.
	if (CurrentCapture)
	{
#if WITH_EDITOR
		if (CurrentCapture->Notification)
		{
			// Replace the hyperlink with a link to show the Output Log window, so the user can easily the error(s) that occurred.
			CurrentCapture->Notification->SetHyperlink(
				FSimpleDelegate::CreateLambda([]() { FGlobalTabmanager::Get()->TryInvokeTab(FName("OutputLog")); }),
				FText::AsCultureInvariant(TEXT("Show errors")));
		}
#endif

		// Although we may be able to continue after an error (and even capture valid frames if the user
		// can correct the issue), this might lead to incomplete/missing data which could cause confusion later.
		// Instead we just stop the capture as soon as possible and notify the user that error(s) occurred.
		// 
		// Rather than calling EndCapture_Internal directly we defer it to the next tick to avoid any 
		// re-entrancy if an error was to be raised whilst we are already ending a capture (e.g. in the destructor
		// of FNGDCExport).
		ExecuteOnGameThread(TEXT(""), [this]() {
			if (CurrentCapture) // Capture may already have been stopped by the time this queued function gets executed
			{
				EndCapture_Internal(TEXT("Error(s) encountered"));
			}
		});
	}
}

void UNeuralGraphicsDataCaptureSubsystem::OnErrorOutsideCapture(FString Error)
{
	UE_LOG(LogNGDC, Error, TEXT("%s"), *Error);

	// Show a notification of the error in the corner of the editor. As there isn't an ongoing capture
	// we have to make a new notification for each error.
	// This is useful because log messages are easy to miss (you may not even have the Output Log window open)
	// and so you might not realise why your capture failed to start. It's also consistent with how we show
	// errors when there _is_ a capture ongoing.
	FAsyncTaskNotificationConfig NotificationConfig;
	NotificationConfig.TitleText = FText::AsCultureInvariant("Neural Graphics Data Capture");
	NotificationConfig.ProgressText = FText::AsCultureInvariant(FString::Printf(TEXT("Error: %s"), *Error));
	NotificationConfig.ExpireDuration = 5.0f; // Increase the expire duration so the user has time to read the error
	FAsyncTaskNotification Notification(NotificationConfig);
#if WITH_EDITOR
	// Add a hyperlink with a link to show the Output Log window, so the user can easily see the full log
	Notification.SetHyperlink(
		FSimpleDelegate::CreateLambda([]() { FGlobalTabmanager::Get()->TryInvokeTab(FName("OutputLog")); }),
		FText::AsCultureInvariant(TEXT("Show log")));
#endif
	Notification.SetComplete(false);
}

void UNeuralGraphicsDataCaptureSubsystem::OnFrameCaptured_RenderThread(FNGDCRendering::FCapturedFrame RenderingCapturedFrame)
{
	UE_LOG(LogNGDC, VeryVerbose, TEXT("UNeuralGraphicsDataCaptureSubsystem::OnFrameCaptured_RenderThread"));
	check(CurrentCapture);

	// Sanity check that we haven't received data for the same frame twice. 
	// We can't reliably report multiple frames to the Export object with the same frame number as it will cause corruption.
	// This could happen if the Rendering object captures different View Families being rendered within the same frame.
	// Although we do try to only capture the 'main' view, there might be some missing checks and so we 
	// do a final check here. 
	if (RenderingCapturedFrame.FrameNumber < CurrentCapture->Export->GetNumFramesExported())
	{
		UE_LOG(LogNGDC, Warning, TEXT("Skipping captured frame %d as we have already received data for this frame"), RenderingCapturedFrame.FrameNumber);
		return;
	}

	// Convert the rendering class' FCapturedFrame output to the export class' FCapturedFrame input.
	// (Though they are very similar, this keeps the interfaces independent)
	FNGDCExport::FCapturedFrame ExportCapturedFrame{ RenderingCapturedFrame.GraphBuilder };
	ExportCapturedFrame.FrameNumber = RenderingCapturedFrame.FrameNumber;
	ExportCapturedFrame.bCameraCut = RenderingCapturedFrame.bCameraCut;
	ExportCapturedFrame.JitterPixelsLowRes = RenderingCapturedFrame.JitterPixelsLowRes;
	ExportCapturedFrame.ViewProjectionMatrix = RenderingCapturedFrame.ViewProjectionMatrix;
	ExportCapturedFrame.FovXRadians = RenderingCapturedFrame.FovXRadians;
	ExportCapturedFrame.FovYRadians = RenderingCapturedFrame.FovYRadians;
	ExportCapturedFrame.Exposure = RenderingCapturedFrame.Exposure;
	ExportCapturedFrame.NearClippingDistance = RenderingCapturedFrame.NearClippingDistance;
	ExportCapturedFrame.GroundTruthColor = RenderingCapturedFrame.GroundTruthColor;
	ExportCapturedFrame.GroundTruthVelocity = RenderingCapturedFrame.GroundTruthVelocity;
	ExportCapturedFrame.JitteredColor = RenderingCapturedFrame.JitteredColor;
	ExportCapturedFrame.JitteredDepth = RenderingCapturedFrame.JitteredDepth;
	ExportCapturedFrame.JitteredVelocity = RenderingCapturedFrame.JitteredVelocity;
	ExportCapturedFrame.JitteredColorExtent = RenderingCapturedFrame.JitteredColor->Desc.Extent;
	ExportCapturedFrame.GroundTruthExtent = RenderingCapturedFrame.GroundTruthColor->Desc.Extent;

	CurrentCapture->Export->ExportFrame_RenderThread(ExportCapturedFrame);

	// Update the notification to show how many frames we've captured. Note that it's safe to call this from the render thread.
	CurrentCapture->Notification->SetProgressText(FText::AsCultureInvariant(FString::Printf(TEXT("Captured %d frames..."), CurrentCapture->Export->GetNumFramesExported())));
}

bool UNeuralGraphicsDataCaptureSubsystem::OnTimer(float DeltaTime)
{
	check(CurrentCapture);
	// End the current capture if cancelled by the user from the notification
	if (CurrentCapture->Notification->GetPromptAction() == EAsyncTaskNotificationPromptAction::Cancel)
	{
		EndCapture_Internal(TEXT("Cancelled"));
	}
	return true; // Keep timer going. It will be stopped when the current capture finishes.
}

void UNeuralGraphicsDataCaptureSubsystem::OnEndPlayMap()
{
	check(CurrentCapture);
	// If the world we are capturing is no longer valid (i.e. it was just destroyed) then 
	// cancel the capture as we will never be able to get any more frames from it.
	if (!GEngine->GetWorldContextFromHandle(CurrentCapture->WorldContextHandle))
	{
		EndCapture_Internal(TEXT("Cancelled"));
	}
}

#if NGDC_DEBUGGING_ENABLED
UE_ENABLE_OPTIMIZATION
#endif
