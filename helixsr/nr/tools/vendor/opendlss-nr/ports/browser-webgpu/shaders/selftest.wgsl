// One entry point per case in src/numerics_cases.js. Concatenated after numerics.wgsl at load time - WGSL has
// no include, and the port keeps the shared arithmetic in one file rather than pasting it into each kernel.
//
// Cases whose input is just the index bind a one-word placeholder for `inputs`, because a bind group has to
// supply every binding the module declares.

@group(0) @binding(0) var<storage, read> inputs : array<u32>;
@group(0) @binding(1) var<storage, read_write> results : array<u32>;

@compute @workgroup_size(64)
fn case_f16_bits(@builtin(global_invocation_id) id : vec3<u32>) {
  let i = id.x;
  if (i >= arrayLength(&results)) { return; }
  results[i] = f16_bits(bitcast<f32>(inputs[i]));
}

@compute @workgroup_size(64)
fn case_e4m3_encode(@builtin(global_invocation_id) id : vec3<u32>) {
  let i = id.x;
  if (i >= arrayLength(&results)) { return; }
  results[i] = encode_e4m3(i);
}

@compute @workgroup_size(64)
fn case_e4m3_decode(@builtin(global_invocation_id) id : vec3<u32>) {
  let i = id.x;
  if (i >= arrayLength(&results)) { return; }
  results[i] = bitcast<u32>(decode_e4m3(i));
}

@compute @workgroup_size(64)
fn case_silu(@builtin(global_invocation_id) id : vec3<u32>) {
  let i = id.x;
  if (i >= arrayLength(&results)) { return; }
  results[i] = f16_bits(mp_cubic_silu(f16_to_f32(i)));
}

@compute @workgroup_size(64)
fn case_exp_weight(@builtin(global_invocation_id) id : vec3<u32>) {
  let i = id.x;
  if (i >= arrayLength(&results)) { return; }
  results[i] = f16_bits(exp_weight(f16_to_f32(i)));
}

@compute @workgroup_size(64)
fn case_vit_exp_weight(@builtin(global_invocation_id) id : vec3<u32>) {
  let i = id.x;
  if (i >= arrayLength(&results)) { return; }
  results[i] = f16_bits(vit_exp_weight(f16_to_f32(i)));
}

@compute @workgroup_size(64)
fn case_fdpa_fp8(@builtin(global_invocation_id) id : vec3<u32>) {
  let c = id.x;
  if (c >= arrayLength(&results)) { return; }
  let base = c * 33u;
  var a : array<f32, 16>;
  var b : array<f32, 16>;
  for (var i = 0u; i < 16u; i = i + 1u) {
    a[i] = bitcast<f32>(inputs[base + i]);
    b[i] = bitcast<f32>(inputs[base + 16u + i]);
  }
  results[c] = f16_bits(ada_fp8_fdpa16(a, b, bitcast<f32>(inputs[base + 32u])));
}

@compute @workgroup_size(64)
fn case_fdpa_f16(@builtin(global_invocation_id) id : vec3<u32>) {
  let c = id.x;
  if (c >= arrayLength(&results)) { return; }
  let base = c * 17u;
  var a : array<f32, 8>;
  var b : array<f32, 8>;
  for (var i = 0u; i < 8u; i = i + 1u) {
    a[i] = bitcast<f32>(inputs[base + i]);
    b[i] = bitcast<f32>(inputs[base + 8u + i]);
  }
  results[c] = f16_bits(ada_f16_fdpa8(a, b, bitcast<f32>(inputs[base + 16u])));
}
