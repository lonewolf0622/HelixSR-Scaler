// The FP8 GEMM as the graph sees it: a flag word, a shape, and a pipeline.
//
// A matrix multiply here is a pipeline, not a dispatch parameter. Shapes, strides and byte offsets are all
// pipeline-creation overrides, so the partial-tile guards and the option branches fold away before the driver
// sees the code; what is left in the uniform is the block the recorder writes anyway. Around two hundred
// pipelines come out of that, which is why they compile off the queue - see pipeline-compiler.js.

import { compileComputePipeline } from './pipeline-compiler.js';
import { createSiluTable } from './silu-table.js';
import { createWeightMetadataTable } from './weight-table.js';
import { createPackedSiluTable } from './packed-activation.js';
import { bitQuantMatmulCode } from './bit-quant.js';
import { productionMatmulCode } from './base.js';
import { variantCode, variantKey } from './variants.js';
import { RESIDUAL_E4 } from './packed-residual.js';

export const FLAG_BIAS = 1;
export const FLAG_RESIDUAL = 2;
export const FLAG_SILU = 4;
export const FLAG_SCALE_RESIDUAL = 8;
export const FLAG_SWIZZLE_INPUT = 16;
export const FLAG_QUANTIZE_OUTPUT = 32;
export const FLAG_BROADCAST_INPUT = 131072;
export { RESIDUAL_E4 as FLAG_RESIDUAL_E4 };

/** The K span whose sums are published to half and added separately, as the flag that selects it. */
const PARTITION_FLAGS = new Map([[0, 1024], [1024, 8192], [512, 16384], [256, 32768]]);

const LAYOUT_FIELDS = ['WEIGHT_BYTE_OFFSET', 'BIAS_BYTE_OFFSET', 'WEIGHT_MATRIX_CHANNELS',
                       'WEIGHT_COLUMN_OFFSET', 'OUTPUT_MATRIX_CHANNELS', 'OUTPUT_COLUMN_OFFSET'];

export class Matmul {
  static async create(device) {
    const matmul = new Matmul();
    matmul.device = device;
    matmul.modules = new Map();
    matmul.siluTable = await createSiluTable(device, bitQuantMatmulCode(productionMatmulCode()));
    matmul.packedSiluTable = await createPackedSiluTable(device, matmul.siluTable);
    matmul.weightMetadata = createWeightMetadataTable(device);
    return matmul;
  }

  module(variant) {
    const key = variantKey(variant);
    let module = this.modules.get(key);
    if (!module) {
      module = this.device.createShaderModule({ label: `gemm ${key}`, code: variantCode(variant) });
      this.modules.set(key, module);
    }
    return module;
  }

  /**
   * The pipeline for one matrix multiply. `flags` selects the options, the rest are the shape and the strides
   * the kernel is specialized to.
   */
  pipeline(layout, variant, { flags, rows, k, n, weightByteOffset, biasByteOffset, weightMatrixChannels,
                              outputMatrixChannels, inputMatrixChannels }) {
    const constants = {
      MATMUL_FLAGS: flags, MATMUL_ROWS: rows, MATMUL_K: k, MATMUL_N: n,
      LAYOUT_WEIGHT_BYTE_OFFSET: weightByteOffset, LAYOUT_BIAS_BYTE_OFFSET: biasByteOffset,
      LAYOUT_WEIGHT_MATRIX_CHANNELS: weightMatrixChannels, LAYOUT_WEIGHT_COLUMN_OFFSET: 0,
      LAYOUT_OUTPUT_MATRIX_CHANNELS: outputMatrixChannels, LAYOUT_OUTPUT_COLUMN_OFFSET: 0,
      ...(variant.batched ? { LAYOUT_INPUT_MATRIX_CHANNELS: inputMatrixChannels } : {}),
    };
    for (const field of LAYOUT_FIELDS) {
      if (constants[`LAYOUT_${field}`] === undefined) throw new Error(`gemm is missing LAYOUT_${field}`);
    }
    return compileComputePipeline(this.device, {
      label: `gemm ${variantKey(variant)} ${rows}x${k}x${n}`,
      layout,
      compute: { module: this.module(variant), entryPoint: 'main', constants },
    });
  }

  /** The table the publication reads: the E4M3 bytes for a packed output, the halves otherwise. */
  siluTableFor(output) { return output === 'half' ? this.siluTable : this.packedSiluTable; }

  static partitionFlag(span) {
    const flag = PARTITION_FLAGS.get(span);
    if (flag === undefined) throw new Error(`no K partition of ${span}`);
    return flag;
  }

  // The three tables are built once per device and shared by every graph on it, exactly like the weights,
  // so tearing a graph down leaves them alone. The host rebuilds the graph on every resize.
  destroy() {}
}
