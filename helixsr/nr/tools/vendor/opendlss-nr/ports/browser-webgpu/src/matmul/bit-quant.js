// RN-even to E4M3's three explicit fraction bits directly in binary32.
// E4 subnormals retain their original fixed 2^-9 quantization grid.
export function bitQuantMatmulCode(code) {
  const start = code.indexOf('fn fp8_domain(');
  const end = code.indexOf('\n}', start) + 2;
  if (start < 0 || end < 2) throw new Error('Missing FP8 publication');
  return code.slice(0, start) + `fn fp8_domain(value: f32) -> f32 {
  if (value != value) { return 0.0; }
  let magnitude = min(abs(value), 448.0);
  if (magnitude == 0.0) { return 0.0; }
  var quantized: f32;
  if (magnitude < 0.015625) {
    quantized = round(magnitude * 512.0) / 512.0;
  } else {
    let bits = bitcast<u32>(magnitude);
    quantized = bitcast<f32>((bits + 0x7ffffu + ((bits >> 20u) & 1u)) & 0xfff00000u);
  }
  return select(quantized, -quantized, value < 0.0);
}` + code.slice(end);
}
