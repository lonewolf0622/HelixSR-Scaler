// Half storage for the boundaries that are not E4M3.
//
// Two of a block's outputs are kept at full half precision rather than published to E4M3: the attention's
// qkv, and the skip the projection adds. A half element is what the tensor holds, so it is also what the
// shader declares - `array<f16>` needs no packing, and the index is the same one the plain kernel uses.

export function halfOutputMatmulCode(code) {
  if (!code.includes('output_values: array<f32>')) throw new Error('Missing the f32 publication to narrow');
  return code.replace('output_values: array<f32>', 'output_values: array<f16>')
    .replace(/output_values\[output_index\] = ([^;]+);/g, 'output_values[output_index] = f16($1);');
}

export function halfResidualMatmulCode(code) {
  if (!code.includes('residual_values: array<f32>')) throw new Error('Missing the f32 skip tensor');
  return code.replace('residual_values: array<f32>', 'residual_values: array<f16>')
    .replaceAll('residual_values[output_index]', 'f32(residual_values[output_index])');
}

// The FFN's consumer produces both: the E4M3 the next matrix multiply reads, and the half the projection
// adds back. One reduction, two publications, so the second costs a store and nothing else.
export function rawHalfOutputMatmulCode(code) {
  const publish = '          published_pair |= publish_e4_code(value) << (local_column * 8u);';
  if (!code.includes(publish)) throw new Error('Missing the E4 publication to pair the half with');
  return code.replace('struct MatmulParams',
    '@group(0) @binding(7) var<storage, read_write> raw_values: array<f16>;\nstruct MatmulParams')
    .replace(publish, `${publish}\n          raw_values[output_index] = f16(value);`);
}
