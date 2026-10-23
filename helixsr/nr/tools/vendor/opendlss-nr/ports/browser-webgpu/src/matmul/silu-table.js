import { compileComputePipeline } from './pipeline-compiler.js';

const tables = new WeakMap();

// One device-local table contains both half and E4 publications for every
// possible half input. Build with the retained shader arithmetic, once.
export async function createSiluTable(device, code) {
  if (tables.has(device)) return tables.get(device);
  const build = (async () => {
    const fn = name => {
      const begin = code.indexOf(`fn ${name}(`), end = code.indexOf('\n}', begin) + 2;
      if (begin < 0 || end < 2) throw new Error(`Missing ${name}`);
      return code.slice(begin, end);
    };
    const shader = `enable f16;
fn round_accumulator(value: f32) -> f32 { return f32(f16(value)); }
${fn('mp_cubic_silu')}
${fn('fp8_domain')}
@group(0) @binding(0) var<storage, read_write> values: array<vec2<f16>>;
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
  let value = f32(bitcast<vec2<f16>>(id.x).x);
  let activated = mp_cubic_silu(value);
  values[id.x] = vec2<f16>(f16(activated), f16(fp8_domain(activated)));
}`;
    const module = device.createShaderModule({label: 'NR native SiLU table builder', code: shader});
    const errors = (await module.getCompilationInfo()).messages.filter(m => m.type === 'error');
    if (errors.length) throw new Error(errors.map(m => m.message).join('\n'));
    const pipeline = await compileComputePipeline(device, {layout: 'auto', compute: {module, entryPoint: 'main'}});
    const buffer = device.createBuffer({label: 'NR complete half SiLU and E4 table', size: 65536 * 4,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC});
    const group = device.createBindGroup({layout: pipeline.getBindGroupLayout(0),
      entries: [{binding: 0, resource: {buffer}}]});
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginComputePass();
    pass.setPipeline(pipeline); pass.setBindGroup(0, group); pass.dispatchWorkgroups(256); pass.end();
    device.queue.submit([encoder.finish()]);
    return buffer;
  })();
  tables.set(device, build);
  return build;
}

export function siluTableMatmulCode(code) {
  code = code.replace('struct MatmulParams',
    '@group(0) @binding(5) var<storage, read> native_silu_table: array<vec2<f16>>;\nstruct MatmulParams');
  const original = `        if ((MATMUL_FLAGS & 4u) != 0u) { value = mp_cubic_silu(value); }
        output_values[output_index] = select(value, fp8_domain(value), (MATMUL_FLAGS & 32u) != 0u);`;
  if (!code.includes(original)) throw new Error('Missing native SiLU publication');
  return code.replace(original, `        if ((MATMUL_FLAGS & 4u) != 0u) {
          let index = bitcast<u32>(vec2<f16>(f16(value), 0.0h)) & 65535u;
          output_values[output_index] = f32(native_silu_table[index][select(0u, 1u, (MATMUL_FLAGS & 32u) != 0u)]);
        } else {
          output_values[output_index] = select(value, fp8_domain(value), (MATMUL_FLAGS & 32u) != 0u);
        }`);
}
