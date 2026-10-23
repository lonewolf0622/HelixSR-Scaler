// Shifted-window attention: the preamble the specialized kernel sits on.
//
// Two things about this attention are worth knowing before reading it, because they are what let the whole
// network run in FP8 (docs/network.md):
//
//   * The scores are cosine similarities. Q and K are normalized to unit length per head and multiplied by a
//     learned per-head scale, so a score lands in a bounded range whatever the activations do.
//   * There is no maximum subtracted before the exponential. The usual softmax subtracts the row maximum to
//     keep exp() from overflowing; here the bounded score range plus a clamp make it unnecessary, and the
//     exponential itself is an affine map dropped into the half exponent field rather than a call to exp.
//
// The keys and values are held in 4x4-tiled ("physical") order inside a window, which is the order the value
// matrix indexes by; queries and the learned prior stay in natural row-major order.
//
// Everything below is names and addresses. The arithmetic is in shaders/numerics.wgsl, which is prepended,
// and the kernel itself is composed in variants.js out of the transforms beside this file.

/** Immutable for one recorded geometry, so each becomes a pipeline override rather than a uniform read. */
export const WINDOW_LAYOUT_FIELDS = ['width', 'height', 'channels', 'shift_x', 'shift_y',
                                     'use_relative_bias'];

const WINDOW_BASE_WGSL = String.raw`
struct WindowParams {
  tokens: u32, heads: u32, width: u32, height: u32,
  channels: u32, shift_x: u32, shift_y: u32, use_relative_bias: u32,
}

@group(0) @binding(0) var<uniform> attention_params: WindowParams;
@group(0) @binding(1) var<storage, read> attention_qkv: array<f16>;      // raw half QKV [tokens][channels*3]
@group(0) @binding(2) var<storage, read> attention_scales: array<f32>;   // the learned per-head scale
@group(0) @binding(3) var<storage, read> attention_prior: array<f16>;    // [heads][64 query][64 key]
@group(0) @binding(6) var<storage, read_write> attention_output: array<u32>;   // E4M3 [tokens][channels]

fn round_attention_half(value: f32) -> f32 { return round_f16(value); }

/** The E4M3 publication as a value, for the operands that stay in registers. */
fn publish_fp8(value: f32) -> f32 { return decode_e4m3(encode_e4m3(f16_bits(value))); }

/** The same publication as the byte the output tensor stores. */
fn publish_e4_code(value: f32) -> u32 { return encode_e4m3(f16_bits(value)); }

fn native_exp_weight(score: f16) -> f16 { return f16(exp_weight(f32(score))); }

// Both operands are halves, so their product is exact in f32 and the single rounding here is the fused
// multiply-add's own. This is the pair fold the normalization's square sum is specified with.
fn nr_norm_fma(a: f16, b: f16, c: f16) -> f16 { return f16(f32(a) * f32(b) + f32(c)); }

/** Physical (4x4-tiled) token -> natural row-major token inside an 8x8 window. */
fn inverse_tiled_token(token: u32) -> u32 {
  let tile = token >> 4u;
  let within = token & 15u;
  let x = (tile & 1u) * 4u + (within & 3u);
  let y = (tile >> 1u) * 4u + (within >> 2u);
  return y * 8u + x;
}

// The learned prior is the C accumulator of the score matrix multiply, so the model stores it as 16x16
// accumulator tiles rather than as a matrix. src/model.js untangles that once, into natural query and key.
fn load_relative_bias(head: u32, query: u32, key: u32) -> f32 {
  return f32(attention_prior[(head * 64u + query) * 64u + key]);
}
`;

/** The preamble alone; shaders/numerics.wgsl, which carries the `enable f16` directive, goes in front. */
export function windowBaseCode() {
  return WINDOW_BASE_WGSL;
}
