// Putting the port together: a device, the weights, the kernels, and a recorded graph that can be replayed.
//
// Everything expensive happens once. The weights are fetched and re-laid out at load, the graph is recorded
// into a fixed list of dispatches for a given resolution, and a frame is then one command buffer.

import { requestDevice, readBack } from './gpu.js';
import { Kernels, Tensors, Recorder, BINDING_COUNT, PARAMS_STRIDE } from './passes.js';
import { Model } from './model.js';
import { Graph } from './graph.js';
import { geometryFromValid } from './geometry.js';
import { Matmul } from './matmul/index.js';
import { WindowAttention, requireWorkgroupStorage } from './window/index.js';

// Where the WGSL lives. Module-relative by default, resolved lazily: a bundler that emits anything other
// than a module leaves `import.meta.url` empty, and a bundled page passes `shaderBase` instead.
const defaultBase = () => new URL('..', import.meta.url);
const fetchText = async (path, base) => {
  const response = await fetch(new URL(path, base));
  if (!response.ok) throw new Error(`cannot read ${path}`);
  return response.text();
};

export class Network {
  /**
   * @param {object} options
   *   weights   - URL of the model directory (manifest.json plus model/stages/*)
   *   width,height - the valid image size; the padded field follows from it
   *   captureBoundaries - keep a copy of every block output, which the parity harness compares
   */
  static async create({ weights, width, height, captureBoundaries = false, onProgress,
                        extraShaders = [], before, after, shaderBase = null,
                        device: existingDevice = null, model: existingModel = null,
                        minimumField = 320 } = {}) {
    const network = new Network();
    // A host that already owns a device and the weights passes them in; the pages that stand alone do not.
    let device = existingDevice;
    if (!device) {
      onProgress?.('requesting a device');
      const acquired = await requestDevice();
      device = acquired.device;
      network.adapterInfo = acquired.info;
    }
    network.device = device;
    requireWorkgroupStorage(device);

    network.geometry = geometryFromValid(width, height, minimumField);
    const geometry = network.geometry;

    onProgress?.('compiling kernels');
    const base = shaderBase ?? defaultBase();
    const numerics = await fetchText('shaders/numerics.wgsl', base);
    const [gemmF16, vit, ops, preprocess] = await Promise.all([
      fetchText('shaders/gemm_f16.wgsl', base), fetchText('shaders/vit.wgsl', base),
      fetchText('shaders/ops.wgsl', base), fetchText('shaders/preprocess.wgsl', base),
    ]);
    const kernels = await Kernels.create(device);
    await kernels.add(numerics, gemmF16, 'gemm_f16.wgsl', ['gemm_f16']);
    // The ViT's shared arrays are sized by the padded token count, which only the geometry knows.
    await kernels.add(numerics, vit, 'vit.wgsl', ['vit_normalize', 'vit_attend'],
                      { PADDED_TOKENS: geometry.paddedVitTokens });
    await kernels.add(numerics, ops, 'ops.wgsl',
                      ['convert_f32_to_f16', 'downsample', 'upsample_residual', 'post_blend']);
    await kernels.add(numerics, preprocess, 'preprocess.wgsl', ['preprocess']);
    for (const { path, entryPoints } of extraShaders) {
      await kernels.add(numerics, await fetchText(path, base), path, entryPoints);
    }
    network.kernels = kernels;
    network.matmul = await Matmul.create(device);
    network.window = WindowAttention.create(device, numerics);

    if (existingModel) {
      network.model = existingModel;
      network.borrowedModel = true;
    } else {
      onProgress?.('loading weights');
      network.model = await new Model(device).load(weights, (loaded, total) => {
        onProgress?.(`loading weights ${(loaded / 1048576).toFixed(0)} / ${(total / 1048576).toFixed(0)} MiB`);
      });
    }

    onProgress?.('recording the graph');
    network.tensors = new Tensors(device);
    network.features = network.tensors.allocate('input features', geometry.fullRows, 16, 'f32');
    network.graph = new Graph({
      device, kernels, matmul: network.matmul, window: network.window,
      tensors: network.tensors, model: network.model, geometry,
    }, { captureBoundaries });
    network.recorder = new Recorder(device, kernels, network.tensors);
    before?.(network);
    network.graph.record(network.recorder, network.features);
    after?.(network);
    await network.recorder.finish((done, count) => onProgress?.(`compiling kernels ${done}/${count}`));
    onProgress?.(`ready: ${network.recorder.dispatchCount} dispatches, ` +
                 `${(network.tensors.total / 1048576).toFixed(0)} MiB of activations, ` +
                 `${(network.model.bytesUploaded / 1048576).toFixed(0)} MiB of weights`);
    return network;
  }

  /**
   * Generate the input features from a recorded display proxy, the way a fixture that carries one wants. One
   * dispatch outside the recorded graph, so it runs and completes before the graph replays.
   */
  async featuresFromProxy(proxy, manifest) {
    const g = this.geometry;
    const [sourceWidth, sourceHeight] = [manifest.proxy.width, manifest.proxy.height];
    const conditioning = manifest.conditioning ?? {};
    const source = this.device.createBuffer({ label: 'fixture proxy', size: proxy.byteLength,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
    this.device.queue.writeBuffer(source, 0, proxy);
    const params = new ArrayBuffer(PARAMS_STRIDE);
    const words = new Uint32Array(params);
    const floats = new Float32Array(params);
    words[0] = g.fullWidth; words[1] = g.fullHeight; words[2] = g.validWidth; words[3] = g.validHeight;
    words[4] = sourceWidth; words[5] = sourceHeight; words[6] = manifest.seed ?? 0;
    floats[7] = manifest.autoMask ? 1 : -1;
    floats[8] = conditioning.localTone ?? 1;
    floats[9] = conditioning.localStructure ?? 1;
    floats[10] = conditioning.skinStructure ?? -1;
    floats[11] = conditioning.style ?? 0;
    const uniform = this.device.createBuffer({ label: 'preprocess parameters', size: PARAMS_STRIDE,
      usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    this.device.queue.writeBuffer(uniform, 0, params);
    const group = this.device.createBindGroup({
      layout: this.kernels.layout,
      entries: Array.from({ length: BINDING_COUNT }, (unused, binding) => ({
        binding,
        resource: binding === 0 ? { buffer: uniform, offset: 0, size: PARAMS_STRIDE }
                : binding === 1 ? { buffer: source }
                : binding === 5 ? { buffer: this.features.buffer }
                : { buffer: this.recorder.unused(binding) },
      })),
    });
    const encoder = this.device.createCommandEncoder({ label: 'preprocess' });
    const pass = encoder.beginComputePass();
    pass.setPipeline(this.kernels.pipeline('preprocess'));
    pass.setBindGroup(0, group, [0]);
    pass.dispatchWorkgroups(Math.ceil(g.fullWidth / 8), Math.ceil(g.fullHeight / 8));
    pass.end();
    this.device.queue.submit([encoder.finish()]);
    await this.device.queue.onSubmittedWorkDone();
    source.destroy();
    uniform.destroy();
  }

  /** Replace the input features with `data` (f32 [fullRows][16]). */
  writeFeatures(data) {
    this.device.queue.writeBuffer(this.features.buffer, 0, data);
  }

  /**
   * One frame. Returns once the GPU has finished, so a caller can time it honestly.
   *
   * The first frame runs inside an error scope. A command buffer WebGPU refuses at finish() is dropped whole,
   * and the only symptom is that every output stays at whatever it was - which reads exactly like a network
   * that computes zeros. Once a frame has been accepted, the same command buffer is valid every time.
   */
  async run() {
    const checking = !this.validated;
    if (checking) this.device.pushErrorScope('validation');
    const encoder = this.device.createCommandEncoder({ label: 'nr frame' });
    this.recorder.encode(encoder);
    this.device.queue.submit([encoder.finish()]);
    if (checking) {
      const error = await this.device.popErrorScope();
      if (error) throw new Error(`the frame was rejected: ${error.message}`);
      this.validated = true;
    }
    await this.device.queue.onSubmittedWorkDone();
  }

  /** The f32 RGBA head, [fullRows][4]. */
  async readHead() {
    const head = this.graph.head;
    return new Float32Array(await readBack(this.device, head.buffer, head.rows * 4 * 4));
  }

  /** Any intermediate tensor, by the label the graph allocated it under. For debugging a wrong result. */
  async readTensorByLabel(label) {
    for (const tensor of this.tensors.byKey.values()) {
      if (tensor.label !== label) continue;
      return { tensor, bytes: new Uint8Array(await readBack(this.device, tensor.buffer, tensor.validBytes)) };
    }
    throw new Error(`no tensor labelled "${label}"; have ` +
                    [...this.tensors.byKey.values()].map((t) => t.label).join(', '));
  }

  async readBoundary(name) {
    const tensor = this.graph.boundaries.get(name);
    if (!tensor) throw new Error(`no captured boundary ${name}`);
    return new Uint8Array(await readBack(this.device, tensor.buffer, tensor.validBytes));
  }

  get boundaryNames() { return [...this.graph.boundaries.keys()]; }

  destroy() {
    this.tensors.destroy();
    this.matmul.destroy();
    // A borrowed model and device outlive this graph: the host rebuilds the graph on every resize.
    if (!this.borrowedModel) { this.model.destroy(); this.device.destroy(); }
  }
}
