// The elementwise steps between the matrix multiplies: the format conversions, the 2x2 pool that moves down a
// level, and the two merges that bring a skip connection back in on the way up.
//
// None of them is a reshape. Each one is arithmetic with its own rounding schedule, and each is part of the
// result - the pool's `((a+b)+(c+d)) * 0.25` in halves, and the merges' single f32 rounding of a product plus
// a residual before the half publication.

struct Params {
  count : u32,         // output values
  channels : u32,
  in_width : u32,
  in_height : u32,
  out_width : u32,
  out_height : u32,
  aux_a : u32,         // half index of the first scale vector
  aux_b : u32,
  flags : u32,
  pad0 : u32,
  pad1 : u32,
  pad2 : u32,
};

const FLAG_DUAL = 1u;   // also publish the raw half output

@group(0) @binding(0) var<uniform> params : Params;
@group(0) @binding(1) var<storage, read> in_f32 : array<f32>;
@group(0) @binding(2) var<storage, read> in_f16 : array<u32>;
@group(0) @binding(3) var<storage, read> in_e4 : array<u32>;
@group(0) @binding(4) var<storage, read> skip_e4 : array<u32>;
@group(0) @binding(8) var<storage, read> aux : array<u32>;      // half pairs
@group(0) @binding(6) var<storage, read_write> out_e4 : array<u32>;
@group(0) @binding(7) var<storage, read_write> out_f16 : array<u32>;

fn half_at(buffer: ptr<storage, array<u32>, read>, index: u32) -> f32 {
  return f16_to_f32(((*buffer)[index >> 1u] >> ((index & 1u) * 16u)) & 0xffffu);
}
fn e4_at(buffer: ptr<storage, array<u32>, read>, index: u32) -> f32 {
  return decode_e4m3(((*buffer)[index >> 2u] >> ((index & 3u) * 8u)) & 0xffu);
}
fn aux_half(index: u32) -> f32 {
  return f16_to_f32((aux[index >> 1u] >> ((index & 1u) * 16u)) & 0xffffu);
}

/// A 1D invocation index. A dispatch wider than 65535 workgroups is folded into the second dimension, which is
/// the only way to cover a full-resolution field: 576x512 alone is 294912 rows.
fn linear_index(id: vec3<u32>) -> u32 { return id.x + id.y * 65535u * 64u; }

fn publish(index: u32, values: array<f32, 4>) {
  var word = 0u;
  for (var i = 0u; i < 4u; i = i + 1u) { word = word | (encode_e4m3(f16_bits(values[i])) << (i * 8u)); }
  out_e4[index >> 2u] = word;
  if ((params.flags & FLAG_DUAL) != 0u) {
    out_f16[index >> 1u] = f16_bits(values[0]) | (f16_bits(values[1]) << 16u);
    out_f16[(index >> 1u) + 1u] = f16_bits(values[2]) | (f16_bits(values[3]) << 16u);
  }
}

/// The f32 input features, published to the half grid for the adapter's A operand.
@compute @workgroup_size(64)
fn convert_f32_to_f16(@builtin(global_invocation_id) id : vec3<u32>) {
  let base = linear_index(id) * 4u;
  if (base >= params.count) { return; }
  out_f16[base >> 1u] = f16_bits(in_f32[base]) | (f16_bits(in_f32[base + 1u]) << 16u);
  out_f16[(base >> 1u) + 1u] = f16_bits(in_f32[base + 2u]) | (f16_bits(in_f32[base + 3u]) << 16u);
}

/// The 2x2 box pool that takes a stage down a level: three half adds as (a+b)+(c+d), one half multiply by
/// 0.25, then the E4M3 publication. Every intermediate is a half, and the association is not free.
@compute @workgroup_size(64)
fn downsample(@builtin(global_invocation_id) id : vec3<u32>) {
  let base = linear_index(id) * 4u;
  if (base >= params.count) { return; }
  let c = base % params.channels;
  let pixel = base / params.channels;
  let ox = pixel % params.out_width;
  let oy = pixel / params.out_width;
  let sx = ox * 2u;
  let sy = oy * 2u;

  var values : array<f32, 4>;
  for (var i = 0u; i < 4u; i = i + 1u) { values[i] = 0.0; }
  if (sx + 1u < params.in_width && sy + 1u < params.in_height) {
    let i00 = (sy * params.in_width + sx) * params.channels + c;
    let i10 = i00 + params.channels;
    let i01 = ((sy + 1u) * params.in_width + sx) * params.channels + c;
    let i11 = i01 + params.channels;
    for (var i = 0u; i < 4u; i = i + 1u) {
      let top = round_f16(half_at(&in_f16, i00 + i) + half_at(&in_f16, i10 + i));
      let bottom = round_f16(half_at(&in_f16, i01 + i) + half_at(&in_f16, i11 + i));
      values[i] = round_f16(round_f16(top + bottom) * 0.25);
    }
  }
  publish(base, values);
}

/// A decoder stage's entry: the level below, doubled, plus the encoder skip times a learned per-channel scale.
/// The residual product is not published before the add - the whole expression rounds once.
@compute @workgroup_size(64)
fn upsample_residual(@builtin(global_invocation_id) id : vec3<u32>) {
  let base = linear_index(id) * 4u;
  if (base >= params.count) { return; }
  let c = base % params.channels;
  let pixel = base / params.channels;
  let source = (((pixel / params.out_width) >> 1u) * params.in_width +
                ((pixel % params.out_width) >> 1u)) * params.channels + c;
  var values : array<f32, 4>;
  for (var i = 0u; i < 4u; i = i + 1u) {
    values[i] = round_f16(half_at(&in_f16, source + i) +
                          e4_at(&skip_e4, base + i) * aux_half(params.aux_a + c + i));
  }
  publish(base, values);
}

/// The last merge before block 70: the level-0 decoder output doubled and scaled, plus block 0's own output
/// scaled. The first product is published to half, the second is not - it is folded into the final rounding.
@compute @workgroup_size(64)
fn post_blend(@builtin(global_invocation_id) id : vec3<u32>) {
  let base = linear_index(id) * 4u;
  if (base >= params.count) { return; }
  let c = base % params.channels;
  let pixel = base / params.channels;
  let source = (((pixel / params.out_width) >> 1u) * params.in_width +
                ((pixel % params.out_width) >> 1u)) * params.channels + c;
  var values : array<f32, 4>;
  for (var i = 0u; i < 4u; i = i + 1u) {
    let upsampled = round_f16(e4_at(&in_e4, source + i) * aux_half(params.aux_a + c + i));
    values[i] = round_f16(upsampled + e4_at(&skip_e4, base + i) * aux_half(params.aux_b + c + i));
  }
  publish(base, values);
}
