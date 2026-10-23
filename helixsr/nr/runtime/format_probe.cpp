// Actual typed-view load/store and guide-format validation. SPDX-License-Identifier: MIT
#include "pch.h"
#include "ProbeSupport.h"
#include "TextureFormats.h"
#include <cmath>
using namespace DirectX::PackedVector;
namespace {
constexpr uint32_t width=64,height=48;
std::vector<uint8_t> colorPixels(DXGI_FORMAT format,uint32_t bytes){
    std::vector<uint8_t> result(width*height*bytes);
    for(uint32_t y=0;y<height;y++)for(uint32_t x=0;x<width;x++){
        float values[4]={.1f+.7f*x/(width-1),.1f+.7f*y/(height-1),.2f+.3f*((x>>3^y>>3)&1),1};
        auto* target=result.data()+(y*width+x)*bytes;
        switch(colorView(format)){
        case DXGI_FORMAT_R16G16B16A16_FLOAT:{uint16_t v[4];for(uint32_t i=0;i<4;i++)v[i]=XMConvertFloatToHalf(values[i]);memcpy(target,v,8);break;}
        case DXGI_FORMAT_R32G32B32A32_FLOAT:values[0]*=200000;memcpy(target,values,16);break;
        case DXGI_FORMAT_R11G11B10_FLOAT:{XMFLOAT3PK v(values[0],values[1],values[2]);memcpy(target,&v,4);break;}
        case DXGI_FORMAT_R8G8B8A8_UNORM:{uint8_t v[4];for(uint32_t i=0;i<4;i++)v[i]=uint8_t(std::round(values[i]*255));memcpy(target,v,4);break;}
        case DXGI_FORMAT_R10G10B10A2_UNORM:{uint32_t v=uint32_t(std::round(values[0]*1023))|(uint32_t(std::round(values[1]*1023))<<10)|(uint32_t(std::round(values[2]*1023))<<20)|0xc0000000u;memcpy(target,&v,4);break;}
        default:throw std::runtime_error("Unsupported probe input");
        }
    }
    return result;
}
void upload(ID3D12Device* device,Queue& queue,ID3D12Resource* target,const std::vector<uint8_t>& data,uint32_t rowBytes){
    Commands commands(device);const auto d=target->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};uint64_t bytes=0;
    device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&bytes);
    auto staging=makeBuffer(device,bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped=nullptr;const D3D12_RANGE noRead{0,0};check(staging->Map(0,&noRead,&mapped),"Map format upload");
    require(data.size()==size_t(rowBytes)*d.Height,"Wrong fixture upload size");
    for(uint32_t y=0;y<d.Height;y++)memcpy(static_cast<uint8_t*>(mapped)+size_t(y)*layout.Footprint.RowPitch,data.data()+size_t(y)*rowBytes,rowBytes);
    staging->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION from{},to{};
    from.pResource=staging.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=layout;
    to.pResource=target;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;commands.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    barrier(commands.list.Get(),target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commands.submit(queue);queue.wait(queue.signal());
}
}
int main(int argc,char** argv){try{
    require(argc==2,"Expected model-cache directory");ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create factory");
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;i++){ComPtr<IDXGIAdapter1> candidate;const auto hr=factory->EnumAdapters1(i,&candidate);if(hr==DXGI_ERROR_NOT_FOUND)break;check(hr,"Enumerate adapter");
        DXGI_ADAPTER_DESC1 d{};check(candidate->GetDesc1(&d),"Describe adapter");if(d.VendorId==0x1002&&d.DeviceId==0x13fe){adapter=candidate;break;}}
    require(adapter!=nullptr,"BC250 adapter missing");ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"Create device");
    Api api;Queue queue(device.Get());const auto cache=std::filesystem::path(argv[1]).wstring();HelixSrNrCreateInfo creation{};
    creation.size=sizeof(creation);creation.apiVersion=2;creation.device=device.Get();creation.cacheDirectory=cache.c_str();creation.planFile=L"runtime-plan.bin";creation.dataFile=L"initial-data.bin";
    creation.displayWidth=creation.networkWidth=width;creation.displayHeight=creation.networkHeight=height;creation.maximumFramesInFlight=4;
    HelixSrNrContext* context=nullptr;api.ok(api.create(&creation,&context),"Create format context");
    HelixSrNrFrameInfo frame{};frame.size=sizeof(frame);frame.colorRegion={0,0,width,height};frame.colorState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    frame.depthState=frame.motionState=frame.colorState;frame.outputState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    frame.reset=frame.autoMask=1;frame.localTone=frame.localStructure=frame.historyBlend=1;frame.skinStructure=-1;frame.depthThreshold=.01f;
    frame.motionScaleX=frame.motionScaleY=1;frame.colorSpace=1;frame.paperWhite=1;
    const auto execute=[&](uint32_t pixelBytes){
        Commands commands(device.Get());Readback capture(device.Get(),width,height,frame.output,pixelBytes);frame.commands=commands.list.Get();uint64_t ticket=0;
        api.ok(api.record(context,&frame,&ticket),"Record format frame");capture.copy(frame.commands,frame.output);
        api.ok(api.prepare(context,ticket,queue.queue.Get()),"Prepare format frame");commands.submit(queue);api.ok(api.submitted(context,ticket,queue.queue.Get()),"Submit format frame");queue.wait(queue.signal());
        const auto data=capture.read();commands.list.Reset();api.ok(api.retire(context,ticket),"Retire format recording");return data;
    };
    const std::pair<DXGI_FORMAT,uint32_t> colors[]={
        {DXGI_FORMAT_R16G16B16A16_FLOAT,8},{DXGI_FORMAT_R16G16B16A16_TYPELESS,8},
        {DXGI_FORMAT_R11G11B10_FLOAT,4},{DXGI_FORMAT_R32G32B32A32_FLOAT,16},{DXGI_FORMAT_R32G32B32A32_TYPELESS,16},
        {DXGI_FORMAT_R8G8B8A8_UNORM,4},{DXGI_FORMAT_R8G8B8A8_TYPELESS,4},
        {DXGI_FORMAT_R10G10B10A2_UNORM,4},{DXGI_FORMAT_R10G10B10A2_TYPELESS,4}};
    uint32_t colorChecks=0;
    for(const auto& [format,pixelBytes]:colors){
        require(supportsTextureView(device.Get(),colorView(format),false)&&supportsTextureView(device.Get(),colorView(format),true),"BC250 lacks tested color view");
        auto input=texture(device.Get(),width,height,format,D3D12_RESOURCE_STATE_COPY_DEST);
        auto output=texture(device.Get(),width,height,format,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto pixels=colorPixels(format,pixelBytes);upload(device.Get(),queue,input.Get(),pixels,width*pixelBytes);
        frame.color=input.Get();frame.output=output.Get();frame.intensity=0;frame.reset=1;
        require(execute(pixelBytes)==pixels,"Typed color load/store changed zero-intensity pixels");
        if(format==DXGI_FORMAT_R11G11B10_FLOAT){frame.intensity=1;const auto neural=execute(pixelBytes);require(neural!=pixels,"R11G11B10 output bypassed the network");
            for(size_t i=0;i<neural.size();i+=4){uint32_t packed=0;memcpy(&packed,neural.data()+i,4);require(((packed>>6)&31)!=31&&((packed>>17)&31)!=31&&((packed>>27)&31)!=31,"Nonfinite packed RGB output");}}
        ++colorChecks;
    }
    const auto pixels=colorPixels(DXGI_FORMAT_R16G16B16A16_FLOAT,8);
    auto input=texture(device.Get(),width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
    auto output=texture(device.Get(),width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    upload(device.Get(),queue,input.Get(),pixels,width*8);frame.color=input.Get();frame.output=output.Get();frame.intensity=1;frame.colorSpace=0;
    std::vector<uint8_t> reference;
    const std::pair<DXGI_FORMAT,uint32_t> motions[]={{DXGI_FORMAT_R16G16_FLOAT,4},{DXGI_FORMAT_R16G16_TYPELESS,4},{DXGI_FORMAT_R32G32_FLOAT,8},{DXGI_FORMAT_R32G32_TYPELESS,8},{DXGI_FORMAT_R16G16B16A16_FLOAT,8},{DXGI_FORMAT_R16G16B16A16_TYPELESS,8},{DXGI_FORMAT_R32G32B32A32_FLOAT,16},{DXGI_FORMAT_R32G32B32A32_TYPELESS,16}};
    uint32_t guideChecks=0;
    for(const auto& [format,pixelBytes]:motions){
        const auto depthFormat=guideChecks%2?DXGI_FORMAT_R32_TYPELESS:DXGI_FORMAT_R32_FLOAT;
        auto depth=texture(device.Get(),width+16,height+16,depthFormat,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        auto motion=texture(device.Get(),width+16,height+16,format,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        std::vector<float> depths((width+16)*(height+16),.5f);std::vector<uint8_t> depthBytes(depths.size()*4),motionBytes(depths.size()*pixelBytes,0);
        // Poison the padding so ignoring the subrect changes motion/reprojection.
        for(uint32_t y=0;y<height+16;y++)for(uint32_t x=0;x<width+16;x++)if(guideChecks!=0&&(x<8||x>=width+8||y<8||y>=height+8)){
            const auto pixel=y*(width+16)+x;depths[pixel]=0;
            for(uint32_t component=0;component<pixelBytes/(motionView(format)==DXGI_FORMAT_R16G16_FLOAT||motionView(format)==DXGI_FORMAT_R16G16B16A16_FLOAT?2:4);component++){
                auto* target=motionBytes.data()+pixel*pixelBytes;
                if(motionView(format)==DXGI_FORMAT_R16G16_FLOAT||motionView(format)==DXGI_FORMAT_R16G16B16A16_FLOAT){const uint16_t value=XMConvertFloatToHalf(.25f);memcpy(target+component*2,&value,2);}
                else {const float value=.25f;memcpy(target+component*4,&value,4);}
            }
        }
        memcpy(depthBytes.data(),depths.data(),depthBytes.size());upload(device.Get(),queue,depth.Get(),depthBytes,(width+16)*4);upload(device.Get(),queue,motion.Get(),motionBytes,(width+16)*pixelBytes);
        frame.depth=depth.Get();frame.motion=motion.Get();frame.depthRegion=frame.motionRegion={8,8,width,height};
        frame.seed=0;frame.reset=1;execute(8);frame.seed=1;frame.reset=0;const auto current=execute(8);
        if(reference.empty())reference=current;else require(current==reference,"Equivalent typed guide views changed temporal output");
        ++guideChecks;
    }
    uint32_t depthChecks=0;
    const std::pair<DXGI_FORMAT,uint32_t> depthFormats[]={
        {DXGI_FORMAT_R32_FLOAT,4},{DXGI_FORMAT_R32_TYPELESS,4},
        {DXGI_FORMAT_R16_FLOAT,2},{DXGI_FORMAT_R16_UNORM,2},{DXGI_FORMAT_R16_TYPELESS,2},
        {DXGI_FORMAT_R24G8_TYPELESS,4},{DXGI_FORMAT_R32G8X24_TYPELESS,8}};
    auto zeroMotion=texture(device.Get(),width,height,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
    upload(device.Get(),queue,zeroMotion.Get(),std::vector<uint8_t>(width*height*4,0),width*4);
    for(const auto& [format,pixelBytes]:depthFormats){
        require(supportsTextureView(device.Get(),depthView(format),false),"BC250 lacks tested depth view");
        auto depth=texture(device.Get(),width+16,height+16,format,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE);
        std::vector<uint8_t> bytes((width+16)*(height+16)*pixelBytes,0);
        for(uint32_t y=8;y<height+8;y++)for(uint32_t x=8;x<width+8;x++){
            auto* target=bytes.data()+(y*(width+16)+x)*pixelBytes;
            switch(depthView(format)){
            case DXGI_FORMAT_R16_FLOAT:{const uint16_t v=XMConvertFloatToHalf(.5f);memcpy(target,&v,2);break;}
            case DXGI_FORMAT_R16_UNORM:{const uint16_t v=32768;memcpy(target,&v,2);break;}
            case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:{const uint32_t v=0x800000;memcpy(target,&v,4);break;}
            default:{const float v=.5f;memcpy(target,&v,4);break;}
            }
        }
        upload(device.Get(),queue,depth.Get(),bytes,(width+16)*pixelBytes);
        frame.depth=depth.Get();frame.motion=zeroMotion.Get();frame.depthRegion={8,8,width,height};frame.motionRegion={0,0,width,height};
        frame.seed=0;frame.reset=1;execute(8);frame.seed=1;frame.reset=0;
        require(execute(8)==reference,"Equivalent depth families changed temporal output");++depthChecks;
    }
    auto badDepth=texture(device.Get(),width,height,DXGI_FORMAT_R32_UINT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_FLAG_NONE);
    Commands invalid(device.Get());frame.commands=invalid.list.Get();frame.depth=badDepth.Get();frame.depthRegion={0,0,width,height};frame.motion=nullptr;uint64_t ticket=99;
    require(api.record(context,&frame,&ticket)==E_INVALIDARG&&ticket==0,"Unsupported integer depth was accepted");check(invalid.list->Close(),"Close invalid format list");
    api.ok(api.destroy(context),"Destroy format context");
    std::cout<<"{\"colorFormatsExactAtZeroIntensity\":"<<colorChecks<<",\"wideHdrZeroIntensityPreserved\":true,\"r11NeuralOutputFiniteAndChanged\":true,\"typedGuideTemporalComparisons\":"<<guideChecks<<",\"depthFamilyTemporalComparisons\":"<<depthChecks<<",\"integerDepthRejected\":true,\"guideSubrectPaddingIgnored\":true}"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
