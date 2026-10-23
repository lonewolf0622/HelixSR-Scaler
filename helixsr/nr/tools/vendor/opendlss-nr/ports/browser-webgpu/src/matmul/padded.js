// Nine words per B column keep adjacent output columns on distinct shared
// memory banks. The packed bytes and F13 arithmetic are unchanged.
export function paddedMatmulCode(code) {
  return code.replace('tile_eb: array<u32, 256>', 'tile_eb: array<u32, 288>')
    .replace('tile_eb[local_index] = eb;', 'tile_eb[tile_outer * 9u + tile_k / 4u] = eb;')
    .replaceAll('tile_eb[column * 8u + k]', 'tile_eb[column * 9u + k]')
    .replaceAll('tile_eb[(column + 1u) * 8u + k]', 'tile_eb[(column + 1u) * 9u + k]');
}
