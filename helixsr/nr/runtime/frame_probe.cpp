// Standalone GPU caller. SPDX-License-Identifier: MIT
#include "pch.h"
#include "ProbeSupport.h"
int main(int argc,char** argv){try{
    require(argc==2||argc==4,"Expected model-cache path [network-width network-height]");
    const auto networkWidth=argc==4?std::stoul(argv[2]):320u,networkHeight=argc==4?std::stoul(argv[3]):320u;
    require(networkWidth>=33&&networkWidth<=4096&&networkHeight>=33&&networkHeight<=4096,"Invalid probe network dimensions");
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create caller DXGI");ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;i++){ComPtr<IDXGIAdapter1> candidate;const auto result=factory->EnumAdapters1(i,&candidate);if(result==DXGI_ERROR_NOT_FOUND)break;check(result,"Enumerate adapters");
        DXGI_ADAPTER_DESC1 desc{};check(candidate->GetDesc1(&desc),"Describe adapter");if(desc.VendorId==0x1002&&desc.DeviceId==0x13fe){adapter=candidate;break;}}
    require(adapter!=nullptr,"BC250 caller adapter missing");ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"Create caller device");
    Api api;Queue queue(device.Get()),otherQueue(device.Get());const auto folder=std::filesystem::path(argv[1]).wstring();
    HelixSrNrCreateInfo creation{};creation.size=sizeof(creation);creation.apiVersion=2;creation.device=device.Get();creation.cacheDirectory=folder.c_str();
    creation.planFile=L"runtime-plan.bin";creation.dataFile=L"initial-data.bin";creation.displayWidth=creation.displayHeight=320;creation.networkWidth=networkWidth;creation.networkHeight=networkHeight;creation.maximumFramesInFlight=4;
    creation.progress=[](void*,uint32_t done,uint32_t total){if(done==1||done%64==0||done==total)std::cout<<"DLL pipeline "<<done<<"/"<<total<<std::endl;};
    HelixSrNrContext* context=nullptr;api.ok(api.create(&creation,&context),"Create caller context");
    auto color=texture(device.Get(),320,320,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
    auto depth=texture(device.Get(),320,320,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
    auto motion=texture(device.Get(),320,320,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
    std::array<ComPtr<ID3D12Resource>,3> outputs;for(auto& output:outputs)output=texture(device.Get(),320,320,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto pixels=probePixels();
    std::vector<float> depthPixels(320*320,.5f);std::vector<uint32_t> motionPixels(320*320,0);
    Commands uploads(device.Get());std::vector<ComPtr<ID3D12Resource>> staging;
    const auto upload=[&](ID3D12Resource* target,const void* data,uint32_t rowBytes,ID3D12GraphicsCommandList* supplied=nullptr){
        auto* commandList=supplied?supplied:uploads.list.Get();
        const auto desc=target->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};uint64_t bytes=0;device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
        auto source=makeBuffer(device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;const D3D12_RANGE noRead{0,0};check(source->Map(0,&noRead,&mapped),"Map caller upload");
        for(uint32_t y=0;y<320;y++)memcpy(static_cast<uint8_t*>(mapped)+size_t(y)*layout.Footprint.RowPitch,static_cast<const uint8_t*>(data)+size_t(y)*rowBytes,rowBytes);source->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=source.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=layout;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=target;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commandList->CopyTextureRegion(&to,0,0,0,&from,nullptr);barrier(commandList,target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);staging.push_back(source);
    };
    upload(color.Get(),pixels.data(),320*8);upload(depth.Get(),depthPixels.data(),320*4);upload(motion.Get(),motionPixels.data(),320*4);
    uploads.submit(queue);queue.wait(queue.signal());staging.clear();
    HelixSrNrFrameInfo frame{};frame.size=sizeof(frame);frame.color=color.Get();frame.depth=depth.Get();frame.motion=motion.Get();frame.output=outputs[0].Get();
    frame.colorState=frame.depthState=frame.motionState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;frame.outputState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    frame.colorRegion=frame.depthRegion=frame.motionRegion={0,0,320,320};frame.motionScaleX=frame.motionScaleY=1;
    frame.reset=1;frame.autoMask=1;frame.intensity=frame.localTone=frame.localStructure=frame.historyBlend=1;frame.skinStructure=-1;frame.depthThreshold=.01f;
    bool pendingRejected=false,destroyPendingRejected=false,aliasRejected=false;
    {
        Commands discarded(device.Get());frame.commands=discarded.list.Get();uint64_t ticket=0;api.ok(api.record(context,&frame,&ticket),"Record discard test");
        uint64_t second=99;pendingRejected=api.record(context,&frame,&second)==E_PENDING&&second==0;
        destroyPendingRejected=api.destroy(context)==E_PENDING;check(discarded.list->Close(),"Close discarded list");api.ok(api.abort(context,ticket),"Discard recorded frame");
    }
    {
        Commands invalid(device.Get());frame.commands=invalid.list.Get();auto* good=frame.output;frame.output=frame.color;uint64_t ticket=99;
        aliasRejected=api.record(context,&frame,&ticket)==E_INVALIDARG&&ticket==0;frame.output=good;check(invalid.list->Close(),"Close invalid list");
    }
    require(pendingRejected&&destroyPendingRejected&&aliasRejected,"Lifecycle preflight checks failed");
    ComPtr<ID3D12Resource> featureBuffer,headBuffer;api.ok(api.endpoints(context,&featureBuffer,&headBuffer),"Get diagnostic endpoints");
    auto featureReadback=makeBuffer(device.Get(),featureBuffer->GetDesc().Width,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    auto headReadback=makeBuffer(device.Get(),headBuffer->GetDesc().Width,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<std::unique_ptr<Commands>> work;std::vector<std::unique_ptr<Readback>> readbacks;std::vector<uint64_t> tickets;
    for(uint32_t i=0;i<3;i++){
        work.push_back(std::make_unique<Commands>(device.Get()));readbacks.push_back(std::make_unique<Readback>(device.Get(),320,320,outputs[i].Get()));
        frame.commands=work.back()->list.Get();frame.output=outputs[i].Get();frame.seed=i;frame.reset=i==0;
        uint64_t ticket=0;api.ok(api.record(context,&frame,&ticket),"Record queued frame");readbacks.back()->copy(frame.commands,frame.output);
        if(i==0){frame.commands->CopyBufferRegion(featureReadback.Get(),0,featureBuffer.Get(),0,featureBuffer->GetDesc().Width);
            frame.commands->CopyBufferRegion(headReadback.Get(),0,headBuffer.Get(),0,headBuffer->GetDesc().Width);}
        auto& actualQueue=i==1?otherQueue:queue;api.ok(api.prepare(context,ticket,actualQueue.queue.Get()),"Prepare actual queue submission");
        work.back()->submit(actualQueue);api.ok(api.submitted(context,ticket,actualQueue.queue.Get()),"Report queued submission");tickets.push_back(ticket);
    }
    queue.wait(queue.signal());api.ok(api.collect(context),"Collect queued frames");
    std::vector<uint8_t> first;
    for(uint32_t i=0;i<3;i++){const auto data=readbacks[i]->read();if(i==0)first=data;save(("dll-frame-"+std::to_string(i)+".rgba16f").c_str(),data);}
    require(api.destroy(context)==E_PENDING,"GPU completion released still-replayable recordings");
    api.ok(api.prepare(context,tickets[0],otherQueue.queue.Get()),"Prepare command-list replay");
    ID3D12CommandList* replay[]={work[0]->list.Get()};otherQueue.queue->ExecuteCommandLists(1,replay);
    api.ok(api.submitted(context,tickets[0],otherQueue.queue.Get()),"Report command-list replay");otherQueue.wait(otherQueue.signal());
    require(readbacks[0]->read()==first,"Replaying the recorded reset frame changed its output");
    work.clear();for(auto ticket:tickets)api.ok(api.retire(context,ticket),"Retire released command-list recording");
    const auto saveBuffer=[&](ID3D12Resource* resource,const char* name){const auto size=size_t(resource->GetDesc().Width);void* mapped=nullptr;D3D12_RANGE range{0,size};check(resource->Map(0,&range,&mapped),"Map diagnostic endpoint");
        std::vector<uint8_t> data(size);memcpy(data.data(),mapped,size);D3D12_RANGE noWrite{0,0};resource->Unmap(0,&noWrite);save(name,data);};
    saveBuffer(featureReadback.Get(),"dll-features-0.bin");saveBuffer(headReadback.Get(),"dll-head-0.bin");
    {
        Commands reset(device.Get());Readback resetOutput(device.Get(),320,320,outputs[0].Get());frame.commands=reset.list.Get();frame.output=outputs[0].Get();frame.seed=0;frame.reset=1;
        uint64_t ticket=0;api.ok(api.record(context,&frame,&ticket),"Record reset frame");resetOutput.copy(frame.commands,frame.output);
        api.ok(api.prepare(context,ticket,queue.queue.Get()),"Prepare reset frame");reset.submit(queue);
        api.ok(api.submitted(context,ticket,queue.queue.Get()),"Submit reset frame");queue.wait(queue.signal());
        const auto data=resetOutput.read();save("dll-reset.rgba16f",data);require(data==first,"Reset did not reproduce the first-frame output");
        reset.list.Reset();api.ok(api.retire(context,ticket),"Retire reset recording");
    }
    api.ok(api.destroy(context),"Destroy retired context");context=nullptr;
    creation.displayWidth=512;creation.displayHeight=288;creation.networkWidth=networkWidth;creation.networkHeight=std::max(33u,std::min(uint32_t(networkHeight),uint32_t(networkWidth)*288u/512u));
    api.ok(api.create(&creation,&context),"Create resized context");
    auto wide=texture(device.Get(),512,288,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bool wrongSizeRejected=false;
    {
        Commands wrongSize(device.Get());frame.commands=wrongSize.list.Get();frame.output=outputs[0].Get();uint64_t ticket=99;
        wrongSizeRejected=api.record(context,&frame,&ticket)==E_INVALIDARG&&ticket==0;check(wrongSize.list->Close(),"Close wrong-size list");
    }
    require(wrongSizeRejected,"Resized context accepted the previous output dimensions");
    std::vector<uint8_t> firstWide;
    for(uint32_t i=0;i<2;i++){
        Commands wideFrame(device.Get());Readback captured(device.Get(),512,288,wide.Get());frame.commands=wideFrame.list.Get();frame.output=wide.Get();
        frame.motion=nullptr;frame.seed=0;frame.reset=i==0;uint64_t ticket=0;api.ok(api.record(context,&frame,&ticket),"Record wide frame without motion");
        captured.copy(frame.commands,frame.output);api.ok(api.prepare(context,ticket,queue.queue.Get()),"Prepare wide frame");
        wideFrame.submit(queue);api.ok(api.submitted(context,ticket,queue.queue.Get()),"Submit wide frame");queue.wait(queue.signal());
        const auto data=captured.read();save(("dll-wide-"+std::to_string(i)+".rgba16f").c_str(),data);
        if(i==0)firstWide=data;else require(data==firstWide,"Missing motion did not consistently disable temporal reprojection");
        wideFrame.list.Reset();api.ok(api.retire(context,ticket),"Retire wide recording");
    }
    api.ok(api.destroy(context),"Destroy resized context");context=nullptr;
    creation.displayWidth=creation.displayHeight=320;creation.networkWidth=networkWidth;creation.networkHeight=networkHeight;
    api.ok(api.create(&creation,&context),"Create color-validation context");
    std::vector<uint8_t> hdrPixels=pixels;
    for(size_t i=0;i<hdrPixels.size()/2;i++)if(i%4!=3){uint16_t value=0;memcpy(&value,hdrPixels.data()+i*2,2);value+=3u<<10;memcpy(hdrPixels.data()+i*2,&value,2);}
    save("dll-hdr-input.rgba16f",hdrPixels);
    auto hdrColor=texture(device.Get(),320,320,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
    Commands hdrUpload(device.Get());upload(hdrColor.Get(),hdrPixels.data(),320*8,hdrUpload.list.Get());hdrUpload.submit(queue);queue.wait(queue.signal());staging.clear();
    for(uint32_t i=0;i<3;i++){
        Commands colored(device.Get());Readback captured(device.Get(),320,320,outputs[0].Get());
        frame.commands=colored.list.Get();frame.output=outputs[0].Get();frame.color=i==2?color.Get():hdrColor.Get();
        frame.colorSpace=i==2?2:1;frame.paperWhite=1;frame.intensity=i==1?1:0;frame.seed=0;frame.reset=1;
        uint64_t ticket=0;api.ok(api.record(context,&frame,&ticket),"Record color-space frame");captured.copy(frame.commands,frame.output);
        api.ok(api.prepare(context,ticket,queue.queue.Get()),"Prepare color-space frame");colored.submit(queue);
        api.ok(api.submitted(context,ticket,queue.queue.Get()),"Submit color-space frame");queue.wait(queue.signal());
        const auto data=captured.read();save(("dll-color-"+std::to_string(i)+".rgba16f").c_str(),data);
        if(i==0)require(data==hdrPixels,"Zero intensity did not preserve HDR scene values");
        if(i==2)require(data==pixels,"Zero intensity did not preserve linear SDR values");
        if(i==1){bool highlight=false;for(size_t j=0;j<data.size()/2;j++)if(j%4!=3){uint16_t value=0;memcpy(&value,data.data()+j*2,2);require((value&0x7c00u)!=0x7c00u,"Nonfinite HDR scene output");highlight|=(value&0x8000u)==0&&value>0x3c00u;}
            require(highlight&&data!=hdrPixels,"HDR composition clipped highlights or bypassed the network");}
        colored.list.Reset();api.ok(api.retire(context,ticket),"Retire color-space recording");
    }
    api.ok(api.destroy(context),"Destroy color-validation context");context=nullptr;
    std::cout<<"{\"queuedFrames\":3,\"pendingRecordRejected\":true,\"pendingDestroyRejected\":true,\"discardSucceeded\":true,\"aliasedInputRejected\":true,\"resetExact\":true,\"destroyRetiredSucceeded\":true,\"resizeRecreateSucceeded\":true,\"wrongOutputSizeRejected\":true,\"missingMotionDisablesHistory\":true,\"commandListReplaySucceeded\":true,\"replayRetainedUntilClosed\":true,\"crossQueueOrderingSucceeded\":true,\"hdrZeroIntensityExact\":true,\"hdrHighlightsPreserved\":true,\"linearSdrZeroIntensityExact\":true}"<<std::endl;
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
