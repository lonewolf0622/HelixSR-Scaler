// The window attention as the graph sees it: a geometry and a pipeline.
//
// The kernel is one module; what varies between the 63 window blocks is the field size, the channel count and
// the window phase, and all of those are pipeline-creation overrides. There are only a handful of distinct
// combinations - one per level per phase - so the pipelines are few, and they compile off the queue with the
// GEMM's (see src/matmul/pipeline-compiler.js).

import { compileComputePipeline } from '../matmul/pipeline-compiler.js';
import { windowAttentionCode, WINDOW_QUERIES, WINDOW_THREADS } from './variants.js';

export { WINDOW_QUERIES, WINDOW_THREADS };

/** Shared memory the widest query tile needs; below this the port has no window attention to run. */
export const WINDOW_WORKGROUP_STORAGE = 32768;

/**
 * Checked wherever a device is first seen. It is the one limit this port cannot work around, and finding out
 * at pipeline creation - after the weights have downloaded - says only that a shader did not compile.
 */
export function requireWorkgroupStorage(device) {
  const offered = device.limits.maxComputeWorkgroupStorageSize;
  if (offered >= WINDOW_WORKGROUP_STORAGE) return;
  throw new Error(`the window attention needs ${WINDOW_WORKGROUP_STORAGE / 1024} KB of workgroup storage; ` +
                  `this adapter offers ${offered}`);
}

export class WindowAttention {
  static create(device, numerics) {
    const attention = new WindowAttention();
    attention.device = device;
    // numerics.wgsl runs on f32 bit patterns and does not enable f16; this kernel needs the type, and the
    // directive has to come before every declaration in the module.
    attention.module = device.createShaderModule({
      label: 'window attention',
      code: ['enable f16;', numerics, windowAttentionCode()].join('\n'),
    });
    return attention;
  }

  pipeline(layout, { width, height, channels, shiftX, shiftY, relativeBias }) {
    return compileComputePipeline(this.device, {
      label: `window attend ${width}x${height} +${shiftX},${shiftY}`,
      layout,
      compute: {
        module: this.module,
        entryPoint: 'attend_window_tiled',
        constants: {
          WINDOW_WIDTH: width, WINDOW_HEIGHT: height, WINDOW_CHANNELS: channels,
          WINDOW_SHIFT_X: shiftX, WINDOW_SHIFT_Y: shiftY, WINDOW_USE_RELATIVE_BIAS: relativeBias,
        },
      },
    });
  }
}
