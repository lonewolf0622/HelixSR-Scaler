// Publish each four-key fragment directly in native value order. One vector
// score cache serves scoring, the unchanged softmax tree and value reduction.
export function compactScoreWindowCode(code, queries, threads = 256) {
  code = code.replace(/var<workgroup> tiled_scores: array<f16, \d+>;\n/, '')
    .replace('return tiled_scores[query * 64u + inverse_tiled_token(index)];',
      'return vector_scores[query * 16u + index / 4u][index % 4u];');
  const begin = code.indexOf('  // Every invocation owns one key;');
  const end = code.indexOf('  workgroupBarrier();',begin);
  if (begin < 0 || end < 0) throw new Error('Missing window score phase');
  const terms = [...'xyzw'].map((p,i)=>`    {
      let key = inverse_tiled_token(first + ${i}u);
      var prior = 0.0;
      if (attention_params.use_relative_bias != 0u) { prior = load_relative_bias(head, query_origin + query, key); }
      let low = tiled_qk(query, key, 0u, prior);
      score.${p} = native_exp_weight(f16(tiled_qk(query, key, 16u, low)));
    }`).join('\n');
  code = code.slice(0,begin) + `  for (var linear = lane; linear < ${queries * 16}u; linear += 256u) {
    let query = linear / 16u; let first = (linear % 16u) * 4u;
    var score = vec4<f16>(0);
${terms}
    vector_scores[linear] = score;
  }
` + code.slice(end);
  const weight = code.indexOf('    let weight = tiled_scores[');
  const normalizeStart = code.lastIndexOf('  for (var linear = lane;',weight);
  const cache = code.indexOf('    let scores = vec4<f16>(tiled_score',weight);
  const cacheEnd = code.indexOf('  workgroupBarrier();',cache);
  if (weight < 0 || normalizeStart < 0 || cache < 0 || cacheEnd < 0) throw new Error('Missing normalized score publication');
  const quantize = [...'xyzw'].map(p=>`    normalized.${p} = f16(publish_fp8(f32(weight.${p})));`).join('\n');
  code = code.slice(0,normalizeStart) + `  for (var linear = lane; linear < ${queries * 16}u; linear += 256u) {
    let weight = vector_scores[linear] * vec4<f16>(tiled_reciprocals[linear / 16u]);
    var normalized = vec4<f16>(0);
${quantize}
    vector_scores[linear] = normalized;
    packed_scores[linear] = packed_window_exponents(vec4<f32>(normalized));
  }
` + code.slice(cacheEnd);
  // Q exponents die before score exponents are published; K exponents die
  // before value reduction. Reuse those disjoint caches with a barrier.
  code = code.replace(/var<workgroup> packed_q: array<u32, \d+>;\n/, '')
    .replaceAll('packed_q[', 'packed_scores[')
    .replace('var<workgroup> packed_v: array<u32, 512>;\n', '')
    .replace('    packed_v[linear] = packed_window_exponents(vec4<f32>(v));\n', '')
    .replaceAll('packed_v[', 'packed_k[');
  const publish = code.indexOf('    let weight = vector_scores[linear]');
  const publishStart = code.lastIndexOf('  for (var linear = lane;',publish);
  code = code.slice(0,publishStart) + `  for (var linear = lane; linear < 512u; linear += 256u) {
    let component = linear % 32u; let c = linear / 32u;
    packed_k[linear] = packed_window_exponents(vec4<f32>(vector_v[component * 17u + c]));
  }
` + code.slice(publishStart);
  return code.replace('@workgroup_size(256)', `@workgroup_size(${threads})`)
    .replaceAll('linear += 256u', `linear += ${threads}u`);
}
