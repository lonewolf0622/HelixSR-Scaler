#pragma once
// SPDX-License-Identifier: MIT
#include "HelixSrNrApi.h"
#include "Common.h"
#include <DirectXPackedVector.h>
#include <chrono>
#include <iostream>
#include <memory>
using namespace helixnr;
inline std::vector<uint8_t> probePixels(){
    std::vector<uint16_t> halves(320*320*4);
    using DirectX::PackedVector::XMConvertFloatToHalf;
    for(uint32_t y=0;y<320;y++)for(uint32_t x=0;x<320;x++){
        const auto i=(y*320+x)*4;
        halves[i]=XMConvertFloatToHalf(.1f+.7f*x/319.f);
        halves[i+1]=XMConvertFloatToHalf(.1f+.7f*y/319.f);
        halves[i+2]=XMConvertFloatToHalf(.2f+.3f*((x>>4^y>>4)&1));
        halves[i+3]=XMConvertFloatToHalf(1.f);
    }
    std::vector<uint8_t> bytes(halves.size()*sizeof(uint16_t));
    memcpy(bytes.data(),halves.data(),bytes.size());return bytes;
}
struct Api {
    HMODULE module=LoadLibraryW(L"helixsr-nr.dll");
    decltype(&HelixSR_NR_Create) create=nullptr;
    decltype(&HelixSR_NR_Record) record=nullptr;
    decltype(&HelixSR_NR_RecordColor) recordColor=nullptr;
    decltype(&HelixSR_NR_Submitted) submitted=nullptr;
    decltype(&HelixSR_NR_PrepareSubmission) prepare=nullptr;
    decltype(&HelixSR_NR_RetireRecording) retire=nullptr;
    decltype(&HelixSR_NR_Abort) abort=nullptr;
    decltype(&HelixSR_NR_Destroy) destroy=nullptr;
    decltype(&HelixSR_NR_Collect) collect=nullptr;
    decltype(&HelixSR_NR_LastError) error=nullptr;
    HRESULT(*endpoints)(HelixSrNrContext*,ID3D12Resource**,ID3D12Resource**)=nullptr;
    Api(){
        require(module!=nullptr,"Load backend DLL");
#define LOAD(name,field) field=reinterpret_cast<decltype(field)>(GetProcAddress(module,"HelixSR_NR_" name));require(field!=nullptr,"Missing backend export")
        LOAD("Create",create);LOAD("Record",record);LOAD("Submitted",submitted);LOAD("Abort",abort);
        LOAD("PrepareSubmission",prepare);LOAD("RetireRecording",retire);
        LOAD("RecordColor",recordColor);
        LOAD("Destroy",destroy);LOAD("Collect",collect);LOAD("LastError",error);LOAD("DebugEndpoints",endpoints);
#undef LOAD
    }
    void ok(HRESULT result,const char* action){if(FAILED(result))throw std::runtime_error(std::string(action)+": "+error());}
    ~Api(){if(module)FreeLibrary(module);}
};
inline ComPtr<ID3D12Resource> texture(ID3D12Device* device,uint32_t width,uint32_t height,DXGI_FORMAT format,D3D12_RESOURCE_STATES state,
                                     D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=format;desc.Flags=flags;
    ComPtr<ID3D12Resource> result;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&result)),"Create caller texture");return result;
}
struct Queue {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;Event event;uint64_t value=0;
    explicit Queue(ID3D12Device* d):device(d){D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(d->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"Create caller queue");check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Create caller fence");}
    uint64_t signal(){check(queue->Signal(fence.Get(),++value),"Signal caller fence");return value;}
    void wait(uint64_t target){check(fence->SetEventOnCompletion(target,event.value),"Caller completion event");
        require(WaitForSingleObject(event.value,60000)==WAIT_OBJECT_0,"Caller GPU wait timed out");check(device->GetDeviceRemovedReason(),"Caller device removed");}
};
struct Commands {
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    explicit Commands(ID3D12Device* device){check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Create caller allocator");
        check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Create caller list");}
    void submit(Queue& queue){check(list->Close(),"Close caller list");ID3D12CommandList* lists[]={list.Get()};queue.queue->ExecuteCommandLists(1,lists);}
};
struct Readback {
    ComPtr<ID3D12Resource> buffer;D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};uint64_t bytes=0;uint32_t width,height,pixelBytes;
    Readback(ID3D12Device* device,uint32_t w,uint32_t h,ID3D12Resource* resource,uint32_t bpp=8):width(w),height(h),pixelBytes(bpp){
        const auto desc=resource->GetDesc();device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
        buffer=makeBuffer(device,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    }
    void copy(ID3D12GraphicsCommandList* list,ID3D12Resource* output){
        barrier(list,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=output;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=buffer.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=layout;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);barrier(list,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    std::vector<uint8_t> read(){void* mapped=nullptr;D3D12_RANGE range{0,size_t(bytes)};check(buffer->Map(0,&range,&mapped),"Map caller readback");
        std::vector<uint8_t> data(size_t(width)*height*pixelBytes);for(uint32_t y=0;y<height;y++)memcpy(data.data()+size_t(y)*width*pixelBytes,static_cast<uint8_t*>(mapped)+size_t(y)*layout.Footprint.RowPitch,size_t(width)*pixelBytes);
        const D3D12_RANGE noWrite{0,0};buffer->Unmap(0,&noWrite);return data;}
};
inline void save(const char* name,const std::vector<uint8_t>& data){std::ofstream file(name,std::ios::binary);require(bool(file.write(reinterpret_cast<const char*>(data.data()),data.size())),"Save caller output");}
