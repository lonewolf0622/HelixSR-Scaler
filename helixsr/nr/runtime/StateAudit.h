#pragma once
// SPDX-License-Identifier: MIT
// Probe-only observation of real D3D12 calls, independent of Scaler's tracker.
#include <detours/detours.h>
#include <map>
#include <mutex>
namespace probe {
using Barriers = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,const D3D12_RESOURCE_BARRIER*);
inline Barriers originalBarriers = nullptr;
inline std::mutex auditMutex;
inline std::map<ID3D12Resource*,D3D12_RESOURCE_STATES> textureStates;
inline uint64_t transitionCount = 0, stateErrors = 0;
inline void seed(ID3D12Resource* resource,D3D12_RESOURCE_STATES state){
    std::lock_guard lock(auditMutex);textureStates[resource]=state;
}
inline void STDMETHODCALLTYPE observe(ID3D12GraphicsCommandList* list,UINT count,const D3D12_RESOURCE_BARRIER* barriers){
    {
        std::lock_guard lock(auditMutex);
        for(UINT i=0;i<count;i++){
            const auto& b=barriers[i];
            if(b.Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){
                textureStates.erase(b.Aliasing.pResourceBefore);textureStates.erase(b.Aliasing.pResourceAfter);continue;
            }
            if(b.Type!=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION||b.Flags!=D3D12_RESOURCE_BARRIER_FLAG_NONE)continue;
            const auto& t=b.Transition;const auto d=t.pResource->GetDesc();
            if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.MipLevels!=1||d.DepthOrArraySize!=1||
               (d.Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))continue;
            auto [state,inserted]=textureStates.try_emplace(t.pResource,t.StateBefore);
            if(state->second!=t.StateBefore){
                if(stateErrors++<8)std::cerr<<"Texture state mismatch: expected "<<unsigned(state->second)
                    <<", barrier says "<<unsigned(t.StateBefore)<<" -> "<<unsigned(t.StateAfter)<<std::endl;
            }
            state->second=t.StateAfter;++transitionCount;
        }
    }
    originalBarriers(list,count,barriers);
}
inline void install(ID3D12Device* device){
    Commands commands(device);check(commands.list->Close(),"Close audit list");
    originalBarriers=reinterpret_cast<Barriers>((*reinterpret_cast<void***>(commands.list.Get()))[26]);
    require(DetourTransactionBegin()==NO_ERROR,"Begin audit hook transaction");
    require(DetourUpdateThread(GetCurrentThread())==NO_ERROR,"Register audit thread");
    require(DetourAttach(reinterpret_cast<PVOID*>(&originalBarriers),reinterpret_cast<PVOID>(&observe))==NO_ERROR,"Attach texture state observer");
    require(DetourTransactionCommit()==NO_ERROR,"Commit texture state observer");
}
inline void expect(ID3D12Resource* resource,D3D12_RESOURCE_STATES expected){
    std::lock_guard lock(auditMutex);const auto state=textureStates.find(resource);
    require(state!=textureStates.end()&&state->second==expected,"Scaler did not restore a caller texture's state");
}
}
