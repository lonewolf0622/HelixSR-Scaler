// Composing the window attention out of its transforms.
//
// The plain form is one workgroup per (window, query row, head): 64 invocations, one key each, staging K and
// V for eight queries. Everything below widens or specializes that without touching a product:
//
//   packed      the exact per-fragment exponents are shared alongside K and V, so the maximum over a
//               16-product group is four byte maxima rather than sixteen branches
//   multirow    32 queries share one K/V window instead of eight, on 256 invocations
//   vector      each four-component fragment is one vec4 in workgroup memory, loaded and multiplied as one
//   compact     scores are published straight into the value order, and the caches that die between phases
//               are reused - Q exponents before the scores, K exponents before the value reduction
//   float-sum   every aligned term is an integer below 2^20, so binary32 holds the fixed-point sum exactly
//   half-value  softmax weights are in [0,1] and V is finite E4M3, so scaling both by four makes every
//               nonzero product an exact normal half
//   unrolled    the fixed 16-term group bounds become literals
//   rotate      the packed exponent maximum as one rotate-and-max chain
//   fused       the cosine normalization happens in this kernel, off the raw qkv, so the network has no
//               separate normalize pass and never stores a normalized tensor
//   packed out  the attended values are published as the E4M3 words the tensor holds
//   layout      the geometry becomes pipeline overrides, so the window arithmetic folds
//
// Nothing here changes a product, a truncation, or a rounding. The parity harness is what says so.

import { windowBaseCode, WINDOW_LAYOUT_FIELDS } from './base.js';
import { TILED_WINDOW_WGSL } from './tiled.js';
import { packedWindowCode } from './packed.js';
import { multirowWindowCode } from './multirow.js';
import { vectorWindowCode } from './vector-loads.js';
import { compactScoreWindowCode } from './compact-scores.js';
import { floatSumWindowCode } from './float-sum.js';
import { halfValueWindowCode } from './half-value.js';
import { unrolledWindowCode } from './unrolled.js';
import { rotateExponentWindowCode } from './rotate-exponent.js';
import { fusedNormalizeWindowCode } from './normalize-fused.js';
import { packedOutputWindowCode } from './packed-output.js';
import { layoutWindowCode } from './layout.js';

/** 32 queries per workgroup on 512 invocations: the widest tile that stays inside 32 KB of shared memory. */
export const WINDOW_QUERIES = 32;
export const WINDOW_THREADS = 512;

export { WINDOW_LAYOUT_FIELDS };

let cached = null;

export function windowAttentionCode() {
  if (cached) return cached;
  const queries = WINDOW_QUERIES;
  const threads = WINDOW_THREADS;
  let code = compactScoreWindowCode(
    vectorWindowCode(multirowWindowCode(packedWindowCode(TILED_WINDOW_WGSL), queries), queries),
    queries, threads);
  code = rotateExponentWindowCode(unrolledWindowCode(halfValueWindowCode(floatSumWindowCode(code))));
  code = packedOutputWindowCode(fusedNormalizeWindowCode(code, threads), queries, threads);
  cached = layoutWindowCode(windowBaseCode() + code);
  return cached;
}
