// Keep the same 32x32 output tile and native K reductions, but distribute it
// across 256 invocations (four outputs each) instead of 64 (sixteen each).
export function retileMatmulCode(code) {
  const legacyStart = code.indexOf('    } else {\n     for (var k = 0u;');
  const legacyEnd = code.indexOf('    let partition_width', legacyStart);
  if (legacyStart < 0 || legacyEnd < 0) throw new Error('Unrecognized matmul source layout');
  // This entry point is selected only for the Ada F13 modes. Removing the
  // unrelated 4x4 scalar fallback permits the accumulator tile to shrink.
  code = code.slice(0, legacyStart) + '    }\n' + code.slice(legacyEnd);
  return code.replace('@workgroup_size(8, 8, 1)', '@workgroup_size(16, 16, 1)')
    .replaceAll('array<f32, 16>()', 'array<f32, 4>()')
    .replaceAll('local_id.y * 4u', 'local_id.y * 2u')
    .replaceAll('local_id.x * 4u', 'local_id.x * 2u')
    .replaceAll('local_row * 4u', 'local_row * 2u')
    .replaceAll('local_row < 4u', 'local_row < 2u')
    .replaceAll('local_column < 4u', 'local_column < 2u')
    .replaceAll('accumulator < 16u', 'accumulator < 4u')
    .replace('linear += 64u', 'linear += 256u');
}

export function batchedMatmulCode(code) {
  return code.replace('  output_column_offset: u32,',
    '  output_column_offset: u32,\n  input_matrix_channels: u32,\n  batches: u32,')
    .replace('  let row_group =', `  let column_groups = (params.output_channels + 31u) / 32u;
  let batch = group_id.x / column_groups;
  let column_group = group_id.x % column_groups;
  let row_group =`)
    .replaceAll('group_id.x * 32u', 'column_group * 32u')
    .replaceAll('+ params.output_column_offset + column',
      '+ params.output_column_offset + batch * params.output_channels + column')
    .replace('input_row * params.input_channels + input_k',
      'input_row * params.input_matrix_channels + select(batch * params.input_channels, 0u, (MATMUL_FLAGS & 131072u) != 0u) + input_k')
    .replace('params.weight_byte_offset + weight_index',
      'params.weight_byte_offset + batch * params.input_channels * params.weight_matrix_channels + weight_index');
}

const QUAD_FDPA = `
fn quad_fdpa(row: u32, column: u32, start: u32, count: u32, accumulator: vec4<f32>) -> vec4<f32> {
  var exponents = select(vec4<i32>(-21),
    max(vec4<i32>((bitcast<vec4<u32>>(abs(accumulator)) >> vec4<u32>(23u)) & vec4<u32>(255u))
      - vec4<i32>(127), vec4<i32>(-14)), accumulator != vec4<f32>(0.0));
  for (var k = start; k < start + count; k++) {
    let a = vec2<f32>(tile_a[row * 32u + k], tile_a[(row + 1u) * 32u + k]);
    let b = vec2<f32>(tile_b[k * 32u + column], tile_b[k * 32u + column + 1u]);
    let ea = vec2<i32>(e4m3_exponent(a.x), e4m3_exponent(a.y));
    let eb = vec2<i32>(e4m3_exponent(b.x), e4m3_exponent(b.y));
    let product_exponents = select(vec4<i32>(-21), ea.xxyy + eb.xyxy,
      (a.xxyy != vec4<f32>(0.0)) & (b.xyxy != vec4<f32>(0.0)));
    exponents = max(exponents, product_exponents);
  }
  let alignment = bitcast<vec4<f32>>(vec4<u32>(vec4<i32>(140) - exponents) << vec4<u32>(23u));
  var sums = trunc(accumulator * alignment);
  for (var k = start; k < start + count; k++) {
    let a = vec2<f32>(tile_a[row * 32u + k], tile_a[(row + 1u) * 32u + k]);
    let b = vec2<f32>(tile_b[k * 32u + column], tile_b[k * 32u + column + 1u]);
    sums += trunc((a.xxyy * b.xyxy) * alignment);
  }
  let scale = bitcast<vec4<f32>>(vec4<u32>(exponents + vec4<i32>(114)) << vec4<u32>(23u));
  // FP8 products and the incoming half accumulator cannot produce a nonzero
  // result below the smallest half subnormal. Native publishes zero as +0.
  let rounded = vec4<f32>(vec4<f16>(sums * scale));
  return select(rounded, vec4<f32>(0.0), rounded == vec4<f32>(0.0));
}
`;

export function quadMatmulCode(code) {
  const start = code.indexOf('    if ((MATMUL_FLAGS & 64512u) != 0u) {');
  const end = code.indexOf('    let partition_width', start);
  if (start < 0 || end < 0) throw new Error('Unrecognized retiled Ada matmul');
  code = code.slice(0, start) + `
    var quad = vec4<f32>(sums[0], sums[1], sums[2], sums[3]);
    var length = 16u;
    if ((MATMUL_FLAGS & 2048u) != 0u) { length = 8u; }
    if ((MATMUL_FLAGS & 4096u) != 0u) { length = 4u; }
    for (var start = 0u; start < 32u; start += length) {
      quad = quad_fdpa(local_id.y * 2u, local_id.x * 2u, start, length, quad);
    }
    sums[0] = quad.x; sums[1] = quad.y; sums[2] = quad.z; sums[3] = quad.w;
` + code.slice(end);
  return code.replace('@compute', QUAD_FDPA + '\n@compute');
}
