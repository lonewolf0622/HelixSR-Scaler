// A larger query tile reuses one K/V window across several query rows.
// Each query retains its native score, softmax, and value reduction order.
export function multirowWindowCode(code, queries) {
  if (![16, 32, 64].includes(queries)) throw new Error('Unsupported window query tile');
  const tiles = 64 / queries;
  code = code.replace('tiled_q: array<f16, 256>', `tiled_q: array<f16, ${queries * 32}>`)
    .replace('tiled_scores: array<f16, 512>', `tiled_scores: array<f16, ${queries * 64}>`)
    .replace('tiled_reciprocals: array<f16, 8>', `tiled_reciprocals: array<f16, ${queries}>`)
    .replace('packed_q: array<u32, 64>', `packed_q: array<u32, ${queries * 8}>`)
    .replace('packed_scores: array<u32, 128>', `packed_scores: array<u32, ${queries * 16}>`)
    .replace('@workgroup_size(64)', '@workgroup_size(256)')
    .replace('windows_x * windows_y * 8u', `windows_x * windows_y * ${tiles}u`)
    .replace('let window = task / 8u;', `let window = task / ${tiles}u;`)
    .replace('let query_row = task % 8u;', `let query_origin = (task % ${tiles}u) * ${queries}u;`)
    .replace('  let qy = wy + i32(query_row);\n  if (qy < 0 || qy >= i32(attention_params.height)) { return; }\n', '')
    .replaceAll('linear += 64u', 'linear += 256u')
    .replaceAll('linear < 256u', `linear < ${queries * 32}u`)
    .replace('let x = wx + i32(linear / 32u);',
      'let query = query_origin + linear / 32u;\n    let x = wx + i32(query % 8u);\n    let qy = wy + i32(query / 8u);')
    .replace('if (x >= 0 && x < i32(attention_params.width)) {',
      'if (x >= 0 && x < i32(attention_params.width) && qy >= 0 && qy < i32(attention_params.height)) {')
    .replace('  packed_q[lane] =', `  for (var lane = lane; lane < ${queries * 8}u; lane += 256u) {\n  packed_q[lane] =`)
    .replace('tiled_q[lane * 4u + 3u])));', 'tiled_q[lane * 4u + 3u])));\n  }')
    .replaceAll('for (var query = 0u; query < 8u; query++) {',
      `for (var linear = lane; linear < ${queries * 64}u; linear += 256u) {\n    let query = linear / 64u;\n    let key = linear % 64u;`)
    .replace('query_row * 8u + query, lane', 'query_origin + query, key')
    .replaceAll('tiled_qk(query, lane,', 'tiled_qk(query, key,')
    .replaceAll('tiled_scores[query * 64u + lane]', 'tiled_scores[query * 64u + key]')
    .replace('if (lane < 8u)', `if (lane < ${queries}u)`)
    .replace('linear < 128u', `linear < ${queries * 16}u`)
    .replace('let qx = wx + i32(query);',
      'let qx = wx + i32((query_origin + query) % 8u);\n    let qy = wy + i32((query_origin + query) / 8u);')
    .replace('if (qx >= 0 && qx < i32(attention_params.width)) {',
      'if (qx >= 0 && qx < i32(attention_params.width) && qy >= 0 && qy < i32(attention_params.height)) {');
  return code;
}
