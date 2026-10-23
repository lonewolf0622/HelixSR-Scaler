// Keep the 32x32 output tile and its shared operands. Two adjacent native
// output quads share one invocation; every output keeps its original F13
// reduction groups, incoming half C, K partitions and final publications.
export function tile128MatmulCode(code) {
  const packedRows = code.includes('@workgroup_size(8, 32, 1)');
  if (!packedRows && !code.includes('@workgroup_size(16, 16, 1)')) throw new Error('Unknown matrix ownership');
  if (code.includes('tile_ea: array<vec2<u32>')) throw new Error('Pair-exchange packed output is unsupported');
  const rows = packedRows ? 1 : 2;
  const mainStart = code.indexOf('@compute');
  if (mainStart < 0) throw new Error('Missing matrix entry point');
  let main = code.slice(mainStart);
  main = main.replace(packedRows ? '@workgroup_size(8, 32, 1)' : '@workgroup_size(16, 16, 1)',
    packedRows ? '@workgroup_size(8, 16, 1)' : '@workgroup_size(16, 8, 1)')
    .replaceAll(packedRows ? 'local_id.y' : 'local_id.y * 2u', `local_id.y * ${rows * 2}u`)
    .replaceAll('array<f32, 4>()', 'array<f32, 8>()')
    .replaceAll(`local_row < ${rows}u`, `local_row < ${rows * 2}u`)
    .replaceAll('accumulator < 4u', 'accumulator < 8u');
  const loadBegin = main.indexOf('    let tile_outer = local_index / 8u;');
  const loadEnd = main.indexOf('    workgroupBarrier();', loadBegin);
  if (loadBegin < 0 || loadEnd < 0) throw new Error('Missing matrix tile loader');
  main = main.slice(0,loadBegin) + `    for (var tile_index = local_index; tile_index < 256u; tile_index += 128u) {
${main.slice(loadBegin,loadEnd).replaceAll('local_index','tile_index')}    }
` + main.slice(loadEnd);
  const quadBegin = main.indexOf('    var quad = vec4<f32>');
  const quadEndText = '    sums[0] = quad.x; sums[1] = quad.y; sums[2] = quad.z; sums[3] = quad.w;';
  const quadEnd = main.indexOf(quadEndText, quadBegin) + quadEndText.length;
  if (quadBegin < 0 || quadEnd < quadEndText.length) throw new Error('Missing native output quads');
  let quads = main.slice(quadBegin,quadEnd).replaceAll('quad', 'quad0')
    .replaceAll('quad0_fdpa_', 'quad_fdpa_');
  quads = quads.replace('    var quad0 = vec4<f32>(sums[0], sums[1], sums[2], sums[3]);',
    '    var quad0 = vec4<f32>(sums[0], sums[1], sums[2], sums[3]);\n    var quad1 = vec4<f32>(sums[4], sums[5], sums[6], sums[7]);');
  let calls = 0;
  quads = quads.replace(/      quad0 = quad_fdpa_\d+_\d+\(([^,]+), ([^,]+), quad0\);/g,
    (line,row,column) => { calls++; return line + '\n' + line.replaceAll('quad0','quad1')
      .replace(`(${row}, ${column},`, `(${row} + ${rows}u, ${column},`); });
  if (calls !== 14) throw new Error(`Expected 14 original reduction calls, got ${calls}`);
  quads += '\n    sums[4] = quad1.x; sums[5] = quad1.y; sums[6] = quad1.z; sums[7] = quad1.w;';
  return code.slice(0,mainStart) + main.slice(0,quadBegin) + quads + main.slice(quadEnd);
}
