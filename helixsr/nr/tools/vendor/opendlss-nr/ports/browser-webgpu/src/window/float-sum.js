// Every aligned term is an integer and the absolute 16-term sum plus C is
// below 2^20. Binary32 therefore preserves every integer sum bit exactly.
export function floatSumWindowCode(code) {
  for (const name of ['tiled_qk', 'tiled_value']) {
    const start = code.indexOf(`fn ${name}(`);
    const end = code.indexOf('\n}', start) + 2;
    if (start < 0 || end < 2) throw new Error(`Missing ${name}`);
    const body = code.slice(start, end)
      .replaceAll(/i32\(trunc\(([^;]*?)\)\)/g, 'trunc($1)')
      .replace('f32(sum)', 'select(sum, 0.0, sum == 0.0)');
    code = code.slice(0, start) + body + code.slice(end);
  }
  return code;
}
