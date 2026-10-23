export const MATMUL_LAYOUT_FIELDS = [
  ['weight_byte_offset', 3], ['bias_byte_offset', 4],
  ['weight_matrix_channels', 6], ['weight_column_offset', 7],
  ['output_matrix_channels', 8], ['output_column_offset', 9],
  ['input_matrix_channels', 10],
];

// Matrix dimensions were already specialized. Make the remaining immutable
// strides and packed-fragment offsets visible to the shader compiler too.
export function layoutMatmulCode(code) {
  for (const [field] of MATMUL_LAYOUT_FIELDS) {
    if (!code.includes(`params.${field}`)) continue;
    const constant = `LAYOUT_${field.toUpperCase()}`;
    code = code.replace('struct MatmulParams', `override ${constant}: u32 = 0u;\nstruct MatmulParams`)
      .replaceAll(`params.${field}`, constant);
  }
  // Retain the common uniform binding in the specialized pipeline layout.
  return code.replace('  let row_group =', '  if (params.rows == 0u) { return; }\n  let row_group =');
}
