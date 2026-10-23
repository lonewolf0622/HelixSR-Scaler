// Four small integer exponents fit exactly in a half vector. Native half2
// add/max can reduce each four-K exponent group without byte extraction.
export function halfExponentMatmulCode(code) {
  code = code.replace('tile_ea: array<u32, 256>', 'tile_ea: array<vec4<f16>, 256>')
    .replace('tile_eb: array<u32, 288>', 'tile_eb: array<vec4<f16>, 288>')
    .replace('    var ea = 0u;', '    var ea = vec4<f16>(-100);')
    .replace('    var eb = 0u;', '    var eb = vec4<f16>(-100);')
    .replace('ea |= select(0u, u32(e4m3_exponent(a) + 106), a != 0.0) << (part * 8u);',
      'ea[part] = f16(select(-100, e4m3_exponent(a), a != 0.0));')
    .replace('eb |= select(0u, u32(e4m3_exponent(b) + 106), b != 0.0) << (part * 8u);',
      'eb[part] = f16(select(-100, e4m3_exponent(b), b != 0.0));')
    .replace('fn quad_fdpa(', `fn maximum_half_exponent(v: vec4<f16>) -> f16 {
  let pair = max(v.xy, v.zw);
  return max(pair.x, pair.y);
}
fn quad_fdpa(`);
  const start = code.indexOf('  var maximum =', code.indexOf('fn quad_fdpa('));
  const end = code.indexOf('  let alignment', start);
  if (start < 0 || end < 0) throw new Error('Missing packed exponent reduction');
  return code.slice(0,start) + `  var maximum = vec4<f16>(exponents);
  for (var k = start / 4u; k < (start + count) / 4u; k++) {
    let a0 = tile_ea[row * 8u + k];
    let a1 = tile_ea[(row + 1u) * 8u + k];
    let b0 = tile_eb[column * 9u + k];
    let b1 = tile_eb[(column + 1u) * 9u + k];
    maximum = max(maximum, vec4<f16>(maximum_half_exponent(a0 + b0),
      maximum_half_exponent(a0 + b1), maximum_half_exponent(a1 + b0), maximum_half_exponent(a1 + b1)));
  }
  exponents = vec4<i32>(maximum);
` + code.slice(end);
}
