// Four exponents fit in one u32. Nonzero E4 exponents are in [-6,8]; encoding
// e+106 gives bytes 100..114. Zero uses 0. Adding two packed operands cannot
// carry across bytes, and a zero product is below the initial -21+212=191.
export function packedExponentMatmulCode(code) {
  code = code.replace('var<workgroup> tile_b: array<f32, 1024>;', `var<workgroup> tile_b: array<f32, 1024>;
var<workgroup> tile_ea: array<u32, 256>;
var<workgroup> tile_eb: array<u32, 256>;`);
  const loadStart = code.indexOf('    for (var linear = local_index; linear < 1024u;');
  const loadEnd = code.indexOf('    workgroupBarrier();', loadStart);
  if (loadStart < 0 || loadEnd < 0) throw new Error('Missing quad tile loader');
  code = code.slice(0, loadStart) + `    let tile_outer = local_index / 8u;
    let tile_k = (local_index % 8u) * 4u;
    let input_row = row_group * 32u + tile_outer;
    let output_column = group_id.x * 32u + tile_outer;
    var ea = 0u;
    var eb = 0u;
    for (var part = 0u; part < 4u; part++) {
      let k = k_base + tile_k + part;
      var input_k = select(k, native_chained_input_index(k), (MATMUL_FLAGS & 16u) != 0u);
      if ((MATMUL_FLAGS & 128u) != 0u) { input_k = native_inverse_chained_input_index(k); }
      var a = 0.0;
      var b = 0.0;
      if (input_row < params.rows && k < params.input_channels) {
        a = input_values[input_row * params.input_channels + input_k];
      }
      if (output_column < params.output_channels && k < params.input_channels) {
        var weight_index: u32;
        if ((MATMUL_FLAGS & 64u) != 0u) {
          weight_index = expert_interleaved_weight_index(k, output_column,
            params.weight_matrix_channels, params.weight_column_offset);
        } else {
          weight_index = packed_weight_index(k, output_column + params.weight_column_offset,
            params.weight_matrix_channels);
        }
        b = decode_e4m3(packed_byte(params.weight_byte_offset + weight_index));
      }
      tile_a[tile_outer * 32u + tile_k + part] = a;
      tile_b[(tile_k + part) * 32u + tile_outer] = b;
      ea |= select(0u, u32(e4m3_exponent(a) + 106), a != 0.0) << (part * 8u);
      eb |= select(0u, u32(e4m3_exponent(b) + 106), b != 0.0) << (part * 8u);
    }
    tile_ea[local_index] = ea;
    tile_eb[local_index] = eb;
` + code.slice(loadEnd);
  const start = code.indexOf('  for (var k = start; k < start + count; k++) {', code.indexOf('fn quad_fdpa('));
  const end = code.indexOf('  let alignment', start);
  if (start < 0 || end < 0) throw new Error('Missing quad exponent loop');
  return code.slice(0, start) + `  var maximum = vec4<u32>(exponents + vec4<i32>(212));
  for (var k = start / 4u; k < (start + count) / 4u; k++) {
    let ea = vec2<u32>(tile_ea[row * 8u + k], tile_ea[(row + 1u) * 8u + k]);
    let eb = vec2<u32>(tile_eb[column * 8u + k], tile_eb[(column + 1u) * 8u + k]);
    let packed = ea.xxyy + eb.xyxy;
    maximum = max(maximum, packed & vec4<u32>(255u));
    maximum = max(maximum, (packed >> vec4<u32>(8u)) & vec4<u32>(255u));
    maximum = max(maximum, (packed >> vec4<u32>(16u)) & vec4<u32>(255u));
    maximum = max(maximum, packed >> vec4<u32>(24u));
  }
  exponents = vec4<i32>(maximum) - vec4<i32>(212);
` + code.slice(end);
}
