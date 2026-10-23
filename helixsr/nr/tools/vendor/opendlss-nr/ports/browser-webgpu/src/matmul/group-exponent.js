// Reduce the native exponent maximum horizontally once per K group, instead
// of once per four-K fragment. All operands are exact small half integers.
export function groupExponentMatmulCode(code) {
  code = code.replace('  var maximum = vec4<f16>(exponents);', `  var maximum00 = vec4<f16>(f16(exponents.x));
  var maximum01 = vec4<f16>(f16(exponents.y));
  var maximum10 = vec4<f16>(f16(exponents.z));
  var maximum11 = vec4<f16>(f16(exponents.w));`);
  const begin = code.indexOf('    maximum = max(maximum, vec4<f16>(maximum_half_exponent');
  const end = code.indexOf('  exponents = vec4<i32>(maximum);', begin);
  if (begin < 0 || end < 0) throw new Error('Missing half exponent reduction');
  return code.slice(0, begin) + `    maximum00 = max(maximum00, a0 + b0);
    maximum01 = max(maximum01, a0 + b1);
    maximum10 = max(maximum10, a1 + b0);
    maximum11 = max(maximum11, a1 + b1);
  }
  let maximum = vec4<f16>(maximum_half_exponent(maximum00), maximum_half_exponent(maximum01),
    maximum_half_exponent(maximum10), maximum_half_exponent(maximum11));
` + code.slice(end);
}
