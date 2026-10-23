// Publish the attended values as the E4M3 bytes the tensor holds.
//
// One invocation owns four adjacent components of one query, which is one whole word, so no two invocations
// write the same one and no exchange or closing barrier is needed. The reduction per component is untouched.

export function packedOutputWindowCode(code, queries, threads) {
  const header = `  for (var linear = lane; linear < ${queries * 32}u; linear += ${threads}u) {`;
  const begin = code.lastIndexOf(header);
  const end = code.lastIndexOf('\n  }');
  if (begin < 0 || end < begin) throw new Error('Missing the window value publication');
  return code.slice(0, begin) + `  for (var linear = lane; linear < ${queries * 8}u; linear += ${threads}u) {
    let query = linear / 8u;
    let first = (linear % 8u) * 4u;
    let qx = wx + i32((query_origin + query) % 8u);
    let qy = wy + i32((query_origin + query) / 8u);
    if (qx >= 0 && qx < i32(attention_params.width) && qy >= 0 && qy < i32(attention_params.height)) {
      var word = 0u;
      for (var i = 0u; i < 4u; i++) {
        let component = first + i;
        var value = 0.0;
        for (var group = 0u; group < 4u; group++) {
          value = tiled_value(query, component, group * 16u, value);
        }
        word |= publish_e4_code(value) << (i * 8u);
      }
      let index = (u32(qy) * attention_params.width + u32(qx)) * attention_params.channels
        + head * 32u + first;
      attention_output[index / 4u] = word;
    }
  }` + code.slice(end + '\n  }'.length);
}
