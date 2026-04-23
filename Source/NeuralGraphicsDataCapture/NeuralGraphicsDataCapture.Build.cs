// SPDX-FileCopyrightText: Copyright 2025-2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: Apache-2.0

using UnrealBuildTool;
using System.IO;

public class NeuralGraphicsDataCapture : ModuleRules
{
	public NeuralGraphicsDataCapture(ReadOnlyTargetRules Target) : base(Target)
	{
		// Enable as OpenEXR uses exceptions - see FNGDCExport::SaveEXR in NGDCExport.cpp
		bEnableExceptions = true;

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Core",
			"Engine",
			"CoreUObject",
			"RenderCore",
			"RHI",
			"Renderer",
			"Projects"
		});

		if (Target.bBuildEditor)
		{
			// For bringing the Output Log window to the foreground
			PrivateDependencyModuleNames.Add("Slate");
		}

		// Dependencies for OpenEXR (to write EXR files)
		AddEngineThirdPartyPrivateStaticDependencies(Target, "Imath", "UEOpenExr");
	}
}
