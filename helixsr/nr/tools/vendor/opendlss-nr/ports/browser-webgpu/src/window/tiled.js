// Eight queries share one K/V window, so K and V are staged once for the eight instead of once each.
// Every publication keeps the F13/FP16 contract the rest of the network is held to.
export const TILED_WINDOW_WGSL = String.raw`
var<workgroup> tiled_q: array<f16, 256>;
var<workgroup> tiled_k: array<f16, 2048>;
var<workgroup> tiled_v: array<f16, 2048>;
var<workgroup> tiled_scores: array<f16, 512>;
var<workgroup> tiled_reciprocals: array<f16, 8>;

fn tiled_qk(query: u32, key: u32, start: u32, accumulator: f32) -> f32 {
  var exponent = -21;
  if (accumulator != 0.0) { exponent = f16_exponent(accumulator); }
  for (var c = start; c < start + 16u; c++) {
    let q = f32(tiled_q[query * 32u + c]);
    let k = f32(tiled_k[c * 64u + key]);
    if (q != 0.0 && k != 0.0) { exponent = max(exponent, e4m3_exponent(q) + e4m3_exponent(k)); }
  }
  let alignment = bitcast<f32>(u32(140 - exponent) << 23u);
  var sum = i32(trunc(accumulator * alignment));
  for (var c = start; c < start + 16u; c++) {
    let q = f32(tiled_q[query * 32u + c]);
    let k = f32(tiled_k[c * 64u + key]);
    sum += i32(trunc((q * k) * alignment));
  }
  return round_attention_half(f32(sum) * bitcast<f32>(u32(exponent + 114) << 23u));
}

fn tiled_score(query: u32, index: u32) -> f16 {
  return tiled_scores[query * 64u + inverse_tiled_token(index)];
}

fn tiled_softmax_pair(query: u32, pair: u32, parity: u32) -> f16 {
  let key = pair * 2u + parity;
  let blocks01 = tiled_score(query, key) + tiled_score(query, key + 8u);
  let blocks23 = tiled_score(query, key + 16u) + tiled_score(query, key + 24u);
  let blocks45 = tiled_score(query, key + 32u) + tiled_score(query, key + 40u);
  let blocks67 = tiled_score(query, key + 48u) + tiled_score(query, key + 56u);
  return ((blocks01 + blocks23) + blocks45) + blocks67;
}

fn tiled_softmax_sum(query: u32) -> f16 {
  let even = ((tiled_softmax_pair(query, 0u, 0u) + tiled_softmax_pair(query, 1u, 0u))
    + tiled_softmax_pair(query, 2u, 0u)) + tiled_softmax_pair(query, 3u, 0u);
  let odd = ((tiled_softmax_pair(query, 0u, 1u) + tiled_softmax_pair(query, 1u, 1u))
    + tiled_softmax_pair(query, 2u, 1u)) + tiled_softmax_pair(query, 3u, 1u);
  return even + odd;
}

fn tiled_value(query: u32, component: u32, start: u32, accumulator: f32) -> f32 {
  var exponent = -21;
  if (accumulator != 0.0) { exponent = f16_exponent(accumulator); }
  for (var c = start; c < start + 16u; c++) {
    let key = inverse_tiled_token(c);
    let weight = f32(tiled_scores[query * 64u + key]);
    let v = f32(tiled_v[key * 32u + component]);
    if (weight != 0.0 && v != 0.0) { exponent = max(exponent, e4m3_exponent(weight) + e4m3_exponent(v)); }
  }
  let alignment = bitcast<f32>(u32(140 - exponent) << 23u);
  var sum = i32(trunc(accumulator * alignment));
  for (var c = start; c < start + 16u; c++) {
    let key = inverse_tiled_token(c);
    let weight = f32(tiled_scores[query * 64u + key]);
    let v = f32(tiled_v[key * 32u + component]);
    sum += i32(trunc((weight * v) * alignment));
  }
  return round_attention_half(f32(sum) * bitcast<f32>(u32(exponent + 114) << 23u));
}

@compute @workgroup_size(64)
fn attend_window_tiled(@builtin(local_invocation_index) lane: u32,
  @builtin(workgroup_id) gid: vec3<u32>) {
  let windows_x = (attention_params.width + attention_params.shift_x + 7u) / 8u;
  let windows_y = (attention_params.height + attention_params.shift_y + 7u) / 8u;
  let task = gid.y + gid.z * 65535u;
  if (task >= windows_x * windows_y * 8u) { return; }
  let window = task / 8u;
  let query_row = task % 8u;
  let wx = i32((window % windows_x) * 8u) - i32(attention_params.shift_x);
  let wy = i32((window / windows_x) * 8u) - i32(attention_params.shift_y);
  let qy = wy + i32(query_row);
  if (qy < 0 || qy >= i32(attention_params.height)) { return; }
  let head = gid.x;
  for (var linear = lane; linear < 2048u; linear += 64u) {
    let key = linear / 32u;
    let component = linear % 32u;
    let x = wx + i32(key & 7u);
    let y = wy + i32(key >> 3u);
    var k = 0.0h;
    var v = 0.0h;
    if (x >= 0 && y >= 0 && x < i32(attention_params.width) && y < i32(attention_params.height)) {
      let base = (u32(y) * attention_params.width + u32(x)) * attention_params.channels * 3u
        + head * 96u + component;
      k = f16(attention_qkv[base + 32u]);
      v = f16(attention_qkv[base + 64u]);
    }
    tiled_k[component * 64u + key] = k;
    tiled_v[linear] = v;
  }
  for (var linear = lane; linear < 256u; linear += 64u) {
    let x = wx + i32(linear / 32u);
    var q = 0.0h;
    if (x >= 0 && x < i32(attention_params.width)) {
      let base = (u32(qy) * attention_params.width + u32(x)) * attention_params.channels * 3u
        + head * 96u + linear % 32u;
      q = f16(attention_qkv[base]);
    }
    tiled_q[linear] = q;
  }
  workgroupBarrier();
  // Every invocation owns one key; all 64 keys execute concurrently.
  for (var query = 0u; query < 8u; query++) {
    var prior = 0.0;
    if (attention_params.use_relative_bias != 0u) {
      prior = load_relative_bias(head, query_row * 8u + query, lane);
    }
    var score = tiled_qk(query, lane, 0u, prior);
    score = tiled_qk(query, lane, 16u, score);
    tiled_scores[query * 64u + lane] = native_exp_weight(f16(score));
  }
  workgroupBarrier();
  if (lane < 8u) { tiled_reciprocals[lane] = f16(1.0 / f32(tiled_softmax_sum(lane))); }
  workgroupBarrier();
  for (var query = 0u; query < 8u; query++) {
    let weight = tiled_scores[query * 64u + lane] * tiled_reciprocals[query];
    tiled_scores[query * 64u + lane] = f16(publish_fp8(f32(weight)));
  }
  workgroupBarrier();
  for (var linear = lane; linear < 256u; linear += 64u) {
    let query = linear / 32u;
    let component = linear % 32u;
    var value = 0.0;
    for (var group = 0u; group < 4u; group++) {
      value = tiled_value(query, component, group * 16u, value);
    }
    let qx = wx + i32(query);
    if (qx >= 0 && qx < i32(attention_params.width)) {
      let index = (u32(qy) * attention_params.width + u32(qx)) * attention_params.channels
        + head * 32u + component;
      attention_output[index] = publish_fp8(value);
    }
  }
}
`;
