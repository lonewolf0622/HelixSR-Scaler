// Loading the weights.
//
// The model file stores every FP8 matrix in the fragment order a tensor-core MMA wants: the bytes of one
// 32x128 weight tile are interleaved so that a warp's 32 lanes each load four of them with one instruction.
// The GEMM addresses that order directly, so a stage file is uploaded as it arrives and a matrix is a byte
// offset into it - no host pass over 141 MiB, and one buffer instead of two hundred.
//
// The A operand's within-32 index rotation stays on the activation side, where the kernel applies it: it
// rotates bits 1-3 and leaves bit 4 alone, so it stays inside a 16-product group and does not change how the
// products are grouped.
//
// What is still rebuilt on the host is what no kernel addresses in place: the two f16 matrices, and the
// attention prior, whose query axis is stored in 4x4-tiled order and wanted in natural order.
//
// See docs/weights.md.

import { alignUp } from './geometry.js';
import { f16ToNumber } from './numerics.js';

/** The within-32 activation index every FP8 GEMM's A operand is in. */
export function packedInputIndex(k) {
  const base = k & ~31;
  const within = k & 31;
  const half = within & 16;
  const quarter = within & 15;
  return base + half + (quarter >> 2) * 2 + (quarter & 1) + ((quarter & 2) !== 0 ? 8 : 0);
}

export function inversePackedInputIndex(k) {
  const base = k & ~31;
  const within = k & 31;
  return base + (within & 17) + ((within & 2) << 1) + ((within & 4) << 1) + ((within & 8) >> 2);
}

/** Byte index of weight (k, n) inside a packed FP8 matrix of N columns, as the model file stores it. */
export function packedWeightIndex(k, n, outputChannels) {
  const kTile = k >> 5, kIn = k & 31;
  const nTile = n >> 7, nIn = n & 127;
  const nHalf = nIn >> 6, nGroup = (nIn & 63) >> 4, nInGroup = nIn & 15;
  const lane = ((nInGroup & 7) << 2) | ((kIn & 15) >> 2);
  const byteInLane = ((nInGroup >> 3) << 3) | ((kIn >> 4) << 2) | (kIn & 3);
  return kTile * outputChannels * 32 + nTile * 4096 + nHalf * 2048 + nGroup * 512 + lane * 16 + byteInLane;
}

/**
 * Half index of an f16 weight inside a packed matrix: 16x16 tiles in (k, n) order, each tile one m16n8k16 B
 * fragment pair. The network's two f16 matrices - the 16 -> 32 input adapter and the 32 -> 4 head - are the
 * two ways this tiling degenerates.
 */
function packedF16WeightIndex(inputChannel, outputChannel, outputChannels) {
  const nTiles = Math.ceil(outputChannels / 16);
  const tile = (inputChannel >> 4) * nTiles + (outputChannel >> 4);
  const k = inputChannel & 15, n = outputChannel & 15;
  const lane = ((n & 7) << 2) | ((k & 7) >> 1);
  const fragment = (k >= 8 ? 2 : 0) + (k & 1);
  return tile * 256 + lane * 8 + ((n >> 3) & 1) * 4 + fragment;
}

/** Natural window token (row-major in the 8x8 window) -> physical token (4x4 tiles of 16). */
export function tiledToken(token) {
  const x = token & 7, y = token >> 3;
  return (y >> 2) * 32 + (x >> 2) * 16 + (y & 3) * 4 + (x & 3);
}

export function inverseTiledToken(token) {
  const tile = token >> 4, within = token & 15;
  const x = (tile & 1) * 4 + (within & 3);
  const y = (tile >> 1) * 4 + (within >> 2);
  return y * 8 + x;
}

export class Model {
  constructor(device) {
    this.device = device;
    this.tensors = new Map();
    this.buffers = new Map();   // rebuilt tensors, keyed so a shared one is built once
    this.stages = new Map();
    this.checked = new Set();
    this.blockCount = 0;
    this.bytesUploaded = 0;
  }

  /**
   * Fetch the manifest and every stage file. `onProgress` is called with (loadedBytes, totalBytes) - the
   * weights are 141 MiB, which is long enough that a demo has to say something while it waits.
   */
  async load(directory, onProgress) {
    const manifest = await (await fetch(`${directory}/manifest.json`)).json();
    this.blockCount = manifest.totals.blockCount;

    const total = manifest.stages.reduce((sum, stage) => sum + stage.packedByteLength, 0);
    let loaded = 0;
    const stages = new Map();
    for (const stage of manifest.stages) {
      const response = await fetch(`${directory}/model/${stage.file}`);
      if (!response.ok) throw new Error(`cannot read stage ${stage.id}`);
      const bytes = new Uint8Array(await response.arrayBuffer());
      if (bytes.byteLength !== stage.packedByteLength) throw new Error(`stage size mismatch: ${stage.id}`);
      stages.set(stage.id, bytes);
      loaded += bytes.byteLength;
      onProgress?.(loaded, total);
    }

    for (const entry of manifest.tensors) {
      const stage = stages.get(entry.stage);
      if (!stage) throw new Error(`tensor references unknown stage ${entry.stage}`);
      if (entry.stageOffset + entry.byteLength > stage.byteLength) throw new Error(`tensor exceeds stage ${entry.name}`);
      this.tensors.set(entry.name, {
        name: entry.name,
        block: entry.block,
        layer: entry.layer,
        stage: entry.stage,
        stageOffset: entry.stageOffset,
        byteLength: entry.byteLength,
        bytes: stage.subarray(entry.stageOffset, entry.stageOffset + entry.byteLength),
      });
    }
    for (const [id, bytes] of stages) {
      // Four bytes of slack past the end: a weight fragment that starts unaligned is read as two words, and
      // the second of them can be one word past the last matrix byte.
      const padded = new Uint8Array(alignUp(bytes.byteLength + 4, 4));
      padded.set(bytes);
      const buffer = this.device.createBuffer({
        label: `stage ${id}`,
        size: padded.byteLength,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
      });
      this.device.queue.writeBuffer(buffer, 0, padded);
      this.bytesUploaded += bytes.byteLength;
      this.stages.set(id, buffer);
    }
    return this;
  }

  tensor(block, layer = 0, parameter = 'layer') {
    const name = `block${block}.layer${layer}.${parameter}`;
    const found = this.tensors.get(name);
    if (!found) throw new Error(`missing tensor ${name}`);
    return found;
  }

  /** An f16 aux value (the per-channel skip scales) as a Number. */
  auxHalf(tensor, byteOffset, column) {
    const p = tensor.bytes;
    const at = byteOffset + column * 2;
    return p[at] | (p[at + 1] << 8);
  }

  /**
   * How much of the reprojected history the temporal blend may keep, as a fraction of what the head's own
   * logit asks for. It is a learned scalar in the model, not a knob: the network was trained with it.
   */
  blendScale() {
    const tensor = this.tensor(70, 0, 'blend_scale');
    if (tensor.byteLength < 2) throw new Error('the model has no blend scale');
    return f16ToNumber(tensor.bytes[0] | (tensor.bytes[1] << 8));
  }

  /** An f32 aux value (the per-head attention scales). */
  auxF32(tensor, byteOffset) {
    return new DataView(tensor.bytes.buffer, tensor.bytes.byteOffset + byteOffset, 4).getFloat32(0, true);
  }

  gpuBuffer(key, build, label) {
    const existing = this.buffers.get(key);
    if (existing) return existing;
    const data = build();
    const buffer = this.device.createBuffer({
      label: label ?? key,
      size: Math.max(4, alignUp(data.byteLength, 4)),
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
    });
    this.device.queue.writeBuffer(buffer, 0, data);
    this.bytesUploaded += data.byteLength;
    this.buffers.set(key, buffer);
    return buffer;
  }

  /**
   * One FP8 matrix, where it already is. The GEMM reads the model's own fragment order, so this is a buffer
   * and a byte offset - plus the one thing the whole specialized reduction rests on, checked here because
   * this is the only place that sees the bytes: every weight satisfies |w| <= 9, which is what makes the
   * product of two operands scaled by four an exact normal half. A matrix that broke it would still produce
   * numbers, just not the right ones, so it is a throw rather than a fallback.
   */
  fp8Matrix(tensor, byteOffset, K, Nmatrix, { batchK = 0 } = {}) {
    const batch = batchK || K;
    if (K % 32 || Nmatrix % 16 || batch % 32 || K % batch) {
      throw new Error(`FP8 matrix shape must be K%32==0, N%16==0, batchK | K: ${tensor.name}`);
    }
    if (byteOffset + K * Nmatrix > tensor.byteLength) {
      throw new Error(`FP8 matrix exceeds tensor ${tensor.name}`);
    }
    const key = `${tensor.name}/${byteOffset}/${K}x${Nmatrix}`;
    if (!this.checked.has(key)) {
      const bytes = tensor.bytes;
      for (let i = byteOffset; i < byteOffset + K * Nmatrix; ++i) {
        const magnitude = bytes[i] & 0x7f;
        // 0x51 is 9; 0x7f is the NaN code, which the weight table decodes to zero as the hardware does.
        if (magnitude > 0x51 && magnitude !== 0x7f) {
          throw new Error(`weight ${i - byteOffset} of ${tensor.name} is outside the bounded-half range`);
        }
      }
      this.checked.add(key);
    }
    return { buffer: this.stage(tensor), byteOffset: tensor.stageOffset + byteOffset,
             k: K, matrixChannels: Nmatrix, batchK: batch };
  }

  stage(tensor) {
    const buffer = this.stages.get(tensor.stage);
    if (!buffer) throw new Error(`tensor ${tensor.name} has no stage buffer`);
    return buffer;
  }

  /** A plain [K][paddedN] f16 matrix, for the input adapter and the head. */
  f16Matrix(tensor, byteOffset, K, N) {
    const paddedN = alignUp(N, 16);
    const key = `${tensor.name}/f16/${byteOffset}/${K}x${N}`;
    const buffer = this.gpuBuffer(key, () => {
      if (K % 16) throw new Error('f16 matrix K must be a multiple of 16');
      const plain = new Uint16Array(K * paddedN);
      for (let k = 0; k < K; ++k) {
        for (let n = 0; n < N; ++n) {
          const halfIndex = (byteOffset >> 1) + packedF16WeightIndex(k, n, N);
          if (halfIndex * 2 + 1 >= tensor.byteLength) throw new Error(`f16 matrix exceeds tensor ${key}`);
          plain[k * paddedN + n] = tensor.bytes[halfIndex * 2] | (tensor.bytes[halfIndex * 2 + 1] << 8);
        }
      }
      return plain;
    });
    return { buffer, paddedN };
  }

  /**
   * The learned attention prior as f16 [heads][64 query][64 key], both tokens in natural row-major order
   * within the window. The model stores it as the C accumulator of the score matrix multiply, so both axes
   * arrive in 4x4-tiled order inside 16x16 fragments; the kernel wants to index it by the token coordinates
   * it already has, so the untangling happens once, here.
   */
  relativeBias(tensor, relativeByteOffset, heads) {
    const key = `${tensor.name}/prior/${relativeByteOffset}/${heads}`;
    return this.gpuBuffer(key, () => {
      const prior = new Uint16Array(heads * 64 * 64);
      for (let head = 0; head < heads; ++head) {
        for (let query = 0; query < 64; ++query) {
          const q = tiledToken(query);
          for (let k = 0; k < 64; ++k) {
            const m = q & 15, n = k & 15;
            const lane = ((m & 7) << 2) | ((n & 7) >> 1);
            const fragment = (m >= 8 ? 2 : 0) + (n & 1);
            const halfIndex = (q >> 4) * 1024 + (k >> 4) * 256 + lane * 8 + (n >> 3) * 4 + fragment;
            const byteIndex = relativeByteOffset + head * 8192 + halfIndex * 2;
            if (byteIndex + 1 >= tensor.byteLength) throw new Error(`relative bias exceeds tensor ${key}`);
            prior[(head * 64 + query) * 64 + inverseTiledToken(k)] =
              tensor.bytes[byteIndex] | (tensor.bytes[byteIndex + 1] << 8);
          }
        }
      }
      return prior;
    });
  }

  /** The per-head f32 attention scales, as their own small buffer. */
  headScales(tensor, byteOffset, heads) {
    const key = `${tensor.name}/heads/${byteOffset}/${heads}`;
    return this.gpuBuffer(key, () => {
      const view = new DataView(tensor.bytes.buffer, tensor.bytes.byteOffset);
      const values = new Float32Array(heads);
      for (let h = 0; h < heads; ++h) values[h] = view.getFloat32(byteOffset + h * 4, true);
      return values;
    });
  }

  /** Two per-channel scale vectors end to end, for the post blend, which reads both. */
  auxPair(tensor, offsetA, offsetB, count) {
    const key = `${tensor.name}/auxpair/${offsetA}/${offsetB}/${count}`;
    return this.gpuBuffer(key, () => {
      const values = new Uint16Array(count * 2);
      for (let i = 0; i < count; ++i) {
        values[i] = this.auxHalf(tensor, offsetA, i);
        values[count + i] = this.auxHalf(tensor, offsetB, i);
      }
      return values;
    });
  }

  /** The per-channel skip scales of one tensor slice, as a half buffer the kernel indexes by column. */
  auxVector(tensor, byteOffset, count) {
    const key = `${tensor.name}/aux/${byteOffset}/${count}`;
    return this.gpuBuffer(key, () => {
      const values = new Uint16Array(count);
      for (let i = 0; i < count; ++i) values[i] = this.auxHalf(tensor, byteOffset, i);
      return values;
    });
  }

  /** The same scales, as the GEMM wants them: an offset into the stage buffer its weights already come from. */
  auxOffset(tensor, byteOffset, count) {
    if (byteOffset % 2) throw new Error(`skip scales of ${tensor.name} are not half-aligned`);
    if (byteOffset + count * 2 > tensor.byteLength) throw new Error(`skip scales exceed ${tensor.name}`);
    return tensor.stageOffset + byteOffset;
  }

  destroy() {
    for (const buffer of this.buffers.values()) buffer.destroy();
    for (const buffer of this.stages.values()) buffer.destroy();
    this.buffers.clear();
    this.stages.clear();
  }
}
