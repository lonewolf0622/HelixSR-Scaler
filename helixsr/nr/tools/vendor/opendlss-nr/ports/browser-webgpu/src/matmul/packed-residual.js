// The skip tensor in the storage the graph actually keeps it in.
//
// Every block boundary is published as E4M3 and most of them are stored that way, one byte per value; the two
// the attention needs at full precision are stored as halves. Neither is the f32 array the plain kernel
// reads. The decode is the weight table's: it holds every E4 byte as a half scaled by four, which is exact to
// undo, and the half case is one unpack.

export const RESIDUAL_E4 = 262144;   // MATMUL_FLAGS bit: the skip tensor is E4M3 rather than half

const LOAD = `fn load_residual(index: u32) -> f32 {
  if ((MATMUL_FLAGS & ${RESIDUAL_E4}u) != 0u) {
    return f32(weight_metadata[(residual_values[index / 4u] >> ((index % 4u) * 8u)) & 255u].x) * 0.25;
  }
  return f32(bitcast<vec2<f16>>(residual_values[index / 2u])[index % 2u]);
}
`;

export function packedResidualMatmulCode(code) {
  if (!code.includes('weight_metadata: array<vec2<f16>>')) {
    throw new Error('the packed skip decode needs the weight metadata table');
  }
  if (!code.includes('residual_values[output_index]')) throw new Error('Missing skip tensor read');
  return code.replace('residual_values: array<f32>', 'residual_values: array<u32>')
    .replaceAll('residual_values[output_index]', 'load_residual(output_index)')
    .replace('struct MatmulParams', `${LOAD}struct MatmulParams`);
}
