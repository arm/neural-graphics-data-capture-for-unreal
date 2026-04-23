// SPDX-FileCopyrightText: Copyright 2025-2026 Arm Limited and/or its affiliates <open-source-office@arm.com>
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Logging/LogMacros.h"

// Log category for all messages from the Neural Graphics Data Capture plugin
DECLARE_LOG_CATEGORY_EXTERN(LogNGDC, Log, All);

// Enabling this will disable compiler optimizations in non-debug build configurations, making it easier to step through code.
#define NGDC_DEBUGGING_ENABLED 0


