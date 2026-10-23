// Four adjacent native K bytes share one packed word. Calculate their native
// fragment address once, retaining a cross-word path for unaligned offsets.
export function weightWordMatmulCode(code) {
  const blockStart = code.indexOf('      if (output_column < params.output_channels && k < params.input_channels) {');
  const blockEnd = code.indexOf('\n      tile_a[', blockStart);
  if (blockStart < 0 || blockEnd < 0) throw new Error('Missing packed matrix weight load');
  const block = code.slice(blockStart,blockEnd);
  const addressStart = block.indexOf('        var weight_index: u32;');
  const addressEnd = block.indexOf('        b = decode_e4m3');
  if (addressStart < 0 || addressEnd < 0) throw new Error('Missing native weight address');
  const address = block.slice(addressStart,addressEnd);
  code = code.slice(0,blockStart) + `      if (k < params.input_channels) {
        b = decode_e4m3((weight_word >> (part * 8u)) & 255u);
      }` + code.slice(blockEnd);
  return code.replace('    var ea = 0u;', `    var weight_word = 0u;
    {
      let k = k_base + tile_k;
      if (output_column < params.output_channels && k < params.input_channels) {
${address}        let byte_offset = params.weight_byte_offset + weight_index;
        weight_word = packed_weights[byte_offset / 4u];
        let shift = (byte_offset % 4u) * 8u;
        if (shift != 0u) {
          weight_word = (weight_word >> shift)
            | (packed_weights[byte_offset / 4u + 1u] << (32u - shift));
        }
      }
    }
    var ea = 0u;`);
}
