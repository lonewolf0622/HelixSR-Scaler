// The thin layer over WebGPU that the rest of the port sits on: getting a device, moving bytes on and off it,
// and running a compute pass. Nothing here knows anything about the network.

/**
 * Ask for a device, preferring the discrete adapter and asking for the largest buffers the implementation
 * will give. `shader-f16` is requested when available but never required - the arithmetic is written on f32
 * bit patterns precisely so that it does not depend on it.
 */
export async function requestDevice({ requiredLimits = {} } = {}) {
  if (!navigator.gpu) throw new Error('this browser has no WebGPU (navigator.gpu is undefined)');
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) throw new Error('no WebGPU adapter; on a laptop, check that the discrete GPU is selected');

  // Raise every limit the port cares about to whatever the adapter allows. Asking for more than the default
  // is the only way to bind a tensor larger than 128 MB or to use a full 32 KB of workgroup storage.
  const wanted = {
    maxStorageBufferBindingSize: adapter.limits.maxStorageBufferBindingSize,
    maxBufferSize: adapter.limits.maxBufferSize,
    maxComputeWorkgroupStorageSize: adapter.limits.maxComputeWorkgroupStorageSize,
    maxComputeInvocationsPerWorkgroup: adapter.limits.maxComputeInvocationsPerWorkgroup,
    maxComputeWorkgroupSizeX: adapter.limits.maxComputeWorkgroupSizeX,
    maxStorageBuffersPerShaderStage: adapter.limits.maxStorageBuffersPerShaderStage,
    ...requiredLimits,
  };
  const requiredFeatures = ['shader-f16', 'timestamp-query'].filter((f) => adapter.features.has(f));
  const device = await adapter.requestDevice({ requiredFeatures, requiredLimits: wanted });
  device.lost.then((info) => {
    if (info.reason !== 'destroyed') console.error(`WebGPU device lost: ${info.reason} ${info.message}`);
  });
  // A validation failure does not throw: the offending call becomes a no-op and the buffer it should have
  // written stays as it was. That reads as "the kernel computed zeros", which is a slow way to find a typo.
  device.addEventListener('uncapturederror', (event) => {
    console.error(`WebGPU ${event.error.constructor.name}: ${event.error.message}`);
  });
  return { adapter, device, info: adapter.info ?? {} };
}

/**
 * An explicit bind group layout over storage buffers, given their types in binding order:
 * 'read-only-storage', 'storage' or 'uniform'.
 *
 * The port never uses `layout: 'auto'`. An automatic layout is derived from what an entry point actually
 * reads, so a binding a particular kernel happens not to touch is dropped from it, and a bind group built for
 * the full set is then rejected. Several entry points sharing one declared interface is the normal case here.
 */
export function bindLayout(device, kinds, label) {
  return device.createBindGroupLayout({
    label,
    entries: kinds.map((type, binding) => ({
      binding,
      visibility: GPUShaderStage.COMPUTE,
      buffer: { type },
    })),
  });
}

/** A storage buffer holding `data`, ready to read in a shader. */
export function storageFrom(device, data, label) {
  const buffer = device.createBuffer({
    label,
    size: Math.max(4, align(data.byteLength, 4)),
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC,
  });
  device.queue.writeBuffer(buffer, 0, data);
  return buffer;
}

/** An empty storage buffer a shader can write. */
export function storage(device, byteLength, label) {
  return device.createBuffer({
    label,
    size: Math.max(4, align(byteLength, 4)),
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC,
  });
}

export const align = (value, to) => Math.ceil(value / to) * to;

/** Copy a device buffer back to an ArrayBuffer. Allocates a staging buffer per call; fine for tests. */
export async function readBack(device, buffer, byteLength) {
  const size = align(byteLength, 4);
  const staging = device.createBuffer({
    size,
    usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
  });
  const encoder = device.createCommandEncoder();
  encoder.copyBufferToBuffer(buffer, 0, staging, 0, size);
  device.queue.submit([encoder.finish()]);
  await staging.mapAsync(GPUMapMode.READ);
  const copy = staging.getMappedRange().slice(0, byteLength);
  staging.unmap();
  staging.destroy();
  return copy;
}

/**
 * Compile one WGSL module and report its diagnostics. Tint's messages carry line numbers into the
 * concatenated source, so the caller passes the assembled text and gets it back for context.
 */
export async function compile(device, code, label) {
  const module = device.createShaderModule({ code, label });
  const info = await module.getCompilationInfo();
  const errors = info.messages.filter((m) => m.type === 'error');
  for (const message of info.messages) {
    const where = `${label}:${message.lineNum}:${message.linePos}`;
    const line = code.split('\n')[message.lineNum - 1] ?? '';
    console[message.type === 'error' ? 'error' : 'warn'](`${where} ${message.message}\n    ${line.trim()}`);
  }
  if (errors.length) throw new Error(`${label} failed to compile: ${errors[0].message}`);
  return module;
}

/** Run one entry point over `workgroups` groups with the given buffers bound in order. */
export function dispatch(device, pipeline, layout, buffers, workgroups, label) {
  const bindGroup = device.createBindGroup({
    layout,
    entries: buffers.map((buffer, binding) => ({ binding, resource: { buffer } })),
  });
  const encoder = device.createCommandEncoder({ label });
  const pass = encoder.beginComputePass({ label });
  pass.setPipeline(pipeline);
  pass.setBindGroup(0, bindGroup);
  pass.dispatchWorkgroups(workgroups);
  pass.end();
  device.queue.submit([encoder.finish()]);
}
