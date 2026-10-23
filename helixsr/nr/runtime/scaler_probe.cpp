// Public NGX caller of the complete Scaler front end. SPDX-License-Identifier: MIT
#include "pch.h"
#include "ProbeSupport.h"
#include <nvsdk_ngx.h>
#include <thread>
#include "StateAudit.h"

int main(int argc,char** argv){try{
    const bool unrealStates=argc==2&&std::string(argv[1])=="--unreal-states";
    const bool auditStates=unrealStates||(argc==2&&std::string(argv[1])=="--audit-states");
    require(argc==1||auditStates,"Expected optional --audit-states or --unreal-states");
    const auto colorState=unrealStates?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_COPY_SOURCE;
    const auto depthState=unrealStates?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_COPY_SOURCE;
    const auto motionState=unrealStates?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_SOURCE;
    const auto outputState=unrealStates?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST;
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Create caller DXGI");ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;i++){ComPtr<IDXGIAdapter1> candidate;const auto result=factory->EnumAdapters1(i,&candidate);if(result==DXGI_ERROR_NOT_FOUND)break;check(result,"Enumerate adapters");
        DXGI_ADAPTER_DESC1 d{};check(candidate->GetDesc1(&d),"Describe adapter");if(d.VendorId==0x1002&&d.DeviceId==0x13fe){adapter=candidate;break;}}
    require(adapter!=nullptr,"BC250 adapter missing");ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"Create device");
    if(auditStates)probe::install(device.Get());
    Queue queue(device.Get());HMODULE scaler=LoadLibraryW(L"nvngx.dll");require(scaler!=nullptr,"Load complete Scaler DLL");
    using Init=NVSDK_NGX_Result(*)(unsigned long long,const wchar_t*,ID3D12Device*,NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
    using Allocate=NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter**);
    using InitProject=NVSDK_NGX_Result(*)(const char*,NVSDK_NGX_EngineType,const char*,const wchar_t*,ID3D12Device*,const NVSDK_NGX_FeatureCommonInfo*,NVSDK_NGX_Version);
    using Create=NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
    using Evaluate=NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback);
    using Release=NVSDK_NGX_Result(*)(NVSDK_NGX_Handle*);
    const auto init=reinterpret_cast<Init>(GetProcAddress(scaler,"NVSDK_NGX_D3D12_Init_Ext"));
    const auto allocate=reinterpret_cast<Allocate>(GetProcAddress(scaler,"NVSDK_NGX_D3D12_AllocateParameters"));
    const auto create=reinterpret_cast<Create>(GetProcAddress(scaler,"NVSDK_NGX_D3D12_CreateFeature"));
    const auto evaluate=reinterpret_cast<Evaluate>(GetProcAddress(scaler,"NVSDK_NGX_D3D12_EvaluateFeature"));
    const auto release=reinterpret_cast<Release>(GetProcAddress(scaler,"NVSDK_NGX_D3D12_ReleaseFeature"));
    require(init&&allocate&&create&&evaluate&&release,"Scaler NGX exports missing");
    auto ngx=[](NVSDK_NGX_Result r,const char* action){if(r!=NVSDK_NGX_Result_Success){char text[200];snprintf(text,sizeof(text),"%s: 0x%08x",action,unsigned(r));throw std::runtime_error(text);}};
    std::filesystem::create_directories(L"ngx-cache");const auto cache=std::filesystem::absolute(L"ngx-cache").wstring();
    if(unrealStates){
        const auto initProject=reinterpret_cast<InitProject>(GetProcAddress(scaler,"NVSDK_NGX_D3D12_Init_with_ProjectID"));
        require(initProject!=nullptr,"Missing project initialization export");
        ngx(initProject("HelixSR-NR-Probe",NVSDK_NGX_ENGINE_TYPE_UNREAL,"5.7",cache.c_str(),device.Get(),nullptr,NVSDK_NGX_Version_API),"Initialize Unreal Scaler path");
    }else ngx(init(0x1337,cache.c_str(),device.Get(),NVSDK_NGX_Version_API,nullptr),"Initialize Scaler");
    NVSDK_NGX_Parameter* parameters=nullptr;ngx(allocate(&parameters),"Allocate Scaler parameters");require(parameters!=nullptr,"Empty parameter map");
    parameters->Set(NVSDK_NGX_Parameter_Width,320u);parameters->Set(NVSDK_NGX_Parameter_Height,320u);
    parameters->Set(NVSDK_NGX_Parameter_OutWidth,320u);parameters->Set(NVSDK_NGX_Parameter_OutHeight,320u);
    parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue,int(NVSDK_NGX_PerfQuality_Value_DLAA));
    parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,int(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
    Commands construction(device.Get());NVSDK_NGX_Handle* feature=nullptr;
    ngx(create(construction.list.Get(),NVSDK_NGX_Feature_SuperSampling,parameters,&feature),"Create Scaler upscaler");require(feature!=nullptr,"Empty upscaler handle");
    construction.submit(queue);queue.wait(queue.signal());
    auto color=texture(device.Get(),320,320,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto depth=texture(device.Get(),320,320,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
    auto motion=texture(device.Get(),320,320,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST);
    auto output=texture(device.Get(),320,320,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if(auditStates){
        for(auto* input:{color.Get(),depth.Get(),motion.Get()})probe::seed(input,D3D12_RESOURCE_STATE_COPY_DEST);
        probe::seed(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    const auto pixels=probePixels();std::vector<float> depths(320*320,.5f);std::vector<uint32_t> motions(320*320,0);
    Commands uploads(device.Get());std::vector<ComPtr<ID3D12Resource>> staging;
    const auto upload=[&](ID3D12Resource* target,const void* data,uint32_t rowBytes){
        const auto d=target->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};uint64_t bytes=0;device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&bytes);
        auto source=makeBuffer(device.Get(),bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;const D3D12_RANGE noRead{0,0};check(source->Map(0,&noRead,&mapped),"Map fixture upload");
        for(uint32_t y=0;y<320;y++)memcpy(static_cast<uint8_t*>(mapped)+size_t(y)*layout.Footprint.RowPitch,static_cast<const uint8_t*>(data)+size_t(y)*rowBytes,rowBytes);source->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=source.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=layout;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=target;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        uploads.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);barrier(uploads.list.Get(),target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);staging.push_back(source);
    };
    upload(color.Get(),pixels.data(),320*8);upload(depth.Get(),depths.data(),320*4);upload(motion.Get(),motions.data(),320*4);uploads.submit(queue);queue.wait(queue.signal());staging.clear();
    if(auditStates){
        Commands states(device.Get());
        barrier(states.list.Get(),color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,colorState);
        barrier(states.list.Get(),depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,depthState);
        barrier(states.list.Get(),motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,motionState);
        barrier(states.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,outputState);
        states.submit(queue);queue.wait(queue.signal());
    }
    parameters->Set(NVSDK_NGX_Parameter_Color,color.Get());parameters->Set(NVSDK_NGX_Parameter_Depth,depth.Get());
    parameters->Set(NVSDK_NGX_Parameter_MotionVectors,motion.Get());parameters->Set(NVSDK_NGX_Parameter_Output,output.Get());
    parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X,320.f);parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y,320.f);
    parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X,0.f);parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,0.f);
    parameters->Set(NVSDK_NGX_Parameter_Sharpness,0.f);parameters->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,16.6667f);
    parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,320u);parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,320u);
    uint64_t counts[4]={0,0,0,0};using Counts=HRESULT(*)(uint64_t*,uint32_t);Counts getCounts=nullptr;
    for(uint32_t frame=0;frame<400;frame++){
        Commands commands(device.Get());Readback capture(device.Get(),320,320,output.Get());parameters->Set(NVSDK_NGX_Parameter_Reset,frame==0?1:0);
        ngx(evaluate(commands.list.Get(),feature,parameters,nullptr),"Evaluate complete Scaler");
        ID3D12Resource* restored=nullptr;parameters->Get(NVSDK_NGX_Parameter_Output,&restored);require(restored==output.Get(),"Scaler replaced the caller's output parameter");
        if(auditStates){
            probe::expect(color.Get(),colorState);probe::expect(depth.Get(),depthState);probe::expect(motion.Get(),motionState);
            probe::expect(output.Get(),outputState);
            barrier(commands.list.Get(),output.Get(),outputState,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        capture.copy(commands.list.Get(),output.Get());
        if(auditStates)barrier(commands.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,outputState);
        commands.submit(queue);queue.wait(queue.signal());
        if(auto module=GetModuleHandleW(L"helixsr-nr.dll"))getCounts=reinterpret_cast<Counts>(GetProcAddress(module,"HelixSR_NR_DebugCounts"));
        if(getCounts){check(getCounts(counts,4),"Read NR diagnostics");if(counts[0]>=4){save("scaler-nr-output.rgba16f",capture.read());break;}}
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(getCounts&&counts[0]>=4,"The complete Scaler front end never dispatched NR");
    if(auditStates){
        Commands failed(device.Get());parameters->Set(NVSDK_NGX_Parameter_Color,static_cast<ID3D12Resource*>(nullptr));
        require(evaluate(failed.list.Get(),feature,parameters,nullptr)!=NVSDK_NGX_Result_Success,"Missing color input was accepted");
        ID3D12Resource* restored=nullptr;parameters->Get(NVSDK_NGX_Parameter_Output,&restored);
        require(restored==output.Get(),"Failed evaluation left the caller's output redirected");
        probe::expect(color.Get(),colorState);probe::expect(depth.Get(),depthState);
        probe::expect(motion.Get(),motionState);probe::expect(output.Get(),outputState);
        parameters->Set(NVSDK_NGX_Parameter_Color,color.Get());failed.submit(queue);queue.wait(queue.signal());
    }
    // The last command list has been released. A real empty submission collects
    // its recording after the host and backend completion fences have finished.
    Commands final(device.Get());final.submit(queue);queue.wait(queue.signal());check(getCounts(counts,4),"Read final NR diagnostics");
    require(counts[1]>=counts[0]&&counts[2]==counts[0],"Automatic submission/reset retirement did not complete");
    ngx(release(feature),"Release Scaler feature");
    if(auditStates)require(probe::transitionCount>0&&probe::stateErrors==0,"Observed inconsistent texture state transitions");
    std::cout<<"{\"fullScalerNgxPath\":true,\"nrRecords\":"<<counts[0]<<",\"nrSubmissions\":"<<counts[1]<<",\"nrRetirements\":"<<counts[2]<<",\"automaticQueueHooks\":true,\"unrealStateInference\":"<<(unrealStates?"true":"false")<<",\"textureStateAudit\":"<<(auditStates?"true":"false")<<",\"failureRestoresOutputAndStates\":"<<(auditStates?"true":"false")<<",\"textureTransitions\":"<<probe::transitionCount<<",\"textureStateErrors\":"<<probe::stateErrors<<"}"<<std::endl;
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
