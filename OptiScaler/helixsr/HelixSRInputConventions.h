// Copyright 2026 lonewolf0622. SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
namespace hxin
{
struct Scale { float x, y; };
// NDC spans two units horizontally and has the opposite vertical axis to
// pixel coordinates. Pixel-space XeSS vectors need only the supplied scale.
inline Scale xess_motion_scale(bool ndc, bool displayVectors, uint32_t renderW, uint32_t renderH,
                               uint32_t outputW, uint32_t outputH, Scale scale)
{
    if (!ndc) return scale;
    return {scale.x * float(displayVectors ? outputW : renderW) * 0.5f,
           -scale.y * float(displayVectors ? outputH : renderH) * 0.5f};
}
}
