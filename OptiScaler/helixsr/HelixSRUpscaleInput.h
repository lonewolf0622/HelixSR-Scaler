// Copyright 2026 lonewolf0622. SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include "ffx_api.h"
// Optional FidelityFX dispatch extension. All numbers already follow NGX conventions.
// Consumers must check HelixSR_UpscaleInputVersion() >= 1 before chaining it.
#define HELIXSR_UPSCALE_INPUT_TYPE 0x48580002u
struct HelixSRUpscaleInput
{
    ffxApiHeader header;
    uint32_t version = 1;
    uint32_t api = 0; // 0 FidelityFX, 1 NGX, 2 XeSS
    uint32_t maskOff = 0;
    float exposureScale = 1.0f;
    float mvOffsetX = 0.0f, mvOffsetY = 0.0f;
};
