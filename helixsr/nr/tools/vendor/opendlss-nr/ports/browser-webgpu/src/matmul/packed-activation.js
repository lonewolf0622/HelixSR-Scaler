import { compileComputePipeline } from './pipeline-compiler.js';

const tables = new WeakMap();

export const PACKED_E4_PUBLICATION_WGSL = `fn publish_e4_code(value: f32) -> u32 {
  if (value != value) { return 0u; }
  let magnitude = min(abs(value), 448.0);
  if (magnitude == 0.0) { return 0u; }
  var code: u32;
  if (magnitude < 0.015625) {
    code = u32(round(magnitude * 512.0));
  } else {
    let bits = bitcast<u32>(magnitude);
    let rounded = (bits + 0x7ffffu + ((bits >> 20u) & 1u)) & 0xfff00000u;
    code = (rounded >> 20u) - 960u;
  }
  return code | select(0u, 128u, value < 0.0);
}`;

// Inputs have already been published as finite E4, including signed zero.
const exactCode = `fn exact_e4_output_code(value: f32) -> u32 {
  let magnitude = abs(value);
  let bits = bitcast<u32>(magnitude);
  let code = select(u32(magnitude * 512.0), (bits >> 20u) - 960u, magnitude >= 0.015625);
  return code | ((bitcast<u32>(value) >> 24u) & 128u);
}`;

// Derive bytes from the retained SiLU table once per device. This changes
// representation only; the native activation is never recomputed here.
export async function createPackedSiluTable(device, source) {
  if (tables.has(device)) return tables.get(device);
  const build = (async () => {
    const module = device.createShaderModule({label: 'NR packed E4 SiLU table builder', code: `enable f16;
${exactCode}
@group(0) @binding(0) var<storage, read> source: array<vec2<f16>>;
@group(0) @binding(1) var<storage, read_write> result: array<u32>;
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
  var word = 0u;
  for (var part = 0u; part < 4u; part++) {
    word |= exact_e4_output_code(f32(source[id.x * 4u + part].y)) << (part * 8u);
  }
  result[id.x] = word;
}`});
    const errors = (await module.getCompilationInfo()).messages.filter(m => m.type === 'error');
    if (errors.length) throw new Error(errors.map(m => m.message).join('\n'));
    const pipeline = await compileComputePipeline(device, {layout: 'auto', compute: {module, entryPoint: 'main'}});
    const buffer = device.createBuffer({label: 'NR all half-input SiLU E4 bytes', size: 65536,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC});
    const group = device.createBindGroup({layout: pipeline.getBindGroupLayout(0), entries: [
      {binding: 0, resource: {buffer: source}}, {binding: 1, resource: {buffer}},
    ]});
    const encoder = device.createCommandEncoder(), pass = encoder.beginComputePass();
    pass.setPipeline(pipeline); pass.setBindGroup(0, group); pass.dispatchWorkgroups(64); pass.end();
    device.queue.submit([encoder.finish()]);
    return buffer;
  })();
  tables.set(device, build);
  return build;
}

// Exact E4 storage at explicitly selected FFN boundaries. Residuals remain
// ordinary activation storage. The default producer owns complete u32 words;
// the diagnostic two-row variant exchanges pairs through dead shared storage.
export function packedActivationMatmulCode(code, {input = false, output = false, prefetch = false, rowOutput = false} = {}) {
  if (!input && !output) throw new Error('Packed activation mode is empty');
  if (input) {
    if (prefetch) {
      const index = code.match(/        a = input_values\[([^\n]+)\];/)?.[1];
      if (!index) throw new Error('Missing packed input index for prefetch');
      const loop = '    for (var part = 0u; part < 4u; part++) {';
      if (!code.includes(loop)) throw new Error('Missing packed input fragment loop');
      const loads = [0, 2].map(part => `    let packed_k${part} = k_base + tile_k + ${part}u;
    var packed_input_k${part} = select(packed_k${part}, native_chained_input_index(packed_k${part}), (MATMUL_FLAGS & 16u) != 0u);
    if ((MATMUL_FLAGS & 128u) != 0u) { packed_input_k${part} = native_inverse_chained_input_index(packed_k${part}); }
    let packed_index${part} = ${index.replace(/\binput_k\b/g, `packed_input_k${part}`)};
    var packed_word${part} = 0u;
    if (input_row < MATMUL_ROWS && packed_k${part} < MATMUL_K) {
      packed_word${part} = input_values[packed_index${part} / 4u];
    }`).join('\n');
      code = code.replace(loop, loads + '\n' + loop)
        .replace(/      var input_k = [^\n]+;\n      if \(\(MATMUL_FLAGS & 128u\)[^\n]+\n/, '')
        .replace(/      if \([^\n]+input_row[^\n]+\) \{\n        a = input_values\[[^\n]+\];\n      }/, `      let word = select(packed_word0, packed_word2, part >= 2u);
      let shift = (select(packed_index0, packed_index2, part >= 2u) % 4u + part % 2u) * 8u;
      let a_metadata = weight_metadata[(word >> shift) & 255u];`)
        .replace('input_values: array<f32>', 'input_values: array<u32>')
        .replace('loaded_a[part] = f16(a * 4.0);', 'loaded_a[part] = a_metadata.x;')
        .replace('ea[part] = f16(select(-100, e4m3_exponent(a), a != 0.0));', 'ea[part] = a_metadata.y;');
      if (!code.includes('let a_metadata = weight_metadata[(word >> shift)')) throw new Error('Missing packed prefetch consumer');
    } else {
      code = code.replace('input_values: array<f32>', 'input_values: array<u32>')
        .replace('      var a = 0.0;', '      var a_metadata = vec2<f16>(0.0h, -100.0h);\n      var a = 0.0;')
        .replace(/        a = input_values\[([^\n]+)\];/, (_, index) => `        let packed_index = ${index};
          let packed_a = input_values[packed_index / 4u];
          a_metadata = weight_metadata[(packed_a >> ((packed_index % 4u) * 8u)) & 255u];`)
        .replace('loaded_a[part] = f16(a * 4.0);', 'loaded_a[part] = a_metadata.x;')
        .replace('ea[part] = f16(select(-100, e4m3_exponent(a), a != 0.0));', 'ea[part] = a_metadata.y;');
      if (!code.includes('let packed_index =')) throw new Error('Missing packed input load');
    }
  }
  if (!output) return code;
  code = code.replace('output_values: array<f32>', 'output_values: array<u32>')
    .replace('native_silu_table: array<vec2<f16>>', 'native_silu_table: array<u32>');
  if (rowOutput) {
    // One invocation owns four adjacent columns in one row, so it can publish
    // a whole packed word without exchanging bytes or adding an end barrier.
    code = code.replace('@workgroup_size(16, 16, 1)', '@workgroup_size(8, 32, 1)')
      .replaceAll('local_id.y * 2u', 'local_id.y')
      .replaceAll('local_id.x * 2u', 'local_id.x * 4u')
      .replaceAll('local_row < 2u', 'local_row < 1u')
      .replaceAll('local_column < 2u', 'local_column < 4u')
      .replaceAll('local_row * 2u + local_column', 'local_row * 4u + local_column')
      .replace(/    let a1 = tile_(?:ea|a)\[[^\n]+\];\n/g, '')
      .replace(/    let b1 = (tile_eb|tile_b)\[\(column \+ 1u\) \* 9u \+ k\];/g, (line, table) => `${line}
    let b2 = ${table}[(column + 2u) * 9u + k];
    let b3 = ${table}[(column + 3u) * 9u + k];`)
      .replaceAll('a1 + b0', 'a0 + b2').replaceAll('a1 + b1', 'a0 + b3');
    for (const part of 'xyzw') code = code
      .replaceAll(`vec4<f16>(a0.${part}, a0.${part}, a1.${part}, a1.${part})`, `vec4<f16>(a0.${part})`)
      .replaceAll(`vec4<f16>(b0.${part}, b1.${part}, b0.${part}, b1.${part})`,
        `vec4<f16>(b0.${part}, b1.${part}, b2.${part}, b3.${part})`);
    if (/\ba1\b/.test(code)) throw new Error('Unconverted two-row packed producer');
  } else code = code
    .replace('tile_ea: array<vec4<f16>, 256>', 'tile_ea: array<vec2<u32>, 256>')
    .replace('tile_ea[local_index] = ea;', 'tile_ea[local_index] = bitcast<vec2<u32>>(ea);')
    .replace(/(let a[01] = )(tile_ea\[[^\n]+?\]);/g, '$1bitcast<vec4<f16>>($2);');
  code = code.replace('struct MatmulParams', `
${PACKED_E4_PUBLICATION_WGSL}
struct MatmulParams`);
  const begin = code.lastIndexOf(`  for (var local_row = 0u; local_row < ${rowOutput ? 1 : 2}u; local_row += 1u) {`);
  const end = code.lastIndexOf('\n}');
  if (begin < 0 || end < begin) throw new Error('Missing packed output publication');
  let tail = code.slice(begin, end);
  const outputIndex = tail.match(/let output_index = ([\s\S]*?);/)?.[1];
  if (!outputIndex) throw new Error('Missing packed output index');
  tail = tail.replace('    let row = row_base + local_row;',
    '    let row = row_base + local_row;\n    var published_pair = 0u;');
  let writes = 0;
  tail = tail.replace(/output_values\[output_index\] = ([^;]+);/g, (_, value) => {
    writes++;
    if (value.includes('native_silu_table')) {
      return `let code = (native_silu_table[index / 4u] >> ((index % 4u) * 8u)) & 255u;
          published_pair |= code << (local_column * 8u);`;
    }
    if (!value.includes('fp8_domain(value)')) throw new Error('Missing E4 quantization contract');
    return 'published_pair |= publish_e4_code(value) << (local_column * 8u);';
  });
  if (writes !== 2) throw new Error(`Expected two E4 publications, got ${writes}`);
  const close = tail.lastIndexOf('  }');
  if (rowOutput) {
    tail = tail.slice(0, close) + `    if (row < MATMUL_ROWS && column_base < MATMUL_N) {
      let column = column_base;
      let output_index = ${outputIndex};
      output_values[output_index / 4u] = published_pair;
    }
` + tail.slice(close);
    return code.slice(0, begin) + tail + code.slice(end);
  }
  tail = tail.slice(0, close) + '    tile_ea[local_id.y * 16u + local_id.x][local_row] = published_pair;\n' + tail.slice(close);
  tail += `
  workgroupBarrier();
  let row = row_group * 32u + local_index / 8u;
  let column = column_base - local_id.x * 2u + (local_index % 8u) * 4u;
  if (row < MATMUL_ROWS && column < MATMUL_N) {
    let owner = (local_index / 16u) * 16u + (local_index % 8u) * 2u;
    let part = (local_index / 8u) % 2u;
    let packed = tile_ea[owner][part] | (tile_ea[owner + 1u][part] << 16u);
    let output_index = ${outputIndex};
    output_values[output_index / 4u] = packed;
  }
`;
  return code.slice(0, begin) + tail + code.slice(end);
}
