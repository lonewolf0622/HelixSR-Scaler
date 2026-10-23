// Composing the transforms, and choosing which composition a given matrix multiply gets.
//
// The order below is not free. Each transform reads the text the one before it produced, so a transform can
// only assume what its predecessors guarantee:
//
//   retile        64 invocations -> 256, four outputs each instead of sixteen
//   quad          the four outputs of one invocation reduce as one vec4
//   packed/padded the shared exponents move into their own tile, nine words per column to spread the banks
//   weight-words  four adjacent K bytes are one word, so the fragment address is computed once
//   vector-loads  the operand tiles become vec4 arrays
//   half-exponents   exponents are exact small integers, so they fit a half vector
//   bounded-half     every weight is |w| <= 9, so scaling both operands by four makes every product an exact
//                    normal half - checked at load, because the whole chain below depends on it
//   unrolled      the 4/8/16-term group bounds become literals
//   group/seeded exponent   one horizontal maximum per group, seeded from the first fragment
//   shape/layout  dimensions and strides become pipeline overrides, so the partial-tile guards fold away
//   bit-quant     E4M3 rounding directly in binary32
//   silu-table    the cubic activation becomes a 256 KiB lookup over every half input
//   weight-table  E4M3 decode and exponent extraction become a 1 KiB lookup over every byte
//   storage       the operands and publications take the format the tensors are actually stored in
//   tile128       two output quads per invocation
//
// Nothing here changes a product, a truncation, or a rounding. The parity harness is what says so.

import { productionMatmulCode } from './base.js';
import { retileMatmulCode, quadMatmulCode, batchedMatmulCode } from './tiled.js';
import { packedExponentMatmulCode } from './packed-exponents.js';
import { paddedMatmulCode } from './padded.js';
import { weightWordMatmulCode } from './weight-words.js';
import { vectorLoadMatmulCode } from './vector-loads.js';
import { halfExponentMatmulCode } from './half-exponents.js';
import { boundedHalfMatmulCode } from './bounded-half.js';
import { unrolledMatmulCode } from './unrolled.js';
import { groupExponentMatmulCode } from './group-exponent.js';
import { seededExponentMatmulCode } from './seeded-exponent.js';
import { shapeMatmulCode } from './shape.js';
import { layoutMatmulCode } from './layout.js';
import { bitQuantMatmulCode } from './bit-quant.js';
import { siluTableMatmulCode } from './silu-table.js';
import { weightTableMatmulCode } from './weight-table.js';
import { packedActivationMatmulCode } from './packed-activation.js';
import { packedResidualMatmulCode } from './packed-residual.js';
import { halfOutputMatmulCode, halfResidualMatmulCode, rawHalfOutputMatmulCode } from './half-storage.js';
import { tile128MatmulCode } from './tile128.js';

let reduction = null;

/** Everything up to the point where the shapes and the storage formats start to matter. */
function sharedReduction() {
  if (reduction) return reduction;
  const quad = quadMatmulCode(retileMatmulCode(productionMatmulCode()));
  const words = vectorLoadMatmulCode(weightWordMatmulCode(paddedMatmulCode(packedExponentMatmulCode(quad))));
  const grouped = unrolledMatmulCode(
    groupExponentMatmulCode(boundedHalfMatmulCode(halfExponentMatmulCode(words))));
  reduction = seededExponentMatmulCode(grouped);
  return reduction;
}

/**
 * @param {object} variant
 *   `output`: 'e4' packed bytes, 'half', or 'dual' for both;
 *   `residual`: 'e4' packed bytes or 'half' - unread when the flags say there is no skip, but it still
 *     decides the binding's type, so it is part of the variant;
 *   `batched`: several independent matrices share the dispatch, or the A tensor is wider than K;
 *   `tile128`: two output quads per invocation.
 */
export function variantCode({ output, residual, batched, tile128 }) {
  let code = sharedReduction();
  if (batched) code = batchedMatmulCode(code);
  code = weightTableMatmulCode(siluTableMatmulCode(bitQuantMatmulCode(layoutMatmulCode(shapeMatmulCode(code)))));
  code = residual === 'e4' ? packedResidualMatmulCode(code) : halfResidualMatmulCode(code);
  if (output === 'half') {
    code = halfOutputMatmulCode(packedActivationMatmulCode(code, { input: true, prefetch: true }));
  } else {
    code = packedActivationMatmulCode(code, { input: true, output: true, prefetch: true, rowOutput: true });
    if (output === 'dual') code = rawHalfOutputMatmulCode(code);
  }
  return tile128 ? tile128MatmulCode(code) : code;
}

export const variantKey = ({ output, residual, batched, tile128 }) =>
  `${output}/${residual}${batched ? '/batched' : ''}${tile128 ? '/tile128' : ''}`;
