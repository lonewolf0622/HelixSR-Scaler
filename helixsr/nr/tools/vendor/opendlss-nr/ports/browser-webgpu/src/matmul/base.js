// The FP8 GEMM, as one plain kernel plus the source transforms that specialize it.
//
// Every matrix multiply in the network except the two f16 ones runs through here. On the Vulkan side this is
// a cooperative-matrix loop: a warp issues one instruction per 16x8x32 tile and the tensor core does the
// rest. WebGPU has no such instruction, so the tile is written out - a workgroup stages a 32-row by 32-deep
// slab of activations and a 32-deep by 32-wide slab of weights in workgroup memory, and walks the fixed-point
// step over them.
//
// The kernel below is the readable form of that: scalar, unspecialized, with every option still a runtime
// branch on `params.flags`. It is also four times slower than the network can afford. The files beside this
// one each take that source and rewrite one part of it - the thread mapping, the exponent reduction, the
// operand format, the publication - and `variants.js` composes them in the order their preconditions allow.
// Each transform is a separate file because each is a separate claim about what may be changed without
// changing the result, and the parity harness checks that claim by running the composition against the
// recorded native boundaries. See docs/numerics.md for the arithmetic those claims rest on.
//
// What must not change is the order. The K loop runs in steps of 32, each step is two groups of 16 products
// sharing one exponent, and the incoming residual is the accumulator the first group starts from rather than
// something added at the end. Reassociating any of that is a different function.

const MATMUL_WGSL = String.raw`
/*__F16_ENABLE__*/
struct MatmulParams {
  rows: u32,
  input_channels: u32,
  output_channels: u32,
  weight_byte_offset: u32,
  bias_byte_offset: u32,
  flags: u32,
  weight_matrix_channels: u32,
  weight_column_offset: u32,
  output_matrix_channels: u32,
  output_column_offset: u32,
}

@group(0) @binding(0) var<storage, read> input_values: array<f32>;
@group(0) @binding(1) var<storage, read> packed_weights: array<u32>;
@group(0) @binding(2) var<storage, read_write> output_values: array<f32>;
@group(0) @binding(3) var<storage, read> residual_values: array<f32>;
@group(0) @binding(4) var<uniform> params: MatmulParams;

var<workgroup> tile_a: array<f32, 1024>;
var<workgroup> tile_b: array<f32, 1024>;

fn packed_byte(byte_offset: u32) -> u32 {
  let word = packed_weights[byte_offset >> 2u];
  return (word >> ((byte_offset & 3u) * 8u)) & 0xffu;
}

fn decode_e4m3(bits: u32) -> f32 {
  let negative = (bits & 0x80u) != 0u;
  let exponent = (bits >> 3u) & 0x0fu;
  let mantissa = bits & 0x07u;
  var value: f32;
  if (exponent == 0u) {
    value = f32(mantissa) * 0.001953125;
  } else if (exponent == 15u && mantissa == 7u) {
    value = 0.0;
  } else {
    value = (1.0 + f32(mantissa) * 0.125) * exp2(f32(i32(exponent) - 7));
  }
  return select(value, -value, negative);
}

fn packed_weight_index(k: u32, n: u32, output_channels: u32) -> u32 {
  let k_tile = k >> 5u;
  let k_in_tile = k & 31u;
  let n_tile = n >> 7u;
  let n_in_tile = n & 127u;
  let n_half = n_in_tile >> 6u;
  let n_group = (n_in_tile & 63u) >> 4u;
  let n_in_group = n_in_tile & 15u;
  let lane = ((n_in_group & 7u) << 2u) | ((k_in_tile & 15u) >> 2u);
  let byte_in_lane = ((n_in_group >> 3u) << 3u) | ((k_in_tile >> 4u) << 2u) | (k_in_tile & 3u);
  return k_tile * output_channels * 32u
    + n_tile * 4096u
    + n_half * 2048u
    + n_group * 512u
    + lane * 16u
    + byte_in_lane;
}

fn expert_interleaved_weight_index(k: u32, n: u32, experts: u32, expert: u32) -> u32 {
  let output_group = n >> 5u;
  let output_in_group = n & 31u;
  return output_group * experts * 1024u + expert * 1024u
    + packed_weight_index(k, output_in_group, 32u);
}

fn native_chained_input_index(k: u32) -> u32 {
  let base = k & ~31u;
  let within = k & 31u;
  let half = within & 16u;
  let quarter = within & 15u;
  return base + half + (quarter >> 2u) * 2u + (quarter & 1u)
    + select(0u, 8u, (quarter & 2u) != 0u);
}

fn native_inverse_chained_input_index(k: u32) -> u32 {
  let base = k & ~31u;
  let within = k & 31u;
  return base + (within & 17u) + ((within & 2u) << 1u)
    + ((within & 4u) << 1u) + ((within & 8u) >> 2u);
}

fn load_aux_half(channel: u32) -> f32 {
  let half_index = (params.bias_byte_offset >> 1u) + channel;
  let halves = unpack2x16float(packed_weights[half_index >> 1u]);
  return select(halves.x, halves.y, (half_index & 1u) != 0u);
}

/*__ROUND_F16__*/

// The network's cubic SiLU. Each step is one operation rounded once to half, and both the constants and the
// order they are applied in are part of the result rather than an implementation detail: see
// docs/numerics.md. The f32 twin in shaders/numerics.wgsl is checked against the same reference.
fn mp_cubic_silu(value: f32) -> f32 {
  let bounded = round_accumulator(clamp(value, -4.0, 4.0));
  let absolute = round_accumulator(abs(bounded));
  let inner = round_accumulator(-0.055908203125 * absolute + 0.447265625);
  let polynomial = round_accumulator(bounded * inner + 0.89453125);
  return round_accumulator(value * polynomial);
}

fn fp8_domain(value: f32) -> f32 {
  if (value != value) { return 0.0; }
  let magnitude = min(abs(value), 448.0);
  if (magnitude == 0.0) { return 0.0; }
  var quantized: f32;
  if (magnitude < 0.015625) {
    quantized = round(magnitude * 512.0) / 512.0;
  } else {
    let step = exp2(floor(log2(magnitude)) - 3.0);
    quantized = min(round(magnitude / step) * step, 448.0);
  }
  return select(quantized, -quantized, value < 0.0);
}

// Bit-accurate Ada FP8 tensor-core arithmetic recovered independently by
// Microsoft's MMA-Sim model.  Ada's m16n8k32 FP8 instruction is two fused
// 16-product reductions.  Each reduction aligns its products and incoming
// FP16 accumulator to a shared exponent, truncates every aligned significand
// to 13 fractional bits, sums those fixed-point terms exactly, then rounds the
// result to FP16.  A scalar IEEE dot product does not reproduce this behavior.
fn normal_exponent(value: f32) -> i32 {
  return i32((bitcast<u32>(abs(value)) >> 23u) & 0xffu) - 127;
}

fn e4m3_exponent(value: f32) -> i32 {
  return max(normal_exponent(value), -6);
}

fn f16_exponent(value: f32) -> i32 {
  return max(normal_exponent(value), -14);
}

fn ada_fp8_fdpa(row: u32, column: u32, k_start: u32, k_count: u32, accumulator: f32) -> f32 {
  // The hardware instruction carries an infinite accumulator through finite E4 products unchanged.
  // Normalizing it with exp2(-128) instead would flush the scale to zero and manufacture a NaN the
  // instruction never produces, so an infinite accumulator returns as it arrived.
  if ((bitcast<u32>(accumulator) & 0x7f800000u) == 0x7f800000u) { return accumulator; }
  var maximum_exponent = -21;
  if (accumulator != 0.0) { maximum_exponent = f16_exponent(accumulator); }
  for (var offset = 0u; offset < k_count; offset += 1u) {
    let a = tile_a[row * 32u + k_start + offset];
    let b = tile_b[(k_start + offset) * 32u + column];
    if (a != 0.0 && b != 0.0) {
      maximum_exponent = max(maximum_exponent, e4m3_exponent(a) + e4m3_exponent(b));
    }
  }

  var fused_significand = 0.0;
  if (accumulator != 0.0) {
    let exponent = f16_exponent(accumulator);
    let significand = accumulator * exp2(-f32(exponent));
    fused_significand += trunc(significand
      * exp2(f32(exponent - maximum_exponent + 13))) * 0.0001220703125;
  }
  for (var offset = 0u; offset < k_count; offset += 1u) {
    let a = tile_a[row * 32u + k_start + offset];
    let b = tile_b[(k_start + offset) * 32u + column];
    if (a != 0.0 && b != 0.0) {
      let exponent_a = e4m3_exponent(a);
      let exponent_b = e4m3_exponent(b);
      let significand_a = a * exp2(-f32(exponent_a));
      let significand_b = b * exp2(-f32(exponent_b));
      let exponent = exponent_a + exponent_b;
      fused_significand += trunc(significand_a * significand_b
        * exp2(f32(exponent - maximum_exponent + 13))) * 0.0001220703125;
    }
  }
  return round_accumulator(fused_significand * exp2(f32(maximum_exponent)));
}

@compute @workgroup_size(8, 8, 1)
fn main(
  @builtin(local_invocation_index) local_index: u32,
  @builtin(workgroup_id) group_id: vec3<u32>,
  @builtin(local_invocation_id) local_id: vec3<u32>,
) {
  let row_group = group_id.y + group_id.z * 65535u;
  let row_base = row_group * 32u + local_id.y * 4u;
  let column_base = group_id.x * 32u + local_id.x * 4u;
  var sums = array<f32, 16>();
  var tile_sums = array<f32, 16>();
  var partition_sums = array<f32, 16>();

  // Native mma.sync uses FP16 accumulators. Residual/bias values initialize
  // those accumulators before the first K tile; adding them after the matrix
  // product changes every subsequent FP16 rounding step.
  for (var local_row = 0u; local_row < 4u; local_row += 1u) {
    let row = row_base + local_row;
    for (var local_column = 0u; local_column < 4u; local_column += 1u) {
      let column = column_base + local_column;
      if (row < params.rows && column < params.output_channels) {
        let output_index = row * params.output_matrix_channels
          + params.output_column_offset + column;
        var initial = 0.0;
        if ((params.flags & 1u) != 0u) { initial += load_aux_half(column); }
        if ((params.flags & 2u) != 0u && (params.flags & 65536u) == 0u) {
          var residual = residual_values[output_index];
          if ((params.flags & 8u) != 0u) { residual *= load_aux_half(column); }
          initial += residual;
        }
        sums[local_row * 4u + local_column] = round_accumulator(initial);
      }
    }
  }

  for (var k_base = 0u; k_base < params.input_channels; k_base += 32u) {
    if ((params.flags & 256u) != 0u) {
      for (var accumulator = 0u; accumulator < 16u; accumulator += 1u) {
        tile_sums[accumulator] = 0.0;
      }
    }
    for (var linear = local_index; linear < 1024u; linear += 64u) {
      let tile_row = linear >> 5u;
      let tile_column = linear & 31u;
      let input_row = row_group * 32u + tile_row;
      let k = k_base + tile_column;
      var input_k = select(k, native_chained_input_index(k), (params.flags & 16u) != 0u);
      if ((params.flags & 128u) != 0u) { input_k = native_inverse_chained_input_index(k); }
      tile_a[linear] = 0.0;
      if (input_row < params.rows && k < params.input_channels) {
        tile_a[linear] = input_values[input_row * params.input_channels + input_k];
      }

      let output_column = group_id.x * 32u + tile_column;
      tile_b[linear] = 0.0;
      if (output_column < params.output_channels && k_base + tile_row < params.input_channels) {
        var weight_index: u32;
        if ((params.flags & 64u) != 0u) {
          weight_index = expert_interleaved_weight_index(
            k_base + tile_row,
            output_column,
            params.weight_matrix_channels,
            params.weight_column_offset,
          );
        } else {
          weight_index = packed_weight_index(
            k_base + tile_row,
            output_column + params.weight_column_offset,
            params.weight_matrix_channels,
          );
        }
        tile_b[linear] = decode_e4m3(packed_byte(params.weight_byte_offset + weight_index));
      }
    }
    workgroupBarrier();

    if ((params.flags & 64512u) != 0u) {
      for (var local_row = 0u; local_row < 4u; local_row += 1u) {
        for (var local_column = 0u; local_column < 4u; local_column += 1u) {
          let accumulator = local_row * 4u + local_column;
          let tile_row = local_id.y * 4u + local_row;
          let tile_column = local_id.x * 4u + local_column;
          var reduction_length = 16u;
          if ((params.flags & 2048u) != 0u) { reduction_length = 8u; }
          if ((params.flags & 4096u) != 0u) { reduction_length = 4u; }
          for (var reduction_start = 0u; reduction_start < 32u;
              reduction_start += reduction_length) {
            sums[accumulator] = ada_fp8_fdpa(tile_row, tile_column,
              reduction_start, reduction_length, sums[accumulator]);
          }
        }
      }
    } else {
     for (var k = 0u; k < 32u; k += 1u) {
      let a0 = tile_a[(local_id.y * 4u + 0u) * 32u + k];
      let a1 = tile_a[(local_id.y * 4u + 1u) * 32u + k];
      let a2 = tile_a[(local_id.y * 4u + 2u) * 32u + k];
      let a3 = tile_a[(local_id.y * 4u + 3u) * 32u + k];
      let b0 = tile_b[k * 32u + local_id.x * 4u + 0u];
      let b1 = tile_b[k * 32u + local_id.x * 4u + 1u];
      let b2 = tile_b[k * 32u + local_id.x * 4u + 2u];
      let b3 = tile_b[k * 32u + local_id.x * 4u + 3u];
      if ((params.flags & 256u) != 0u) {
        tile_sums[0] += a0 * b0; tile_sums[1] += a0 * b1; tile_sums[2] += a0 * b2; tile_sums[3] += a0 * b3;
        tile_sums[4] += a1 * b0; tile_sums[5] += a1 * b1; tile_sums[6] += a1 * b2; tile_sums[7] += a1 * b3;
        tile_sums[8] += a2 * b0; tile_sums[9] += a2 * b1; tile_sums[10] += a2 * b2; tile_sums[11] += a2 * b3;
        tile_sums[12] += a3 * b0; tile_sums[13] += a3 * b1; tile_sums[14] += a3 * b2; tile_sums[15] += a3 * b3;
      } else {
        sums[0] += a0 * b0; sums[1] += a0 * b1; sums[2] += a0 * b2; sums[3] += a0 * b3;
        sums[4] += a1 * b0; sums[5] += a1 * b1; sums[6] += a1 * b2; sums[7] += a1 * b3;
        sums[8] += a2 * b0; sums[9] += a2 * b1; sums[10] += a2 * b2; sums[11] += a2 * b3;
        sums[12] += a3 * b0; sums[13] += a3 * b1; sums[14] += a3 * b2; sums[15] += a3 * b3;
      }
     }
     for (var accumulator = 0u; accumulator < 16u; accumulator += 1u) {
      if ((params.flags & 256u) != 0u) {
        let tile_sum = select(tile_sums[accumulator], round_accumulator(tile_sums[accumulator]),
          (params.flags & 512u) != 0u);
        sums[accumulator] = round_accumulator(sums[accumulator] + tile_sum);
      } else {
        sums[accumulator] = round_accumulator(sums[accumulator]);
      }
      }
    }
    let partition_width = select(
      select(1024u, 512u, (params.flags & 16384u) != 0u),
      256u,
      (params.flags & 32768u) != 0u,
    );
    if ((params.flags & 57344u) != 0u
        && (((k_base + 32u) % partition_width) == 0u
          || (k_base + 32u) >= params.input_channels)) {
      for (var accumulator = 0u; accumulator < 16u; accumulator += 1u) {
        if (k_base < partition_width) {
          partition_sums[accumulator] = sums[accumulator];
        } else {
          partition_sums[accumulator] = round_accumulator(
            partition_sums[accumulator] + sums[accumulator]);
        }
        sums[accumulator] = 0.0;
      }
    }
    workgroupBarrier();
  }

  if ((params.flags & 57344u) != 0u) {
    for (var accumulator = 0u; accumulator < 16u; accumulator += 1u) {
      sums[accumulator] = partition_sums[accumulator];
    }
  }

  for (var local_row = 0u; local_row < 4u; local_row += 1u) {
    let row = row_base + local_row;
    for (var local_column = 0u; local_column < 4u; local_column += 1u) {
      let column = column_base + local_column;
      if (row < params.rows && column < params.output_channels) {
        let output_index = row * params.output_matrix_channels
          + params.output_column_offset + column;
        var value = sums[local_row * 4u + local_column];
        if ((params.flags & 2u) != 0u && (params.flags & 65536u) != 0u) {
          var residual = residual_values[output_index];
          if ((params.flags & 8u) != 0u) {
            value = round_accumulator(value + residual * load_aux_half(column));
          } else {
            value = round_accumulator(value + residual);
          }
        }
        if ((params.flags & 4u) != 0u) { value = mp_cubic_silu(value); }
        output_values[output_index] = select(value, fp8_domain(value), (params.flags & 32u) != 0u);
      }
    }
  }
}
`;

// Align directly into native fixed-point units. All operands are binary
// FP8/FP16 numbers: the omitted normalization factors are exact powers of two.
// This preserves the shared-exponent truncation and final half rounding while
// removing four exponentials per product from the scalar MMA emulation.
const PRODUCTION_FDPA = `fn ada_fp8_fdpa(row: u32, column: u32, k_start: u32, k_count: u32, accumulator: f32) -> f32 {
  var maximum_exponent = -21;
  if (accumulator != 0.0) { maximum_exponent = f16_exponent(accumulator); }
  for (var offset = 0u; offset < k_count; offset += 1u) {
    let a = tile_a[row * 32u + k_start + offset];
    let b = tile_b[(k_start + offset) * 32u + column];
    if (a != 0.0 && b != 0.0) {
      maximum_exponent = max(maximum_exponent, e4m3_exponent(a) + e4m3_exponent(b));
    }
  }
  let alignment = bitcast<f32>(u32(140 - maximum_exponent) << 23u);
  var fixed_sum = select(0.0, trunc(accumulator * alignment), accumulator != 0.0);
  for (var offset = 0u; offset < k_count; offset += 1u) {
    let a = tile_a[row * 32u + k_start + offset];
    let b = tile_b[(k_start + offset) * 32u + column];
    fixed_sum += trunc((a * b) * alignment);
  }
  return round_accumulator(fixed_sum * bitcast<f32>(u32(maximum_exponent + 114) << 23u));
}`;

/** The kernel as compiled: f16 enabled, and the exponentials the reduction does not need removed. */
export function productionMatmulCode() {
  return MATMUL_WGSL
    .replace('/*__F16_ENABLE__*/', 'enable f16;')
    .replace('/*__ROUND_F16__*/',
      'fn round_accumulator(value: f32) -> f32 { return f32(f16(value)); }')
    .replace(/fn ada_fp8_fdpa\([\s\S]*?\n}/, PRODUCTION_FDPA)
    // In these bounded normal ranges, power-of-two construction and binary exponent extraction are exact;
    // no exp2/log2 approximation is needed, and WGSL allows those two an error the arithmetic cannot take.
    .replace('exp2(f32(i32(exponent) - 7))', 'bitcast<f32>((exponent + 120u) << 23u)')
    .replace('exp2(floor(log2(magnitude)) - 3.0)',
      'bitcast<f32>((((bitcast<u32>(magnitude) >> 23u) & 255u) - 3u) << 23u)')
    // The flags are a pipeline override, not a uniform: every branch on them folds at compile time.
    .replaceAll('params.flags', 'MATMUL_FLAGS')
    .replace('struct MatmulParams', 'override MATMUL_FLAGS: u32 = 0u;\nstruct MatmulParams');
}
