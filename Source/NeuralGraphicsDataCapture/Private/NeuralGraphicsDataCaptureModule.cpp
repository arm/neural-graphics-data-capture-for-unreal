// SPDX-FileCopyrightText: Copyright 2025-2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: Apache-2.0

#include "NeuralGraphicsDataCaptureModule.h"

#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogNGDC);

class FNeuralGraphicsDataCaptureModule : public IModuleInterface
{
public:
	void StartupModule() override
	{
		// Register our plugin's Shader dir so that we can load shaders using paths starting with '/Plugin/NeuralGraphicsDataCapture'
		const FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("NeuralGraphicsDataCapture"))->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/NeuralGraphicsDataCapture"), PluginShaderDir);
	}

	void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FNeuralGraphicsDataCaptureModule, NeuralGraphicsDataCapture);
