// The network: 71 blocks over a six-level encoder/decoder with a global ViT at the bottom, recorded once into
// a list of dispatches. Mirrors src/nr_graph.cpp; docs/network.md explains the shape and why it is that shape.
//
// This is the unfused route. The Vulkan implementation has a second one where a whole 32-channel block runs as
// a single kernel with the intermediates staying in registers, and a third that chains launches through device
// counters instead of barriers - both worth a great deal of time, and both needing things WebGPU does not
// have. What is left is the plain form the fused route is checked against, which is also the clearest one to
// read: every tensor between two kernels is a real buffer, and every one of them can be compared.

import {
  fusedLayout, preFusedLayout, upsampleFusedLayout, postFusedLayout, windowPhase, WindowPhases, alignUp,
} from './geometry.js';
import {
  Matmul, FLAG_RESIDUAL, FLAG_SILU, FLAG_SCALE_RESIDUAL, FLAG_SWIZZLE_INPUT, FLAG_QUANTIZE_OUTPUT,
  FLAG_BROADCAST_INPUT, FLAG_RESIDUAL_E4,
} from './matmul/index.js';
import { WindowAttention, WINDOW_QUERIES } from './window/index.js';

// Flags of the hand-written kernels, matching their WGSL.
const WRITE_E4 = 2, WRITE_F16 = 4, WRITE_F32 = 32;
const DUAL = 1;   // ops.wgsl

const MAX_GROUPS = 65535;

/** A 1D dispatch of `count` invocations at 64 per workgroup, folded into two dimensions past the limit. */
function grid1d(count) {
  const groups = Math.ceil(count / 64);
  if (groups <= MAX_GROUPS) return [groups, 1];
  return [MAX_GROUPS, Math.ceil(groups / MAX_GROUPS)];
}

export class Graph {
  /**
   * @param {object} context {device, kernels, tensors, model, geometry}
   * @param {object} options {captureBoundaries} - keep a copy of every block output, for the parity harness
   */
  constructor(context, options = {}) {
    Object.assign(this, context);
    this.options = options;
    this.phases = new WindowPhases();
    this.boundaries = new Map();
    if (this.model.blockCount !== 71) {
      throw new Error(`the model has ${this.model.blockCount} blocks; this graph is the 71-block network`);
    }
  }

  // -------------------------------------------------------------------------------------------------------
  // The kernels, as the graph wants to call them.
  // -------------------------------------------------------------------------------------------------------

  /**
   * One FP8 matrix multiply. `residual`, when given, is not added afterwards - it is the value the
   * accumulator starts from, scaled by the block's learned per-channel vector. That ordering is the whole
   * difference between this and a matmul followed by an add.
   *
   * Every argument that is not a buffer ends up as a pipeline override, so two calls that differ only in
   * shape are two pipelines. src/matmul/ is where that goes.
   */
  gemm({ input, weights, output, outputF16, rows, k, n, batches = 1, broadcast = false, partition = 0,
         silu = false, residual = null, aux = 0, label }) {
    const target = output ?? outputF16;
    const mode = output && outputF16 ? 'dual' : output ? 'e4' : 'half';
    if (output && outputF16 && output.channels !== outputF16.channels) {
      throw new Error(`${label} publishes both tensors at one index, so they must share a stride`);
    }
    if (mode !== 'e4' && silu) throw new Error(`${label} activates on a boundary that is not E4M3`);
    if (residual && residual.channels !== target.channels) {
      throw new Error(`${label} reads its skip at the output index, so the strides must agree`);
    }
    // The A tensor is wider than K when several matrices share it, which is the batched kernel's own case.
    const batched = batches > 1 || input.channels !== k;
    if (weights.k !== k * batches || weights.batchK !== k) {
      throw new Error(`${label} dispatches ${batches}x${k} against a ${weights.k} matrix`);
    }
    let flags = Matmul.partitionFlag(partition) | FLAG_SWIZZLE_INPUT;
    if (silu) flags |= FLAG_SILU;
    if (mode !== 'half') flags |= FLAG_QUANTIZE_OUTPUT;
    if (broadcast) flags |= FLAG_BROADCAST_INPUT;
    if (residual) {
      flags |= FLAG_RESIDUAL | FLAG_SCALE_RESIDUAL;
      if (residual.format === 'e4') flags |= FLAG_RESIDUAL_E4;
    }
    const variant = { output: mode, residual: residual?.format === 'half' ? 'half' : 'e4',
                      batched, tile128: true };
    const pipeline = this.matmul.pipeline(this.kernels.gemmPipelineLayout, variant, {
      flags, rows, k, n,
      weightByteOffset: weights.byteOffset, biasByteOffset: aux,
      weightMatrixChannels: weights.matrixChannels, outputMatrixChannels: target.channels,
      inputMatrixChannels: input.channels,
    });
    const params = new Uint32Array([rows, k, n, weights.byteOffset, aux, flags, weights.matrixChannels,
                                    0, target.channels, 0, input.channels, batches]);
    const rowTiles = Math.ceil(rows / 32);
    this.recorder.specialized(pipeline, { kernel: 'gemm_fp8', gemm: true }, {
      0: input.buffer, 1: weights.buffer, 2: target.buffer,
      3: residual ? residual.buffer : undefined,
      5: this.matmul.siluTableFor(mode), 6: this.matmul.weightMetadata,
      7: mode === 'dual' ? outputF16.buffer : undefined,
    }, params, [Math.ceil(n / 32) * batches, Math.min(rowTiles, MAX_GROUPS),
                Math.ceil(rowTiles / MAX_GROUPS)], label);
  }

  /** The f16 matrix multiply: only the input adapter and the head. */
  gemmF16({ input, weights, paddedN, output, outputF16, outputF32, rows, k, n, label }) {
    let flags = 0;
    if (output) flags |= WRITE_E4;
    if (outputF16) flags |= WRITE_F16;
    if (outputF32) flags |= WRITE_F32;
    const target = outputF32 ?? outputF16 ?? output;
    const params = new Uint32Array([rows, k, n, paddedN, input.channels, target.channels, flags, 0]);
    this.recorder.pass('gemm_f16', {
      1: input.buffer, 2: weights,
      5: output ? output.buffer : undefined, 6: outputF16 ? outputF16.buffer : undefined,
      7: outputF32 ? outputF32.buffer : undefined,
    }, params, grid1d(rows * (n / 4)), label);
  }

  op(entryPoint, { count, channels, inWidth = 0, inHeight = 0, outWidth = 0, outHeight = 0, auxA = 0, auxB = 0,
                   dual = false, inF32, inF16, inE4, skipE4, aux, outE4, outF16, label }) {
    const params = new Uint32Array([count, channels, inWidth, inHeight, outWidth, outHeight, auxA, auxB,
                                    dual ? DUAL : 0, 0, 0, 0]);
    this.recorder.pass(entryPoint, {
      1: inF32?.buffer, 2: inF16?.buffer, 3: inE4?.buffer, 4: skipE4?.buffer, 8: aux,
      6: outE4?.buffer, 7: outF16?.buffer,
    }, params, grid1d(count / 4), label);
  }

  /**
   * One shifted-window attention. The cosine normalization is part of this kernel rather than a pass of its
   * own: it is eight lanes per token over the raw qkv, and fusing it means the normalized tensor is never
   * written or read back. src/window/ is where the kernel comes from.
   */
  windowAttention({ qkv, attended, prior, scales, width, height, heads, phase, label }) {
    const [shiftX, shiftY] = windowPhase(phase);
    const windowsX = Math.ceil((width + shiftX) / 8);
    const windowsY = Math.ceil((height + shiftY) / 8);
    const tasks = windowsX * windowsY * (64 / WINDOW_QUERIES);
    const geometry = { width, height, channels: heads * 32, shiftX, shiftY, relativeBias: 1 };
    const params = new Uint32Array([width * height, heads, width, height, heads * 32, shiftX, shiftY, 1]);
    this.recorder.specialized(this.window.pipeline(this.kernels.pipelineLayout, geometry),
                              { kernel: 'window_attend' },
                              { 1: qkv.buffer, 2: scales, 3: prior, 6: attended.buffer },
                              params, [heads, Math.min(tasks, MAX_GROUPS), Math.ceil(tasks / MAX_GROUPS)],
                              `${label} attend`);
  }

  // -------------------------------------------------------------------------------------------------------

  tensorFor(label, rows, channels, format) { return this.tensors.allocate(label, rows, channels, format); }

  capture(name, source) {
    if (!this.options.captureBoundaries) return;
    const copy = this.tensors.allocate(`boundary ${name}`, source.rows, source.channels, source.format);
    this.recorder.copy(source.buffer, copy.buffer, copy.byteLength, `capture ${name}`);
    this.boundaries.set(name, copy);
  }

  // -------------------------------------------------------------------------------------------------------
  // One block.
  // -------------------------------------------------------------------------------------------------------

  /**
   * FFN -> QKV -> window attention -> projection, with the two scaled skips that make it a residual block.
   *
   * Under 64 channels the FFN is one dense C -> 128 -> C path. At 64 and above it is C/32 independent experts,
   * each C -> 128 -> 32, whose 32-wide outputs are concatenated and run through one more C -> C layer; that
   * last layer is what carries the FFN's skip.
   */
  block({ block, channels, width, height, layout, tensor, temps, state, output, outputF16 = null,
          ffnSkipOverride = null, phase }) {
    const rows = width * height;
    const model = this.model;
    const label = `block ${block}`;
    const residual = ffnSkipOverride ?? state;
    const ffnAux = model.auxOffset(tensor, layout.ffnCosSkip, channels);

    if (layout.expertFfn) {
      const experts = layout.expertCount;
      const w2Base = layout.expand + experts * channels * 128;
      const w3Base = w2Base + experts * 128 * 32;
      this.gemm({
        input: state, weights: model.fp8Matrix(tensor, layout.expand, experts * channels, 128, { batchK: channels }),
        output: temps.ffn, rows, k: channels, n: 128, batches: experts, broadcast: true, silu: true,
        label: `${label} expert expand`,
      });
      this.gemm({
        input: temps.ffn, weights: model.fp8Matrix(tensor, w2Base, experts * 128, 32, { batchK: 128 }),
        output: temps.ffnNarrow, rows, k: 128, n: 32, batches: experts, label: `${label} expert contract`,
      });
      this.gemm({
        input: temps.ffnNarrow, weights: model.fp8Matrix(tensor, w3Base, channels, channels),
        output: temps.ffnQuantized, outputF16: temps.ffnResidual, rows, k: channels, n: channels,
        residual, aux: ffnAux, label: `${label} expert merge`,
      });
    } else {
      this.gemm({
        input: state, weights: model.fp8Matrix(tensor, layout.expand, channels, layout.hidden),
        output: temps.ffn, rows, k: channels, n: layout.hidden, silu: true, label: `${label} expand`,
      });
      this.gemm({
        input: temps.ffn, weights: model.fp8Matrix(tensor, layout.contractWeights, layout.hidden, channels),
        output: temps.ffnQuantized, outputF16: temps.ffnResidual, rows, k: layout.hidden, n: channels,
        residual, aux: ffnAux, label: `${label} contract`,
      });
    }

    this.gemm({
      input: temps.ffnQuantized, weights: model.fp8Matrix(tensor, layout.qkv, channels, channels * 3),
      outputF16: temps.qkv, rows, k: channels, n: channels * 3, label: `${label} qkv`,
    });
    this.windowAttention({
      qkv: temps.qkv, attended: temps.attended,
      prior: model.relativeBias(tensor, layout.relative, layout.heads),
      scales: model.headScales(tensor, layout.scale, layout.heads),
      width, height, heads: layout.heads, phase, label,
    });

    // The attention skip: the E4M3 publication of the FFN for the expert blocks, the raw half for the others.
    this.gemm({
      input: temps.attended, weights: model.fp8Matrix(tensor, layout.projection, channels, channels),
      output, outputF16, rows, k: channels, n: channels,
      residual: layout.expertFfn ? temps.ffnQuantized : temps.ffnResidual,
      aux: model.auxOffset(tensor, layout.attnCosSkip, channels), label: `${label} projection`,
    });
  }

  /**
   * The 512 stage. Its FFN is not the expert pattern: eight independent 64-wide branches, each widened to 256
   * and brought back, with the activation on the middle layer only, all on top of one 512 -> 512 layer.
   */
  splitBlock({ block, width, height, temps, state, output, outputF16 = null, phase }) {
    const rows = width * height;
    const model = this.model;
    const label = `block ${block}`;
    const channels = 512, branches = 8, branchChannels = 64, middleChannels = 256, heads = 16;
    const branchTensor = model.tensor(block, 0);
    const contract = model.tensor(block, 1);
    const qkvTensor = model.tensor(block, 2);
    const projection = model.tensor(block, 3);
    const w2Base = branches * channels * branchChannels;
    const w3Base = w2Base + branches * branchChannels * middleChannels;
    const qkvRelative = channels * channels * 3;
    const qkvScale = qkvRelative + heads * 8192;

    this.gemm({ input: state, weights: model.fp8Matrix(branchTensor, 0, channels, channels),
                output: temps.branch, rows, k: channels, n: channels, label: `${label} split layer0` });
    this.gemm({
      input: temps.branch,
      weights: model.fp8Matrix(branchTensor, w2Base, branches * branchChannels, middleChannels,
                               { batchK: branchChannels }),
      output: temps.middle, rows, k: branchChannels, n: middleChannels, batches: branches, silu: true,
      label: `${label} split expand`,
    });
    this.gemm({
      input: temps.middle,
      weights: model.fp8Matrix(branchTensor, w3Base, branches * middleChannels, branchChannels,
                               { batchK: middleChannels }),
      output: temps.layer0, rows, k: middleChannels, n: branchChannels, batches: branches,
      label: `${label} split contract`,
    });
    this.gemm({
      input: temps.layer0, weights: model.fp8Matrix(contract, 0, channels, channels),
      output: temps.ffnResidual, rows, k: channels, n: channels,
      residual: state, aux: model.auxOffset(contract, channels * channels, channels),
      label: `${label} split merge`,
    });
    this.gemm({
      input: temps.ffnResidual, weights: model.fp8Matrix(qkvTensor, 0, channels, channels * 3),
      outputF16: temps.qkv, rows, k: channels, n: channels * 3, label: `${label} qkv`,
    });
    this.windowAttention({
      qkv: temps.qkv, attended: temps.attended,
      prior: model.relativeBias(qkvTensor, qkvRelative, heads),
      scales: model.headScales(qkvTensor, qkvScale, heads),
      width, height, heads, phase, label,
    });
    this.gemm({
      input: temps.attended, weights: model.fp8Matrix(projection, 0, channels, channels),
      output, outputF16, rows, k: channels, n: channels,
      residual: temps.ffnResidual, aux: model.auxOffset(projection, channels * channels, channels),
      label: `${label} projection`,
    });
  }

  /** The global ViT: eight blocks whose attention spans every token of the coarsest level. */
  vit(state, tokens) {
    const channels = 1024, heads = 32, ffnChannels = 4096;
    const padded = this.geometry.paddedVitTokens;
    const model = this.model;
    const expanded = this.tensorFor('vit expand', tokens, ffnChannels, 'e4');
    const ffnResidual = this.tensorFor('vit residual', tokens, channels, 'e4');
    const qkv = this.tensorFor('vit qkv', tokens, channels * 3, 'f16');
    const normalized = this.tensorFor('vit normalized', padded, channels * 3, 'e4');
    const attended = this.tensorFor('vit attended', tokens, channels, 'e4');

    for (let block = 31; block <= 38; ++block) {
      const expand = model.tensor(block, 0);
      const contract = model.tensor(block, 1);
      const qkvTensor = model.tensor(block, 2);
      const projection = model.tensor(block, 4);
      const label = `block ${block}`;
      this.gemm({ input: state, weights: model.fp8Matrix(expand, 0, channels, ffnChannels),
                  output: expanded, rows: tokens, k: channels, n: ffnChannels, silu: true,
                  label: `${label} expand` });
      this.gemm({ input: expanded, weights: model.fp8Matrix(contract, 0, ffnChannels, channels),
                  output: ffnResidual, rows: tokens, k: ffnChannels, n: channels, partition: 1024,
                  residual: state, aux: model.auxOffset(contract, ffnChannels * channels, channels),
                  label: `${label} contract` });
      // The ViT's qkv tensor puts its per-head scales before the weights, where every other block puts them
      // after. Nothing depends on that but the offsets.
      this.gemm({ input: ffnResidual, weights: model.fp8Matrix(qkvTensor, heads * 4, channels, channels * 3),
                  outputF16: qkv, rows: tokens, k: channels, n: channels * 3, partition: 512,
                  label: `${label} qkv` });
      const params = new Uint32Array([tokens, heads, channels, padded]);
      this.recorder.pass('vit_normalize',
                         { 1: qkv.buffer, 2: model.headScales(qkvTensor, 0, heads), 5: normalized.buffer },
                         params, grid1d(tokens * heads * 8), `${label} normalize`);
      this.recorder.pass('vit_attend', { 4: normalized.buffer, 6: attended.buffer },
                         params, [heads, tokens, 1], `${label} attend`);
      this.gemm({ input: attended, weights: model.fp8Matrix(projection, 0, channels, channels),
                  output: state, rows: tokens, k: channels, n: channels, partition: 256,
                  residual: ffnResidual, aux: model.auxOffset(projection, channels * channels, channels),
                  label: `${label} projection` });
      this.capture(`block-${block}`, state);
    }
  }

  // -------------------------------------------------------------------------------------------------------

  temporaries(label, rows, channels, layout) {
    const hidden = layout.expertFfn ? layout.expertCount * 128 : layout.hidden;
    return {
      ffn: this.tensorFor(`${label} ffn`, rows, hidden, 'e4'),
      ffnNarrow: layout.expertFfn ? this.tensorFor(`${label} ffn narrow`, rows, channels, 'e4') : null,
      ffnResidual: this.tensorFor(`${label} ffn residual`, rows, channels, 'f16'),
      ffnQuantized: this.tensorFor(`${label} ffn quantized`, rows, channels, 'e4'),
      qkv: this.tensorFor(`${label} qkv`, rows, channels * 3, 'f16'),
      attended: this.tensorFor(`${label} attended`, rows, channels, 'e4'),
    };
  }

  splitTemporaries(label, rows) {
    return {
      branch: this.tensorFor(`${label} branch`, rows, 512, 'e4'),
      middle: this.tensorFor(`${label} middle`, rows, 2048, 'e4'),
      layer0: this.tensorFor(`${label} layer0`, rows, 512, 'e4'),
      ffnResidual: this.tensorFor(`${label} split residual`, rows, 512, 'e4'),
      qkv: this.tensorFor(`${label} split qkv`, rows, 1536, 'f16'),
      attended: this.tensorFor(`${label} split attended`, rows, 512, 'e4'),
    };
  }

  /** Record the whole network. `features` is f32 [fullRows][16]. */
  record(recorder, features) {
    this.recorder = recorder;
    this.phases.reset();
    const g = this.geometry;
    const model = this.model;
    const fullRows = g.fullRows;
    const [d0, d1, d2, d3, d4, d5] = g.levels;

    // ---- Block 0 at full resolution, behind the f16 input adapter.
    const preTensor = model.tensor(0);
    const preLayout = preFusedLayout();
    if (preTensor.byteLength !== preLayout.endWithoutPadding + 16) throw new Error('unexpected block 0 layout');
    const featuresHalf = this.tensorFor('features f16', fullRows, 16, 'f16');
    this.op('convert_f32_to_f16', { count: fullRows * 16, channels: 16, inF32: features, outF16: featuresHalf,
                                    label: 'features to half' });
    const adapterF16 = this.tensorFor('adapter f16', fullRows, 32, 'f16');
    const adapterE4 = this.tensorFor('adapter e4', fullRows, 32, 'e4');
    {
      const { buffer, paddedN } = model.f16Matrix(preTensor, preLayout.inputAdapter, 16, 32);
      this.gemmF16({ input: featuresHalf, weights: buffer, paddedN, output: adapterE4, outputF16: adapterF16,
                     rows: fullRows, k: 16, n: 32, label: 'input adapter' });
    }
    const block0 = this.tensorFor('block 0 out', fullRows, 32, 'e4');
    const block0Raw = this.tensorFor('block 0 raw', fullRows, 32, 'f16');
    const fullTemps = this.temporaries('full', fullRows, 32, preLayout);
    this.block({ block: 0, channels: 32, width: g.fullWidth, height: g.fullHeight, layout: preLayout,
                 tensor: preTensor, temps: fullTemps, state: adapterE4, output: block0, outputF16: block0Raw,
                 ffnSkipOverride: adapterF16, phase: this.phases.take(6) });
    this.capture('block-0', block0);

    // ---- Down to level 0 and the four 32-channel blocks there.
    const rows0 = d0.rows;
    let state = this.tensorFor('level0 in', rows0, 32, 'e4');
    this.op('downsample', { count: rows0 * 32, channels: 32, inWidth: g.fullWidth, inHeight: g.fullHeight,
                            outWidth: d0.width, outHeight: d0.height, inF16: block0Raw, outE4: state,
                            label: 'pool 0' });
    this.capture('transition-0-1', state);

    let scratch = this.tensorFor('level0 state', rows0, 32, 'e4');
    const level0Raw = this.tensorFor('level0 raw', rows0, 32, 'f16');
    const level0Temps = this.temporaries('level0', rows0, 32, fusedLayout(32));
    for (let block = 1; block <= 4; ++block) {
      this.block({ block, channels: 32, width: d0.width, height: d0.height, layout: fusedLayout(32),
                   tensor: model.tensor(block), temps: level0Temps, state, output: scratch,
                   outputF16: block === 4 ? level0Raw : null, phase: this.phases.take(0) });
      [state, scratch] = [scratch, state];
      this.capture(`block-${block}`, state);
    }
    const skip32 = state;

    // ---- Encoder stages 64 / 128 / 256, each ending in a pool and a widening transition.
    const pooled32 = this.tensorFor('pool 4', d1.rows, 32, 'e4');
    this.op('downsample', { count: d1.rows * 32, channels: 32, inWidth: d0.width, inHeight: d0.height,
                            outWidth: d1.width, outHeight: d1.height, inF16: level0Raw, outE4: pooled32,
                            label: 'pool 4' });
    this.capture('pooled-4-5', pooled32);
    let stageInput = this.tensorFor('stage 64 in', d1.rows, 64, 'e4');
    this.gemm({ input: pooled32,
                weights: model.fp8Matrix(model.tensor(4), fusedLayout(32).endWithoutPadding, 32, 64),
                output: stageInput, rows: d1.rows, k: 32, n: 64, label: 'transition 4-5' });
    this.capture('transition-4-5', stageInput);

    const encoderStages = [
      { level: d1, next: d2, channels: 64, first: 5, last: 8, levelIndex: 1 },
      { level: d2, next: d3, channels: 128, first: 9, last: 14, levelIndex: 2 },
      { level: d3, next: d4, channels: 256, first: 15, last: 22, levelIndex: 3 },
    ];
    const skips = [];
    for (const stage of encoderStages) {
      const rows = stage.level.rows;
      const label = `encoder ${stage.channels}`;
      const layout = fusedLayout(stage.channels);
      let st = stageInput;
      let sc = this.tensorFor(`${label} state`, rows, stage.channels, 'e4');
      const raw = this.tensorFor(`${label} raw`, rows, stage.channels, 'f16');
      const temps = this.temporaries(label, rows, stage.channels, layout);
      for (let block = stage.first; block <= stage.last; ++block) {
        this.block({ block, channels: stage.channels, width: stage.level.width, height: stage.level.height,
                     layout, tensor: model.tensor(block), temps, state: st, output: sc,
                     outputF16: block === stage.last ? raw : null, phase: this.phases.take(stage.levelIndex) });
        [st, sc] = [sc, st];
        this.capture(`block-${block}`, st);
      }
      skips.push(st);
      const pooled = this.tensorFor(`${label} pooled`, stage.next.rows, stage.channels, 'e4');
      this.op('downsample', { count: stage.next.rows * stage.channels, channels: stage.channels,
                              inWidth: stage.level.width, inHeight: stage.level.height,
                              outWidth: stage.next.width, outHeight: stage.next.height,
                              inF16: raw, outE4: pooled, label: `pool ${stage.last}` });
      this.capture(`pooled-${stage.last}-${stage.last + 1}`, pooled);
      const next = this.tensorFor(`${label} next`, stage.next.rows, stage.channels * 2, 'e4');
      this.gemm({ input: pooled,
                  weights: model.fp8Matrix(model.tensor(stage.last), layout.endWithoutPadding,
                                           stage.channels, stage.channels * 2),
                  output: next, rows: stage.next.rows, k: stage.channels, n: stage.channels * 2,
                  label: `transition ${stage.last}` });
      this.capture(`transition-${stage.last}-${stage.last + 1}`, next);
      stageInput = next;
    }
    const [skip64, skip128, skip256] = skips;

    // ---- The 512 stage, the pool into the ViT, and the ViT.
    let skip512;
    {
      const rows = d4.rows;
      let st = stageInput;
      let sc = this.tensorFor('encoder 512 state', rows, 512, 'e4');
      const raw = this.tensorFor('encoder 512 raw', rows, 512, 'f16');
      const temps = this.splitTemporaries('encoder 512', rows);
      for (let block = 23; block <= 30; ++block) {
        this.splitBlock({ block, width: d4.width, height: d4.height, temps, state: st, output: sc,
                          outputF16: block === 30 ? raw : null, phase: this.phases.take(4) });
        [st, sc] = [sc, st];
        this.capture(`block-${block}`, st);
      }
      skip512 = st;
      const tokens = g.vitTokens;
      const pooled = this.tensorFor('vit pooled', tokens, 512, 'e4');
      this.op('downsample', { count: tokens * 512, channels: 512, inWidth: d4.width, inHeight: d4.height,
                              outWidth: d5.width, outHeight: d5.height, inF16: raw, outE4: pooled,
                              label: 'pool 30' });
      const vitState = this.tensorFor('vit state', tokens, 1024, 'e4');
      this.gemm({ input: pooled, weights: model.fp8Matrix(model.tensor(30, 4), 0, 512, 1024),
                  output: vitState, rows: tokens, k: 512, n: 1024, label: 'transition 30-31' });
      this.vit(vitState, tokens);

      // ---- Decoder 512: the ViT output projected, doubled, and merged onto the encoder skip.
      const projected = this.tensorFor('decoder 512 projection', tokens, 512, 'f16');
      this.gemm({ input: vitState, weights: model.fp8Matrix(model.tensor(39), 0, 1024, 512),
                  outputF16: projected, rows: tokens, k: 1024, n: 512, partition: 256,
                  label: 'transition 38-39' });
      const merged = this.tensorFor('decoder 512 merge', d4.rows, 512, 'e4');
      this.op('upsample_residual', {
        count: d4.rows * 512, channels: 512, inWidth: d5.width, inHeight: d5.height,
        outWidth: d4.width, outHeight: d4.height,
        inF16: projected, skipE4: skip512, aux: model.auxVector(model.tensor(39), 1024 * 512, 512),
        outE4: merged, label: 'block 39 merge',
      });
      this.capture('block-39', merged);

      let dst = merged;
      let dsc = this.tensorFor('decoder 512 state', d4.rows, 512, 'e4');
      const dtemps = this.splitTemporaries('decoder 512', d4.rows);
      for (let block = 40; block <= 47; ++block) {
        this.splitBlock({ block, width: d4.width, height: d4.height, temps: dtemps, state: dst, output: dsc,
                          phase: this.phases.take(4) });
        [dst, dsc] = [dsc, dst];
        this.capture(`block-${block}`, dst);
      }
      stageInput = dst;
    }

    // ---- Decoder stages 256 / 128 / 64 / 32.
    const decoderStages = [
      { low: d4, high: d3, channels: 256, first: 48, last: 55, levelIndex: 3, skip: skip256 },
      { low: d3, high: d2, channels: 128, first: 56, last: 61, levelIndex: 2, skip: skip128 },
      { low: d2, high: d1, channels: 64, first: 62, last: 65, levelIndex: 1, skip: skip64 },
      { low: d1, high: d0, channels: 32, first: 66, last: 69, levelIndex: 0, skip: skip32 },
    ];
    for (const stage of decoderStages) {
      const rows = stage.high.rows;
      const label = `decoder ${stage.channels}`;
      const transition = model.tensor(stage.first);
      const layout = upsampleFusedLayout(stage.channels * 2, stage.channels);
      if (transition.byteLength !== layout.endWithoutPadding + 16) {
        throw new Error(`unexpected upsample layout for block ${stage.first}`);
      }
      const projection = this.tensorFor(`${label} projection`, stage.low.rows, stage.channels, 'f16');
      this.gemm({ input: stageInput,
                  weights: model.fp8Matrix(transition, layout.upsampleWeight, stage.channels * 2, stage.channels),
                  outputF16: projection, rows: stage.low.rows, k: stage.channels * 2, n: stage.channels,
                  label: `transition ->${stage.first}` });
      const merged = this.tensorFor(`${label} merge`, rows, stage.channels, 'e4');
      const mergedRaw = stage.channels === 32 ? this.tensorFor(`${label} merge raw`, rows, 32, 'f16') : null;
      this.op('upsample_residual', {
        count: rows * stage.channels, channels: stage.channels,
        inWidth: stage.low.width, inHeight: stage.low.height, outWidth: stage.high.width,
        outHeight: stage.high.height, dual: mergedRaw !== null,
        inF16: projection, skipE4: stage.skip,
        aux: model.auxVector(transition, layout.transitionScale, stage.channels),
        outE4: merged, outF16: mergedRaw, label: `block ${stage.first} merge`,
      });

      let st = merged;
      let sc = this.tensorFor(`${label} state`, rows, stage.channels, 'e4');
      const temps = this.temporaries(label, rows, stage.channels, layout);
      for (let block = stage.first; block <= stage.last; ++block) {
        this.block({
          block, channels: stage.channels, width: stage.high.width, height: stage.high.height,
          layout: block === stage.first ? layout : fusedLayout(stage.channels),
          tensor: model.tensor(block), temps, state: st, output: sc,
          ffnSkipOverride: block === stage.first ? mergedRaw : null,
          phase: this.phases.take(stage.levelIndex),
        });
        [st, sc] = [sc, st];
        this.capture(`block-${block}`, st);
      }
      stageInput = st;
    }

    // ---- The post block at full resolution, and the head.
    {
      const tensor = model.tensor(70);
      const layout = postFusedLayout();
      if (tensor.byteLength !== layout.endWithoutPadding) throw new Error('unexpected block 70 layout');
      const mergedRaw = this.tensorFor('post merge raw', fullRows, 32, 'f16');
      const merged = this.tensorFor('post merge', fullRows, 32, 'e4');
      this.op('post_blend', {
        count: fullRows * 32, channels: 32, inWidth: d0.width, inHeight: d0.height,
        outWidth: g.fullWidth, outHeight: g.fullHeight, dual: true,
        auxA: 0, auxB: 32, inE4: stageInput, skipE4: block0,
        aux: model.auxPair(tensor, layout.inputScale, layout.adapterScale, 32),
        outE4: merged, outF16: mergedRaw, label: 'post blend',
      });
      const blockRaw = this.tensorFor('post block raw', fullRows, 32, 'f16');
      const postTemps = this.temporaries('post', fullRows, 32, layout);
      this.block({ block: 70, channels: 32, width: g.fullWidth, height: g.fullHeight, layout, tensor,
                   temps: postTemps, state: merged, output: null, outputF16: blockRaw,
                   ffnSkipOverride: mergedRaw, phase: this.phases.take(6) });
      this.head = this.tensorFor('head', fullRows, 4, 'f32');
      const { buffer, paddedN } = model.f16Matrix(tensor, layout.postWeights, 32, 4);
      this.gemmF16({ input: blockRaw, weights: buffer, paddedN, outputF32: this.head,
                     rows: fullRows, k: 32, n: 4, label: 'head' });
    }

    return this;
  }

}
