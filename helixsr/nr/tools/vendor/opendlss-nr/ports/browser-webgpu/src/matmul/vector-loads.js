// Read four adjacent K values as one shared-memory vector. This amortizes
// address calculation and robustness checks without changing any reduction.
export function vectorLoadMatmulCode(code) {
  code = code.replace(/fn ada_fp8_fdpa\([\s\S]*?\n}/, '')
    .replace('tile_a: array<f32, 1024>', 'tile_a: array<vec4<f32>, 256>')
    .replace('tile_b: array<f32, 1024>', 'tile_b: array<vec4<f32>, 288>')
    .replace('    var ea = 0u;', '    var loaded_a = vec4<f32>(0);\n    var loaded_b = vec4<f32>(0);\n    var ea = 0u;')
    .replace('tile_a[tile_outer * 32u + tile_k + part] = a;', 'loaded_a[part] = a;')
    .replace('tile_b[(tile_k + part) * 32u + tile_outer] = b;', 'loaded_b[part] = b;')
    .replace('    tile_ea[local_index] = ea;', `    tile_a[local_index] = loaded_a;
    tile_b[tile_outer * 9u + tile_k / 4u] = loaded_b;
    tile_ea[local_index] = ea;`);
  const start = code.indexOf('  for (var k = start; k < start + count; k++) {', code.indexOf('fn quad_fdpa('));
  const end = code.indexOf('  let scale', start);
  if (start < 0 || end < 0) throw new Error('Missing quad product loop');
  const terms = [...'xyzw'].map(c => `    sums += trunc((vec4<f32>(a0.${c}, a0.${c}, a1.${c}, a1.${c})
      * vec4<f32>(b0.${c}, b1.${c}, b0.${c}, b1.${c})) * alignment);`).join('\n');
  return code.slice(0,start) + `  for (var k = start / 4u; k < (start + count) / 4u; k++) {
    let a0 = tile_a[row * 8u + k];
    let a1 = tile_a[(row + 1u) * 8u + k];
    let b0 = tile_b[column * 9u + k];
    let b1 = tile_b[(column + 1u) * 9u + k];
${terms}
  }
` + code.slice(end);
}
