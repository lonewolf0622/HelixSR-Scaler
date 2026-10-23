// SPDX-License-Identifier: MIT
#include "pch.h"
#include "Graph.h"
#include <atomic>
#include <future>
#include <mutex>
namespace helixnr {
namespace {
class Reader {
    std::vector<uint8_t> data;size_t cursor=0;
  public:
    explicit Reader(const std::filesystem::path& path):data(readFile(path,8*1024*1024)){}
    void bytes(void* out,size_t count){require(count<=data.size()-cursor,"Truncated graph plan");memcpy(out,data.data()+cursor,count);cursor+=count;}
    uint32_t u32(){uint32_t v;bytes(&v,4);return v;}uint64_t u64(){uint64_t v;bytes(&v,8);return v;}
    std::string shader(){const auto n=u32();require(n==72,"Invalid shader cache name");std::string s(n,'\0');bytes(s.data(),n);
        require(s.substr(0,4)=="cso/"&&s.substr(68)==".cso","Invalid shader path");
        require(std::all_of(s.begin()+4,s.begin()+68,[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');}),"Invalid shader digest name");return s;}
    void end(){require(cursor==data.size(),"Unexpected trailing graph data");}
};
}
Graph::Graph(ID3D12Device* device,const HelixSrNrCreateInfo& info){
    const auto folder=std::filesystem::path(info.cacheDirectory);
    require(info.planFile&&info.dataFile,"Missing graph cache names");
    const auto validBase=[](const wchar_t* name){const auto path=std::filesystem::path(name);return path.filename()==path&&!path.empty()&&path!=L"."&&path!=L"..";};
    require(validBase(info.planFile)&&validBase(info.dataFile),"Graph cache names must be basenames");
    Reader reader(folder/info.planFile);char magic[8];reader.bytes(magic,8);require(!memcmp(magic,"NRPLAN02",8),"Unsupported graph plan format");
    const auto resourceCount=reader.u32(),pipelineCount=reader.u32(),dispatchCount=reader.u32();
    inputIndex=reader.u32();outputIndex=reader.u32();const auto outputBytes=reader.u32();
    fieldWidth=reader.u32();fieldHeight=reader.u32();const auto validWidth=reader.u32(),validHeight=reader.u32();
    require(resourceCount>0&&resourceCount<=4096&&pipelineCount>0&&pipelineCount<=4096&&dispatchCount==451,"Unsupported network graph");
    require(inputIndex<resourceCount&&outputIndex<resourceCount&&inputIndex!=outputIndex,"Invalid network endpoints");
    // Native caches retain the 320 floor; explicitly prepared compact caches can be smaller.
    require(fieldWidth>=128&&fieldWidth<=4096&&fieldHeight>=128&&fieldHeight<=4096&&validWidth>0&&validWidth<=fieldWidth&&validHeight>0&&validHeight<=fieldHeight,"Invalid field geometry");
    require(uint64_t(fieldWidth)*fieldHeight*16==outputBytes,"Invalid head size");
    require(info.networkWidth>0&&info.networkWidth<=fieldWidth&&info.networkHeight>0&&info.networkHeight<=fieldHeight,"Invalid requested network resolution");
    std::array<uint8_t,32> expectedData;reader.bytes(expectedData.data(),expectedData.size());
    const auto data=readFile(folder/info.dataFile);require(sha256(data)==expectedData,"Model cache checksum mismatch");
    uint64_t totalBytes=0,zeroBytes=4;
    for(uint32_t i=0;i<resourceCount;i++){
        Resource r{reader.u64(),reader.u64(),reader.u64(),reader.u32()!=0,{}};
        require(r.bytes>0&&r.bytes%4==0&&r.bytes<=(1ull<<30)&&r.offset<=data.size()&&r.dataBytes<=data.size()-r.offset,"Invalid graph resource bounds");
        require(r.dataBytes==0||r.dataBytes==r.bytes,"Partial graph data is unsupported");
        totalBytes+=r.bytes;require(totalBytes<=(4ull<<30),"Graph exceeds memory budget");
        if(!r.dataBytes)zeroBytes=std::max(zeroBytes,r.bytes);
        if(i==inputIndex)r.writable=true;resources.push_back(std::move(r));
    }
    require(resources[inputIndex].bytes==uint64_t(fieldWidth)*fieldHeight*64&&resources[outputIndex].bytes==outputBytes,"Endpoint tensor shape mismatch");
    for(uint32_t i=0;i<pipelineCount;i++){
        Pipeline p;const auto count=reader.u32();require(count>0&&count<=16,"Invalid pipeline layout");
        for(uint32_t j=0;j<count;j++){Layout l{reader.u32(),reader.u32()};require(l.binding==j&&l.type<=2,"Invalid binding layout");p.layout.push_back(l);}
        p.file=reader.shader();reader.bytes(p.hash.data(),p.hash.size());pipelines.push_back(std::move(p));
    }
    uint32_t descriptors=0;
    for(uint32_t i=0;i<dispatchCount;i++){
        Dispatch d{reader.u32(),reader.u32(),reader.u32(),reader.u32(),descriptors,{},{}};
        require(d.pipeline<pipelineCount&&d.x>0&&d.x<=65535&&d.y>0&&d.y<=65535&&d.z>0&&d.z<=65535,"Invalid dispatch dimensions");
        const auto count=reader.u32();require(count==pipelines[d.pipeline].layout.size(),"Graph binding count mismatch");
        std::map<uint32_t,D3D12_RESOURCE_STATES> desired;
        for(uint32_t j=0;j<count;j++){
            Binding b{reader.u32(),reader.u32(),reader.u32(),reader.u64(),reader.u64()};
            require(b.binding==j&&b.type==pipelines[d.pipeline].layout[j].type&&b.resource<resourceCount,"Invalid dispatch binding");
            const auto& r=resources[b.resource];
            require(b.bytes>0&&b.offset<=r.bytes&&b.bytes<=r.bytes-b.offset&&b.bytes%4==0&&b.offset%4==0,"Descriptor exceeds resource bounds");
            require(b.type!=0||(b.offset%256==0&&b.bytes%256==0&&b.bytes<=65536),"Invalid uniform buffer range");
            require(b.type!=2||r.writable,"Writable descriptor references read-only storage");
            const auto state=b.type==0?D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER:b.type==1?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            const auto found=desired.find(b.resource);require(found==desired.end()||found->second==state,"Aliased conflicting resource roles");
            desired[b.resource]=state;d.bindings.push_back(b);
        }
        d.desired.assign(desired.begin(),desired.end());descriptors+=count;dispatches.push_back(std::move(d));
    }
    reader.end();states.resize(resourceCount,D3D12_RESOURCE_STATE_COMMON);
    std::map<std::string,ComPtr<ID3D12RootSignature>> roots;
    for(auto& p:pipelines){
        std::string key;for(const auto& l:p.layout)key.push_back(char('0'+l.type));
        if(!roots.count(key)){
            std::vector<D3D12_DESCRIPTOR_RANGE> ranges(p.layout.size());std::vector<D3D12_ROOT_PARAMETER> parameters(p.layout.size());
            for(uint32_t j=0;j<ranges.size();j++){
                ranges[j].RangeType=p.layout[j].type==0?D3D12_DESCRIPTOR_RANGE_TYPE_CBV:p.layout[j].type==1?D3D12_DESCRIPTOR_RANGE_TYPE_SRV:D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                ranges[j].NumDescriptors=1;ranges[j].BaseShaderRegister=j;
                parameters[j].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[j].DescriptorTable={1,&ranges[j]};
                parameters[j].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
            }
            D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=UINT(parameters.size());desc.pParameters=parameters.data();
            ComPtr<ID3DBlob> blob,errors;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors),"Serialize network root");
            ComPtr<ID3D12RootSignature> root;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"Create network root");roots[key]=root;
        }
        p.root=roots[key];
    }
    std::atomic<uint32_t> next{0},completed{0};std::mutex progressMutex;std::vector<std::future<void>> workers;
    for(uint32_t worker=0;worker<std::min(4u,pipelineCount);worker++)workers.push_back(std::async(std::launch::async,[&]{
        for(;;){const auto index=next.fetch_add(1);if(index>=pipelineCount)return;auto& p=pipelines[index];
            const auto shader=readFile(folder/p.file,16*1024*1024);require(sha256(shader)==p.hash,"Shader cache checksum mismatch");
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=p.root.Get();desc.CS={shader.data(),shader.size()};
            check(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&p.gpu)),"Create network pipeline");
            if(info.progress){std::lock_guard lock(progressMutex);info.progress(info.progressUser,++completed,pipelineCount);}
        }
    }));
    for(auto& worker:workers)worker.get();
    auto upload=makeBuffer(device,data.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto zeros=makeBuffer(device,zeroBytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped=nullptr;const D3D12_RANGE noRead{0,0};check(upload->Map(0,&noRead,&mapped),"Map model upload");
    memcpy(mapped,data.data(),data.size());upload->Unmap(0,nullptr);check(zeros->Map(0,&noRead,&mapped),"Map zero upload");
    memset(mapped,0,zeroBytes);zeros->Unmap(0,nullptr);
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC queueDesc{};queueDesc.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;
    check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"Create model upload queue");
    ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(queueDesc.Type,IID_PPV_ARGS(&allocator)),"Create model upload allocator");
    ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,queueDesc.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Create model upload list");
    for(auto& r:resources){
        r.gpu=makeBuffer(device,r.bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,r.writable?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE);
        list->CopyBufferRegion(r.gpu.Get(),0,r.dataBytes?upload.Get():zeros.Get(),r.dataBytes?r.offset:0,r.bytes);
        barrier(list.Get(),r.gpu.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    }
    check(list->Close(),"Close model upload");ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Create upload fence");
    Event event;ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),1),"Signal model upload");
    check(fence->SetEventOnCompletion(1,event.value),"Set model upload completion");
    // On a worker, keep initialization allocations alive until completion (or
    // device removal), rather than freeing in-flight uploads after a timeout.
    require(WaitForSingleObject(event.value,INFINITE)==WAIT_OBJECT_0,"Wait for model upload");
    check(device->GetDeviceRemovedReason(),"Device removed during upload");
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};heapDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors=descriptors;heapDesc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&heapDesc,IID_PPV_ARGS(&heap)),"Create network descriptors");stride=device->GetDescriptorHandleIncrementSize(heapDesc.Type);
    for(const auto& d:dispatches)for(uint32_t j=0;j<d.bindings.size();j++){
        const auto& b=d.bindings[j];auto* r=resources[b.resource].gpu.Get();auto cpu=heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=uint64_t(d.descriptorOffset+j)*stride;
        if(b.type==0){D3D12_CONSTANT_BUFFER_VIEW_DESC v{r->GetGPUVirtualAddress()+b.offset,UINT(b.bytes)};device->CreateConstantBufferView(&v,cpu);}
        else if(b.type==1){D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_TYPELESS;v.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
            v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Buffer.FirstElement=b.offset/4;v.Buffer.NumElements=UINT(b.bytes/4);
            v.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;device->CreateShaderResourceView(r,&v,cpu);}
        else{D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=DXGI_FORMAT_R32_TYPELESS;v.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
            v.Buffer.FirstElement=b.offset/4;v.Buffer.NumElements=UINT(b.bytes/4);v.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;device->CreateUnorderedAccessView(r,nullptr,&v,cpu);}
    }
}
void Graph::record(ID3D12GraphicsCommandList* list){
    std::fill(states.begin(),states.end(),D3D12_RESOURCE_STATE_COMMON);states[inputIndex]=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12DescriptorHeap* heaps[]={heap.Get()};list->SetDescriptorHeaps(1,heaps);
    for(const auto& d:dispatches){
        for(const auto& [index,state]:d.desired){barrier(list,resources[index].gpu.Get(),states[index],state);states[index]=state;}
        uavBarrier(list);const auto& p=pipelines[d.pipeline];list->SetPipelineState(p.gpu.Get());list->SetComputeRootSignature(p.root.Get());
        for(uint32_t j=0;j<d.bindings.size();j++){auto gpu=heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=uint64_t(d.descriptorOffset+j)*stride;list->SetComputeRootDescriptorTable(j,gpu);}
        list->Dispatch(d.x,d.y,d.z);
    }
    for(uint32_t i=0;i<resources.size();i++)barrier(list,resources[i].gpu.Get(),states[i],D3D12_RESOURCE_STATE_COMMON);
}
}
