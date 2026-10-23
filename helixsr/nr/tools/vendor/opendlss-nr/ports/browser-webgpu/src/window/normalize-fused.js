// Normalize directly into the attention tile. Eight lanes reduce one token's
// Q/K together, preserving the native first-pair FMA and half addition tree.
export function fusedNormalizeWindowCode(code, threads = 256) {
  code = `var<workgroup> window_norms: array<vec2<f16>, 64>;
` + code;
  const loads = [0, 8, 16, 24].map(offset => `
      q${offset} = f16(attention_qkv[base + ${offset}u]);
      k${offset} = f16(attention_qkv[base + ${32 + offset}u]);`).join('');
  code = code.replace('  let head = gid.x;', `  let head = gid.x;
  for (var first = 0u; first < 64u; first += ${threads / 8}u) {
    let token = first + lane / 8u; let component = lane % 8u;
    let nx = wx + i32(token % 8u); let ny = wy + i32(token / 8u);
    var q0 = 0.0h; var q8 = 0.0h; var q16 = 0.0h; var q24 = 0.0h;
    var k0 = 0.0h; var k8 = 0.0h; var k16 = 0.0h; var k24 = 0.0h;
    if (nx >= 0 && ny >= 0 && nx < i32(attention_params.width) && ny < i32(attention_params.height)) {
      let base = (u32(ny) * attention_params.width + u32(nx)) * attention_params.channels * 3u
        + head * 96u + component;
${loads}
    }
    let q_low = nr_norm_fma(q0, q0, q16 * q16);
    let q_high = nr_norm_fma(q8, q8, q24 * q24);
    let k_low = nr_norm_fma(k0, k0, k16 * k16);
    let k_high = nr_norm_fma(k8, k8, k24 * k24);
    vector_k[lane] = vec4<f16>(q_low + q_high, k_low + k_high, 0.0h, 0.0h);
    workgroupBarrier();
    for (var stride = 4u; stride > 0u; stride >>= 1u) {
      if (component < stride) { vector_k[lane] += vector_k[lane + stride]; }
      workgroupBarrier();
    }
    if (component == 0u) {
      let squares = vector_k[lane];
      // 1/sqrt rather than inverseSqrt: WGSL gives the latter an accuracy allowance, and while the half
      // rounding would absorb it almost everywhere, "almost" is not what this port is checked against.
      window_norms[token] = vec2<f16>(f16(1.0 / sqrt(f32(squares.x))), f16(1.0 / sqrt(f32(squares.y))));
    }
    workgroupBarrier();
  }
  let scale = attention_scales[head];
`);
  for (const part of 'xyzw') {
    code = code.replace(new RegExp(`k\\.${part} = f16\\((attention_qkv\\[base \\+ \\d+u\\])\\);`),
      `k.${part} = f16(publish_fp8(f32(f16($1) * window_norms[key].y)));`);
    code = code.replace(new RegExp(`q\\.${part} = f16\\((attention_qkv\\[base \\+ \\d+u\\])\\);`),
      `q.${part} = f16(publish_fp8(f32(f16(f16($1) * window_norms[query].x) * f16(scale))));`);
    code = code.replace(new RegExp(`v\\.${part} = f16\\((attention_qkv\\[[\\s\\S]*?\\])\\);`),
      `v.${part} = f16(publish_fp8(f32($1)));`);
  }
  return code;
}
