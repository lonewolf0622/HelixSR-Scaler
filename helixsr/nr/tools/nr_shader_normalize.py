"""Preserve the reference's explicit half-publication semantics across shader backends.

MIT-derived helpers are read from the pinned OpenDLSS-NR numerics.wgsl source.
The native half type remains available for storage and exact FP8 products.
"""
import re

def extract_function(source, name):
    start = source.index(f'fn {name}(')
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        if end >= len(source):
            raise ValueError(f'Unclosed function: {name}')
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def closing_parenthesis(source, opening):
    depth, end = 1, opening + 1
    while depth:
        if end >= len(source):
            raise ValueError('Unclosed constructor')
        depth += (source[end] == '(') - (source[end] == ')')
        end += 1
    return end - 1

def normalize(code, numerics):
    # The driver must not fold a narrowing immediately widened to f32. Force
    # the publication through the already-validated integer half-bit rounder.
    publication = re.compile(r'(?P<outer>f32|vec[234]<f32>)\s*\(\s*(?P<inner>f16|vec[234]<f16>)\s*\(')
    sites = 0
    for match in reversed(list(publication.finditer(code))):
        outer, inner = match['outer'], match['inner']
        width = 1 if outer == 'f32' else int(outer[3])
        if inner != ('f16' if width == 1 else f'vec{width}<f16>'):
            continue
        opening = code.index('(', match.start())
        inner_opening = match.end() - 1
        inner_end = closing_parenthesis(code, inner_opening)
        outer_end = closing_parenthesis(code, opening)
        if code[inner_end + 1:outer_end].strip():
            continue  # an f16 expression, rather than a directly widened constructor
        argument = code[inner_opening + 1:inner_end]
        helper = 'nr_round_f16' if width == 1 else f'nr_round_half{width}'
        code = code[:match.start()] + f'{helper}({outer}({argument}))' + code[outer_end + 1:]
        sites += 1
    if sites:
        names = ('round_shift_right_even', 'f16_bits', 'f16_to_f32', 'round_f16')
        helpers = '\n'.join(extract_function(numerics, name) for name in names)
        for name in names:
            helpers = re.sub(rf'\b{name}\b', f'nr_{name}', helpers)
        for width in (2, 3, 4):
            components = ', '.join(f'nr_round_f16(value.{component})' for component in 'xyzw'[:width])
            helpers += f'\nfn nr_round_half{width}(value:vec{width}<f32>)->vec{width}<f32>{{return vec{width}<f32>({components});}}\n'
        code += '\n' + helpers

    # Naga does not accept storage-array pointer parameters in HLSL. These
    # reference helpers are called with a fixed global buffer, so specialize it.
    for helper in ('half_at', 'e4_at', 'scene_rgb'):
        pattern = (rf'fn {helper}\(buffer: ptr<storage, array<u32>, read>, (?P<args>[^)]+)\) -> (?P<result>[^{{]+)'
                   r'\{(?P<body>[^{}]+)\}')
        match = re.search(pattern, code)
        if not match:
            continue
        targets = sorted(set(re.findall(rf'\b{helper}\(&([A-Za-z_]\w*),\s*', code)))
        body, arguments, result = match['body'], match['args'], match['result'].strip()
        code = code[:match.start()] + code[match.end():]
        for target in targets:
            code = re.sub(rf'\b{helper}\(&{target},\s*', f'{helper}_{target}(', code)
            code += f'\nfn {helper}_{target}({arguments}) -> {result} {{' + body.replace('(*buffer)', target) + '}\n'
        if re.search(rf'\b{helper}\(', code):
            raise ValueError(f'Unresolved storage-pointer helper: {helper}')
    if 'bitcast<vec2<f16>>' in code:
        code = code.replace('bitcast<vec2<f16>>', 'nr_unpack_half_pair')
        code += '\nfn nr_unpack_half_pair(bits:u32)->vec2<f16>{return vec2<f16>(unpack2x16float(bits));}\n'
    if 'bitcast<u32>(vec2<f16>' in code:
        code = code.replace('bitcast<u32>(vec2<f16>', 'nr_pack_half_pair(vec2<f16>')
        code += '\nfn nr_pack_half_pair(value:vec2<f16>)->u32{return pack2x16float(vec2<f32>(value));}\n'
    return code, sites
