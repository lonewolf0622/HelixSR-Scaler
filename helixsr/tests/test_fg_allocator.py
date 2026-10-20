#!/usr/bin/env python3
"""CPU mock execution of the production allocator fence wait/submit methods."""
from pathlib import Path
import subprocess, tempfile, os
root = Path(__file__).resolve().parents[2]
src = (root/'OptiScaler/framegen/IFGFeature_Dx12.cpp').read_text()
a=src.index('bool IFGFeature_Dx12::WaitForUIAllocator'); b=src.index('ID3D12GraphicsCommandList* IFGFeature_Dx12::GetUICommandList',a)
methods=src[a:b]
code=r'''
#include <cstdint>
#include <cassert>
#include <cstddef>
using UINT=unsigned; using UINT64=uint64_t; using HANDLE=void*;
#define BUFFER_COUNT 4
#define FAILED(x) ((x)<0)
#define LOG_ERROR(...)
#define LOG_DEBUG(...)
#define WAIT_OBJECT_0 0
unsigned waitResult=0, waits=0; uint64_t* completion=nullptr; uint64_t completeTo=0;
unsigned WaitForSingleObject(HANDLE,unsigned timeout) {assert(timeout==5000); ++waits; if (completion && waitResult==0) *completion=completeTo; return waitResult;}
struct Fence {uint64_t completed=0; int eventResult=0; uint64_t requested=0;
uint64_t GetCompletedValue(){return completed;} int SetEventOnCompletion(uint64_t v,HANDLE){requested=v;return eventResult;}};
struct List {int closeResult=0; int Close(){return closeResult;}};
using ID3D12CommandList=List;
struct Queue {unsigned executes=0; int signalResult=0; uint64_t lastSignal=0;
void ExecuteCommandLists(unsigned,List**){++executes;} int Signal(Fence*,uint64_t v){lastSignal=v; return signalResult;}};
struct IFGFeature_Dx12 {
Fence *_uiFence=nullptr,*_scFence=nullptr; HANDLE _uiFenceEvent=nullptr,_scFenceEvent=nullptr;
uint64_t _uiAllocatorFenceValues[4]={},_scAllocatorFenceValues[4]={},_uiFenceValue=0;
bool _uiFenceOnSubmission=true; bool _uiCommandListResetted[4]={}; List* _uiCommandList[4]={}; Queue* _gameCommandQueue=nullptr;
bool WaitForUIAllocator(UINT); bool WaitForSCAllocator(UINT); bool SubmitUICommandList(UINT);
};
'''+methods+r'''
int main(){
IFGFeature_Dx12 f; Fence fence; Queue queue; List list;
f._uiFence=f._scFence=&fence; f._uiFenceEvent=f._scFenceEvent=(void*)1;
assert(f.WaitForUIAllocator(0)); assert(waits==0);
f._uiAllocatorFenceValues[0]=7; fence.completed=7; assert(f.WaitForUIAllocator(0)); assert(waits==0);
completion=&fence.completed; completeTo=7; fence.completed=6; assert(f.WaitForUIAllocator(0)); assert(waits==1 && fence.requested==7);
fence.completed=6; waitResult=258; assert(!f.WaitForUIAllocator(0));
fence.eventResult=-1; assert(!f.WaitForUIAllocator(0));
fence.eventResult=0; fence.completed=UINT64_MAX; assert(!f.WaitForUIAllocator(0));
fence.completed=0; f._scAllocatorFenceValues[1]=4; assert(!f.WaitForSCAllocator(1));
waitResult=0; completeTo=3; assert(!f.WaitForSCAllocator(1)); // stale event cannot authorize reset
completeTo=4; assert(f.WaitForSCAllocator(1));
f._gameCommandQueue=&queue; f._uiCommandList[0]=&list; f._uiCommandListResetted[0]=true;
assert(f.SubmitUICommandList(0)); assert(queue.executes==1 && queue.lastSignal==1 && !f._uiCommandListResetted[0]);
f._uiCommandList[1]=&list; f._uiCommandListResetted[1]=true; assert(f.SubmitUICommandList(1)); assert(queue.lastSignal==2);
f._uiCommandListResetted[0]=true; queue.signalResult=-1; assert(!f.SubmitUICommandList(0)); assert(f._uiAllocatorFenceValues[0]==UINT64_MAX); assert(!f.WaitForUIAllocator(0));
f._uiCommandListResetted[1]=true; list.closeResult=-1; assert(!f.SubmitUICommandList(1)); assert(queue.executes==3);
}
'''
with tempfile.TemporaryDirectory(prefix='helixsr-allocator-',dir=Path.home()) as tmp:
 p=Path(tmp); (p/'test.cpp').write_text(code)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: production allocator methods: completion, timeout, removed device, monotonic submit, failure retirement')
