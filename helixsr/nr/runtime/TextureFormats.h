// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>

namespace helixnr {
// Typeless inputs use the floating-point view of their family, except depth
// families whose shader-readable view is explicitly normalized.
inline DXGI_FORMAT colorView(DXGI_FORMAT format){
    switch(format){
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:return format;
    default:return DXGI_FORMAT_UNKNOWN;
    }
}
inline DXGI_FORMAT motionView(DXGI_FORMAT format){
    switch(format){
    case DXGI_FORMAT_R16G16_TYPELESS:return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:return format;
    default:return DXGI_FORMAT_UNKNOWN;
    }
}
inline DXGI_FORMAT depthView(DXGI_FORMAT format){
    switch(format){
    case DXGI_FORMAT_R32_TYPELESS:return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:return format;
    default:return DXGI_FORMAT_UNKNOWN;
    }
}
inline bool supportsTextureView(ID3D12Device* device,DXGI_FORMAT format,bool writable){
    if(!device||format==DXGI_FORMAT_UNKNOWN)return false;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};support.Format=format;
    if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support))))return false;
    return writable ? (support.Support1&D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)&&
                      (support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)
                    : (support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)!=0;
}
}
