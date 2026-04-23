// SPDX-FileCopyrightText: Copyright 2025-2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Subsystems/EngineSubsystem.h"
#include "Misc/AsyncTaskNotification.h"
#include "Containers/Ticker.h"
#include "NGDCRendering.h"
#include "NGDCExport.h"

#include "UNeuralGraphicsDataCaptureSubsystem.generated.h"

// All the settings that control an individual capture.
USTRUCT(BlueprintType)
struct FNGDCCaptureSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings", meta = (ToolTip = "Settings related to rendering the frames."))
	FNGDCRenderingSettings Rendering;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings", meta = (ToolTip = "Settings related to exporting captured frames to disk."))
	FNGDCExportSettings Export;

	// Checks if the settings are valid. 
	// Returns false if there are any issues, with errors added to the given array.
	// 
	// Even though Unreal Engine can use the metadata on fields (e.g. ClampMin/Max) to automatically 
	// validate when using the Editor, this doesn't catch all cases and an invalid value could still 
	// be set by C++ code (for example). Therefore we provide our own validation check as well.
	bool Validate(TArray<FString>& OutErrors) const;
};

// Top level object for the Neural Graphics Data Capture plugin.
// This is an 'engine subsystem' which means that exactly one of these objects will be associated
// with each UEngine, which is always one. 
// This object is responsible for coordinating the starting and stopping of captures, but the 
// underlying logic of rendering frames and exporting to disk etc. are handled by sub-objects.
UCLASS()
class UNeuralGraphicsDataCaptureSubsystem : public UEngineSubsystem 
{ 
	GENERATED_BODY()

public:
	// Begin UEngineSubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	// End UEngineSubsystem

	// Begins capturing frames with the given settings until EndCapture() is called.
	// Will print an error instead if a capture is already in progress
	// or the given settings are invalid.
	UFUNCTION(BlueprintCallable, Category="ML")
	void BeginCapture(FNGDCCaptureSettings Settings);

	// Ends capturing frames.
	// Will print an error if a capture is not already in progress.
	UFUNCTION(BlueprintCallable, Category="ML")
	void EndCapture();

	// Checks if frames are currently being captured.
	UFUNCTION(BlueprintCallable, Category="ML")
	bool IsCapturing() const;

private:
	// Data/state for an ongoing capture.
	// One of these is created when a capture starts and destroyed when it finishes.
	struct FNGDCCaptureContext
	{
		FNGDCCaptureSettings Settings; // Settings being used for this capture
		TUniquePtr<FNGDCExport> Export; // Handles exporting of captured frames to disk. Wrapped inside a pointer to ease initialization.

		// Handle to the world context we are capturing from. There can be multiple worlds in the engine
		// (e.g. editor + PIE), but we only support capturing from one at a time.
		FName WorldContextHandle; 

		TUniquePtr<FAsyncTaskNotification> Notification; // Notification shown to the user while capturing is in progress
		FTSTicker::FDelegateHandle TimerHandle; // Handle for the periodic timer callback (see OnTimer)

		bool bAppliedFixedFrameRate = false;
		bool bPrevUseFixedFrameRate = false;
		float PrevFixedFrameRate = 0.0f;
	};

	// Internal version of EndCapture which takes an optional error reason that is shown to the user.
	void EndCapture_Internal(FString ErrorReason);

	// Registered as a callback by other parts of the capturing plugin when an error occurs whilst performing a capture.
	// This centralises all the errors so that we can report them to the user in a nice way.
	void OnErrorDuringCapture_GameThread(FString Error);

	// Called when errors occur whilst not performing a capture, e.g. errors before starting a capture.
	// This centralises all the errors so that we can report them to the user in a nice way.
	void OnErrorOutsideCapture(FString Error);

	// Registered as a callback with the FNGDCRendering class, so that this is called (on the render thread) 
	// whenever a frame is captured.
	void OnFrameCaptured_RenderThread(FNGDCRendering::FCapturedFrame CapturedFrame);

	// Called periodically (on the game thread) whilst a capture is ongoing. 
	// The gandle for this timer entry is stored in the capture context object.
	// Used to check for user cancellation from the notification.
	bool OnTimer(float DeltaTime);

	// Registered as a callback when a World is destroyed, so we can stop capturing
	// if the world we were capturing gets destroyed.
	void OnEndPlayMap();

	// Object which handles the rendering side of the capture. This needs to remain alive for the lifetime of the plugin
	// (as it owns a scene view extension - see comments in its header) so isn't part of the capture context.
	// Wrapped inside a pointer to allow construction in the Initialize() method.
	TUniquePtr<FNGDCRendering> Rendering;

	// Data/state for an ongoing capture. Set when a capture starts and unset when it finishes.
	TUniquePtr<FNGDCCaptureContext> CurrentCapture;
};
