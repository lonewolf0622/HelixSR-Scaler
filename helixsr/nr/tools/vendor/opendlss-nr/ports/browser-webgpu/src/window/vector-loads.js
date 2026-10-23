// Store each native four-K fragment together, so product loops issue vector
// shared loads instead of repeated scalar addresses and token permutations.
export function vectorWindowCode(code, queries) {
  code = code.replace(/var<workgroup> tiled_[qkv]: array<f16, \d+>;\n/g, '');
  code = `var<workgroup> vector_q: array<vec4<f16>, ${queries * 8}>;
var<workgroup> vector_k: array<vec4<f16>, 576>;
var<workgroup> vector_v: array<vec4<f16>, 544>;
var<workgroup> vector_scores: array<vec4<f16>, ${queries * 16}>;
` + code;
  for (const [name, left, right] of [
    ['tiled_qk', 'vector_q[query * 8u + c]', 'vector_k[key * 9u + c]'],
    ['tiled_value', 'vector_scores[query * 16u + c]', 'vector_v[component * 17u + c]'],
  ]) {
    const begin = code.indexOf('  for (var c = start; c < start + 16u; c++) {', code.indexOf(`fn ${name}(`));
    const end = code.indexOf('  return round_attention_half', begin);
    if (begin < 0 || end < 0) throw new Error(`Missing ${name} product loop`);
    const terms = [...'xyzw'].map(p => `    sum += i32(trunc((a.${p} * b.${p}) * alignment));`).join('\n');
    code = code.slice(0, begin) + `  for (var c = start / 4u; c < (start + 16u) / 4u; c++) {
    let a = vec4<f32>(${left});
    let b = vec4<f32>(${right});
${terms}
  }
` + code.slice(end);
  }
  const begin = code.indexOf('  for (var linear = lane; linear < 2048u;', code.indexOf('fn attend_window_tiled'));
  const end = code.indexOf('  // Every invocation owns one key;', begin);
  if (begin < 0 || end < 0) throw new Error('Missing window cache loading');
  const kvParts = [...'xyzw'].map((p,i) => `
    {
      let token = inverse_tiled_token(first + ${i}u);
      let vx = wx + i32(token % 8u); let vy = wy + i32(token / 8u);
      if (vx >= 0 && vy >= 0 && vx < i32(attention_params.width) && vy < i32(attention_params.height)) {
        v.${p} = f16(attention_qkv[(u32(vy) * attention_params.width + u32(vx)) * attention_params.channels * 3u
          + head * 96u + 64u + component]);
      }
    }`).join('');
  const loadQuad = (name, base) => [...'xyzw'].map((p,i) => `      ${name}.${p} = f16(attention_qkv[${base} + ${i}u]);`).join('\n');
  code = code.slice(0, begin) + `  for (var linear = lane; linear < 512u; linear += 256u) {
    let key = linear % 64u; let c = linear / 64u;
    let x = wx + i32(key % 8u); let y = wy + i32(key / 8u);
    var k = vec4<f16>(0);
    if (x >= 0 && y >= 0 && x < i32(attention_params.width) && y < i32(attention_params.height)) {
      let base = (u32(y) * attention_params.width + u32(x)) * attention_params.channels * 3u
        + head * 96u + 32u + c * 4u;
${loadQuad('k', 'base')}
    }
    vector_k[key * 9u + c] = k;
    packed_k[linear] = packed_window_exponents(vec4<f32>(k));
    let component = linear % 32u; let first = (linear / 32u) * 4u;
    var v = vec4<f16>(0);
${kvParts}
    vector_v[component * 17u + first / 4u] = v;
    packed_v[linear] = packed_window_exponents(vec4<f32>(v));
  }
  for (var linear = lane; linear < ${queries * 8}u; linear += 256u) {
    let query = query_origin + linear / 8u;
    let x = wx + i32(query % 8u); let y = wy + i32(query / 8u);
    var q = vec4<f16>(0);
    if (x >= 0 && y >= 0 && x < i32(attention_params.width) && y < i32(attention_params.height)) {
      let base = (u32(y) * attention_params.width + u32(x)) * attention_params.channels * 3u
        + head * 96u + (linear % 8u) * 4u;
${loadQuad('q', 'base')}
    }
    vector_q[linear] = q;
    packed_q[linear] = packed_window_exponents(vec4<f32>(q));
  }
  workgroupBarrier();
` + code.slice(end);
  return code.replace(`    packed_scores[linear] = packed_window_exponents(vec4<f32>(vec4<f16>(
      tiled_score(query, c), tiled_score(query, c + 1u),
      tiled_score(query, c + 2u), tiled_score(query, c + 3u))));`, `    let scores = vec4<f16>(tiled_score(query, c), tiled_score(query, c + 1u),
      tiled_score(query, c + 2u), tiled_score(query, c + 3u));
    vector_scores[linear] = scores;
    packed_scores[linear] = packed_window_exponents(vec4<f32>(scores));`);
}
