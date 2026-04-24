<!--
SPDX-FileCopyrightText: Copyright 2025-2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
SPDX-License-Identifier: Apache-2.0
-->

# Neural Graphics Data Capture Plugin for Unreal® Engine

## Introduction

The Neural Graphics Data Capture plugin is a runtime Unreal® Engine plugin for building datasets for neural graphics workflows. It hooks into the engine through a capture subsystem, records the rendered view alongside per-frame metadata, and writes the results to a dataset folder that can be used for training, evaluation, and offline analysis.

Datasets captured with this plugin can be used as input to the [Neural Graphics Model Gym](https://github.com/arm/neural-graphics-model-gym) to train or finetune neural graphics models, such as [Neural Super Sampling](https://huggingface.co/Arm/neural-super-sampling). Specifically, see [this](https://github.com/arm/neural-graphics-model-gym/blob/main/docs/nss/nss_data_generation.md) document on how to convert your captured dataset to a format compatible with the Model Gym.

## Quick Setup

1. Copy this folder into the `Plugins/` folder in either your Unreal® `Engine` folder or your project folder.

2. Enable the plugin (e.g. in your `.uproject` file or using the Plugins window).

3. Build the engine for your project.

4. Open the Level Blueprint for your level and paste the code snippet from the appendix below.

5. Play your level in Standalone Game. Note that if using New Editor Window you may see that captured frames are not saved at the expected size.

6. Press 'C' to start capturing frames and 'V' to stop capturing

7. Find the captured dataset in your project's `Saved/NeuralGraphicsDataset` folder

## Recommended System Specification
An example system specification for smooth running of the plugin is:
- RAM: 64 GB system memory
- GPU: NVIDIA GeForce RTX 4080 (driver 572.96 or newer)
- OS: 64-bit Windows or Linux

A similar configuration (or indeed higher-spec) is what we recommend for smooth out-of-box captures.

This plugin currently only supports Unreal® Engine version 5.5


## Upscaling Ratio

Use the `UpscalingRatio` property on `NGDCRenderingSettings` (the `Rendering` field on `NGDCCaptureSettings`) to pick the jittered-input to ground-truth ratio.
Fractional values are supported (e.g. `1.5`), and the jittered inputs will render at `TargetResolution / UpscalingRatio`.
For example, setting it to `2` will render jittered inputs at half the ground-truth resolution and export them under an `x2` folder (with matching metadata) inside the dataset directory.

## Camera Cuts

- Every frame written to the dataset JSON now contains a `CameraCut` boolean that indicates if the frame starts a new shot. Consumers can use this to split clips without re-deriving cuts from matrices or timestamps.
- Unreal® Engine sets the flag automatically whenever the active view switches, and the plugin also exposes `Rendering.CameraCutTranslationThreshold` and `Rendering.CameraCutRotationThresholdDegrees` to heuristically mark cuts when the engine flag is unavailable. Set either threshold to `0` to disable that specific heuristic.
- Verbose logging reports whenever a heuristic cut is triggered, which can help with tuning thresholds for a given project.

## Fixed Frame Rate

Use the `Fixed Frame Rate` property on `NGDCRenderingSettings` to fix the frame rate of your content.

## Troubleshooting
1. **Plugin absent or disabled:** ensure the entire folder lives under `Plugins/` and the module is enabled in your .uproject, then rebuild the project so the editor sees NeuralGraphicsDataCapture. If you moved the plugin after generating project files, rerun your IDE’s "Generate Project Files" step to refresh module lists. For more information, see Unreal Engine's guides on [generating IDE project files](https://dev.epicgames.com/documentation/en-us/unreal-engine/how-to-generate-unreal-engine-project-files-for-your-ide) and [using the UnrealVS extension](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-the-unrealvs-extension-for-unreal-engine-cplusplus-projects).

2. **Capture hotkeys do nothing:** double-check that the Level Blueprint contains the provided subsystem graph (see Appendix) and that nothing else consumes the C/V input events before they reach BeginCapture/EndCapture.

3. **Need more diagnostics:** enable verbose logging for LogNGDC (e.g. run `LogNGDC verbose` in the console) to see when the subsystem initializes, starts capture, or reports error.

4. **Dataset folder stays empty:** verify the capture actually starts (enable verbose logging and then watch the Output Log for LogNGDC entries) and that NGDCExportSettings.DatasetDir/CaptureName point somewhere writable. By default this is `Saved/NeuralGraphicsDataset/<CaptureName>`.

5. **Captures crash or stutter:** compare your hardware to the recommended spec and temporarily lower SupersamplingRatio via NGDCRenderingSettings in the blueprint if you have lower-spec hardware. Lower ratios reduce GPU pressure and can help avoid out-of-memory failures.

6. **Capture size not as expected:** ensure you are running using the Standalone Game option. If playing in New Editor Window then you may see your captured frames are slightly larger than expected.

## Appendix
### Level Blueprint Snippet

```
Begin Object Class=/Script/BlueprintGraph.K2Node_GetEngineSubsystem Name="K2Node_GetEngineSubsystem_0" ExportPath="/Script/BlueprintGraph.K2Node_GetEngineSubsystem'/Game/VehicleTemplate/Maps/VehicleAdvExampleMap.VehicleAdvExampleMap:PersistentLevel.VehicleAdvExampleMap.EventGraph.K2Node_GetEngineSubsystem_0'"
   CustomClass="/Script/CoreUObject.Class'/Script/NeuralGraphicsDataCapture.NeuralGraphicsDataCaptureSubsystem'"
   NodePosX=656
   NodePosY=832
   NodeGuid=7BCB14E44BC451507DE2BCAEA797E5B3
   CustomProperties Pin (PinId=516F2613410AB7D802B130A311C9BEE0,PinName="ReturnValue",Direction="EGPD_Output",PinType.PinCategory="object",PinType.PinSubCategory="",PinType.PinSubCategoryObject="/Script/CoreUObject.Class'/Script/NeuralGraphicsDataCapture.NeuralGraphicsDataCaptureSubsystem'",PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,LinkedTo=(K2Node_CallFunction_0 687869F649C1CA0D2F0E81895738A64F,K2Node_CallFunction_3 6625EA724D260811D88AA3962335FF9E,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
End Object
Begin Object Class=/Script/BlueprintGraph.K2Node_InputKey Name="K2Node_InputKey_0" ExportPath="/Script/BlueprintGraph.K2Node_InputKey'/Game/VehicleTemplate/Maps/VehicleAdvExampleMap.VehicleAdvExampleMap:PersistentLevel.VehicleAdvExampleMap.EventGraph.K2Node_InputKey_0'"
   InputKey=C
   NodePosX=-32
   NodePosY=672
   NodeGuid=DAAC7B7B4895050333ED198D7DD4B231
   CustomProperties Pin (PinId=2121517B434A6AD00E0EDCA25E795E19,PinName="Pressed",Direction="EGPD_Output",PinType.PinCategory="exec",PinType.PinSubCategory="",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,LinkedTo=(K2Node_CallFunction_3 C385C0DA45A52E77420FDAAA29898268,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=4BC76B7A4CF3C72644F163B35BC6D646,PinName="Released",Direction="EGPD_Output",PinType.PinCategory="exec",PinType.PinSubCategory="",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=C7F56C7848AD05404DCCBE89AD4CAF9E,PinName="Key",Direction="EGPD_Output",PinType.PinCategory="struct",PinType.PinSubCategory="",PinType.PinSubCategoryObject="/Script/CoreUObject.ScriptStruct'/Script/InputCore.Key'",PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,DefaultValue="None",AutogeneratedDefaultValue="None",PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
End Object
Begin Object Class=/Script/BlueprintGraph.K2Node_InputKey Name="K2Node_InputKey_1" ExportPath="/Script/BlueprintGraph.K2Node_InputKey'/Game/VehicleTemplate/Maps/VehicleAdvExampleMap.VehicleAdvExampleMap:PersistentLevel.VehicleAdvExampleMap.EventGraph.K2Node_InputKey_1'"
   InputKey=V
   NodePosX=-16
   NodePosY=1072
   NodeGuid=8D32ECAC4A2F471A65F5EE9918AE9E26
   CustomProperties Pin (PinId=7932C7CF4CCB42209AF3348F2405233B,PinName="Pressed",Direction="EGPD_Output",PinType.PinCategory="exec",PinType.PinSubCategory="",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,LinkedTo=(K2Node_CallFunction_0 2BFC6E514505E387324EB28586B8462F,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=EC0E19E54F33336FB6B17395B476751C,PinName="Released",Direction="EGPD_Output",PinType.PinCategory="exec",PinType.PinSubCategory="",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=2D07EC804606879ACA890AB04613C678,PinName="Key",Direction="EGPD_Output",PinType.PinCategory="struct",PinType.PinSubCategory="",PinType.PinSubCategoryObject="/Script/CoreUObject.ScriptStruct'/Script/InputCore.Key'",PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,DefaultValue="None",AutogeneratedDefaultValue="None",PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
End Object
Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name="K2Node_CallFunction_0" ExportPath="/Script/BlueprintGraph.K2Node_CallFunction'/Game/VehicleTemplate/Maps/VehicleAdvExampleMap.VehicleAdvExampleMap:PersistentLevel.VehicleAdvExampleMap.EventGraph.K2Node_CallFunction_0'"
   FunctionReference=(MemberParent="/Script/CoreUObject.Class'/Script/NeuralGraphicsDataCapture.NeuralGraphicsDataCaptureSubsystem'",MemberName="EndCapture")
   NodePosX=1072
   NodePosY=1056
   NodeGuid=F32A703B44DCDBE0E94BD7876E40EC7A
   CustomProperties Pin (PinId=2BFC6E514505E387324EB28586B8462F,PinName="execute",PinType.PinCategory="exec",PinType.PinSubCategory="",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,LinkedTo=(K2Node_InputKey_1 7932C7CF4CCB42209AF3348F2405233B,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=8EB813AB4438171048774EAF84C87987,PinName="then",Direction="EGPD_Output",PinType.PinCategory="exec",PinType.PinSubCategory="",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=687869F649C1CA0D2F0E81895738A64F,PinName="self",PinFriendlyName=NSLOCTEXT("K2Node", "Target", "Target"),PinType.PinCategory="object",PinType.PinSubCategory="",PinType.PinSubCategoryObject="/Script/CoreUObject.Class'/Script/NeuralGraphicsDataCapture.NeuralGraphicsDataCaptureSubsystem'",PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,LinkedTo=(K2Node_GetEngineSubsystem_0 516F2613410AB7D802B130A311C9BEE0,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
End Object
Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name="K2Node_CallFunction_3" ExportPath="/Script/BlueprintGraph.K2Node_CallFunction'/Game/VehicleTemplate/Maps/VehicleAdvExampleMap.VehicleAdvExampleMap:PersistentLevel.VehicleAdvExampleMap.EventGraph.K2Node_CallFunction_3'"
   FunctionReference=(MemberParent="/Script/CoreUObject.Class'/Script/NeuralGraphicsDataCapture.NeuralGraphicsDataCaptureSubsystem'",MemberName="BeginCapture")
   NodePosX=1232
   NodePosY=656
   ErrorType=1
   NodeGuid=83D363CF4230CE4E6D916CB7C4A9CD66
   CustomProperties Pin (PinId=C385C0DA45A52E77420FDAAA29898268,PinName="execute",PinType.PinCategory="exec",PinType.PinSubCategory="",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,LinkedTo=(K2Node_InputKey_0 2121517B434A6AD00E0EDCA25E795E19,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=6D916BBD4E121527B53B599D95F8F618,PinName="then",Direction="EGPD_Output",PinType.PinCategory="exec",PinType.PinSubCategory="",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=6625EA724D260811D88AA3962335FF9E,PinName="self",PinFriendlyName=NSLOCTEXT("K2Node", "Target", "Target"),PinType.PinCategory="object",PinType.PinSubCategory="",PinType.PinSubCategoryObject="/Script/CoreUObject.Class'/Script/NeuralGraphicsDataCapture.NeuralGraphicsDataCaptureSubsystem'",PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,LinkedTo=(K2Node_GetEngineSubsystem_0 516F2613410AB7D802B130A311C9BEE0,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=BB7C1A8E447106D4EF8DEBAAAF0180DA,PinName="Settings",PinType.PinCategory="struct",PinType.PinSubCategory="",PinType.PinSubCategoryObject="/Script/CoreUObject.ScriptStruct'/Script/NeuralGraphicsDataCapture.NGDCCaptureSettings'",PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,LinkedTo=(K2Node_MakeStruct_1 D98CD88F4D23D72EC3E15988EA889FAF,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
End Object
Begin Object Class=/Script/BlueprintGraph.K2Node_MakeStruct Name="K2Node_MakeStruct_1" ExportPath="/Script/BlueprintGraph.K2Node_MakeStruct'/Game/VehicleTemplate/Maps/VehicleAdvExampleMap.VehicleAdvExampleMap:PersistentLevel.VehicleAdvExampleMap.EventGraph.K2Node_MakeStruct_1'"
   bMadeAfterOverridePinRemoval=True
   ShowPinForProperties(0)=(PropertyName="Rendering",PropertyFriendlyName="Rendering",PropertyTooltip=NSLOCTEXT("UObjectToolTips", "NGDCCaptureSettings:Rendering", "Settings related to rendering the frames."),CategoryName="Settings",bShowPin=True,bCanToggleVisibility=True)
   ShowPinForProperties(1)=(PropertyName="Export",PropertyFriendlyName="Export",PropertyTooltip=NSLOCTEXT("UObjectToolTips", "NGDCCaptureSettings:Export", "Settings related to exporting captured frames to disk."),CategoryName="Settings",bShowPin=True,bCanToggleVisibility=True)
   StructType="/Script/CoreUObject.ScriptStruct'/Script/NeuralGraphicsDataCapture.NGDCCaptureSettings'"
   NodePosX=752
   NodePosY=432
   ErrorType=3
   NodeGuid=3B868F7441F3D9D7A444A1899F2164B4
   CustomProperties Pin (PinId=D98CD88F4D23D72EC3E15988EA889FAF,PinName="NGDCCaptureSettings",Direction="EGPD_Output",PinType.PinCategory="struct",PinType.PinSubCategory="",PinType.PinSubCategoryObject="/Script/CoreUObject.ScriptStruct'/Script/NeuralGraphicsDataCapture.NGDCCaptureSettings'",PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,LinkedTo=(K2Node_CallFunction_3 BB7C1A8E447106D4EF8DEBAAAF0180DA,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=7036BB8A44B0431990530B962DE7F50B,PinName="Rendering",PinFriendlyName=INVTEXT("Rendering"),PinToolTip="Rendering\nNGDCRendering Settings Structure\n\nSettings related to rendering the frames.",PinType.PinCategory="struct",PinType.PinSubCategory="",PinType.PinSubCategoryObject="/Script/CoreUObject.ScriptStruct'/Script/NeuralGraphicsDataCapture.NGDCRenderingSettings'",PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,DefaultValue="(SupersamplingRatio=4,UpscalingRatio=2.000000,FixedFrameRate=0.000000,CameraCutTranslationThreshold=200.000000,CameraCutRotationThresholdDegrees=30.000000)",AutogeneratedDefaultValue="(SupersamplingRatio=4,UpscalingRatio=2.000000,FixedFrameRate=0.000000,CameraCutTranslationThreshold=200.000000,CameraCutRotationThresholdDegrees=30.000000)",LinkedTo=(K2Node_MakeStruct_0 C3F944A54825BE2D824F86BB432AE47A,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=01540EC040BB241FC6CEE5A430CB07C8,PinName="Export",PinFriendlyName=INVTEXT("Export"),PinToolTip="Export\nNGDCExport Settings Structure\n\nSettings related to exporting captured frames to disk.",PinType.PinCategory="struct",PinType.PinSubCategory="",PinType.PinSubCategoryObject="/Script/CoreUObject.ScriptStruct'/Script/NeuralGraphicsDataCapture.NGDCExportSettings'",PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,DefaultValue="(DatasetDir=\"NeuralGraphicsDataset\",CaptureName=\"0000\")",AutogeneratedDefaultValue="(DatasetDir=\"NeuralGraphicsDataset\",CaptureName=\"0000\")",LinkedTo=(K2Node_MakeStruct_2 2C68BCE6466CA7792D3BC7B1488D2A96,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
End Object
Begin Object Class=/Script/BlueprintGraph.K2Node_MakeStruct Name="K2Node_MakeStruct_2" ExportPath="/Script/BlueprintGraph.K2Node_MakeStruct'/Game/VehicleTemplate/Maps/VehicleAdvExampleMap.VehicleAdvExampleMap:PersistentLevel.VehicleAdvExampleMap.EventGraph.K2Node_MakeStruct_2'"
   bMadeAfterOverridePinRemoval=True
   ShowPinForProperties(0)=(PropertyName="DatasetDir",PropertyFriendlyName="Dataset Dir",PropertyTooltip=NSLOCTEXT("UObjectToolTips", "NGDCExportSettings:DatasetDir", "Folder of the dataset where captured frames will be saved. Relative paths are interpreted as relative to the project\'s Saved/ folder."),CategoryName="Settings",bShowPin=True,bCanToggleVisibility=True)
   ShowPinForProperties(1)=(PropertyName="CaptureName",PropertyFriendlyName="Capture Name",PropertyTooltip=NSLOCTEXT("UObjectToolTips", "NGDCExportSettings:CaptureName", "Name of the capture within the dataset."),CategoryName="Settings",bShowPin=True,bCanToggleVisibility=True)
   StructType="/Script/CoreUObject.ScriptStruct'/Script/NeuralGraphicsDataCapture.NGDCExportSettings'"
   NodePosX=256
   NodePosY=544
   NodeGuid=5A0B51514D5D207D5D49C9BF90ADB7FF
   CustomProperties Pin (PinId=2C68BCE6466CA7792D3BC7B1488D2A96,PinName="NGDCExportSettings",Direction="EGPD_Output",PinType.PinCategory="struct",PinType.PinSubCategory="",PinType.PinSubCategoryObject="/Script/CoreUObject.ScriptStruct'/Script/NeuralGraphicsDataCapture.NGDCExportSettings'",PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,LinkedTo=(K2Node_MakeStruct_1 01540EC040BB241FC6CEE5A430CB07C8,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=503FA12D4CC0901576B1EC8CF35364CF,PinName="DatasetDir",PinFriendlyName=INVTEXT("Dataset Dir"),PinToolTip="Dataset Dir\nString\n\nFolder of the dataset where captured frames will be saved. Relative paths are interpreted as relative to the project\'s Saved/ folder.",PinType.PinCategory="string",PinType.PinSubCategory="",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,DefaultValue="NeuralGraphicsDataset",AutogeneratedDefaultValue="NeuralGraphicsDataset",PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=23B504344516649C2578AE9E56CC4B24,PinName="CaptureName",PinFriendlyName=INVTEXT("Capture Name"),PinToolTip="Capture Name\nString\n\nName of the capture within the dataset.",PinType.PinCategory="string",PinType.PinSubCategory="",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,DefaultValue="0000",AutogeneratedDefaultValue="0000",PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
End Object
Begin Object Class=/Script/BlueprintGraph.K2Node_MakeStruct Name="K2Node_MakeStruct_0" ExportPath="/Script/BlueprintGraph.K2Node_MakeStruct'/Game/VehicleTemplate/Maps/VehicleAdvExampleMap.VehicleAdvExampleMap:PersistentLevel.VehicleAdvExampleMap.EventGraph.K2Node_MakeStruct_0'"
   bMadeAfterOverridePinRemoval=True
   ShowPinForProperties(0)=(PropertyName="SupersamplingRatio",PropertyFriendlyName="Supersampling Ratio",PropertyTooltip=NSLOCTEXT("UObjectToolTips", "NGDCRenderingSettings:SupersamplingRatio", "Supersampling ratio for rendering high-quality frames before downsampling/decimation. Lowering this value can save GPU time and memory but will reduce quality of captured frames. Be very careful with high values!"),CategoryName="Settings",bShowPin=True,bCanToggleVisibility=True)
   ShowPinForProperties(1)=(PropertyName="UpscalingRatio",PropertyFriendlyName="Upscaling Ratio",PropertyTooltip=NSLOCTEXT("UObjectToolTips", "NGDCRenderingSettings:UpscalingRatio", "Upscaling ratio between the jittered inputs and the ground truth outputs."),CategoryName="Settings",bShowPin=True,bCanToggleVisibility=True)
   ShowPinForProperties(2)=(PropertyName="FixedFrameRate",PropertyFriendlyName="Fixed Frame Rate",PropertyTooltip=NSLOCTEXT("UObjectToolTips", "NGDCRenderingSettings:FixedFrameRate", "When > 0, fixes the frame rate during capture and writes it to the output JSON (EmulatedFramerate). Set to 0 to leave the current frame rate unchanged."),CategoryName="Timing",bShowPin=True,bCanToggleVisibility=True)
   ShowPinForProperties(3)=(PropertyName="CameraCutTranslationThreshold",PropertyFriendlyName="Camera Cut Translation Threshold",PropertyTooltip=NSLOCTEXT("UObjectToolTips", "NGDCRenderingSettings:CameraCutTranslationThreshold", "World-space distance (cm) that triggers a camera cut heuristic when the automatic flag is missing. Set to 0 to disable the translation check."),CategoryName="Camera Cuts",bShowPin=True,bCanToggleVisibility=True)
   ShowPinForProperties(4)=(PropertyName="CameraCutRotationThresholdDegrees",PropertyFriendlyName="Camera Cut Rotation Threshold Degrees",PropertyTooltip=NSLOCTEXT("UObjectToolTips", "NGDCRenderingSettings:CameraCutRotationThresholdDegrees", "Angular delta (degrees) that triggers a camera cut heuristic when the automatic flag is missing. Set to 0 to disable the rotation check."),CategoryName="Camera Cuts",bShowPin=True,bCanToggleVisibility=True)
   StructType="/Script/CoreUObject.ScriptStruct'/Script/NeuralGraphicsDataCapture.NGDCRenderingSettings'"
   NodePosX=176
   NodePosY=240
   AdvancedPinDisplay=Shown
   NodeGuid=81995F7E479CE69E7645D3ACBBD1B0DA
   CustomProperties Pin (PinId=C3F944A54825BE2D824F86BB432AE47A,PinName="NGDCRenderingSettings",Direction="EGPD_Output",PinType.PinCategory="struct",PinType.PinSubCategory="",PinType.PinSubCategoryObject="/Script/CoreUObject.ScriptStruct'/Script/NeuralGraphicsDataCapture.NGDCRenderingSettings'",PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,LinkedTo=(K2Node_MakeStruct_1 7036BB8A44B0431990530B962DE7F50B,),PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=487E07AC4695AD4FB4C1538E4BD8BEE5,PinName="SupersamplingRatio",PinFriendlyName=INVTEXT("Supersampling Ratio"),PinToolTip="Supersampling Ratio\nInteger\n\nSupersampling ratio for rendering high-quality frames before downsampling/decimation. Lowering this value can save GPU time and memory but will reduce quality of captured frames. Be very careful with high values!",PinType.PinCategory="int",PinType.PinSubCategory="",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,DefaultValue="4",AutogeneratedDefaultValue="4",PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=E53F93BA4021707421D53391C9500515,PinName="UpscalingRatio",PinFriendlyName=INVTEXT("Upscaling Ratio"),PinToolTip="Upscaling Ratio\nFloat (single-precision)\n\nUpscaling ratio between the jittered inputs and the ground truth outputs.",PinType.PinCategory="real",PinType.PinSubCategory="float",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,DefaultValue="2.000000",AutogeneratedDefaultValue="2.000000",PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=False,bOrphanedPin=False,)
   CustomProperties Pin (PinId=E545E5974910BFA19F6B1FAFD15A4CC1,PinName="FixedFrameRate",PinFriendlyName=INVTEXT("Fixed Frame Rate"),PinToolTip="Fixed Frame Rate\nFloat (single-precision)\n\nWhen > 0, fixes the frame rate during capture and writes it to the output JSON (EmulatedFramerate). Set to 0 to leave the current frame rate unchanged.",PinType.PinCategory="real",PinType.PinSubCategory="float",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,DefaultValue="0.000000",AutogeneratedDefaultValue="0.000000",PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=True,bOrphanedPin=False,)
   CustomProperties Pin (PinId=610B4F24482A24AA2A00E181EB85FA1B,PinName="CameraCutTranslationThreshold",PinFriendlyName=INVTEXT("Camera Cut Translation Threshold"),PinToolTip="Camera Cut Translation Threshold\nFloat (single-precision)\n\nWorld-space distance (cm) that triggers a camera cut heuristic when the automatic flag is missing. Set to 0 to disable the translation check.",PinType.PinCategory="real",PinType.PinSubCategory="float",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,DefaultValue="0.000000",AutogeneratedDefaultValue="200.000000",PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=True,bOrphanedPin=False,)
   CustomProperties Pin (PinId=F08566AF4EB3FA8CE1433AA1265EB6A9,PinName="CameraCutRotationThresholdDegrees",PinFriendlyName=INVTEXT("Camera Cut Rotation Threshold Degrees"),PinToolTip="Camera Cut Rotation Threshold Degrees\nFloat (single-precision)\n\nAngular delta (degrees) that triggers a camera cut heuristic when the automatic flag is missing. Set to 0 to disable the rotation check.",PinType.PinCategory="real",PinType.PinSubCategory="float",PinType.PinSubCategoryObject=None,PinType.PinSubCategoryMemberReference=(),PinType.PinValueType=(),PinType.ContainerType=None,PinType.bIsReference=False,PinType.bIsConst=False,PinType.bIsWeakPointer=False,PinType.bIsUObjectWrapper=False,PinType.bSerializeAsSinglePrecisionFloat=False,DefaultValue="0.000000",AutogeneratedDefaultValue="30.000000",PersistentGuid=00000000000000000000000000000000,bHidden=False,bNotConnectable=False,bDefaultValueIsReadOnly=False,bDefaultValueIsIgnored=False,bAdvancedView=True,bOrphanedPin=False,)
End Object



```

## Trademarks and Copyrights

* NVIDIA and the NVIDIA logo are trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. and other countries.
* Unreal® is a trademark or registered trademark of Epic Games, Inc. in the United States of America and elsewhere.
* Windows is a registered trademark or trademark of Microsoft Corporation in the US and other jurisdictions.
