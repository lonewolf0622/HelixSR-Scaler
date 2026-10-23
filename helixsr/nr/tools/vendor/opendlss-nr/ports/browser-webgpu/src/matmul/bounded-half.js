// Requires immutable |B|<=9 and the original finite E4 input contract. Scaling
// A and B by four gives products in [2^-14,64512], with <=8 significant bits:
// every nonzero product is an exact normal half. C/F13 sums stay unchanged.
export function boundedHalfMatmulCode(code) {
  code = code.replace('tile_a: array<vec4<f32>, 256>', 'tile_a: array<vec4<f16>, 256>')
    .replace('tile_b: array<vec4<f32>, 288>', 'tile_b: array<vec4<f16>, 288>')
    .replace('var loaded_a = vec4<f32>(0);', 'var loaded_a = vec4<f16>(0);')
    .replace('var loaded_b = vec4<f32>(0);', 'var loaded_b = vec4<f16>(0);')
    .replace('loaded_a[part] = a;', 'loaded_a[part] = f16(a * 4.0);')
    .replace('loaded_b[part] = b;', 'loaded_b[part] = f16(b * 4.0);')
    .replace('  var sums = trunc(accumulator * alignment);', `  var sums = trunc(accumulator * alignment);
  let product_alignment = bitcast<vec4<f32>>(vec4<u32>(vec4<i32>(136) - exponents) << vec4<u32>(23u));`);
  const start = code.indexOf('    sums += trunc((vec4<f32>(a0.x');
  const end = code.indexOf('\n  }', start);
  if (start < 0 || end < 0) throw new Error('Missing vector products');
  const terms = [...'xyzw'].map(p => `    sums += trunc(vec4<f32>(vec4<f16>(a0.${p}, a0.${p}, a1.${p}, a1.${p})
      * vec4<f16>(b0.${p}, b1.${p}, b0.${p}, b1.${p})) * product_alignment);`).join('\n');
  return code.slice(0, start) + terms + code.slice(end);

}
