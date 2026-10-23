// Seed each exponent vector from its first actual product fragment. Include
// the incoming C exponent only once, after the exact product maximum.
export function seededExponentMatmulCode(code) {
  for (const count of [4, 8, 16]) for (let first = 0; first < 32; first += count) {
    const begin = code.indexOf(`fn quad_fdpa_${count}_${first}(`);
    const end = code.indexOf('\n}', begin) + 2;
    if (begin < 0 || end < 2) throw new Error('Missing specialized exponent reduction');
    let body = code.slice(begin, end);
    for (const [name, a, b, component] of [['00','a0','b0','x'], ['01','a0','b1','y'],
      ['10','a1','b0','z'], ['11','a1','b1','w']]) {
      body = body.replace(`var maximum${name} = vec4<f16>(f16(exponents.${component}));`,
        `var maximum${name}: vec4<f16>;`)
        .replace(`maximum${name} = max(maximum${name}, ${a} + ${b});`,
          `maximum${name} = ${a} + ${b};`);
    }
    body = body.replace('  exponents = vec4<i32>(maximum);',
      '  exponents = max(exponents, vec4<i32>(maximum));');
    code = code.slice(0, begin) + body + code.slice(end);
  }
  return code;
}
