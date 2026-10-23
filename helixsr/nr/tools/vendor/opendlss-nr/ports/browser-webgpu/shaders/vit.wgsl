// The global ViT at the bottom of the U-net: eight 1024-channel blocks whose attention is over every token of
// the coarsest level at once, with no window and no learned prior.
//
// It differs from the window attention in four ways that all matter to the result:
//
//   * the query carries an extra sqrt(32) alongside the learned scale, as a separate half multiply;
//   * the normalization's pair squares are summed in f32 and rounded once, not folded by a half fma;
//   * the exponential has its own constants and its own shift, and it is evaluated in halves;
//   * the attention weights are published unnormalized and the reciprocal is applied to the value sum
//     afterwards, rather than to the weights first.
//
// The token count is padded up to a multiple of 64. Padding tokens have zero key and value, so they add
// nothing to the numerator - but their score is zero and zero still exponentiates, so the denominator carries
// `padding * exp(0)`, which is subtracted once at the end. Leaving them out of the sum instead would be a
// different function, not an optimization.

override PADDED_TOKENS : u32 = 64u;

struct Params {
  tokens : u32,
  heads : u32,
  channels : u32,      // heads * 32
  padded : u32,
};

@group(0) @binding(0) var<uniform> params : Params;
@group(0) @binding(1) var<storage, read> qkv_half : array<u32>;       // raw half QKV [tokens][channels*3]
@group(0) @binding(2) var<storage, read> aux : array<u32>;            // f32 per-head scales
@group(0) @binding(4) var<storage, read> normalized : array<u32>;     // E4M3 [padded][channels*3]
@group(0) @binding(5) var<storage, read_write> out_normalized : array<u32>;
@group(0) @binding(6) var<storage, read_write> attended : array<u32>; // E4M3 [tokens][channels]

fn qkv_at(index: u32) -> f32 {
  return f16_to_f32((qkv_half[index >> 1u] >> ((index & 1u) * 16u)) & 0xffffu);
}
fn normalized_at(index: u32) -> f32 {
  return decode_e4m3((normalized[index >> 2u] >> ((index & 3u) * 8u)) & 0xffu);
}

fn tree_sum16(r: array<f32, 16>) -> f32 {
  var s8 : array<f32, 8>;
  for (var c = 0u; c < 8u; c = c + 1u) { s8[c] = round_f16(r[c] + r[c + 8u]); }
  var s4 : array<f32, 4>;
  for (var c = 0u; c < 4u; c = c + 1u) { s4[c] = round_f16(s8[c] + s8[c + 4u]); }
  return round_f16(round_f16(s4[0] + s4[2]) + round_f16(s4[1] + s4[3]));
}

/// The ViT's variant: the pair square is summed in f32 and published once, where the window blocks fuse it.
fn vit_norm(values: array<f32, 32>) -> f32 {
  var r : array<f32, 16>;
  for (var c = 0u; c < 16u; c = c + 1u) {
    let high_square = round_f16(values[c + 16u] * values[c + 16u]);
    r[c] = round_f16(values[c] * values[c] + high_square);
  }
  return round_f16(1.0 / sqrt(tree_sum16(r)));
}

@compute @workgroup_size(64)
fn vit_normalize(@builtin(global_invocation_id) id : vec3<u32>) {
  // One invocation per (token, head, four channels), over a folded 1D grid.
  let per_token = params.heads * 8u;
  let linear = id.x + id.y * 65535u * 64u;
  let token = linear / per_token;
  let head = (linear % per_token) / 8u;
  let quad = (linear % 8u) * 4u;
  if (token >= params.tokens || head >= params.heads) { return; }

  let base = token * params.channels * 3u + head * 96u;
  var q : array<f32, 32>;
  var k : array<f32, 32>;
  for (var c = 0u; c < 32u; c = c + 1u) {
    q[c] = qkv_at(base + c);
    k[c] = qkv_at(base + 32u + c);
  }
  let q_norm = vit_norm(q);
  let k_norm = vit_norm(k);
  let learned = round_f16(bitcast<f32>(aux[head]));
  let head_scale = round_f16(sqrt(32.0));

  var word_q = 0u;
  var word_k = 0u;
  var word_v = 0u;
  for (var i = 0u; i < 4u; i = i + 1u) {
    let c = quad + i;
    // Three half multiplies, in this order: the norm, then sqrt(head_dim), then the learned scale.
    let nq = round_f16(round_f16(round_f16(q[c] * q_norm) * head_scale) * learned);
    word_q = word_q | (encode_e4m3(f16_bits(nq)) << (i * 8u));
    word_k = word_k | (encode_e4m3(f16_bits(round_f16(k[c] * k_norm))) << (i * 8u));
    word_v = word_v | (encode_e4m3(f16_bits(qkv_at(base + 64u + c))) << (i * 8u));
  }
  out_normalized[(base + quad) >> 2u] = word_q;
  out_normalized[(base + 32u + quad) >> 2u] = word_k;
  out_normalized[(base + 64u + quad) >> 2u] = word_v;
}

// ---------------------------------------------------------------------------------------------------------
// Attention: one workgroup per (head, query), the 64 threads splitting the keys and then the components.
// ---------------------------------------------------------------------------------------------------------

var<workgroup> scores : array<f32, PADDED_TOKENS>;
var<workgroup> weight_values : array<f32, PADDED_TOKENS>;
var<workgroup> query_vector : array<f32, 32>;
var<workgroup> reciprocal : f32;

fn softmax_pair(base: u32, pair: u32, parity: u32) -> f32 {
  let key = base + pair * 2u + parity;
  let a = round_f16(scores[key] + scores[key + 8u]);
  let b = round_f16(scores[key + 16u] + scores[key + 24u]);
  let c = round_f16(scores[key + 32u] + scores[key + 40u]);
  let d = round_f16(scores[key + 48u] + scores[key + 56u]);
  return round_f16(round_f16(round_f16(a + b) + c) + d);
}

fn softmax64(base: u32) -> f32 {
  let e0 = round_f16(softmax_pair(base, 0u, 0u) + softmax_pair(base, 1u, 0u));
  let e1 = round_f16(e0 + softmax_pair(base, 2u, 0u));
  let even = round_f16(e1 + softmax_pair(base, 3u, 0u));
  let o0 = round_f16(softmax_pair(base, 0u, 1u) + softmax_pair(base, 1u, 1u));
  let o1 = round_f16(o0 + softmax_pair(base, 2u, 1u));
  let odd = round_f16(o1 + softmax_pair(base, 3u, 1u));
  return round_f16(even + odd);
}

@compute @workgroup_size(64)
fn vit_attend(@builtin(workgroup_id) group : vec3<u32>, @builtin(local_invocation_index) thread : u32) {
  let head = group.x;
  let token = group.y + group.z * 65535u;
  if (token >= params.tokens) { return; }
  let stride3 = params.channels * 3u;
  let head_base = head * 96u;

  if (thread < 32u) {
    query_vector[thread] = normalized_at(token * stride3 + head_base + thread);
  }
  workgroupBarrier();

  var a : array<f32, 16>;
  var b : array<f32, 16>;
  for (var key = thread; key < PADDED_TOKENS; key = key + 64u) {
    var score = 0.0;
    for (var half_step = 0u; half_step < 2u; half_step = half_step + 1u) {
      let c0 = half_step * 16u;
      for (var i = 0u; i < 16u; i = i + 1u) {
        a[i] = query_vector[c0 + i];
        b[i] = normalized_at(key * stride3 + head_base + 32u + c0 + i);
      }
      score = ada_fp8_fdpa16(a, b, score);
    }
    scores[key] = vit_exp_weight(score);
  }
  workgroupBarrier();

  if (thread == 0u) {
    var total = 0.0;
    for (var base = 0u; base < PADDED_TOKENS; base = base + 64u) { total = round_f16(total + softmax64(base)); }
    let padding = PADDED_TOKENS - params.tokens;
    if (padding > 0u) {
      let correction = round_f16(vit_exp_weight(0.0) * f32(padding));
      total = round_f16(total - correction);
    }
    reciprocal = round_f16(1.0 / total);
  }
  // The weights are published unnormalized; the reciprocal reaches the result through the value sum.
  for (var key = thread; key < PADDED_TOKENS; key = key + 64u) {
    weight_values[key] = decode_e4m3(encode_e4m3(f16_bits(scores[key])));
  }
  workgroupBarrier();

  // Each of the first 32 threads takes one component and walks every key in k32 steps.
  if (thread < 32u) {
    var value = 0.0;
    for (var kb = 0u; kb < PADDED_TOKENS; kb = kb + 32u) {
      for (var half_step = 0u; half_step < 2u; half_step = half_step + 1u) {
        let k0 = kb + half_step * 16u;
        for (var i = 0u; i < 16u; i = i + 1u) {
          a[i] = weight_values[k0 + i];
          b[i] = normalized_at((k0 + i) * stride3 + head_base + 64u + thread);
        }
        value = ada_fp8_fdpa16(a, b, value);
      }
    }
    scores[thread] = round_f16(value * reciprocal);   // reuse: the scores are finished with
  }
  workgroupBarrier();

  if (thread < 8u) {
    var word = 0u;
    for (var i = 0u; i < 4u; i = i + 1u) {
      word = word | (encode_e4m3(f16_bits(scores[thread * 4u + i])) << (i * 8u));
    }
    attended[((token * params.channels + head * 32u) >> 2u) + thread] = word;
  }
}
