// SPDX-License-Identifier: MIT
#ifndef HELIXSR_NR_FRAME_CONSTANTS_H
#define HELIXSR_NR_FRAME_CONSTANTS_H
#ifdef __cplusplus
#include <cstdint>
struct NrUInt2 {uint32_t x,y;};
struct NrFloat2 {float x,y;};
#define NR_UINT uint32_t
#else
#define NR_UINT uint
#define NrUInt2 uint2
#define NrFloat2 float2
#endif
struct NrFrameConstants {
    NrUInt2 displaySize, networkSize, fieldSize, colorBase, colorSize;
    NrUInt2 motionBase, motionSize;
    NrFloat2 motionScale;
    NrUInt2 depthBase, depthSize;
    NR_UINT seed, historyValid, autoMask, depthInverted;
    float localTone, localStructure, skinStructure, style;
    float intensity, historyBlend, depthThreshold, reserved;
    NR_UINT colorSpace;
    float paperWhite,colorStrength,reserved2;
};
#ifdef __cplusplus
static_assert(sizeof(NrFrameConstants)==144,"GPU root constant layout changed");
#else
#undef NrUInt2
#undef NrFloat2
#endif
#undef NR_UINT
#endif
