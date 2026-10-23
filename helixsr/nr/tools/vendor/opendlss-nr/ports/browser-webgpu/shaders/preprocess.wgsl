// Input features from a recorded display proxy, for the parity harness.
//
// The demo makes its features from a rendered HDR frame (shaders/frame.wgsl); a fixture instead records the
// proxy the network was actually given, four f32 code values per pixel, so that the comparison starts from
// the same numbers native started from. Everything after the proxy is the same sixteen lanes either way.
//
// The three noise lanes are Box-Muller over the GPU's *approximate* f32 transcendentals. Their last bits
// depend on which transcendental unit evaluated them, which is why a fixture that carries its features uses
// those instead - this entry point is for the fixtures that carry only a proxy.

struct PreprocessParams {
  full_width : u32,
  full_height : u32,
  valid_width : u32,
  valid_height : u32,
  source_width : u32,
  source_height : u32,
  seed : u32,
  auto_mask : f32,          // > 0 enables the auto-mask lane pair
  local_tone : f32,
  local_structure : f32,
  skin_structure : f32,
  style : f32,
};

@group(0) @binding(0) var<uniform> pre : PreprocessParams;
@group(0) @binding(1) var<storage, read> proxy : array<f32>;        // rgba f32 code values
@group(0) @binding(5) var<storage, read_write> pre_features : array<f32>;

fn pre_hash_uniform(value: u32) -> f32 {
  var mixed = value;
  mixed = (mixed >> ((mixed >> 28u) + 4u)) ^ mixed;
  mixed = mixed * 0x108ef2d9u;
  let integer = ((mixed >> 30u) ^ (mixed >> 8u)) + 1u;
  return f32(integer) * bitcast<f32>(0x33800000u);
}

fn pre_gaussian3(x: u32, y: u32, seed: u32) -> vec3<f32> {
  var base = (x * 0x8da6b343u) ^ (seed * 0x9e3779b9u) ^ (y * 0xd8163841u) ^ 0x243f6a88u;
  base = (base >> ((base >> 28u) + 4u)) ^ base;
  base = base * 0x108ef2d9u;
  base = (base >> 22u) ^ base;
  let u0 = pre_hash_uniform(base * 0x2c9277b5u + 0xac564b05u);
  let u1 = pre_hash_uniform(base * 0xfa6dc5f9u + 0x4712a88eu);
  let u2 = pre_hash_uniform(base * 0xcaa5b80du + 0x21dd796bu);
  let u3 = pre_hash_uniform(base * 0x83232c31u + 0x3463e0acu);
  let radius0 = sqrt(log2(u0) * bitcast<f32>(0x3f317218u) * -2.0);
  let radius1 = sqrt(log2(u2) * bitcast<f32>(0x3f317218u) * -2.0);
  let angle0 = u1 * bitcast<f32>(0x40c90fdbu);
  let angle1 = u3 * bitcast<f32>(0x40c90fdbu);
  return vec3<f32>(round_f16(radius0 * cos(angle0)), round_f16(radius0 * sin(angle0)),
                   round_f16(radius1 * cos(angle1)));
}

@compute @workgroup_size(8, 8)
fn preprocess(@builtin(global_invocation_id) id : vec3<u32>) {
  if (id.x >= pre.full_width || id.y >= pre.full_height) { return; }
  // Outside the valid image the source is mirrored, while the noise still hashes the padded coordinate.
  let source_x = select(2u * pre.valid_width - id.x - 2u, id.x, id.x < pre.valid_width);
  let source_y = select(2u * pre.valid_height - id.y - 2u, id.y, id.y < pre.valid_height);
  let image_x = ((2u * source_x + 1u) * pre.source_width) / (2u * pre.valid_width);
  let image_y = ((2u * source_y + 1u) * pre.source_height) / (2u * pre.valid_height);
  let texel = (image_y * pre.source_width + image_x) * 4u;
  // Sample, then narrow to half, subtract a half, scale by an eighth - each step its own rounding.
  let r = round_f16(round_f16(round_f16(proxy[texel]) - 0.5) * 0.125);
  let g = round_f16(round_f16(round_f16(proxy[texel + 1u]) - 0.5) * 0.125);
  let b = round_f16(round_f16(round_f16(proxy[texel + 2u]) - 0.5) * 0.125);
  let noise = pre_gaussian3(id.x, id.y, pre.seed);
  let base = (id.y * pre.full_width + id.x) * 16u;
  pre_features[base + 0u] = noise.x;
  pre_features[base + 1u] = noise.y;
  pre_features[base + 2u] = noise.z;
  pre_features[base + 3u] = 1.0;
  pre_features[base + 4u] = r;
  pre_features[base + 5u] = g;
  pre_features[base + 6u] = b;
  pre_features[base + 7u] = r;
  pre_features[base + 8u] = g;
  pre_features[base + 9u] = b;
  pre_features[base + 10u] = pre.style / 128.0;
  pre_features[base + 11u] = round_f16(pre.local_tone);
  pre_features[base + 12u] = round_f16(select(pre.local_structure, 1.0, pre.auto_mask > 0.0));
  pre_features[base + 13u] = round_f16(select(-1.0,
      select(pre.skin_structure, pre.local_structure, pre.skin_structure < 0.0), pre.auto_mask > 0.0));
  pre_features[base + 14u] = round_f16(select(-1.0, pre.local_structure, pre.auto_mask > 0.0));
  pre_features[base + 15u] = 0.0;
}
