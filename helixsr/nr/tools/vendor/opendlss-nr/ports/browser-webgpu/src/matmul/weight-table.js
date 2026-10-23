import { e4m3ToNumber, f16Bits } from '../numerics.js';

const e4m3ToFloat16Bits = (byte) => f16Bits(e4m3ToNumber(byte));

const tables = new WeakMap();

export function weightMetadataWords() {
  const words = new Uint32Array(256);
  const halfInteger = value => {
    if (value === 0) return 0;
    const magnitude = Math.abs(value), exponent = Math.floor(Math.log2(magnitude));
    return (value < 0 ? 0x8000 : 0) | ((exponent + 15) << 10)
      | ((magnitude / 2 ** exponent - 1) * 1024);
  };
  for (let byte = 0; byte < 256; byte++) {
    const magnitude = byte & 127, zero = magnitude === 0 || magnitude === 127;
    const scaled = zero ? (byte & 128) << 8 : e4m3ToFloat16Bits(byte) + 2048;
    const exponent = zero ? -100 : Math.max((magnitude >> 3) - 7, -6);
    words[byte] = scaled | (halfInteger(exponent) << 16);
  }
  return words;
}

export function createWeightMetadataTable(device) {
  if (tables.has(device)) return tables.get(device);
  const data = weightMetadataWords();
  const buffer = device.createBuffer({label: 'NR all E4 weight metadata', size: data.byteLength,
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST});
  device.queue.writeBuffer(buffer, 0, data);
  tables.set(device, buffer);
  return buffer;
}

// A 1 KiB lookup replaces per-tile immutable B decoding and exponent
// extraction. Original packed weight addressing and native products remain.
export function weightTableMatmulCode(code) {
  code = code.replace('struct MatmulParams',
    '@group(0) @binding(6) var<storage, read> weight_metadata: array<vec2<f16>>;\nstruct MatmulParams');
  const load = '      var b = 0.0;';
  if (!code.includes(load)) throw new Error('Missing weight metadata loader');
  return code.replace(load, load + '\n      let metadata = weight_metadata[(weight_word >> (part * 8u)) & 255u];')
    .replace('loaded_b[part] = f16(b * 4.0);', 'loaded_b[part] = metadata.x;')
    .replace('eb[part] = f16(select(-100, e4m3_exponent(b), b != 0.0));', 'eb[part] = metadata.y;');
}
