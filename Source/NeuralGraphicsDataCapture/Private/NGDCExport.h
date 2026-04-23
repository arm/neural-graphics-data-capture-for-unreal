// SPDX-FileCopyrightText: Copyright 2025-2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "RHIGPUReadback.h"
#include "RenderGraphFwd.h"
#include "Tasks/Task.h"
#include "Serialization/JsonWriter.h"
#include <atomic>

#include "NGDCExport.generated.h"

// Settings for the 'export' part of the plugin.
USTRUCT(BlueprintType)
struct FNGDCExportSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings", meta = (ToolTip = "Folder of the dataset where captured frames will be saved. Relative paths are interpreted as relative to the project's Saved/ folder."))
	FString DatasetDir = "NeuralGraphicsDataset";

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settings", meta = (ToolTip = "Name of the capture within the dataset."))
	FString CaptureName = "0000";

	bool Validate(TArray<FString>& OutErrors) const;
};

// Responsible for saving captured frames to disk in the dataset spec format. 
// Also writes out the JSON file with appropriate metadata.
// Settings are provided at creation time and can't be modified later. 
// Initialize() must be called (and may fail!) before any frames can be exported.
// Frames are exported by calling ExportFrame_RenderThread().
class FNGDCExport
{
public:
	// Details of a frame to be exported. Note that the textures here are RDG textures
	// so won't actually have been rendered yet (just recorded into the render graph).
	struct FCapturedFrame
	{
		FRDGBuilder& GraphBuilder; // The graph builder that owns the textures below
		uint32 FrameNumber;
		bool bCameraCut = false;

		FVector2f JitterPixelsLowRes; // The jitter (in pixels in the low resolution space, -0.5 to +0.5) applied to the jittered textures below
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
		FIntPoint JitteredColorExtent;  // We store a copy of JiteredColor.Extent so it can be used after the RDG textures have been cleaned up. This is used on a worker thread(s) that writes to JSON file and might execute after the render thread has moved to a new frame.
	};

	FNGDCExport(FNGDCExportSettings Settings, float UpscalingRatio, float FixedFrameRate, TFunction<void(FString)> ErrorCallback_GameThread);
	~FNGDCExport();

	// Performs setup steps that may fail (e.g. creating folders), so this is separate from the constructor.
	bool Initialize(FString& OutErrorMessage);

	// Gets the absolute path to the JSON metadata file which we are appending frame metadata to.
	const FString& GetAbsJsonPath() const;

	// Counter for the number of frames _queued_ to be exported (not necessarily actually written to disk yet!).
	// Can be called from any thread.
	uint32 GetNumFramesExported() const;

	// Exports the given frame to disk. Must be called on the render thread.
	void ExportFrame_RenderThread(FCapturedFrame Frame);

private:
	// Queues up the given RDG texture to be read back to the CPU and exported to disk.
	void QueueTextureExport(FRDGBuilder& GraphBuilder, FRDGTextureRef Texture, FString Filename);
	
	// Saves the given texture data to disk as an EXR file.
	bool SaveEXR(void* TextureData, const FRHITextureDesc& TextureDesc, int32 RowPitchInPixels, const FString& Filename);

	// Checks the status of any pending texture readbacks and starts writing them to disk if they are ready.
	// Returns the number of still-pending readbacks.
	int32 TickQueuedTextureExports();

	// Appends metadata for the given frame to the JSON file. 
	// Note this is called on a worker thread, but can't be called concurrently!
	void AppendFrameToJson(FCapturedFrame Frame);

	FString GetUpscalingRatioLabel() const
	{
		FString RatioString = FString::SanitizeFloat(UpscalingRatio);

		int32 DecimalIndex;
		if (RatioString.FindChar('.', DecimalIndex))
		{
			while (RatioString.EndsWith(TEXT("0")))
			{
				RatioString.LeftChopInline(1);
			}
			if (RatioString.EndsWith(TEXT(".")))
			{
				RatioString.LeftChopInline(1);
			}
		}

		RatioString.ReplaceInline(TEXT("."), TEXT("_"));
		return RatioString;
	}
	FString GetUpscalingFolderName() const { return FString::Printf(TEXT("x%s"), *GetUpscalingRatioLabel()); }
	FString GetUpscalingIndexKey() const { return FString::Printf(TEXT("x%s_index"), *GetUpscalingRatioLabel()); }

	FNGDCExportSettings Settings; // Settings for the current capture
	// Ratio between ground truth and jittered inputs for the current export session.
	float UpscalingRatio;
	float FixedFrameRate;
	FString AbsDatasetPath; // Absolute version of Settings.DatasetDir (so that relative paths are interpreted consistently)
	FString AbsLowResPath; // Absolute path to the subfolder in the dataset dir for the current upscaling ratio (e.g. x2/)
	FString AbsJsonPath; // Absolute path to the JSON file which we are writing metadata to

	// Counter for the number of frames _queued_ to be exported (not necessarily actually written to disk yet!).
	// Atomic so that can be written from the render thread but read from any thread.
	std::atomic<uint32> NumFramesExported = 0;

	// The JSON file object which we append to as frames are captured (the handle remains open for the lifetime of this object)
	TUniquePtr<FArchive> JsonFileArchive;
	// A writer object which wraps JsonFileArchive with methods to write JSON entries.
	TSharedPtr<TJsonWriter<ANSICHAR>> JsonWriter;
	// Frames are written to the JSON file on a worker thread.
	// This is the latest task which is appending a frame to the JSON file. There could be multiple frames waiting to be appended, but this
	// will always be the last one and will be dependent on the previous one. All the pending tasks will be chained like this,
	// so waiting on this last one is sufficient to wait on all of them.
	UE::Tasks::TTask<void> JsonAppendTask; 
	bool bTargetResolutionWritten = false;
	bool bFramesArrayStarted = false;
	bool bSamplesWritten = false;

	// The set of texture properties that need to be the same in order to re-use a readback object.
	// This is essentially just a tuple of two values, implementing hashing and equality as required by TMap.
	struct FReadbackReuseKey
	{
		FIntPoint Extent;
		EPixelFormat Format;

		bool operator==(const FReadbackReuseKey& Rhs) const { return Extent == Rhs.Extent && Format == Rhs.Format; }
		friend uint32 GetTypeHash(const FReadbackReuseKey& K) { return HashCombine(GetTypeHash(K.Extent), GetTypeHash((uint8)K.Format)); }
	};

	// Details for a texture that is queued for export.
	// These objects are re-used multiple times (to avoid creating loads of readback objects each frame),
	// which unfortunately complicates things a bit. The lifecycle of these objects is as follows:
	//
	//    Created -> Unused -> Reserved -> Readback enqueued -> Readback ready -> Encoding -> Encoding done 
	//                  ^                                                                          |
	//                  |                                                                          |
	//                   --------------------------------------------------------------------------
	struct FQueuedTextureExport
	{
		FString ExportFilename; // The filename this texture will be written to. Blank if this readback is done and can be re-used.
		// The GPU readback object - this contains a CPU-accessible staging texture.
		FRHIGPUTextureReadback Readback = FRHIGPUTextureReadback("FNGDCExportReadback");
		// As we re-use the readback objects we need a way to distinguish between the "Readback ready" state
		// and the "Reserved" state (readback has been enqueued into the RDG but hasn't had its EnqueueCopy() method called yet).
		// In both cases, Readback.IsReady() will return true, so if we don't detect the second case then we will incorrectly think
		// that the readback is ready immediately, even though it hasn't even been queued yet.
		// This is down to the distinction between being added to an RDG pass and that RDG pass actually being executed.
		// This flag gives us that extra distinction, and is set to true once the readback has been enqueued.
		bool bIsReadbackEnqueued = false;
		UE::Tasks::TTask<void> EncodingTask; // The task-graph task writing this texture to disk. Empty/invalid if not currently being written.
	};

	// Stores all the pending readbacks for textures that have been queued for exporting.
	// Note this might include textures for several previous frames.
	// Readbacks will be re-used if the texture properties match (see FReadbackReuseKey),
	// to avoid creating too many readback objects.
	// Note we wrap the FQueuedTextureExport objects in TUniquePtrs as the array will be modified 
	// which invalidates pointers to elements, but we need to store pointers to the queue entries
	// in RDG passes.
	TMap<FReadbackReuseKey, TArray<TUniquePtr<FQueuedTextureExport>>> Readbacks_RenderThread;

	// Callback which this object calls (on the game thread) when an error occurs, 
	// used to report errors to the parent subsystem. This is used where we can't easily report errors
	// through a return value, e.g. where running on a background thread.
	TFunction<void(FString)> ErrorCallback_GameThread;
};
