// CPU-only preparation of a D3D12 graph from pinned MIT graph declarations.
// This recorder does not execute or validate neural shaders. Naga/DXC and GPU
// validation remain separate steps. Generated model data stays private.
import {readFile,writeFile,mkdir,open} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {pathToFileURL} from 'node:url';
import {f16Bits,f16ToNumber,mpCubicSilu,e4m3FromNumber,e4m3ToNumber}
  from './vendor/opendlss-nr/ports/browser-webgpu/src/numerics.js';

const width=Number(process.argv[2]??320),height=Number(process.argv[3]??320);
if(!Number.isInteger(width)||!Number.isInteger(height)||width<33||height<33||width>4096||height>4096)
  throw new Error('Invalid network dimensions');
if(process.argv.length!==6&&process.argv.length!==7)throw new Error('Expected width height model-directory output-directory [native|compact]');
const fieldMode=process.argv[6]??'native';
if(fieldMode!=='native'&&fieldMode!=='compact')throw new Error('Invalid field mode');
const model=pathToFileURL(process.argv[4].replace(/\\/g,'/')+'/');
const output=pathToFileURL(process.argv[5].replace(/\\/g,'/')+'/');
await mkdir(new URL('wgsl/',output),{recursive:true});
Object.assign(globalThis,{
  GPUBufferUsage:{MAP_READ:1,MAP_WRITE:2,COPY_SRC:4,COPY_DST:8,INDEX:16,VERTEX:32,UNIFORM:64,STORAGE:128,INDIRECT:256,QUERY_RESOLVE:512},
  GPUShaderStage:{VERTEX:1,FRAGMENT:2,COMPUTE:4},GPUMapMode:{READ:1,WRITE:2},
});
const originalFetch=globalThis.fetch;
globalThis.fetch=async(input,options)=>{
  const url=input instanceof URL?input:new URL(input);
  if(url.protocol!=='file:')return originalFetch(input,options);
  try{return new Response(await readFile(url));}
  catch(error){if(error.code==='ENOENT')return new Response('',{status:404});throw error;}
};
const buffers=[],pipelines=[];
class CpuBuffer {
  constructor(desc){
    if(!Number.isSafeInteger(desc.size)||desc.size<1||desc.size>1024**3)throw new Error('Invalid buffer');
    this.id=buffers.length;this.size=desc.size;this.label=desc.label??'';this.data=null;this.destroyed=false;
    buffers.push(this);if(desc.mappedAtCreation)this.getMappedRange();
  }
  ensureData(){return this.data??=new ArrayBuffer(this.size);}
  getMappedRange(offset=0,size=this.size-offset){
    if(offset!==0||size!==this.size)throw new Error('Partial mapped ranges are not supported by offline preparation');
    return this.ensureData();
  }
  unmap(){}
  destroy(){this.destroyed=true;}
}
function writeBuffer(buffer,offset,data,dataOffset=0,size){
  const view=ArrayBuffer.isView(data),element=view?(data.BYTES_PER_ELEMENT??1):1;
  const sourceLength=view?data.byteLength:data.byteLength;
  const begin=dataOffset*element,length=size===undefined?sourceLength-begin:size*element;
  if(!Number.isSafeInteger(offset)||offset<0||begin<0||length<0||offset+length>buffer.size||begin+length>sourceLength)
    throw new Error('Invalid CPU buffer write');
  const source=new Uint8Array(view?data.buffer:data,(view?data.byteOffset:0)+begin,length);
  new Uint8Array(buffer.ensureData(),offset,length).set(source);
}
function buildTable(command){
  const {pipeline,group}=command,label=pipeline.compute.module.label;
  const binding=index=>group.entries.find(e=>e.binding===index)?.resource.buffer;
  if(label==='NR native SiLU table builder'){
    const data=new Uint16Array(65536*2);
    for(let i=0;i<65536;i++){
      const activated=mpCubicSilu(f16ToNumber(i));
      const code=activated===0?0:e4m3FromNumber(activated);
      data[2*i]=f16Bits(activated);data[2*i+1]=f16Bits(e4m3ToNumber(code));
    }
    writeBuffer(binding(0),0,data);
  }else if(label==='NR packed E4 SiLU table builder'){
    const source=new Uint16Array(binding(0).ensureData()),packed=new Uint8Array(65536);
    for(let i=0;i<65536;i++)packed[i]=e4m3FromNumber(f16ToNumber(source[2*i+1]));
    writeBuffer(binding(1),0,packed);
  }else throw new Error(`Offline recorder cannot execute shader ${label}`);
}
class CpuEncoder {
  commands=[];
  beginComputePass(){
    let pipeline,group;
    return {setPipeline:value=>{pipeline=value;},setBindGroup:(index,value)=>{
      if(index!==0)throw new Error('Unexpected bind group index');group=value;
    },dispatchWorkgroups:(x,y=1,z=1)=>this.commands.push({kind:'dispatch',pipeline,group,x,y,z}),end:()=>{}};
  }
  copyBufferToBuffer(source,sourceOffset,destination,destinationOffset,size){
    this.commands.push({kind:'copy',source,sourceOffset,destination,destinationOffset,size});
  }
  clearBuffer(buffer,offset=0,size=buffer.size-offset){this.commands.push({kind:'clear',buffer,offset,size});}
  finish(){return this.commands;}
}
const device={
  limits:{maxComputeWorkgroupStorageSize:32768},features:new Set(['shader-f16']),
  createBuffer:desc=>new CpuBuffer(desc),
  createShaderModule:desc=>({...desc,hash:createHash('sha256').update(desc.code).digest('hex'),
    getCompilationInfo:async()=>({messages:[]})}),
  createBindGroupLayout:desc=>desc,
  createPipelineLayout:desc=>desc,
  createBindGroup:desc=>desc,
  createComputePipelineAsync:async desc=>{
    const pipeline={...desc,id:pipelines.length,getBindGroupLayout:index=>{
      if(index!==0)throw new Error('Unsupported pipeline group');
      return desc.layout==='auto'?{}:desc.layout.bindGroupLayouts[index];
    }};pipelines.push(pipeline);return pipeline;
  },
  createCommandEncoder:()=>new CpuEncoder(),
  destroy:()=>{},
  pushErrorScope:()=>{},popErrorScope:async()=>null,
  queue:{writeBuffer,onSubmittedWorkDone:async()=>{},submit:submissions=>{
    for(const commands of submissions)for(const command of commands){
      if(command.kind==='dispatch')buildTable(command);
      else if(command.kind==='copy')writeBuffer(command.destination,command.destinationOffset,
        command.source.ensureData(),command.sourceOffset,command.size);
      else if(command.kind==='clear'){
        if(command.buffer.data)new Uint8Array(command.buffer.data,command.offset,command.size).fill(0);
      }else throw new Error('Unknown offline preparation command');
    }
  }},
};
const {Network}=await import('./vendor/opendlss-nr/ports/browser-webgpu/src/network.js');
const network=await Network.create({weights:model.href,
  width,height,device,minimumField:fieldMode==='compact'?128:320});
const referenced=new Set(),usedPipelines=new Map();
const dispatches=network.recorder.passes.map(pass=>{
  if(pass.kind!=='dispatch')throw new Error('Unexpected graph copy');
  const pipeline=pass.pipeline,group=pass.bindGroup;
  if(pipeline.layout==='auto')throw new Error('Neural graph must have explicit layouts');
  usedPipelines.set(pipeline.id,pipeline);
  const bindings=group.entries.map(entry=>{
    const descriptor=group.layout.entries.find(item=>item.binding===entry.binding);
    const buffer=entry.resource.buffer;
    if(!descriptor||buffer.destroyed)throw new Error('Invalid graph resource');
    referenced.add(buffer.id);
    const offset=(entry.resource.offset??0)+(descriptor.buffer.hasDynamicOffset?pass.index*256:0);
    return {binding:entry.binding,buffer:buffer.id,offset,bytes:entry.resource.size??buffer.size-offset,
      type:descriptor.buffer.type};
  });
  return {pipeline:pipeline.id,label:pass.label,groups:[pass.x,pass.y,pass.z],bindings};
});
referenced.add(network.graph.head.buffer.id);
const resources=[],binary=await open(new URL('initial-data.bin',output),'w');let cursor=0;
try{
  for(const id of referenced){
    const buffer=buffers[id],data=buffer.data?new Uint8Array(buffer.data):null;
    resources.push({id,label:buffer.label,bytes:buffer.size,dataOffset:cursor,dataBytes:data?.byteLength??0});
    if(data){await binary.write(data);cursor+=data.byteLength;}
  }
}finally{await binary.close();}
const definitions=[];
for(const pipeline of usedPipelines.values()){
  const module=pipeline.compute.module;
  await writeFile(new URL(`wgsl/${module.hash}.wgsl`,output),module.code);
  definitions.push({id:pipeline.id,shader:`wgsl/${module.hash}.wgsl`,entryPoint:pipeline.compute.entryPoint,
    constants:pipeline.compute.constants??{}});
}
const plan={format:'nr-d3d12-offline-1',width,height,fieldMode,geometry:network.geometry,
  referenceCommit:'9d08f4184bbcb9d858e2fb7a7834ec0837a9d2f1',
  inputBuffer:network.features.buffer.id,outputBuffer:network.graph.head.buffer.id,
  outputBytes:network.graph.head.validBytes,buffers:resources,pipelines:definitions,dispatches};
await writeFile(new URL('plan.json',output),JSON.stringify(plan));
console.log(JSON.stringify({width,height,fieldWidth:network.geometry.fullWidth,fieldHeight:network.geometry.fullHeight,
  buffers:resources.length,pipelines:definitions.length,dispatches:dispatches.length,staticBytes:cursor,
  gpuUsed:false,neuralExecutionValidated:false}));
network.destroy();
