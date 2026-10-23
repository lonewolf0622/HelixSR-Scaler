// SPDX-License-Identifier: MIT
#include "pch.h"
#include "ProbeSupport.h"
#include <array>
#include <cmath>
#include <limits>

namespace {
float component(const std::vector<uint8_t>& bytes,size_t i){
    uint16_t bits;memcpy(&bits,bytes.data()+i*2,2);
    return DirectX::PackedVector::XMConvertHalfToFloat(bits);
}
float linear(float x){return x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f);}
float y(const std::array<float,3>& rgb){return rgb[0]*.212639f+rgb[1]*.715169f+rgb[2]*.072192f;}
}
int main(int argc,char** argv){try{
    require(argc==2||argc==4,"Expected model-cache path [network-width network-height]");
    const uint32_t nw=argc==4?std::stoul(argv[2]):320u,nh=argc==4?std::stoul(argv[3]):320u;
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create color-test DXGI");
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;i++){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};check(candidate->GetDesc1(&d),"Describe color-test adapter");
        if(d.VendorId==0x1002&&d.DeviceId==0x13fe){adapter=candidate;break;}}
    require(adapter!=nullptr,"BC250 color-test adapter missing");
    ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"Create color-test device");
    Api api;Queue queue(device.Get());const auto path=std::filesystem::path(argv[1]).wstring();
    HelixSrNrCreateInfo creation{};creation.size=sizeof(creation);creation.apiVersion=2;creation.device=device.Get();
    creation.cacheDirectory=path.c_str();creation.planFile=L"runtime-plan.bin";creation.dataFile=L"initial-data.bin";
    creation.displayWidth=creation.displayHeight=320;creation.networkWidth=nw;creation.networkHeight=nh;creation.maximumFramesInFlight=4;
    HelixSrNrContext* context=nullptr;api.ok(api.create(&creation,&context),"Create color-test context");
    auto input=texture(device.Get(),320,320,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto output=texture(device.Get(),320,320,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const std::array<std::array<float,3>,8> chart{{{{.8f,.04f,.02f}},{{.03f,.7f,.04f}},{{.02f,.04f,.8f}},
        {{.7f,.55f,.02f}},{{.6f,.03f,.5f}},{{.02f,.65f,.6f}},{{.25f,.25f,.25f}},{{0,0,0}}}};
    HelixSrNrFrameInfo frame{};frame.size=sizeof(frame);frame.color=input.Get();frame.output=output.Get();
    frame.colorState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;frame.outputState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    frame.colorRegion={0,0,320,320};frame.reset=frame.autoMask=1;frame.intensity=frame.localTone=frame.localStructure=1;
    frame.skinStructure=-1;frame.historyBlend=1;frame.depthThreshold=.01f;frame.paperWhite=1;
    for(float invalid:{-.1f,1.1f,std::numeric_limits<float>::quiet_NaN()}){
        Commands cmd(device.Get());frame.commands=cmd.list.Get();uint64_t ticket=99;
        require(api.recordColor(context,&frame,invalid,&ticket)==E_INVALIDARG&&ticket==0,"Invalid color strength was accepted");
        check(cmd.list->Close(),"Close rejected color test");
    }
    uint32_t coloredPixels=0;float maximumError=0;bool changedBrightness=false;
    for(uint32_t space=0;space<3;space++){
        frame.colorSpace=space;
        std::vector<uint16_t> pixels(320*320*4);
        for(uint32_t row=0;row<320;row++)for(uint32_t col=0;col<320;col++){
            const auto index=(row*320+col)*4;const float scale=space==1?8.f:1.f;
            for(uint32_t c=0;c<3;c++)pixels[index+c]=DirectX::PackedVector::XMConvertFloatToHalf(chart[col/40][c]*scale*(.3f+.7f*row/319.f));
            pixels[index+3]=0x3c00;
        }
        std::vector<uint8_t> original(pixels.size()*2);memcpy(original.data(),pixels.data(),original.size());
        save(("color-input-"+std::to_string(space)+".rgba16f").c_str(),original);
        {
            Commands cmd(device.Get());D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};uint64_t bytes=0;const auto desc=input->GetDesc();
            device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
            auto staging=makeBuffer(device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            void* mapped=nullptr;check(staging->Map(0,nullptr,&mapped),"Map color chart");
            for(uint32_t row=0;row<320;row++)memcpy(static_cast<uint8_t*>(mapped)+row*layout.Footprint.RowPitch,original.data()+row*320*8,320*8);
            staging->Unmap(0,nullptr);barrier(cmd.list.Get(),input.Get(),frame.colorState,D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=staging.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=layout;
            to.pResource=input.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;cmd.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            barrier(cmd.list.Get(),input.Get(),D3D12_RESOURCE_STATE_COPY_DEST,frame.colorState);cmd.submit(queue);queue.wait(queue.signal());
        }
        const auto render=[&](float strength,bool legacy=false){
            Commands cmd(device.Get());Readback capture(device.Get(),320,320,output.Get());frame.commands=cmd.list.Get();uint64_t ticket=0;
            api.ok(legacy?api.record(context,&frame,&ticket):api.recordColor(context,&frame,strength,&ticket),"Record color chart");
            capture.copy(cmd.list.Get(),output.Get());api.ok(api.prepare(context,ticket,queue.queue.Get()),"Prepare color chart");
            cmd.submit(queue);api.ok(api.submitted(context,ticket,queue.queue.Get()),"Submit color chart");queue.wait(queue.signal());
            const auto data=capture.read();
            // Replaying must retain this recording's color setting and resources.
            api.ok(api.prepare(context,ticket,queue.queue.Get()),"Prepare color replay");
            ID3D12CommandList* lists[]={cmd.list.Get()};queue.queue->ExecuteCommandLists(1,lists);
            api.ok(api.submitted(context,ticket,queue.queue.Get()),"Submit color replay");queue.wait(queue.signal());
            require(capture.read()==data,"Color recording replay changed its output");
            cmd.list.Reset();api.ok(api.retire(context,ticket),"Retire color chart");return data;
        };
        const auto neural=render(1),legacy=render(1,true),preserved=render(0),half=render(.5f);
        require(neural==legacy,"Full color extension changed legacy output");
        save(("color-preserved-"+std::to_string(space)+".rgba16f").c_str(),preserved);
        save(("color-neural-"+std::to_string(space)+".rgba16f").c_str(),neural);
        frame.intensity=0;require(render(0)==original&&render(.5f)==original,"Color control changed zero-intensity input");frame.intensity=1;
        for(size_t i=0;i<320*320;i++){
            std::array<float,3> a{},b{},c{},m{};
            for(size_t channel=0;channel<3;channel++){
                a[channel]=component(original,i*4+channel);b[channel]=component(neural,i*4+channel);
                c[channel]=component(preserved,i*4+channel);m[channel]=component(half,i*4+channel);
                require(std::isfinite(b[channel])&&std::isfinite(c[channel])&&std::isfinite(m[channel]),"Nonfinite color output");
                if(space==0){a[channel]=linear(a[channel]);b[channel]=linear(b[channel]);c[channel]=linear(c[channel]);m[channel]=linear(m[channel]);}
            }
            const float sourceY=y(a),gain=sourceY<=.000001f?1.f:std::clamp(y(b)/sourceY,0.f,4.f);
            for(size_t channel=0;channel<3;channel++){
                const float expected=a[channel]*gain,error=std::abs(c[channel]-expected);
                maximumError=std::max(maximumError,error);
                require(error<=.002f+std::abs(expected)*.003f,"Game color proportions were not preserved");
                require(std::abs(m[channel]-(c[channel]+b[channel])*.5f)<=.002f+std::abs(m[channel])*.003f,"Color-strength midpoint is not linear light");
            }
            if(sourceY>.00001f){coloredPixels++;changedBrightness|=std::abs(y(c)-sourceY)>.005f;}
            else require(c[0]==0&&c[1]==0&&c[2]==0,"Black chart acquired a color cast");
        }
    }
    require(changedBrightness,"Color preservation bypassed neural lighting changes");api.ok(api.destroy(context),"Destroy color-test context");
    std::cout<<"{\"colorSpaces\":3,\"colorStrengths\":3,\"legacyExact\":true,\"zeroIntensityExact\":true,\"replayExact\":true,\"invalidStrengthsRejected\":3,\"sourceColorPreserved\":true,\"linearLightMidpoint\":true,\"neuralBrightnessRetained\":true,\"coloredPixelsChecked\":"<<coloredPixels<<",\"maximumAbsoluteError\":"<<maximumError<<"}"<<std::endl;
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
