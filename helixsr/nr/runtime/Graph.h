// SPDX-License-Identifier: MIT
#pragma once
#include "Common.h"
#include "HelixSrNrApi.h"
#include <map>
namespace helixnr {
class Graph final {
    struct Resource {uint64_t bytes,offset,dataBytes;bool writable;ComPtr<ID3D12Resource> gpu;};
    struct Layout {uint32_t binding,type;};
    struct Pipeline {std::vector<Layout> layout;std::string file;std::array<uint8_t,32> hash;
        ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> gpu;};
    struct Binding {uint32_t binding,resource,type;uint64_t offset,bytes;};
    struct Dispatch {uint32_t pipeline,x,y,z,descriptorOffset;std::vector<Binding> bindings;
        std::vector<std::pair<uint32_t,D3D12_RESOURCE_STATES>> desired;};
    std::vector<Resource> resources;
    std::vector<Pipeline> pipelines;
    std::vector<Dispatch> dispatches;
    std::vector<D3D12_RESOURCE_STATES> states;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT stride=0;
    uint32_t inputIndex=0,outputIndex=0,fieldWidth=0,fieldHeight=0;
  public:
    Graph(ID3D12Device*,const HelixSrNrCreateInfo&);
    void record(ID3D12GraphicsCommandList*); // input is UAV; every buffer returns COMMON
    ID3D12Resource* input()const{return resources[inputIndex].gpu.Get();}
    ID3D12Resource* output()const{return resources[outputIndex].gpu.Get();}
    uint32_t width()const{return fieldWidth;}uint32_t height()const{return fieldHeight;}
};
}
