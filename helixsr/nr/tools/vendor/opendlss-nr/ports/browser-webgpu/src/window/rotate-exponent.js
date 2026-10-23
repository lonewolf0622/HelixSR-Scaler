// The largest byte is the high byte of the unsigned maximum of all four
// rotations. Preserve the exact packed exponent maximum without extra cache.
export function rotateExponentWindowCode(code) {
  const begin = code.indexOf('fn maximum_window_exponent('), end = code.indexOf('\n}', begin) + 2;
  if (begin < 0 || end < 2) throw new Error('Missing packed window maximum');
  return code.slice(0, begin) + `fn maximum_window_exponent(current: u32, packed: u32) -> u32 {
  let rotated8 = (packed << 8u) | (packed >> 24u);
  let rotated16 = (packed << 16u) | (packed >> 16u);
  let rotated24 = (packed << 24u) | (packed >> 8u);
  return max(current, max(max(packed, rotated8), max(rotated16, rotated24)) >> 24u);
}` + code.slice(end);
}
