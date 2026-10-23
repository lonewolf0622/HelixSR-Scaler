// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace helixnr {
struct Error : std::runtime_error {HRESULT result;Error(HRESULT value,const std::string& text):std::runtime_error(text),result(value){}};
inline void require(bool ok,const char* text){if(!ok)throw Error(E_INVALIDARG,text);}
inline void check(HRESULT result,const char* text){if(FAILED(result))throw Error(result,text);}
inline std::vector<uint8_t> readFile(const std::filesystem::path& path,size_t limit=1024ull*1024*1024){
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)throw Error(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),"Required model/cache file is missing");
    const auto size=file.tellg();require(size>0&&uint64_t(size)<=limit,"Invalid cache file size");
    std::vector<uint8_t> data(static_cast<size_t>(size));file.seekg(0);
    require(bool(file.read(reinterpret_cast<char*>(data.data()),size)),"Truncated cache file");return data;
}
inline std::array<uint8_t,32> sha256(const std::vector<uint8_t>& data){
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::array<uint8_t,32> result{};
    auto status=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0);
    if(status>=0)status=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);
    if(status>=0)status=BCryptHashData(hash,const_cast<PUCHAR>(data.data()),static_cast<ULONG>(data.size()),0);
    if(status>=0)status=BCryptFinishHash(hash,result.data(),static_cast<ULONG>(result.size()),0);
    if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
    if(status<0)throw Error(E_FAIL,"SHA256 validation failed");return result;
}
inline ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* device,uint64_t bytes,D3D12_HEAP_TYPE heap,
                                        D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
    D3D12_HEAP_PROPERTIES properties{};properties.Type=heap;properties.CreationNodeMask=properties.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=std::max<uint64_t>(bytes,4);
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;
    desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"Create graph buffer");
    return resource;
}
inline D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};return barrier;
}
inline void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
    if(from!=to){const auto value=transition(resource,from,to);list->ResourceBarrier(1,&value);}
}
inline void uavBarrier(ID3D12GraphicsCommandList* list){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;list->ResourceBarrier(1,&b);}
struct Event {HANDLE value=CreateEventW(nullptr,FALSE,FALSE,nullptr);Event(){if(!value)throw Error(HRESULT_FROM_WIN32(GetLastError()),"Create completion event");}~Event(){CloseHandle(value);}};
}
