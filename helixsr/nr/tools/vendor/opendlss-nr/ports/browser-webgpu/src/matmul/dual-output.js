// The FFN consumer already produces the raw half residual. Publish its E4
// attention branch alongside it, preserving the intermediate half boundary.
// Both destinations keep ordinary activation storage; no packing is added.
export function dualOutputMatmulCode(code) {
  const publication = '          output_values[output_index] = select(value, fp8_domain(value), (MATMUL_FLAGS & 32u) != 0u);';
  if (!code.includes(publication)) throw new Error('Missing FFN dual publication');
  return code.replace('struct MatmulParams',
    '@group(0) @binding(7) var<storage, read_write> quantized_values: array<f32>;\nstruct MatmulParams')
    .replace(publication, publication + '\n          quantized_values[output_index] = fp8_domain(round_accumulator(value));');
}
