// Known matrix dimensions let the compiler remove redundant partial-tile
// guards and simplify fixed shape addressing. Partial tiles retain bounds.
export function shapeMatmulCode(code) {
  for (const [field, constant] of [['rows', 'MATMUL_ROWS'], ['input_channels', 'MATMUL_K'],
    ['output_channels', 'MATMUL_N']]) {
    code = code.replace('struct MatmulParams', `override ${constant}: u32 = 32u;\nstruct MatmulParams`)
      .replaceAll(`params.${field}`, constant);
  }
  return code.replaceAll('input_row < MATMUL_ROWS', '(MATMUL_ROWS % 32u == 0u || input_row < MATMUL_ROWS)')
    .replace(/\bk < MATMUL_K/g, '(MATMUL_K % 32u == 0u || k < MATMUL_K)')
    .replaceAll('output_column < MATMUL_N', '(MATMUL_N % 32u == 0u || output_column < MATMUL_N)')
    .replace(/\brow < MATMUL_ROWS/g, '(MATMUL_ROWS % 32u == 0u || row < MATMUL_ROWS)')
    .replace(/\bcolumn < MATMUL_N/g, '(MATMUL_N % 32u == 0u || column < MATMUL_N)');
}
