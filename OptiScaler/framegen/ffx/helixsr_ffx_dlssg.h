/* helixsr_ffx_dlssg.h: the game's DLSS Frame Generation inputs, chained to a FidelityFX frame-generation prepare.
 *
 * Copyright 2026 lonewolf0622. MIT License: permission is hereby granted, free of charge, to any person obtaining a
 * copy of this file, to deal in it without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of it, subject to this notice being included in all
 * copies. THE FILE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 *
 * A front end that takes frame-generation inputs from DLSS Frame Generation (Streamline tags and constants) and drives
 * a FidelityFX 3.1 frame-generation DLL may chain this description to its ffxDispatchDescFrameGenerationPrepare (or
 * ...PrepareV2), right after the prepare's own header, when the DLL exports HelixSR_FfxFgDlssgVersion() and that
 * returns HELIXSR_FFX_DLSSG_INPUTS_VERSION or more. The DLL then reads the frame's values as the game gave them to
 * DLSS Frame Generation, and removes the description from the chain before the prepare goes on to another module.
 * Any other DLL never sees it.
 *
 * Plain data only: values are exactly the game's (sl::Constants, kStructVersion2 field for field), nothing converted.
 * Matrices are 16 floats in the memory order of the game's float4x4 (rows). Resource states are D3D12_RESOURCE_STATES
 * values, valid on the prepare's command list and, as the FidelityFX descriptions' states, at that frame's generation.
 */
#ifndef HELIXSR_FFX_DLSSG_H
#define HELIXSR_FFX_DLSSG_H
#include <stdint.h>

#define HELIXSR_FFX_DESC_TYPE_DLSSG_INPUTS 0x48580001u /* "HX" 0001: outside AMD's effect ranges */
#define HELIXSR_FFX_DLSSG_INPUTS_VERSION 1u

/* HelixSRFfxDlssgResource.flags */
#define HELIXSR_DLSSG_RES_COPY 0x1u      /* an exact copy made when the game tagged it (the tag was valid only then) */
#define HELIXSR_DLSSG_RES_CONVERTED 0x2u /* a copy in another format (not exact: a front-end format change) */

/* HelixSRFfxDlssgInputs.flags: the game's booleans */
#define HELIXSR_DLSSG_DEPTH_INVERTED 0x001u
#define HELIXSR_DLSSG_CAMERA_MOTION_INCLUDED 0x002u
#define HELIXSR_DLSSG_MOTION_VECTORS_3D 0x004u
#define HELIXSR_DLSSG_RESET 0x008u
#define HELIXSR_DLSSG_ORTHOGRAPHIC 0x010u
#define HELIXSR_DLSSG_MOTION_VECTORS_DILATED 0x020u
#define HELIXSR_DLSSG_MOTION_VECTORS_JITTERED 0x040u
/* the front end's knowledge about the UI image (not a game boolean) */
#define HELIXSR_DLSSG_UI_PREMULTIPLIED 0x100u /* uiColor's colour is premultiplied by its alpha */

typedef struct HelixSRFfxDlssgResource
{
    void* resource;     /* ID3D12Resource*; null: not given for this frame */
    uint32_t state;     /* D3D12_RESOURCE_STATES */
    uint32_t left, top; /* the tagged region (the game's extent; 0, 0, resource size when it gave none) */
    uint32_t width, height;
    uint32_t flags; /* HELIXSR_DLSSG_RES_* */
    uint32_t reserved;
} HelixSRFfxDlssgResource;

typedef struct HelixSRFfxDlssgInputs
{
    uint64_t type;       /* HELIXSR_FFX_DESC_TYPE_DLSSG_INPUTS (the layout of ffxApiHeader) */
    void* pNext;         /* the rest of the prepare's chain */
    uint32_t structSize; /* sizeof(HelixSRFfxDlssgInputs) */
    uint32_t version;    /* HELIXSR_FFX_DLSSG_INPUTS_VERSION */
    uint64_t frameID;    /* the FidelityFX frame ID of the prepare (the same frame) */
    uint64_t dlssgFrame; /* the game's Streamline frame index of these values (0: not known) */

    HelixSRFfxDlssgResource depth; /* as tagged for DLSS Frame Generation */
    HelixSRFfxDlssgResource motionVectors;
    HelixSRFfxDlssgResource hudlessColor; /* null when the front end gives none */
    HelixSRFfxDlssgResource uiColor;      /* null when the front end gives none */

    float cameraViewToClip[16];
    float clipToCameraView[16];
    float clipToLensClip[16];
    float clipToPrevClip[16];
    float prevClipToClip[16];

    float jitterOffset[2];
    float mvecScale[2]; /* the game's (Streamline's normalized scale: times the motion-vector region's size) */
    float cameraPinholeOffset[2];
    float cameraPos[3];
    float cameraUp[3];
    float cameraRight[3];
    float cameraFwd[3];

    float cameraNear;
    float cameraFar;
    float cameraFOV;
    float cameraAspectRatio;
    float motionVectorsInvalidValue;
    float minRelativeLinearDepthObjectSeparation;

    uint32_t flags; /* HELIXSR_DLSSG_* */
    uint32_t reserved;
} HelixSRFfxDlssgInputs;

#endif
