// SPDX-License-Identifier: MIT
#include "pch.h"
#include "HelixSrNrApi.h"
#include "Common.h"
#include "Graph.h"
#include "FrameConstants.h"
#include "TextureFormats.h"
#include <generated/FramePre.h>
#include <generated/FramePost.h>
#include <cmath>
#include <memory>
#include <mutex>
#include <atomic>
using namespace helixnr;
namespace {
thread_local std::string lastError;
#ifdef HELIXSR_NR_DIAGNOSTICS
std::atomic_uint64_t diagnosticRecords {0}, diagnosticSubmissions {0}, diagnosticRetirements {0}, diagnosticAborts {0};
#endif
template<class F> HRESULT guarded(F&& function){
    try{lastError.clear();function();return S_OK;}
    catch(const Error& e){lastError=e.what();return e.result;}
    catch(const std::bad_alloc&){lastError="Not enough memory for Neural Rendering";return E_OUTOFMEMORY;}
    catch(const std::exception& e){lastError=e.what();return E_FAIL;}
    catch(...){lastError="Unexpected Neural Rendering failure";return E_FAIL;}
}
ComPtr<ID3D12Resource> makeTexture(ID3D12Device* device,uint32_t width,uint32_t height,DXGI_FORMAT format){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> resource;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&resource)),"Create temporal history");return resource;
}
void validateDevice(ID3D12Device* device,ID3D12DeviceChild* child){
    ComPtr<ID3D12Device> owner;check(child->GetDevice(IID_PPV_ARGS(&owner)),"Get resource device");
    require(owner.Get()==device,"GPU objects belong to different devices");
}
void validateTexture(ID3D12Device* device,ID3D12Resource* texture,DXGI_FORMAT format,const HelixSrNrRegion& region,bool writable,const char* role){
    require(texture!=nullptr,"A required frame texture is missing");validateDevice(device,texture);
    const auto d=texture->GetDesc();require(d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.DepthOrArraySize==1&&d.SampleDesc.Count==1&&d.MipLevels==1,"Unsupported frame texture shape");
    const auto error=std::string("Unsupported ")+role+" texture format ("+std::to_string(unsigned(d.Format))+")";
    require(supportsTextureView(device,format,writable),error.c_str());
    require(region.width>0&&region.height>0&&region.x<=d.Width&&region.width<=d.Width-region.x&&region.y<=d.Height&&region.height<=d.Height-region.y,"Frame subrect exceeds its texture");
    require(!writable||(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),"Output texture requires UAV access");
    require(writable||!(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE),"Input texture denies shader access");
}
void finiteRange(float value,float low,float high,const char* text){require(std::isfinite(value)&&value>=low&&value<=high,text);}
}
struct HelixSrNrContext {
    struct Frame {
        uint64_t id=0;D3D12_COMMAND_LIST_TYPE type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        uint64_t lastFence=0;
        bool submitted=false,prepared=false,closed=false;
        ComPtr<ID3D12CommandQueue> submissionQueue;
        ComPtr<ID3D12DescriptorHeap> heap;
        std::array<ComPtr<ID3D12Resource>,4> inputs;
        uint32_t oldHistoryIndex=0;bool oldHistoryValid=false;
    };
    std::mutex mutex;
    ComPtr<ID3D12Device> device;
    std::unique_ptr<Graph> graph;
    ComPtr<ID3D12RootSignature> frameRoot;
    ComPtr<ID3D12PipelineState> pre,post;
    ComPtr<ID3D12DescriptorHeap> clearDescriptors;
    std::array<ComPtr<ID3D12Resource>,2> history,depthHistory;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandQueue> queue;
    Event completion;
    std::vector<Frame> frames;
    uint64_t nextId=1,pending=0,lastSubmitted=0;
    uint32_t historyIndex=0,displayWidth=0,displayHeight=0,networkWidth=0,networkHeight=0,maximumInFlight=0;
    bool historyValid=false,poisoned=false;
    UINT descriptorStride=0;

    explicit HelixSrNrContext(const HelixSrNrCreateInfo& info):device(info.device),displayWidth(info.displayWidth),displayHeight(info.displayHeight),
        networkWidth(info.networkWidth),networkHeight(info.networkHeight),maximumInFlight(info.maximumFramesInFlight){
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_2};
        check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&shaderModel,sizeof(shaderModel)),"Query shader model");
        if(shaderModel.HighestShaderModel<D3D_SHADER_MODEL_6_2)throw Error(E_NOTIMPL,"Neural Rendering needs shader model 6.2");
        D3D12_FEATURE_DATA_D3D12_OPTIONS4 options{};check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4,&options,sizeof(options)),"Query native half support");
        if(!options.Native16BitShaderOpsSupported)throw Error(E_NOTIMPL,"Neural Rendering needs native 16-bit shader operations");
        graph=std::make_unique<Graph>(device.Get(),info);
        for(auto& h:history)h=makeTexture(device.Get(),displayWidth,displayHeight,DXGI_FORMAT_R16G16B16A16_FLOAT);
        for(auto& h:depthHistory)h=makeTexture(device.Get(),displayWidth,displayHeight,DXGI_FORMAT_R32_FLOAT);
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,6,0,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,4,0,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
        D3D12_ROOT_PARAMETER parameters[3]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants={0,0,36};parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        for(uint32_t i=0;i<2;i++){parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i+1].DescriptorTable={1,&ranges[i]};parameters[i+1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;}
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=3;desc.pParameters=parameters;
        ComPtr<ID3DBlob> blob,errors;check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors),"Serialize frame root");
        check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&frameRoot)),"Create frame root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=frameRoot.Get();
        pipeline.CS={HelixSrNrFramePre,sizeof(HelixSrNrFramePre)};check(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&pre)),"Create feature shader");
        pipeline.CS={HelixSrNrFramePost,sizeof(HelixSrNrFramePost)};check(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&post)),"Create composition shader");
        check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Create frame retirement fence");
        descriptorStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC clearDesc{};clearDesc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;clearDesc.NumDescriptors=4;
        check(device->CreateDescriptorHeap(&clearDesc,IID_PPV_ARGS(&clearDescriptors)),"Create CPU clear descriptors");
        for(uint32_t i=0;i<2;i++){
            auto cpu=clearDescriptors->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=uint64_t(i*2)*descriptorStride;
            D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;v.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            device->CreateUnorderedAccessView(history[i].Get(),nullptr,&v,cpu);cpu.ptr+=descriptorStride;v.Format=DXGI_FORMAT_R32_FLOAT;
            device->CreateUnorderedAccessView(depthHistory[i].Get(),nullptr,&v,cpu);
        }
        frames.reserve(maximumInFlight);
    }
    void collect(){
        const auto completed=fence->GetCompletedValue();
        if(completed==UINT64_MAX){check(device->GetDeviceRemovedReason(),"Neural Rendering device removed");throw Error(E_FAIL,"Invalid retirement fence");}
        frames.erase(std::remove_if(frames.begin(),frames.end(),[&](const Frame& f){return f.closed&&!f.prepared&&f.submitted&&f.lastFence<=completed;}),frames.end());
    }
    Frame makeFrame(const HelixSrNrFrameInfo& info){
        Frame frame;frame.id=nextId;frame.type=info.commands->GetType();frame.oldHistoryIndex=historyIndex;frame.oldHistoryValid=historyValid;
        frame.inputs={info.color,info.depth,info.motion,info.output};
        D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;desc.NumDescriptors=12;desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&frame.heap)),"Create immutable frame descriptors");
        const auto cpu=[&](uint32_t index){auto h=frame.heap->GetCPUDescriptorHandleForHeapStart();h.ptr+=uint64_t(index)*descriptorStride;return h;};
        const auto srv=[&](uint32_t index,ID3D12Resource* resource,DXGI_FORMAT format){
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=format;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;device->CreateShaderResourceView(resource,&v,cpu(index));
        };
        const auto uav=[&](uint32_t index,ID3D12Resource* resource,DXGI_FORMAT format){
            D3D12_UNORDERED_ACCESS_VIEW_DESC v{};v.Format=format;v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(resource,nullptr,&v,cpu(index));
        };
        srv(0,info.color,colorView(info.color->GetDesc().Format));
        srv(1,info.motion,info.motion?motionView(info.motion->GetDesc().Format):DXGI_FORMAT_R16G16_FLOAT);
        srv(2,info.depth,info.depth?depthView(info.depth->GetDesc().Format):DXGI_FORMAT_R32_FLOAT);
        srv(3,history[historyIndex].Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);srv(4,depthHistory[historyIndex].Get(),DXGI_FORMAT_R32_FLOAT);
        D3D12_SHADER_RESOURCE_VIEW_DESC head{};head.Format=DXGI_FORMAT_R32_TYPELESS;head.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        head.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;head.Buffer.NumElements=UINT(graph->output()->GetDesc().Width/4);head.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(graph->output(),&head,cpu(5));
        D3D12_UNORDERED_ACCESS_VIEW_DESC features{};features.Format=DXGI_FORMAT_R32_TYPELESS;features.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
        features.Buffer.NumElements=UINT(graph->input()->GetDesc().Width/4);features.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(graph->input(),nullptr,&features,cpu(6));
        uav(7,info.output,colorView(info.output->GetDesc().Format));uav(8,history[1-historyIndex].Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
        uav(9,depthHistory[1-historyIndex].Get(),DXGI_FORMAT_R32_FLOAT);
        uav(10,history[historyIndex].Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);uav(11,depthHistory[historyIndex].Get(),DXGI_FORMAT_R32_FLOAT);
        return frame;
    }
    void validate(const HelixSrNrFrameInfo& info){
        require(info.size==sizeof(info)&&info.commands&&info.color&&info.output,"Invalid frame descriptor");validateDevice(device.Get(),info.commands);
        const auto type=info.commands->GetType();require(type==D3D12_COMMAND_LIST_TYPE_DIRECT||type==D3D12_COMMAND_LIST_TYPE_COMPUTE,"Frame needs a direct or compute command list");
        require(info.color!=info.output&&info.depth!=info.output&&info.motion!=info.output&&info.color!=info.depth&&info.color!=info.motion,"Aliased frame inputs are unsupported");
        validateTexture(device.Get(),info.color,colorView(info.color->GetDesc().Format),info.colorRegion,false,"color");
        validateTexture(device.Get(),info.output,colorView(info.output->GetDesc().Format),{0,0,displayWidth,displayHeight},true,"output");
        require(info.output->GetDesc().Width==displayWidth&&info.output->GetDesc().Height==displayHeight,"Output dimensions changed; recreate the NR context");
        if(info.depth)validateTexture(device.Get(),info.depth,depthView(info.depth->GetDesc().Format),info.depthRegion,false,"depth");
        if(info.motion)validateTexture(device.Get(),info.motion,motionView(info.motion->GetDesc().Format),info.motionRegion,false,"motion");
        finiteRange(info.intensity,0,1,"Intensity is out of range");finiteRange(info.localTone,0,2,"Tone is out of range");
        finiteRange(info.localStructure,0,2,"Structure is out of range");finiteRange(info.skinStructure,-1,2,"Skin structure is out of range");
        finiteRange(info.historyBlend,0,1,"History blend is out of range");finiteRange(info.depthThreshold,0,1,"Depth threshold is out of range");
        finiteRange(info.motionScaleX,-65536,65536,"Motion scale is invalid");finiteRange(info.motionScaleY,-65536,65536,"Motion scale is invalid");
        if(info.style!=0)throw Error(E_NOTIMPL,"The frame backend currently accepts the neutral style only");
        require(info.autoMask<=1&&info.depthInverted<=1&&info.reset<=1,"Invalid frame control flags");
        require(info.colorSpace<=2,"Unsupported color encoding");
        if(info.colorSpace!=0)finiteRange(info.paperWhite,.05f,65504.f,"Invalid paper white");
    }
    void record(const HelixSrNrFrameInfo& info,uint64_t* ticket){
        if(poisoned)throw Error(E_FAIL,"The NR context failed submission; its resources are retained");
        if(pending)throw Error(E_PENDING,"Report submission or discard the previous recorded frame first");
        collect();if(frames.size()>=maximumInFlight)throw Error(E_PENDING,"NR frame retirement is pending");
        require(nextId!=UINT64_MAX,"Frame ticket overflow");validate(info);
        auto frame=makeFrame(info);const auto current=historyIndex,next=1-historyIndex;
        NrFrameConstants constants{};constants.displaySize={displayWidth,displayHeight};constants.networkSize={networkWidth,networkHeight};constants.fieldSize={graph->width(),graph->height()};
        constants.colorBase={info.colorRegion.x,info.colorRegion.y};constants.colorSize={info.colorRegion.width,info.colorRegion.height};
        if(info.motion){constants.motionBase={info.motionRegion.x,info.motionRegion.y};constants.motionSize={info.motionRegion.width,info.motionRegion.height};}
        if(info.depth){constants.depthBase={info.depthRegion.x,info.depthRegion.y};constants.depthSize={info.depthRegion.width,info.depthRegion.height};}
        constants.motionScale={info.motionScaleX,info.motionScaleY};constants.seed=info.seed;
        constants.historyValid=historyValid&&!info.reset&&info.motion;constants.autoMask=info.autoMask;constants.depthInverted=info.depthInverted;
        constants.localTone=info.localTone;constants.localStructure=info.localStructure;constants.skinStructure=info.skinStructure;
        constants.style=info.style;constants.intensity=info.intensity;constants.historyBlend=info.historyBlend;constants.depthThreshold=info.depthThreshold;
        constants.colorSpace=info.colorSpace;constants.paperWhite=info.paperWhite;
        // Everything that can fail or allocate is complete before writing any
        // GPU commands, so an E_PENDING/validation failure allows host fallback.
        frames.push_back(std::move(frame));auto& retained=frames.back();pending=retained.id;*ticket=pending;nextId++;
        auto* list=info.commands;ID3D12DescriptorHeap* heaps[]={retained.heap.Get()};list->SetDescriptorHeaps(1,heaps);
        const auto gpu=[&](uint32_t index){auto h=retained.heap->GetGPUDescriptorHandleForHeapStart();h.ptr+=uint64_t(index)*descriptorStride;return h;};
        if(!historyValid){
            const float zero[4]={0,0,0,0};barrier(list,history[current].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            barrier(list,depthHistory[current].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            auto clearCpu=clearDescriptors->GetCPUDescriptorHandleForHeapStart();clearCpu.ptr+=uint64_t(current*2)*descriptorStride;
            list->ClearUnorderedAccessViewFloat(gpu(10),clearCpu,history[current].Get(),zero,0,nullptr);clearCpu.ptr+=descriptorStride;
            list->ClearUnorderedAccessViewFloat(gpu(11),clearCpu,depthHistory[current].Get(),zero,0,nullptr);
            barrier(list,history[current].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barrier(list,depthHistory[current].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        barrier(list,info.color,info.colorState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(info.depth)barrier(list,info.depth,info.depthState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(info.motion)barrier(list,info.motion,info.motionState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list,graph->input(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(frameRoot.Get());list->SetComputeRoot32BitConstants(0,36,&constants,0);
        list->SetComputeRootDescriptorTable(1,gpu(0));list->SetComputeRootDescriptorTable(2,gpu(6));list->SetPipelineState(pre.Get());
        list->Dispatch((graph->width()+7)/8,(graph->height()+7)/8,1);
        graph->record(list);
        list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(frameRoot.Get());list->SetComputeRoot32BitConstants(0,36,&constants,0);
        list->SetComputeRootDescriptorTable(1,gpu(0));list->SetComputeRootDescriptorTable(2,gpu(6));list->SetPipelineState(post.Get());
        barrier(list,graph->output(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list,info.output,info.outputState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(list,history[next].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(list,depthHistory[next].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->Dispatch((displayWidth+7)/8,(displayHeight+7)/8,1);
        barrier(list,history[next].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list,depthHistory[next].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list,graph->output(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        barrier(list,info.color,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,info.colorState);
        if(info.depth)barrier(list,info.depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,info.depthState);
        if(info.motion)barrier(list,info.motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,info.motionState);
        if(info.outputState!=D3D12_RESOURCE_STATE_UNORDERED_ACCESS)barrier(list,info.output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,info.outputState);
        else uavBarrier(list);
        historyIndex=next;historyValid=true;
#ifdef HELIXSR_NR_DIAGNOSTICS
        ++diagnosticRecords;
#endif
    }
};
extern "C" {
HRESULT HelixSR_NR_Create(const HelixSrNrCreateInfo* info,HelixSrNrContext** output){
    if(output)*output=nullptr;
    return guarded([&]{require(info&&output&&info->size==sizeof(*info)&&info->apiVersion==2&&info->device&&info->cacheDirectory,"Invalid creation descriptor");
        require(info->displayWidth>0&&info->displayWidth<=4096&&info->displayHeight>0&&info->displayHeight<=4096&&info->maximumFramesInFlight>0&&info->maximumFramesInFlight<=16,"Invalid display size or queue depth");
        auto context=std::make_unique<HelixSrNrContext>(*info);*output=context.release();});
}
HRESULT HelixSR_NR_Record(HelixSrNrContext* context,const HelixSrNrFrameInfo* info,uint64_t* ticket){
    if(ticket)*ticket=0;
    return guarded([&]{require(context&&info&&ticket,"Invalid record arguments");std::lock_guard lock(context->mutex);context->record(*info,ticket);});
}
HRESULT HelixSR_NR_PrepareSubmission(HelixSrNrContext* context,uint64_t ticket,ID3D12CommandQueue* queue){
    return guarded([&]{require(context&&queue&&ticket,"Invalid submission arguments");std::lock_guard lock(context->mutex);
        auto found=std::find_if(context->frames.begin(),context->frames.end(),[&](const auto& f){return f.id==ticket;});
        require(found!=context->frames.end()&&!found->closed,"The recording is no longer replayable");
        if(found->prepared)throw Error(E_PENDING,"Submission was already prepared");
        validateDevice(context->device.Get(),queue);require(found->type==queue->GetDesc().Type,"Submission queue type differs from its command list");
        if(context->queue&&context->queue.Get()!=queue&&context->lastSubmitted)
            check(queue->Wait(context->fence.Get(),context->lastSubmitted),"Order NR work across queues");
        found->submissionQueue=queue;found->prepared=true;
    });
}
HRESULT HelixSR_NR_Submitted(HelixSrNrContext* context,uint64_t ticket,ID3D12CommandQueue* queue){
    return guarded([&]{require(context&&queue&&ticket,"Invalid submission arguments");std::lock_guard lock(context->mutex);
        auto found=std::find_if(context->frames.begin(),context->frames.end(),[&](const auto& f){return f.id==ticket;});
        require(found!=context->frames.end()&&found->prepared&&found->submissionQueue.Get()==queue,"Submission does not match its preparation");
        require(context->lastSubmitted<UINT64_MAX-1,"Submission timeline overflow");
        const auto value=context->lastSubmitted+1;
        const auto result=queue->Signal(context->fence.Get(),value);
        if(FAILED(result)){context->poisoned=true;check(result,"Signal frame retirement; GPU resources retained");}
        found->prepared=false;found->submitted=true;found->lastFence=value;
        context->queue=queue;context->lastSubmitted=value;if(context->pending==ticket)context->pending=0;
#ifdef HELIXSR_NR_DIAGNOSTICS
        ++diagnosticSubmissions;
#endif
    });
}
HRESULT HelixSR_NR_RetireRecording(HelixSrNrContext* context,uint64_t ticket){
    return guarded([&]{require(context&&ticket,"Invalid recording retirement arguments");std::lock_guard lock(context->mutex);
        auto found=std::find_if(context->frames.begin(),context->frames.end(),[&](const auto& f){return f.id==ticket;});
        require(found!=context->frames.end()&&found->submitted&&!found->prepared,"Recording has not finished submission");
        found->closed=true;context->collect();
#ifdef HELIXSR_NR_DIAGNOSTICS
        ++diagnosticRetirements;
#endif
    });
}
HRESULT HelixSR_NR_Abort(HelixSrNrContext* context,uint64_t ticket){
    return guarded([&]{require(context&&ticket,"Invalid discarded-frame arguments");std::lock_guard lock(context->mutex);
        require(context->pending==ticket&&!context->poisoned,"Only an unsubmitted frame may be discarded");
        if(context->frames.back().prepared)throw Error(E_PENDING,"Submission preparation is already active");
        const auto& frame=context->frames.back();context->historyIndex=frame.oldHistoryIndex;context->historyValid=frame.oldHistoryValid;
        context->frames.pop_back();context->pending=0;
#ifdef HELIXSR_NR_DIAGNOSTICS
        ++diagnosticAborts;
#endif
    });
}
HRESULT HelixSR_NR_Collect(HelixSrNrContext* context){return guarded([&]{require(context,"Invalid context");std::lock_guard lock(context->mutex);context->collect();});}
HRESULT HelixSR_NR_Destroy(HelixSrNrContext* context){
    return guarded([&]{require(context,"Invalid context");{
        std::lock_guard lock(context->mutex);
        if(SUCCEEDED(context->device->GetDeviceRemovedReason())){
            if(context->pending||std::any_of(context->frames.begin(),context->frames.end(),[](const auto& f){return !f.closed||f.prepared;}))
                throw Error(E_PENDING,"Retire replayable recordings before destruction");
            if(context->lastSubmitted&&context->fence->GetCompletedValue()<context->lastSubmitted){
                check(context->fence->SetEventOnCompletion(context->lastSubmitted,context->completion.value),"Set final retirement event");
                const auto waited=WaitForSingleObject(context->completion.value,60000);
                if(waited!=WAIT_OBJECT_0)throw Error(DXGI_ERROR_WAIT_TIMEOUT,"GPU retirement pending; context retained");
            }
        }
    }delete context;});
}
const char* HelixSR_NR_LastError(void){return lastError.c_str();}
#ifdef HELIXSR_NR_DIAGNOSTICS
__declspec(dllexport) HRESULT HelixSR_NR_DebugCounts(uint64_t* values,uint32_t count){
    if(!values||count!=4)return E_INVALIDARG;
    values[0]=diagnosticRecords.load();values[1]=diagnosticSubmissions.load();values[2]=diagnosticRetirements.load();values[3]=diagnosticAborts.load();return S_OK;
}
__declspec(dllexport) HRESULT HelixSR_NR_DebugEndpoints(HelixSrNrContext* context,ID3D12Resource** features,ID3D12Resource** head){
    return guarded([&]{require(context&&features&&head,"Invalid diagnostic arguments");std::lock_guard lock(context->mutex);
        *features=context->graph->input();(*features)->AddRef();*head=context->graph->output();(*head)->AddRef();});
}
#endif
}
