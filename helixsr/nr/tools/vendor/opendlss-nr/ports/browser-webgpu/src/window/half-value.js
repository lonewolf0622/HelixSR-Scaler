// Exact normal-half value products. Native positive softmax
// weights publish to E4 in [0,1]; V is finite E4. Scaling both by four keeps
// every nonzero product normal and exact in half, in [2^-14,7168].
export function halfValueWindowCode(code) {
  const start = code.indexOf('fn tiled_value(');
  const end = code.indexOf('\n}', start) + 2;
  if (start < 0 || end < 2) throw new Error('Missing window value reduction');
  let body = code.slice(start, end).replace('  var sum = trunc(accumulator * alignment);',
    '  var sum = trunc(accumulator * alignment);\n  let product_alignment = bitcast<f32>(u32(136 - exponent) << 23u);');
  const terms = body.indexOf('    let a = vec4<f32>(vector_scores');
  const termsEnd = body.indexOf('\n  }', terms);
  if (terms < 0 || termsEnd < 0) throw new Error('Missing vector value products');
  body = body.slice(0, terms) + `    let product = vec4<f32>(vector_scores[query * 16u + c]
      * vector_v[component * 17u + c]);
${[...'xyzw'].map(p => `    sum += trunc(product.${p} * product_alignment);`).join('\n')}` + body.slice(termsEnd);
  code = code.slice(0, start) + body + code.slice(end);
  return code.replace('    vector_scores[linear] = normalized;',
    '    vector_scores[linear] = normalized * vec4<f16>(4);')
    .replace('    packed_k[linear] = packed_window_exponents(vec4<f32>(vector_v[component * 17u + c]));',
      `    let v = vector_v[component * 17u + c];
    packed_k[linear] = packed_window_exponents(vec4<f32>(v));
    vector_v[component * 17u + c] = v * vec4<f16>(4);`);
}
