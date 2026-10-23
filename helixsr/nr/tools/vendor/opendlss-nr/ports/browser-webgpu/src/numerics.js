// The network's publication grid, on the CPU. Mirrors src/numeric.h and src/reference.cpp of the Vulkan
// implementation; shaders/numerics.wgsl mirrors this in turn, and web/selftest.html checks that the two agree
// over every half bit pattern. See docs/numerics.md for why the schedule of roundings, and not an error bound,
// is the contract.
//
// This module is the port's oracle: the parity harness and the self-test both compare the GPU against it, so
// it is written for legibility rather than speed, and it never takes a shortcut the WGSL cannot take.

const scratch = new DataView(new ArrayBuffer(4));

/** f32 bit pattern of a number. */
export function f32Bits(value) {
  scratch.setFloat32(0, value);
  return scratch.getUint32(0);
}

/** Number from an f32 bit pattern. */
export function f32FromBits(bits) {
  scratch.setUint32(0, bits >>> 0);
  return scratch.getFloat32(0);
}

/** Round an arbitrary double down to what an f32 would hold, which is where the WGSL side always starts. */
export const toF32 = (value) => Math.fround(value);

/** Shift right with round-to-nearest-even, the rounding every narrowing conversion here uses. */
export function roundShiftRightEven(value, shift) {
  if (shift === 0) return value >>> 0;
  if (shift > 31) return 0;
  const quotient = value >>> shift;
  const remainder = value & ((1 << shift) - 1);
  const halfway = 1 << (shift - 1);
  return quotient + (remainder > halfway || (remainder === halfway && (quotient & 1)) ? 1 : 0);
}

/** IEEE binary16 bit pattern, round-to-nearest-even. */
export function f16Bits(value) {
  const bits = f32Bits(value);
  const sign = (bits >>> 16) & 0x8000;
  const exponent = (bits >>> 23) & 0xff;
  const mantissa = bits & 0x7fffff;
  if (exponent === 0xff) return sign | (mantissa ? 0x7e00 : 0x7c00);
  let halfExponent = exponent - 112;
  if (halfExponent >= 31) return sign | 0x7c00;
  if (halfExponent <= 0) {
    if (halfExponent < -10) return sign;
    return sign | roundShiftRightEven(mantissa | 0x800000, 14 - halfExponent);
  }
  let rounded = roundShiftRightEven(mantissa, 13);
  if (rounded === 0x400) { rounded = 0; halfExponent += 1; }
  if (halfExponent >= 31) return sign | 0x7c00;
  return sign | (halfExponent << 10) | rounded;
}

/** Number from an IEEE binary16 bit pattern. */
export function f16ToNumber(bits) {
  const sign = (bits & 0x8000) ? -1 : 1;
  const exponent = (bits >>> 10) & 0x1f;
  const mantissa = bits & 0x3ff;
  if (exponent === 0) return sign * mantissa * 2 ** -24;
  if (exponent === 0x1f) return mantissa ? NaN : sign * Infinity;
  return sign * (1 + mantissa / 1024) * 2 ** (exponent - 15);
}

/** One publication to the half grid. */
export const roundF16 = (value) => f16ToNumber(f16Bits(value));

/**
 * E4M3FN code of an already half-rounded value: RNE, finite saturation at 448, NaN -> +0. The sign of a zero
 * survives, as the hardware conversion leaves it: +0 and -0 decode alike, but the published bytes differ.
 */
export function e4m3FromF16Bits(half) {
  if ((half & 0x7c00) === 0x7c00 && (half & 0x03ff) !== 0) return 0;
  if ((half & 0x7fff) === 0) return (half >>> 8) & 0x80;
  const negative = (half & 0x8000) ? 0x80 : 0;
  const exponent = (half >>> 10) & 0x1f;
  const mantissa = half & 0x3ff;
  let code;
  if (exponent === 31) {
    code = 0x7e;
  } else if (exponent <= 8) {
    // Below 2^-6 the E4M3 grid is subnormal: the half significand shifts down onto a fixed step of 2^-9.
    const significand = exponent === 0 ? mantissa : 1024 + mantissa;
    code = Math.min(roundShiftRightEven(significand, exponent === 0 ? 15 : 16 - exponent), 8);
  } else {
    let e4Exponent = exponent - 8;
    let e4Mantissa = roundShiftRightEven(mantissa, 7);
    if (e4Mantissa === 8) { e4Mantissa = 0; e4Exponent += 1; }
    code = (e4Exponent > 15 || (e4Exponent === 15 && e4Mantissa > 6)) ? 0x7e : (e4Exponent << 3) | e4Mantissa;
  }
  return negative | code;
}

export const e4m3FromNumber = (value) => e4m3FromF16Bits(f16Bits(value));

/**
 * E4M3FN value of a byte. The one NaN code reads as a signed zero, which is what the shaders do with it -
 * note that the Vulkan tree's host-side helper (src/numeric.h) returns NaN there instead. Nothing can tell
 * the two apart: an activation never publishes that code, and no weight contains it.
 */
export function e4m3ToNumber(byte) {
  const sign = (byte & 0x80) ? -1 : 1;
  const exponent = (byte >>> 3) & 0xf;
  const mantissa = byte & 0x7;
  if (exponent === 0) return sign * mantissa * 2 ** -9;
  if (exponent === 0xf && mantissa === 0x7) return sign * 0;
  return sign * (1 + mantissa / 8) * 2 ** (exponent - 7);
}

export const normalExponent = (value) => (((f32Bits(Math.abs(value)) >>> 23) & 0xff) - 127);
export const e4m3Exponent = (value) => Math.max(normalExponent(value), -6);
export const f16Exponent = (value) => Math.max(normalExponent(value), -14);

// The tensor core's dot product, as fixed point. See the long comment in shaders/numerics.wgsl: the terms are
// accumulated as exact integers in units of 2^(max - 13) so that the JS and the WGSL cannot drift apart
// through a difference in summation order or a fused multiply-add the shader compiler decided to emit.

/** The shared exponent of a step that starts from this accumulator; -21 is the empty value. */
export const f13Start = (accumulator) => (accumulator !== 0 ? f16Exponent(accumulator) : -21);

/** Widen the shared exponent to cover one product; a zero operand is skipped, not clamped. */
export function f13Cover(maximumExponent, a, b) {
  if (a === 0 || b === 0) return maximumExponent;
  return Math.max(maximumExponent, e4m3Exponent(a) + e4m3Exponent(b));
}

/** One term of the aligned sum, in units of 2^(max - 13). */
export const f13Term = (value, scale) => Math.trunc(toF32(value * scale));

/** The aligned sum, published once to the half grid. */
export const f13Finish = (units, maximumExponent) => roundF16(units * 2 ** (maximumExponent - 13));

/**
 * Exact signed integer times 2^binaryExponent -> half. The f16 step needs this rather than a rounding of the
 * sum, because its fixed-point total carries more significand than a half holds.
 */
export function fixedToF16(fixedSum, binaryExponent) {
  if (fixedSum === 0) return 0;
  const negative = fixedSum < 0;
  const magnitude = Math.abs(fixedSum);
  const msb = 31 - Math.clz32(magnitude);
  let valueExponent = msb + binaryExponent;
  let halfBits = negative ? 0x8000 : 0;
  if (valueExponent >= -14) {
    let significand = msb > 10 ? roundShiftRightEven(magnitude, msb - 10) : magnitude << (10 - msb);
    if (significand >= 2048) { significand = 1024; valueExponent += 1; }
    if (valueExponent >= 16) halfBits |= 0x7c00;
    else halfBits |= ((valueExponent + 15) << 10) | (significand - 1024);
  } else {
    const subnormalScale = binaryExponent + 24;
    const mantissa = subnormalScale >= 0 ? magnitude << subnormalScale
                                         : roundShiftRightEven(magnitude, -subnormalScale);
    halfBits |= Math.min(mantissa, 1024);
  }
  return f16ToNumber(halfBits);
}

/** One half of a k32 FP8 step: 16 products plus the incoming accumulator, aligned, truncated, summed, rounded. */
export function adaFp8Fdpa16(a, b, count, accumulator) {
  if (!Number.isFinite(accumulator)) return accumulator;
  let maximumExponent = f13Start(accumulator);
  for (let i = 0; i < count; ++i) maximumExponent = f13Cover(maximumExponent, a[i], b[i]);
  const scale = 2 ** (13 - maximumExponent);
  let units = f13Term(accumulator, scale);
  for (let i = 0; i < count; ++i) units += f13Term(toF32(a[i] * b[i]), scale);
  return f13Finish(units, maximumExponent);
}

/** The f16 step: 8 products against 24 fractional bits, for the chains that never narrow to E4M3. */
export function adaF16Fdpa8(a, b, count, accumulator) {
  let maximumExponent = f13Start(accumulator);
  for (let i = 0; i < count; ++i) {
    if (a[i] !== 0 && b[i] !== 0) {
      maximumExponent = Math.max(maximumExponent, f16Exponent(a[i]) + f16Exponent(b[i]));
    }
  }
  const scale = 2 ** (24 - maximumExponent);
  let units = Math.trunc(toF32(accumulator * scale));
  for (let i = 0; i < count; ++i) units += Math.trunc(toF32(toF32(a[i] * b[i]) * scale));
  return fixedToF16(units, maximumExponent - 24);
}

// The three functions below each round in a particular precision, and JavaScript's doubles are wider than all
// of them, so every f32-level step is put through Math.fround. Where native fuses a multiply and an add,
// the product happens to be exact in f32, so one fround over the whole expression is that fused operation.

/** MpCubicSiLU: five half publications, the two inner steps f32 fused multiply-adds with exact products. */
export function mpCubicSilu(value) {
  const bounded = roundF16(Math.min(Math.max(value, -4), 4));
  const absolute = roundF16(Math.abs(bounded));
  const inner = roundF16(toF32(-0.055908203125 * absolute + 0.447265625));
  const polynomial = roundF16(toF32(bounded * inner + 0.89453125));
  return roundF16(toF32(value * polynomial));
}

/**
 * The window blocks' attention weight: an affine map on the score, dropped into the half exponent field.
 * The clamp is what keeps the shifted pattern a finite half, so it belongs to the function, not to safety.
 */
export function expWeight(score) {
  const affine = roundF16(toF32(score * 0.044921875 + 1.30078125));
  const clamped = Math.min(Math.max(affine, 1.03125), 1.5693359375);
  return f16ToNumber((((f16Bits(clamped) << 5) >>> 0) + 0x8000) & 0xffff);
}

/**
 * The ViT's variant: a 4-bit shift instead of 5, a different bias, and an affine evaluated in halves rather
 * than f32. The constants are the half values native holds - the f32 spellings they are usually written
 * as (0.08953946828842163, 1.7093614339828491) are not the numbers the arithmetic sees. A double multiplies
 * and adds two halves exactly, so rounding the result once to half is the f16 fused multiply-add.
 */
export function vitExpWeight(score) {
  const affine = roundF16(score * 0.08953857421875 + 1.708984375);
  const clamped = Math.min(Math.max(affine, 1.439453125), 1.9775390625);
  return f16ToNumber((((f16Bits(clamped) << 4) >>> 0) + 0x4000) & 0xffff);
}

/** The SiLU lookup table the fused kernels index by half bit pattern, built once. */
export function siluTable() {
  const table = new Uint16Array(65536);
  for (let bits = 0; bits < 65536; ++bits) table[bits] = f16Bits(mpCubicSilu(f16ToNumber(bits)));
  return table;
}
