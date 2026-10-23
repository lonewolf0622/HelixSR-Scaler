"""Compile an offline MIT graph to HLSL/DXIL and the bounded NRPLAN02 format."""
# SPDX-License-Identifier: MIT
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import time
from nr_shader_normalize import normalize, extract_function
from nr_shader_optimize import optimize_source, remove_redundant_initialization

arguments = argparse.ArgumentParser(description=__doc__)
arguments.add_argument('--graph', type=Path, required=True)
arguments.add_argument('--naga', required=True)
arguments.add_argument('--dxc', required=True)
arguments.add_argument('--jobs', type=int, default=4)
arguments.add_argument('--math', choices=('accurate', 'fast'), default='accurate',
                       help='Accurate preserves NR reductions; fast uses approximate IEEE group sums')
args = arguments.parse_args()
root = Path(__file__).resolve().parent
folder = args.graph.resolve(strict=True)
plan = json.loads((folder / 'plan.json').read_text())
if plan.get('referenceCommit') != '9d08f4184bbcb9d858e2fb7a7834ec0837a9d2f1':
    raise ValueError('Unsupported graph source')
if len(plan['dispatches']) != 451:
    raise ValueError('Expected the complete 451-dispatch graph')
rounding = 'hybrid'
translator = 'naga30-sm62-bounds-ieee-scaled-subnormal-layout-v10'
normalized_directory = 'normalized-optimized'
plan_file = 'runtime-plan.bin'
plan_version = 2
(folder / 'cso').mkdir(exist_ok=True)
(folder / 'hlsl').mkdir(exist_ok=True)
(folder / normalized_directory).mkdir(exist_ok=True)
profiles = {}
jobs = {}
numerics = (root / 'vendor/opendlss-nr/ports/browser-webgpu/shaders/numerics.wgsl').read_text()
for pipeline in plan['pipelines']:
    settings = json.dumps({'shader': pipeline['shader'], 'entry': pipeline['entryPoint'],
                           'constants': pipeline['constants'], 'translator': translator,
                           'math': args.math}, sort_keys=True)
    key = hashlib.sha256(settings.encode()).hexdigest()
    pipeline['compiledFile'] = f'cso/{key}.cso'
    jobs.setdefault(key, pipeline)

def run(command):
    completed = subprocess.run(command, capture_output=True, text=True, timeout=120)
    if completed.returncode:
        raise RuntimeError(f'{command[0]} failed for {command[-2:]}: {completed.stdout[-2000:]}\n{completed.stderr[-7000:]}')

def compile_one(key, pipeline):
    output = folder / pipeline['compiledFile']
    if output.is_file() and output.stat().st_size:
        return key
    hlsl = folder / 'hlsl' / f'{key}.hlsl'
    naga = [args.naga, '--shader-model', '62', '--entry-point', pipeline['entryPoint'],
            '--compact', '--buffer-bounds-check-policy', 'ReadZeroSkipWrite',
            '--index-bounds-check-policy', 'ReadZeroSkipWrite']
    if pipeline['constants']:
        naga += ['--override', ','.join(f'{name}={str(value).lower()}' for name, value in sorted(pipeline['constants'].items()))]
    source = folder / pipeline['shader']
    code, optimization = optimize_source(source.read_text(), pipeline['entryPoint'], args.math)
    code, _ = normalize(code, numerics)
    if rounding == 'hybrid' and 'fn nr_round_f16(' in code:
        code = code.replace(extract_function(code, 'nr_round_f16'), '''fn nr_round_f16(value:f32)->f32 {
  let bits = bitcast<u32>(value);
  let magnitude = bits & 0x7fffffffu;
  if (magnitude > 0x477fe000u) {
    return nr_f16_to_f32(nr_f16_bits(value));
  }
  let normal = unpack2x16float(pack2x16float(vec2<f32>(value, 0.0))).x;
  // A half subnormal is an integer multiple of 2^-24. Scaling by powers
  // of two is exact in f32; WGSL round / DXIL Round_ne supplies ties-to-even.
  // Restore the sign explicitly so negative values rounded to zero stay -0.
  let subnormal = bitcast<f32>(bitcast<u32>(round(abs(value) * 16777216.0) * 0.000000059604644775390625) | (bits & 0x80000000u));
  return select(normal, subnormal, magnitude < 0x38800000u);
}''')
    normalized = folder / normalized_directory / source.name
    # Several pipeline specializations can share this source; identical atomic
    # replacement prevents one translator reading another worker's partial write.
    temporary = normalized.with_name(f'{key}.tmp')
    temporary.write_text(code)
    temporary.replace(normalized)
    naga += [str(normalized), str(hlsl)]
    run(naga)
    translated = remove_redundant_initialization(hlsl.read_text(), optimization)
    hlsl.write_text(translated)
    names = re.findall(r'\[numthreads\([^\]]+\)\]\s*void\s+(\w+)\s*\(', translated)
    if len(names) != 1:
        raise ValueError('Expected exactly one translated compute entry point')
    run([args.dxc, '-T', 'cs_6_2', '-E', names[0], '-enable-16bit-types',
         '-Gis', '-HV', '2018', '-Wno-parentheses-equality', str(hlsl), '-Fo', str(output)])
    return key

started = time.monotonic()
with ThreadPoolExecutor(max_workers=args.jobs) as workers:
    futures = [workers.submit(compile_one, key, pipeline) for key, pipeline in jobs.items()]
    for index, future in enumerate(as_completed(futures), 1):
        future.result()
        if index == 1 or index % 16 == 0 or index == len(futures):
            print(f'compiled {index}/{len(futures)} shaders', flush=True)

resource_ids = {entry['id']: i for i, entry in enumerate(plan['buffers'])}
pipeline_ids = {entry['id']: i for i, entry in enumerate(plan['pipelines'])}
binding_types = {'uniform': 0, 'read-only-storage': 1, 'storage': 2}
writable = {binding['buffer'] for dispatch in plan['dispatches'] for binding in dispatch['bindings']
            if binding['type'] == 'storage'}
for dispatch in plan['dispatches']:
    profile = [(binding['binding'], binding_types[binding['type']]) for binding in dispatch['bindings']]
    existing = profiles.setdefault(dispatch['pipeline'], profile)
    if existing != profile:
        raise ValueError('Pipeline used with inconsistent resource layouts')

with (folder / plan_file).open('wb') as output:
    def u32(*values): output.write(struct.pack('<' + 'I' * len(values), *values))
    def u64(*values): output.write(struct.pack('<' + 'Q' * len(values), *values))
    if plan_version == 1:
        output.write(b'NRPLAN01')
        u32(len(plan['buffers']), len(plan['pipelines']), len(plan['dispatches']),
            resource_ids[plan['outputBuffer']], plan['outputBytes'])
    else:
        output.write(b'NRPLAN02')
        u32(len(plan['buffers']),len(plan['pipelines']),len(plan['dispatches']),
            resource_ids[plan['inputBuffer']],resource_ids[plan['outputBuffer']],plan['outputBytes'],
            plan['geometry']['fullWidth'],plan['geometry']['fullHeight'],plan['width'],plan['height'])
        output.write(hashlib.sha256((folder/'initial-data.bin').read_bytes()).digest())
    for resource in plan['buffers']:
        u64(resource['bytes'], resource['dataOffset'], resource['dataBytes'])
        u32(resource['id'] in writable)
    for pipeline in plan['pipelines']:
        profile = profiles[pipeline['id']]
        u32(len(profile))
        for binding, kind in profile: u32(binding, kind)
        name = pipeline['compiledFile'].encode('ascii')
        u32(len(name)); output.write(name)
        if plan_version == 2:
            output.write(hashlib.sha256((folder/pipeline['compiledFile']).read_bytes()).digest())
    for dispatch in plan['dispatches']:
        u32(pipeline_ids[dispatch['pipeline']], *dispatch['groups'], len(dispatch['bindings']))
        for binding in dispatch['bindings']:
            u32(binding['binding'], resource_ids[binding['buffer']], binding_types[binding['type']])
            u64(binding['offset'], binding['bytes'])
summary = {'pipelines': len(plan['pipelines']), 'uniqueShaders': len(jobs),
           'dispatches': len(plan['dispatches']), 'elapsedSeconds': time.monotonic() - started,
           'rounding': rounding, 'math': args.math, 'planFile': plan_file}
(folder / 'compile-summary.json').write_text(json.dumps(summary, indent=2))
print(json.dumps(summary), flush=True)
