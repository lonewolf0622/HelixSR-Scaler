"""BC250-tuned layouts for the pinned NR graph; accurate arithmetic is default."""
# SPDX-License-Identifier: MIT
import hashlib
import re

from nr_shader_normalize import extract_function


MATRIX_SOURCES = {
    '7d438f8fe9f46c2ad50fb3a71381be27b103f009ac830ecc3d8306e6595ad7e9',
    'fbe2cc0db1b0310b9a9000dbdf2090febf5b5702e1d4e862ad520d4cbd3d8340',
    '77d6b7d87b303a6787448be2db906261a6b5556787efc68c2fecede1eb456680',
    'c462c181d4a36fb0ec6bdd2b0ffc372f46bcc97327e267b0d16e0769649094c9',
}
WINDOW_SOURCE = '9903cba20514ba991a0b242f463c437f07213588a87b072bdc13cedb111fbd70'


def retile_matrix(code):
    """Keep the 32x32 tile and each output's reduction order, using 256 lanes."""
    if '@workgroup_size(8, 16, 1)' in code:
        code = code.replace('@workgroup_size(8, 16, 1)', '@workgroup_size(8, 32, 1)')
        code = code.replace('local_id.y * 2u', 'local_id.y')
        code = code.replace('local_row < 2u', 'local_row < 1u')
    elif '@workgroup_size(16, 8, 1)' in code:
        code = code.replace('@workgroup_size(16, 8, 1)', '@workgroup_size(16, 16, 1)')
        code = code.replace('local_id.y * 4u', 'local_id.y * 2u')
        code = code.replace('local_row < 4u', 'local_row < 2u')
    elif '@workgroup_size(16, 16, 1)' in code and 'var quad1 =' not in code:
        return code  # This storage variant already has 256 lanes.
    else:
        raise ValueError('Unsupported NR matrix ownership')
    code = code.replace('array<f32, 8>()', 'array<f32, 4>()')
    code = code.replace('accumulator < 8u', 'accumulator < 4u')
    code = code.replace('tile_index += 128u', 'tile_index += 256u')
    code = '\n'.join(line for line in code.splitlines() if not (
        'var quad1 =' in line or 'quad1 = quad_fdpa_' in line or 'sums[4] = quad1' in line))
    if 'quad1' in code:
        raise ValueError('Incomplete NR matrix ownership transform')
    return code


def fast_reductions(code):
    """Optional approximation: IEEE sums within the existing half-publication groups.

    This deliberately omits Ada's product alignment/truncation. It preserves
    group boundaries, incoming half C, K partitions and final publications,
    but is not bit-equivalent to the accurate graph.
    """
    names = re.findall(r'fn (quad_fdpa_\d+_\d+)\(', code)
    if len(names) != 14:
        raise ValueError('Unsupported NR reduction functions')
    for name in names:
        original = extract_function(code, name)
        header = original[:original.index('{') + 1]
        body = original[original.index('  let alignment ='):]
        body = re.sub(r'  let alignment =[^;]+;', '  let alignment = vec4<f32>(16.0);', body)
        body = re.sub(r'  let product_alignment =[^;]+;', '  let product_alignment = vec4<f32>(1.0);', body)
        body = re.sub(r'  let scale =[^;]+;', '  let scale = vec4<f32>(0.0625);', body)
        body = body.replace('trunc(', '(')
        code = code.replace(original, header + '\n' + body)
    return code


def optimize_source(code, entry, math_mode):
    if math_mode not in ('accurate', 'fast'):
        raise ValueError('Unsupported NR arithmetic mode')
    digest = hashlib.sha256(code.encode('utf-8')).hexdigest()
    if 'fn quad_fdpa_' in code:
        if digest not in MATRIX_SOURCES or entry != 'main':
            raise ValueError('Unrecognized NR matrix source; optimization requires a new audit')
        code = retile_matrix(code)
        if math_mode == 'fast':
            code = fast_reductions(code)
        return code, 'matrix'
    if entry == 'attend_window_tiled':
        if digest != WINDOW_SOURCE:
            raise ValueError('Unrecognized NR window source; initialization requires a new audit')
        return code, 'window'
    return code, None


def remove_redundant_initialization(hlsl, kind):
    """Elide only audited Naga clears whose consumed entries are explicitly filled.

    Matrix loaders cover every A/B value and exponent, including guarded edges,
    before the first barrier. B's ninth padding vector is never consumed.
    Window attention fills all 64 norms, 256 queries, 512 K/V/score vectors,
    512 packed exponents and 32 reciprocals before their respective barriers.
    Padded K/V slots are unread; reused scratch is overwritten before each use.
    Source hashes above bind this reasoning to the audited shader versions.
    """
    if kind is None:
        return hlsl
    allowed = {'tile_a', 'tile_b', 'tile_ea', 'tile_eb'} if kind == 'matrix' else {
        'window_norms', 'vector_q', 'vector_k', 'vector_v', 'vector_scores',
        'packed_k', 'packed_scores', 'tiled_reciprocals'}
    pattern = (r'    if \([A-Za-z_]\w* == 0u?\) \{\n'
               r'((?:        [A-Za-z_]\w* = [^\n]+\n)+)    \}\n'
               r'    GroupMemoryBarrierWithGroupSync\(\);\n')
    matches = list(re.finditer(pattern, hlsl))
    if len(matches) != 1:
        raise ValueError('Unexpected Naga workgroup initialization')
    match = matches[0]
    variables = set(re.findall(r'^        (\w+) = ', match[1], re.M))
    if variables != allowed:
        raise ValueError('Unexpected initialized NR workgroup arrays')
    return hlsl[:match.start()] + hlsl[match.end():]
