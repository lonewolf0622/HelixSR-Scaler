// Share exact exponent bytes as well as K/V. The biased pair sum excludes
// zero products without per-product branches; no native arithmetic changes.
export function packedWindowCode(code) {
  code = `var<workgroup> packed_q: array<u32, 64>;
var<workgroup> packed_k: array<u32, 512>;
var<workgroup> packed_v: array<u32, 512>;
var<workgroup> packed_scores: array<u32, 128>;
fn packed_window_exponents(v: vec4<f32>) -> u32 {
  let exponents = vec4<i32>(e4m3_exponent(v.x), e4m3_exponent(v.y),
    e4m3_exponent(v.z), e4m3_exponent(v.w));
  let bytes = select(vec4<u32>(0), vec4<u32>(exponents + vec4<i32>(106)), v != vec4<f32>(0));
  return bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);
}
fn maximum_window_exponent(current: u32, packed: u32) -> u32 {
  return max(max(max(max(current, packed & 255u), (packed >> 8u) & 255u),
    (packed >> 16u) & 255u), packed >> 24u);
}
` + code;
  for (const [name, left, right] of [
    ['tiled_qk', 'packed_q[query * 8u + c]', 'packed_k[c * 64u + key]'],
    ['tiled_value', 'packed_scores[query * 16u + c]', 'packed_v[c * 32u + component]'],
  ]) {
    const start = code.indexOf('  var exponent = -21;', code.indexOf(`fn ${name}(`));
    const end = code.indexOf('  let alignment', start);
    if (start < 0 || end < 0) throw new Error(`Missing ${name} exponent scan`);
    code = code.slice(0,start) + `  var maximum = 191u;
  if (accumulator != 0.0) { maximum = u32(f16_exponent(accumulator) + 212); }
  for (var c = start / 4u; c < (start + 16u) / 4u; c++) {
    maximum = maximum_window_exponent(maximum, ${left} + ${right});
  }
  let exponent = i32(maximum) - 212;
` + code.slice(end);
  }
  const scoreStart = code.indexOf('  // Every invocation owns one key;');
  if (scoreStart < 0) throw new Error('Missing window score phase');
  code = code.slice(0,scoreStart) + `  for (var linear = lane; linear < 512u; linear += 64u) {
    let key = linear % 64u;
    let first = (linear / 64u) * 4u;
    packed_k[linear] = packed_window_exponents(vec4<f32>(vec4<f16>(
      tiled_k[first * 64u + key], tiled_k[(first + 1u) * 64u + key],
      tiled_k[(first + 2u) * 64u + key], tiled_k[(first + 3u) * 64u + key])));
    let component = linear % 32u;
    let c = (linear / 32u) * 4u;
    packed_v[linear] = packed_window_exponents(vec4<f32>(vec4<f16>(
      tiled_v[inverse_tiled_token(c) * 32u + component],
      tiled_v[inverse_tiled_token(c + 1u) * 32u + component],
      tiled_v[inverse_tiled_token(c + 2u) * 32u + component],
      tiled_v[inverse_tiled_token(c + 3u) * 32u + component])));
  }
  packed_q[lane] = packed_window_exponents(vec4<f32>(vec4<f16>(tiled_q[lane * 4u],
    tiled_q[lane * 4u + 1u], tiled_q[lane * 4u + 2u], tiled_q[lane * 4u + 3u])));
  workgroupBarrier();
` + code.slice(scoreStart);
  const valueStart = code.lastIndexOf('  for (var linear = lane; linear < 256u; linear += 64u) {');
  if (valueStart < 0) throw new Error('Missing window value phase');
  return code.slice(0,valueStart) + `  for (var linear = lane; linear < 128u; linear += 64u) {
    let query = linear / 16u;
    let c = (linear % 16u) * 4u;
    packed_scores[linear] = packed_window_exponents(vec4<f32>(vec4<f16>(
      tiled_score(query, c), tiled_score(query, c + 1u),
      tiled_score(query, c + 2u), tiled_score(query, c + 3u))));
  }
  workgroupBarrier();
` + code.slice(valueStart);
}
