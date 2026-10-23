// The two f16 matrices: the 16 -> 32 input adapter in front of block 0, and the 32 -> 4 head after block 70.
//
// They are the only places in the network where both operands are halves rather than E4M3, so the tensor-core
// step is the f16 one: eight products against 24 fractional bits, and an exact integer -> half conversion at
// the end instead of a rounding of the sum. Both are small and run once a frame, so there is no tiling here -
// one invocation takes four output columns of one row, which is what makes its stores whole words.

struct Params {
  rows : u32,
  k : u32,
  n : u32,
  padded_n : u32,        // the weight matrix is padded to a multiple of 16 columns
  input_stride : u32,
  output_stride : u32,
  flags : u32,
  pad : u32,
};

const FLAG_WRITE_E4  = 2u;
const FLAG_WRITE_F16 = 4u;
const FLAG_WRITE_F32 = 32u;

@group(0) @binding(0) var<uniform> params : Params;
@group(0) @binding(1) var<storage, read> activations : array<u32>;   // half pairs
@group(0) @binding(2) var<storage, read> weights : array<u32>;       // half pairs, [k][padded_n]
@group(0) @binding(5) var<storage, read_write> out_e4 : array<u32>;
@group(0) @binding(6) var<storage, read_write> out_f16 : array<u32>;
@group(0) @binding(7) var<storage, read_write> out_f32 : array<f32>;

fn activation_half(index: u32) -> f32 {
  return f16_to_f32((activations[index >> 1u] >> ((index & 1u) * 16u)) & 0xffffu);
}
fn weight_half(index: u32) -> f32 {
  return f16_to_f32((weights[index >> 1u] >> ((index & 1u) * 16u)) & 0xffffu);
}

@compute @workgroup_size(64)
fn gemm_f16(@builtin(global_invocation_id) id : vec3<u32>) {
  // One invocation per (row, four columns), over a 1D grid folded past 65535 workgroups.
  let quads_per_row = params.n / 4u;
  let linear = id.x + id.y * 65535u * 64u;
  let row = linear / quads_per_row;
  let quad = (linear % quads_per_row) * 4u;
  if (row >= params.rows) { return; }

  var value : array<f32, 4>;
  for (var c = 0u; c < 4u; c = c + 1u) { value[c] = 0.0; }
  var a : array<f32, 8>;
  var b : array<f32, 8>;
  for (var base = 0u; base < params.k; base = base + 8u) {
    for (var i = 0u; i < 8u; i = i + 1u) { a[i] = activation_half(row * params.input_stride + base + i); }
    for (var c = 0u; c < 4u; c = c + 1u) {
      for (var i = 0u; i < 8u; i = i + 1u) { b[i] = weight_half((base + i) * params.padded_n + quad + c); }
      value[c] = ada_f16_fdpa8(a, b, value[c]);
    }
  }

  let index = row * params.output_stride + quad;
  if ((params.flags & FLAG_WRITE_F32) != 0u) {
    for (var c = 0u; c < 4u; c = c + 1u) { out_f32[index + c] = value[c]; }
  }
  if ((params.flags & FLAG_WRITE_F16) != 0u) {
    out_f16[index >> 1u] = f16_bits(value[0]) | (f16_bits(value[1]) << 16u);
    out_f16[(index >> 1u) + 1u] = f16_bits(value[2]) | (f16_bits(value[3]) << 16u);
  }
  if ((params.flags & FLAG_WRITE_E4) != 0u) {
    var word = 0u;
    for (var c = 0u; c < 4u; c = c + 1u) { word = word | (encode_e4m3(f16_bits(value[c])) << (c * 8u)); }
    out_e4[index >> 2u] = word;
  }
}
