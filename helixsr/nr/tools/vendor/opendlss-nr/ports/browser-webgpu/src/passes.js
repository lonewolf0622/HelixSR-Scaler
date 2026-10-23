// Pipelines, tensors, and the recorder the graph builds into.
//
// The Vulkan implementation records the whole network into one command buffer once and replays it every
// frame. This does the same: src/graph.js declares the passes, `Recorder.finish()` packs every pass's
// parameters into one uniform buffer and builds its bind group, and `encode()` then replays them with no
// per-frame allocation. A frame is one compute pass - WebGPU orders dispatches inside a pass and inserts the
// barriers between them, which is exactly the dependency the graph needs and all it needs.
//
// The hand-written kernels share one bind group layout. They do not agree on what each binding means, but
// they do agree on how many there are and which are writable, and one layout keeps the recorder from having
// to know which kernel it is looking at. The two generated kernels - the GEMM and the window attention -
// are recorded through `specialized` instead: every dispatch of them is its own pipeline, because their
// shapes and strides are pipeline overrides rather than uniform reads. The window attention still uses the
// shared layout; the GEMM brings its own binding order.

import { align, compile } from './gpu.js';

/** The shared interface: a dynamic uniform at 0, four read-only storage buffers, three writable, one more read. */
export const BINDING_COUNT = 9;
const KINDS = ['uniform', 'read-only-storage', 'read-only-storage', 'read-only-storage', 'read-only-storage',
               'storage', 'storage', 'storage', 'read-only-storage'];

export const PARAMS_STRIDE = 256;   // minUniformBufferOffsetAlignment; also the largest parameter block

/** The GEMM's own interface: input, weights, output, skip, params, the two lookup tables, the raw half. */
export const GEMM_BINDING_COUNT = 8;
const GEMM_KINDS = ['read-only-storage', 'read-only-storage', 'storage', 'read-only-storage', 'uniform',
                    'read-only-storage', 'read-only-storage', 'storage'];

export class Tensors {
  constructor(device) {
    this.device = device;
    this.byKey = new Map();
    this.total = 0;
    // Placeholders for the bindings a kernel declares but does not use. The writable ones need a buffer each:
    // WebGPU rejects a command buffer in which two writable storage bindings overlap, and it rejects it at
    // encoder.finish(), where nothing is running yet and there is no dispatch to blame.
    this.dummy = device.createBuffer({ label: 'unused', size: 4, usage: GPUBufferUsage.STORAGE });
    this.dummyWritable = [5, 6, 7].map((binding) => device.createBuffer({
      label: `unused ${binding}`, size: 4, usage: GPUBufferUsage.STORAGE,
    }));
    this.dummyGemmWritable = [2, 7].map((binding) => device.createBuffer({
      label: `unused gemm ${binding}`, size: 4, usage: GPUBufferUsage.STORAGE,
    }));
  }

  /**
   * An activation tensor. Rows are padded to a multiple of 64 and the whole allocation is zeroed, because a
   * kernel reading a window or a key block at the end of a tensor reads past the last valid row - and what it
   * reads there has to be zero, not whatever was left behind.
   */
  allocate(label, rows, channels, format) {
    const bytesPerValue = format === 'f32' ? 4 : format === 'f16' ? 2 : 1;
    const allocRows = align(rows, 64);
    const key = `${label}/${rows}x${channels}/${format}`;
    const existing = this.byKey.get(key);
    if (existing) return existing;
    const size = align(allocRows * channels * bytesPerValue, 4);
    const buffer = this.device.createBuffer({
      label: key,
      size,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST,
    });
    const tensor = { label, rows, channels, format, allocRows, buffer, byteLength: size,
                     validBytes: rows * channels * bytesPerValue };
    this.total += size;
    this.byKey.set(key, tensor);
    return tensor;
  }

  destroy() {
    for (const tensor of this.byKey.values()) tensor.buffer.destroy();
    this.byKey.clear();
    this.dummy.destroy();
    for (const buffer of this.dummyWritable) buffer.destroy();
    for (const buffer of this.dummyGemmWritable) buffer.destroy();
  }
}

export class Kernels {
  static async create(device) {
    const kernels = new Kernels();
    kernels.device = device;
    kernels.layout = device.createBindGroupLayout({
      label: 'nr',
      entries: KINDS.map((type, binding) => ({
        binding,
        visibility: GPUShaderStage.COMPUTE,
        buffer: binding === 0 ? { type, hasDynamicOffset: true, minBindingSize: PARAMS_STRIDE } : { type },
      })),
    });
    kernels.pipelineLayout = device.createPipelineLayout({ bindGroupLayouts: [kernels.layout] });
    kernels.gemmLayout = device.createBindGroupLayout({
      label: 'gemm',
      entries: GEMM_KINDS.map((type, binding) => ({
        binding,
        visibility: GPUShaderStage.COMPUTE,
        buffer: binding === 4 ? { type, hasDynamicOffset: true, minBindingSize: PARAMS_STRIDE } : { type },
      })),
    });
    kernels.gemmPipelineLayout = device.createPipelineLayout({ bindGroupLayouts: [kernels.gemmLayout] });
    return kernels;
  }

  /** Compile one WGSL file with numerics.wgsl in front of it and make a pipeline per entry point. */
  async add(numerics, source, name, entryPoints, constants) {
    const module = await compile(this.device, `${numerics}\n${source}`, name);
    this.pipelines = this.pipelines ?? new Map();
    for (const entryPoint of entryPoints) {
      this.pipelines.set(entryPoint, await this.device.createComputePipelineAsync({
        label: entryPoint,
        layout: this.pipelineLayout,
        compute: { module, entryPoint, constants },
      }));
    }
  }

  pipeline(name) {
    const found = this.pipelines?.get(name);
    if (!found) throw new Error(`no pipeline ${name}`);
    return found;
  }
}

export class Recorder {
  constructor(device, kernels, tensors) {
    this.device = device;
    this.kernels = kernels;
    this.tensors = tensors;
    this.passes = [];
    this.params = [];
    this.finished = false;
  }

  /**
   * Record one dispatch. `buffers` is sparse - an object keyed by binding index - because no kernel uses all
   * nine, and naming the ones it does use reads better than a list of nulls.
   */
  pass(entryPoint, buffers, params, workgroups, label) {
    if (this.finished) throw new Error('the graph is already recorded');
    const [x, y = 1, z = 1] = Array.isArray(workgroups) ? workgroups : [workgroups];
    if (x > 65535 || y > 65535 || z > 65535) throw new Error(`dispatch ${label} exceeds 65535 groups`);
    if (params.length * 4 > PARAMS_STRIDE) throw new Error(`parameter block of ${label} is too large`);
    this.passes.push({ kind: 'dispatch', entryPoint, buffers, index: this.params.length, x, y, z, label });
    this.params.push(params);
    return this;
  }

  /**
   * Record a dispatch of a specialized kernel - one whose shape is a pipeline override rather than a uniform,
   * so there is no shared pipeline to look up. `pipeline` is a promise, which `finish` waits on. `kernel`
   * names which kernel it is, for the profile; `gemm` says which of the two binding interfaces it uses.
   */
  specialized(pipeline, { kernel, gemm = false }, buffers, params, workgroups, label) {
    if (this.finished) throw new Error('the graph is already recorded');
    const [x, y = 1, z = 1] = workgroups;
    if (x > 65535 || y > 65535 || z > 65535) throw new Error(`dispatch ${label} exceeds 65535 groups`);
    if (params.length * 4 > PARAMS_STRIDE) throw new Error(`parameter block of ${label} is too large`);
    this.passes.push({ kind: 'dispatch', specialized: true, gemm, entryPoint: kernel, pipeline, buffers,
                       index: this.params.length, x, y, z, label });
    this.params.push(params);
    return this;
  }

  /**
   * Copy a tensor as it stands at this point in the graph. A copy cannot happen inside a compute pass, so
   * recording one splits the pass - which is the point: the graph reuses two buffers per stage, so a copy
   * taken at the end would show whichever block wrote last, not the one being captured.
   */
  copy(from, to, byteLength, label) {
    if (this.finished) throw new Error('the graph is already recorded');
    this.passes.push({ kind: 'copy', from, to, byteLength, label });
    return this;
  }

  /**
   * `onProgress` is called while the specialized pipelines compile. That is the long wait at startup - a few
   * hundred of them - and the only part of bringing the graph up that is worth a progress count.
   */
  async finish(onProgress = null) {
    const stride = PARAMS_STRIDE / 4;
    const words = new Uint32Array(Math.max(1, this.params.length) * stride);
    this.params.forEach((block, i) => words.set(block, i * stride));
    this.paramsBuffer = this.device.createBuffer({
      label: 'graph parameters',
      size: Math.max(PARAMS_STRIDE, words.byteLength),
      usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
    });
    this.device.queue.writeBuffer(this.paramsBuffer, 0, words);

    const specialized = this.passes.filter((item) => item.specialized).length;
    let compiled = 0;
    for (const pass of this.passes) {
      if (pass.kind !== 'dispatch') continue;
      if (pass.specialized) {
        pass.pipeline = await pass.pipeline;
        compiled += 1;
        if (onProgress && (compiled === 1 || compiled % 16 === 0)) onProgress(compiled, specialized);
        const uniform = pass.gemm ? 4 : 0;
        pass.bindGroup = this.device.createBindGroup({
          layout: pass.gemm ? this.kernels.gemmLayout : this.kernels.layout,
          entries: Array.from({ length: pass.gemm ? GEMM_BINDING_COUNT : BINDING_COUNT },
            (unused, binding) => ({
              binding,
              resource: binding === uniform
                ? { buffer: this.paramsBuffer, offset: 0, size: PARAMS_STRIDE }
                : { buffer: pass.buffers[binding]
                            ?? (pass.gemm ? this.unusedGemm(binding) : this.unused(binding)) },
            })),
        });
        continue;
      }
      pass.pipeline = this.kernels.pipeline(pass.entryPoint);
      pass.bindGroup = this.device.createBindGroup({
        layout: this.kernels.layout,
        entries: Array.from({ length: BINDING_COUNT }, (unused, binding) => ({
          binding,
          resource: binding === 0
            ? { buffer: this.paramsBuffer, offset: 0, size: PARAMS_STRIDE }
            : { buffer: pass.buffers[binding] ?? this.unused(binding) },
        })),
      });
    }
    this.finished = true;
    return this;
  }

  unused(binding) {
    return binding >= 5 && binding <= 7 ? this.tensors.dummyWritable[binding - 5] : this.tensors.dummy;
  }

  unusedGemm(binding) {
    if (binding === 2) return this.tensors.dummyGemmWritable[0];
    if (binding === 7) return this.tensors.dummyGemmWritable[1];
    return this.tensors.dummy;
  }

  /**
   * Time every dispatch, grouped by kernel. Each pass gets its own compute pass so the timestamps bracket one
   * dispatch, which costs a little in pass overhead and tells you where the frame actually goes.
   */
  enableProfiling() {
    if (!this.device.features.has('timestamp-query')) return false;
    const count = this.passes.filter((item) => item.kind === 'dispatch').length * 2;
    this.querySet = this.device.createQuerySet({ type: 'timestamp', count });
    this.queryResolve = this.device.createBuffer({ size: count * 8,
      usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC });
    this.queryRead = this.device.createBuffer({ size: count * 8,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    this.profiling = true;
    return true;
  }

  /** Nanoseconds per kernel, summed over the frame. */
  async readProfile() {
    if (!this.profiling) return null;
    await this.queryRead.mapAsync(GPUMapMode.READ);
    const stamps = new BigUint64Array(this.queryRead.getMappedRange().slice(0));
    this.queryRead.unmap();
    const totals = new Map();
    let index = 0;
    for (const item of this.passes) {
      if (item.kind !== 'dispatch') continue;
      const nanoseconds = Number(stamps[index * 2 + 1] - stamps[index * 2]);
      totals.set(item.entryPoint, (totals.get(item.entryPoint) ?? 0) + nanoseconds);
      index += 1;
    }
    return [...totals.entries()].map(([name, ns]) => ({ name, milliseconds: ns / 1e6 }))
      .sort((a, b) => b.milliseconds - a.milliseconds);
  }

  encode(encoder) {
    if (this.profiling) return this.encodeProfiled(encoder);
    let pass = null;
    for (const item of this.passes) {
      if (item.kind === 'copy') {
        if (pass) { pass.end(); pass = null; }
        encoder.copyBufferToBuffer(item.from, 0, item.to, 0, item.byteLength);
        continue;
      }
      if (!pass) pass = encoder.beginComputePass({ label: 'nr' });
      pass.setPipeline(item.pipeline);
      pass.setBindGroup(0, item.bindGroup, [item.index * PARAMS_STRIDE]);
      pass.dispatchWorkgroups(item.x, item.y, item.z);
    }
    if (pass) pass.end();
  }

  encodeProfiled(encoder) {
    let index = 0;
    for (const item of this.passes) {
      if (item.kind === 'copy') {
        encoder.copyBufferToBuffer(item.from, 0, item.to, 0, item.byteLength);
        continue;
      }
      const pass = encoder.beginComputePass({ label: item.label,
        timestampWrites: { querySet: this.querySet, beginningOfPassWriteIndex: index * 2,
                           endOfPassWriteIndex: index * 2 + 1 } });
      pass.setPipeline(item.pipeline);
      pass.setBindGroup(0, item.bindGroup, [item.index * PARAMS_STRIDE]);
      pass.dispatchWorkgroups(item.x, item.y, item.z);
      pass.end();
      index += 1;
    }
    encoder.resolveQuerySet(this.querySet, 0, index * 2, this.queryResolve, 0);
    encoder.copyBufferToBuffer(this.queryResolve, 0, this.queryRead, 0, index * 8 * 2);
  }

  get dispatchCount() { return this.passes.filter((item) => item.kind === 'dispatch').length; }
}
