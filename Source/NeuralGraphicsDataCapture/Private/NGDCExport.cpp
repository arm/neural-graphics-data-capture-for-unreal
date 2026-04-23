// SPDX-FileCopyrightText: Copyright 2025-2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: Apache-2.0

#include "NGDCExport.h"

#include "NeuralGraphicsDataCaptureModule.h"
#include "RenderGraph.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "Async/Async.h"
#include "Math/UnrealMathUtility.h"

// Include OpenEXR headers for writing out EXR files
THIRD_PARTY_INCLUDES_START
#include "OpenEXR/ImfOutputFile.h"
#include "OpenEXR/ImfPixelType.h"
#include "OpenEXR/ImfHeader.h"
#include "OpenEXR/ImfFrameBuffer.h"
#include "OpenEXR/ImfChannelList.h"
THIRD_PARTY_INCLUDES_END

#if NGDC_DEBUGGING_ENABLED
UE_DISABLE_OPTIMIZATION
#endif

bool FNGDCExportSettings::Validate(TArray<FString>& OutErrors) const
{
	bool bValid = true;
	// Nothing to validate yet
	return bValid;
}

FNGDCExport::FNGDCExport(FNGDCExportSettings Settings, float InUpscalingRatio, float InFixedFrameRate, TFunction<void(FString)> ErrorCallback_GameThread)
	: Settings(Settings)
	, UpscalingRatio(InUpscalingRatio)
	, FixedFrameRate(InFixedFrameRate)
	, AbsDatasetPath(FPaths::IsRelative(Settings.DatasetDir) ? (FPaths::ProjectSavedDir() / Settings.DatasetDir) : Settings.DatasetDir)
	, AbsLowResPath(AbsDatasetPath / GetUpscalingFolderName())
	, AbsJsonPath(AbsDatasetPath / (Settings.CaptureName + ".json"))
	, ErrorCallback_GameThread(ErrorCallback_GameThread)
{
}

bool FNGDCExport::Initialize(FString& OutErrorMessage)
{
	// Make sure required directories exist
	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.MakeDirectory(*AbsDatasetPath, true)) 
	{
		OutErrorMessage = FString::Format(TEXT("Failed to make directory: {0}."), { AbsDatasetPath });
		return false;
	}

	// The sub-directories for the frames are emptied first if they have any old frames in them, 
	// otherwise the new frames could get mixed up with the old ones.
	FString SubDirs[] = {
		AbsDatasetPath / "ground_truth" / Settings.CaptureName,
		AbsDatasetPath / "motion_gt" / Settings.CaptureName,
		AbsLowResPath / "color" / Settings.CaptureName,
		AbsLowResPath / "depth" / Settings.CaptureName,
		AbsLowResPath / "motion" / Settings.CaptureName,
	};
	for (FString SubDir : SubDirs)
	{
		if (!FileManager.DeleteDirectory(*SubDir, false, true))
		{
			OutErrorMessage = FString::Format(TEXT("Failed to delete directory: {0}."), { SubDir });
			return false;
		}
		if (!FileManager.MakeDirectory(*SubDir, true))
		{
			OutErrorMessage = FString::Format(TEXT("Failed to make directory: {0}."), { SubDir });
			return false;
		}
	}

	// Open a JSON file to write the metadata to. The handle remains open for the lifetime of this object and 
	// we will append frames to it as we go.
	JsonFileArchive = TUniquePtr<FArchive>(FileManager.CreateFileWriter(*AbsJsonPath, EFileWrite::FILEWRITE_AllowRead));
	if (!JsonFileArchive.IsValid())
	{
		OutErrorMessage = FString::Format(TEXT("Failed to create file: {0}."), { AbsJsonPath });
		return false;
	}
	JsonWriter = TJsonWriter<ANSICHAR>::Create(JsonFileArchive.Get()).ToSharedPtr();

	// Start writing out the JSON metadata file. We'll write the header here and then append frames one-by-one as they are captured.
	JsonWriter->WriteObjectStart();
	JsonWriter->WriteValue("Version", TEXT("1.0.1"));
	JsonWriter->WriteValue("EmulatedFramerate", FixedFrameRate);

	JsonWriter->WriteValue("ReverseZ", true); // Unreal Engine always uses reverse-Z

	JsonWriter->WriteObjectStart("UpscalingRatiosIndices");
	const FString UpscalingIndexKey = GetUpscalingIndexKey();
	JsonWriter->WriteValue(*UpscalingIndexKey, 0);
	JsonWriter->WriteObjectEnd();

	return true;
}

const FString& FNGDCExport::GetAbsJsonPath() const
{
	return AbsJsonPath;
}

uint32 FNGDCExport::GetNumFramesExported() const
{
	return NumFramesExported;
}

FNGDCExport::~FNGDCExport()
{
	// Wait for all pending readbacks (on the render thread!), so that we have a clean shutdown
	ENQUEUE_RENDER_COMMAND(WaitForReadbacks)
		(
			[&](FRHICommandListImmediate& RHICmdList)
			{
				int32 PendingReadbacks = TickQueuedTextureExports();
				if (PendingReadbacks > 0)
				{
					UE_LOG(LogNGDC, Verbose, TEXT("Waiting for %d readbacks to complete"), PendingReadbacks);
					FGenericPlatformProcess::ConditionalSleep([&]() { return TickQueuedTextureExports() > 0; }, 0.01f);
					UE_LOG(LogNGDC, Verbose, TEXT("All readbacks complete!"));
				}
			}
		);
	FlushRenderingCommands();

	// Close off the JSON file
	// Wait for any outstanding writes (these are done from a worker thread).
	JsonAppendTask.Wait(); // Note this is a no-op if no frames were ever written
	if (JsonWriter) // May be null if initialization failed
	{
		if (!bTargetResolutionWritten)
		{
			JsonWriter->WriteObjectStart("TargetResolution");
			JsonWriter->WriteValue("X", 0);
			JsonWriter->WriteValue("Y", 0);
			JsonWriter->WriteObjectEnd();
			bTargetResolutionWritten = true;
		}
		if (!bFramesArrayStarted)
		{
			JsonWriter->WriteArrayStart("Frames");
			bFramesArrayStarted = true;
		}
		JsonWriter->WriteArrayEnd(); // end of `Frames` array
		if (!bSamplesWritten)
		{
			JsonWriter->WriteObjectStart("Samples");
			JsonWriter->WriteValue("Sequence", TEXT("TiledHalton"));
			JsonWriter->WriteValue("Count", static_cast<int32>(GetNumFramesExported()));
			JsonWriter->WriteValue("Quantized", true);
			JsonWriter->WriteObjectEnd();
			bSamplesWritten = true;
		}
		JsonWriter->WriteObjectEnd(); // end of root object
	}
}

void FNGDCExport::ExportFrame_RenderThread(FCapturedFrame Frame)
{
	FString Filename = FString::Printf(TEXT("%04d"), Frame.FrameNumber);

	// Export each of the textures to separate EXR files
	QueueTextureExport(Frame.GraphBuilder, Frame.GroundTruthColor, AbsDatasetPath / "ground_truth" / Settings.CaptureName / (Filename + ".exr"));
	QueueTextureExport(Frame.GraphBuilder, Frame.GroundTruthVelocity, AbsDatasetPath / "motion_gt" / Settings.CaptureName / (Filename + ".exr"));
	QueueTextureExport(Frame.GraphBuilder, Frame.JitteredColor, AbsLowResPath / "color" / Settings.CaptureName / (Filename + ".exr"));
	QueueTextureExport(Frame.GraphBuilder, Frame.JitteredDepth, AbsLowResPath / "depth" / Settings.CaptureName / (Filename + ".exr"));
	QueueTextureExport(Frame.GraphBuilder, Frame.JitteredVelocity, AbsLowResPath / "motion" / Settings.CaptureName / (Filename + ".exr"));

	// Progress any pending exports
	TickQueuedTextureExports();

	// Append an entry to the JSON file. Do this on a worker thread to avoid stalling the render thread.
	// Add a dependency on the previous frame's append task so that we are never writing multiple frames at the same time.
	JsonAppendTask = UE::Tasks::Launch(TEXT("AppendFrameToJSON"), [this, Frame]() {
		AppendFrameToJson(Frame);
	}, JsonAppendTask, UE::Tasks::ETaskPriority::BackgroundNormal);

	NumFramesExported++;
}

void FNGDCExport::AppendFrameToJson(FCapturedFrame Frame)
{
	check(JsonWriter); // Initialize() must have succeeded
	if (!bTargetResolutionWritten)
	{
		JsonWriter->WriteObjectStart("TargetResolution");
		JsonWriter->WriteValue("X", Frame.GroundTruthExtent.X);
		JsonWriter->WriteValue("Y", Frame.GroundTruthExtent.Y);
		JsonWriter->WriteObjectEnd();
		bTargetResolutionWritten = true;
	}
	if (!bFramesArrayStarted)
	{
		JsonWriter->WriteArrayStart("Frames");
		bFramesArrayStarted = true;
	}

	JsonWriter->WriteObjectStart();
	JsonWriter->WriteValue("FrameNumber", Frame.FrameNumber);
	JsonWriter->WriteValue("FovX", Frame.FovXRadians);
	JsonWriter->WriteValue("FovY", Frame.FovYRadians);
	JsonWriter->WriteValue("CameraNearPlane", Frame.NearClippingDistance);
	JsonWriter->WriteValue("CameraFarPlane", -1.0); // Indicates an infinite far plane
	JsonWriter->WriteValue("Exposure", Frame.Exposure);
	JsonWriter->WriteValue("CameraCut", Frame.bCameraCut);

	JsonWriter->WriteArrayStart("ViewProjection");
	for (int Col = 0; Col < 4; ++Col)
	{
		for (int Row = 0; Row < 4; ++Row)
		{
			JsonWriter->WriteValue(Frame.ViewProjectionMatrix.M[Row][Col]);
		}
	}
	JsonWriter->WriteArrayEnd();

	JsonWriter->WriteObjectStart("Jitter");
	JsonWriter->WriteValue("X", Frame.JitterPixelsLowRes.X);
	JsonWriter->WriteValue("Y", Frame.JitterPixelsLowRes.Y);
	JsonWriter->WriteObjectEnd();

	JsonWriter->WriteArrayStart("NormalizedPerRatioJitter");
	JsonWriter->WriteObjectStart();
	// As we are on a worker thread we can't use the Frame.JitteredColor texture pointer as it might already have been cleaned up.
	JsonWriter->WriteValue("X", Frame.JitterPixelsLowRes.X / (float)Frame.JitteredColorExtent.X);
	JsonWriter->WriteValue("Y", Frame.JitterPixelsLowRes.Y / (float)Frame.JitteredColorExtent.Y);

	JsonWriter->WriteObjectEnd();
	JsonWriter->WriteArrayEnd();

	JsonWriter->WriteObjectEnd();

	JsonFileArchive->Flush(); // In case we crash/error, make sure partial data is written out
}

BEGIN_SHADER_PARAMETER_STRUCT(FEnqueueCopyTexturePass, )
	RDG_TEXTURE_ACCESS(Texture, ERHIAccess::CopySrc)
END_SHADER_PARAMETER_STRUCT()

void FNGDCExport::QueueTextureExport(FRDGBuilder& GraphBuilder, FRDGTextureRef Texture, FString Filename)
{
	check(IsInRenderingThread());

	// There are (at least) two different ways we could export the textures to disk:
	// 
	// The UImageWriteBlueprintLibrary is quite easy to use but requires 'UTexture'-style textures, which are more heavyweight
	// than what we need and because it internally uses RHIReadSurfaceData it exposes a bug in the Vulkan RHI where fp32 textures get
	// squashed into 8-bit values (doing an unwanted gamma conversion and losing precision). 
	// This approach also introduces stalls as RHIReadSurfaceData waits for the data to be copied to CPU,
	// which hurts performance.
	// 
	// Instead we take a more manual approach using FRHIGPUTextureReadback and handling the synchronisation outselves.
	// To avoid stalls, we maintain multiple readback objects so that many can be pending at the same time,
	// and we never need to wait. This does mean that things are less synchronised and the textures will end up 
	// getting written to disk a couple of frames after they are first rendered, but it gives a nice performance boost.

	// Find the next free readback object, or if none are available then make a new one.
	// We can re-use readback objects if the texture properties match (and the previous use is finished!).
	FReadbackReuseKey ReuseKey{ Texture->Desc.Extent, Texture->Desc.Format };
	TArray<TUniquePtr<FQueuedTextureExport>>& Queue = Readbacks_RenderThread.FindOrAdd(ReuseKey);
	TUniquePtr<FQueuedTextureExport>* QueueEntryPtr = Queue.FindByPredicate([](TUniquePtr<FQueuedTextureExport>& E) { return E->ExportFilename.IsEmpty(); });
	if (!QueueEntryPtr) {
		QueueEntryPtr = &Queue.Emplace_GetRef(MakeUnique<FQueuedTextureExport>());
	}
	FQueuedTextureExport* QueueEntry = QueueEntryPtr->Get();

	// Set up the queue entry for our texture
	QueueEntry->ExportFilename = Filename;
	QueueEntry->bIsReadbackEnqueued = false;

	// Unfortunately we can't use the RDG helper function AddEnqueueCopyPass and must instead do it manually.
	// This is so that we have the opportunity to set the bIsReadbackEnqueued flag to true at the right time.
	FEnqueueCopyTexturePass* PassParameters = GraphBuilder.AllocParameters<FEnqueueCopyTexturePass>();
	PassParameters->Texture = Texture;
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("EnqueueCopy(%s)", Texture->Name),
		PassParameters,
		ERDGPassFlags::Readback,
		[QueueEntry, Texture](FRDGAsyncTask, FRHICommandList& RHICmdList)
		{
			QueueEntry->Readback.EnqueueCopy(RHICmdList, Texture->GetRHI());
			QueueEntry->bIsReadbackEnqueued = true;
		});
}

int32 FNGDCExport::TickQueuedTextureExports()
{
	check(IsInRenderingThread());

	// Check for any readbacks that are ready and write them to disk
	int32 NumPendingReadbacks = 0;
	for (auto& ReadbackQueue : Readbacks_RenderThread)
	{
		for (TUniquePtr<FQueuedTextureExport>& QueueEntry : ReadbackQueue.Value)
		{
			if (QueueEntry->ExportFilename.IsEmpty())
			{
				// This queue entry is not in use - ignore
				continue;
			}

			++NumPendingReadbacks;

			if (QueueEntry->bIsReadbackEnqueued && QueueEntry->Readback.IsReady() && !QueueEntry->EncodingTask.IsValid())
			{
				// Texture data is ready to be read!
				const FRHITextureDesc& TextureDesc = QueueEntry->Readback.DestinationStagingTextures[0]->GetDesc();

				// Lock the staging texture to give us a CPU pointer to the unpacked data (i.e. may have padding at the end of each row).
				int32 RowPitchInPixels;
				int32 Height;
				void* SrcPtr = QueueEntry->Readback.Lock(RowPitchInPixels, &Height);

				// Encode the EXR on a worker thread to avoid blocking the render thread.
				// We still need to come back to the render thread though to Unlock the readback once the export is done.
				QueueEntry->EncodingTask = UE::Tasks::Launch(TEXT("SaveEXR"), [this, SrcPtr, &TextureDesc, &Filename = QueueEntry->ExportFilename, RowPitchInPixels]() {
					SaveEXR(SrcPtr, TextureDesc, RowPitchInPixels, Filename);
				}, UE::Tasks::ETaskPriority::BackgroundNormal);
			}
			else if (QueueEntry->EncodingTask.IsValid() && QueueEntry->EncodingTask.IsCompleted())
			{
				QueueEntry->Readback.Unlock();

				// Even though we're finished with this entry, leave it in the array so that it can be re-used by later frames.
				// We mark it as done by clearing the filename.
				QueueEntry->ExportFilename.Empty();
				QueueEntry->EncodingTask = UE::Tasks::TTask<void>();
			}
		}
	}

	return NumPendingReadbacks;
}

// Saves the image from a given texture readback to an EXR file.
// We could have used IImageWriteQueue or IImageWrapper which handles some of this stuff for us,
// but this introduces a bunch of additional data copies (which will affect performance) and limitations 
// (like not supporting 2-channel images).
// Returns true on success, otherwise calls the ErrorCallback_GameThread callback with an error and returns false.
bool FNGDCExport::SaveEXR(void* TextureData, const FRHITextureDesc& TextureDesc, int32 RowPitchInPixels, const FString& Filename)
{
	TRACE_CPUPROFILER_EVENT_SCOPE("FNGDCExport SaveEXR");

	check(TextureDesc.Dimension == ETextureDimension::Texture2D);
	const FPixelFormatInfo& FormatInfo = GPixelFormats[TextureDesc.Format];
	check(FormatInfo.BlockSizeX == 1 && FormatInfo.BlockSizeY == 1); // No compression

	const int32 BytesPerPixel = FormatInfo.BlockBytes;
	const int32 BitsPerChannel = (BytesPerPixel * 8) / FormatInfo.NumComponents;
	check(BitsPerChannel == 16 || BitsPerChannel == 32);
	Imf::PixelType ImfPixelType = BitsPerChannel == 16 ? Imf::HALF : Imf::FLOAT;

	// Use RLE encoding as it's lossless and provides a good trade-off between CPU usage for compression and file size.
	// Here are some rough results for the different lossless compression methods (on a relatively small resolution capture).
	//
	//         Method                 Avg. file size (for one frame)      Total Render Thread time (no async task)      Total Render Thread time (with async task)        CPU usage
	//      ===============================================================================================================================================================================
	//		No compression                       6.0 MB                                    25 ms                                        20ms                                21 %
	//		RLE                                  3.6 MB                                    30 ms                                        20ms                                22 %
	//		ZIP                                  2.5 MB                                    50 ms                                        20ms                                31 %
	//		PIZ                                  2.8 MB                                    70 ms                                        20ms                                27 %
	//
	// Note that when doing the compression on a worker thread, the render thread times are all the same but CPU usage does vary so it's 
	// still worth considering this.
	Imf::Compression Compression = Imf::Compression::RLE_COMPRESSION;

	Imf::Header ImfHeader(TextureDesc.Extent.X, TextureDesc.Extent.Y, 1, Imath::V2f(0, 0), 1, Imf::LineOrder::INCREASING_Y, Compression);
	Imf::FrameBuffer ImfFrameBuffer;

	const char* ChannelNames[] = { "R", "G", "B", "A" };
	check(FormatInfo.NumComponents <= 4);
	for (int32 C = 0; C < FormatInfo.NumComponents; ++C)
	{
		ImfHeader.channels().insert(ChannelNames[C], Imf::Channel(ImfPixelType));

		size_t Offset = C * (BitsPerChannel / 8);
		ImfFrameBuffer.insert(ChannelNames[C], Imf::Slice(ImfPixelType, (char*)TextureData + Offset, BytesPerPixel, RowPitchInPixels * BytesPerPixel));
	}

	// OpenEXR reports errors via exceptions
	try
	{
		Imf::OutputFile ImfFile(StringCast<char>(*Filename).Get(), ImfHeader, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
		ImfFile.setFrameBuffer(ImfFrameBuffer);
		ImfFile.writePixels(TextureDesc.Extent.Y);
		return true;
	}
	catch (const std::exception& Exception)
	{
		FString Error = FString::Format(TEXT("Error saving EXR file to {0}: {1}"), { Filename, Exception.what() });
		// We're on a worker thread here, but the error callback needs to be called on the game thread
		AsyncTask(ENamedThreads::GameThread, [this, Error]() { this->ErrorCallback_GameThread(Error); });
		return false;
	}
}

#if NGDC_DEBUGGING_ENABLED
UE_ENABLE_OPTIMIZATION
#endif
