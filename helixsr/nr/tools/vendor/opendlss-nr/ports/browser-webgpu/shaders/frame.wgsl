// The frame around the network: turning a rendered image into the sixteen input lanes, and turning the head
// back into a picture. See docs/frame.md.
//
// The network does not see the rendered image. It sees a *display proxy* of it - the scene divided by paper
// white, pushed through a soft shoulder, sRGB encoded, and centred on the half grid - which is a low dynamic
// range stand-in with the same tone response a display would give. It also sees the previous frame's output,
// centred the same way, so the temporal decision is made on like against like.

struct Params {
  full_width : u32,
  full_height : u32,
  valid_width : u32,
  valid_height : u32,
  seed : u32,
  history_valid : u32,
  nr_enabled : u32,
  image_pitch : u32,        // pixels per row of `image`, padded so each row is a multiple of 256 bytes
  paper_white : f32,
  style : f32,
  local_tone : f32,
  local_structure : f32,
  skin_structure : f32,
  auto_mask : f32,
  blend_scale : f32,
  intensity : f32,
  color_strength : f32,
  // WebGL reads a framebuffer bottom-up and WebGPU presents top-down, so a frame that arrives through a
  // readback has to be flipped. The velocity vectors flip with it: their v component is measured in the
  // same space as the rows they came from.
  flip_y : u32,
};

@group(0) @binding(0) var<uniform> params : Params;
@group(0) @binding(1) var<storage, read> scene : array<u32>;      // rgba16float, two channels per word
@group(0) @binding(2) var<storage, read> history : array<u32>;    // rgba16float, the previous frame's output
@group(0) @binding(3) var<storage, read> head : array<f32>;       // f32 [full rows][4]
@group(0) @binding(4) var<storage, read> motion : array<u32>;     // rg16float, current -> previous, in uv
@group(0) @binding(5) var<storage, read_write> features : array<f32>;
@group(0) @binding(6) var<storage, read_write> next_history : array<u32>;
@group(0) @binding(7) var<storage, read_write> image : array<u32>;   // packed bgra8unorm for the canvas

/// The row this pixel lives on in an incoming buffer.
fn source_row(y: u32) -> u32 {
  return select(y, params.valid_height - 1u - y, params.flip_y != 0u);
}

fn scene_rgb(buffer: ptr<storage, array<u32>, read>, pixel: u32) -> vec3<f32> {
  let base = pixel * 2u;                      // four halves = two words
  let rg = (*buffer)[base];
  let ba = (*buffer)[base + 1u];
  return vec3<f32>(f16_to_f32(rg & 0xffffu), f16_to_f32(rg >> 16u), f16_to_f32(ba & 0xffffu));
}

fn motion_at(x: u32, y: u32) -> vec2<f32> {
  let word = motion[source_row(y) * params.valid_width + x];
  let value = vec2<f32>(f16_to_f32(word & 0xffffu), f16_to_f32(word >> 16u));
  return select(value, vec2<f32>(value.x, -value.y), params.flip_y != 0u);
}

/// The rendered frame at a top-down pixel.
fn scene_at(x: u32, y: u32) -> vec3<f32> {
  return scene_rgb(&scene, source_row(y) * params.valid_width + x);
}

fn history_texel(x: i32, y: i32) -> vec3<f32> {
  let cx = u32(clamp(x, 0, i32(params.valid_width) - 1));
  let cy = u32(clamp(y, 0, i32(params.valid_height) - 1));
  return scene_rgb(&history, cy * params.valid_width + cx);
}

fn history_bilinear(uv: vec2<f32>) -> vec3<f32> {
  let size = vec2<f32>(f32(params.valid_width), f32(params.valid_height));
  let position = uv * size - vec2<f32>(0.5);
  let base = floor(position);
  let f = position - base;
  let x = i32(base.x);
  let y = i32(base.y);
  let top = mix(history_texel(x, y), history_texel(x + 1, y), f.x);
  let bottom = mix(history_texel(x, y + 1), history_texel(x + 1, y + 1), f.x);
  return mix(top, bottom, f.y);
}

/// Where this pixel was last frame, reconstructed with a five-tap Catmull-Rom.
///
/// The filter is not decoration. The history is fed back into the network's own input, so any softening here
/// compounds: a box or bilinear fetch loses a little detail every frame, and after a second of standing still
/// the image has visibly melted. Catmull-Rom keeps the edges the network just reconstructed.
fn reprojected_history(px: u32, py: u32) -> vec3<f32> {
  let valid = vec2<f32>(f32(params.valid_width), f32(params.valid_height));
  let uv = (vec2<f32>(f32(px), f32(py)) + vec2<f32>(0.5)) / valid;
  let position = (motion_at(px, py) + uv) * valid;
  let base = floor(position - vec2<f32>(0.5)) + vec2<f32>(0.5);
  let f = clamp(position - base, vec2<f32>(0.0), vec2<f32>(1.0));
  let square = f * f;
  let cube = f * square;
  let w0 = square - 0.5 * (f + cube);
  let w1 = (cube * 1.5 - square * 2.5) + vec2<f32>(1.0);
  let w3 = (cube - square) * 0.5;
  let w2 = ((vec2<f32>(1.0) - w0) - w1) - w3;
  let middle = w1 + w2;
  // Four of the sixteen taps are folded into the bilinear unit's own weights; that is the whole trick.
  let low = clamp(base - vec2<f32>(1.0), vec2<f32>(0.5), valid - vec2<f32>(0.5)) / valid;
  let center = clamp(base + w2 / middle, vec2<f32>(0.5), valid - vec2<f32>(0.5)) / valid;
  let high = clamp(base + vec2<f32>(2.0), vec2<f32>(0.5), valid - vec2<f32>(0.5)) / valid;
  let a = w0.x * middle.y;
  let b = w0.y * middle.x;
  let c = middle.x * middle.y;
  let d = w3.y * middle.x;
  let e = w3.x * middle.y;
  var value = history_bilinear(vec2<f32>(low.x, center.y)) * a + history_bilinear(vec2<f32>(center.x, low.y)) * b;
  value = history_bilinear(center) * c + value;
  value = history_bilinear(vec2<f32>(center.x, high.y)) * d + value;
  value = history_bilinear(vec2<f32>(high.x, center.y)) * e + value;
  return value * (1.0 / (e + (d + (c + (a + b)))));
}

fn srgb_encode(value: f32) -> f32 {
  let bounded = clamp(value, 0.0, 1.0);
  return select(1.055 * pow(bounded, 1.0 / 2.4) - 0.055, 12.92 * bounded, bounded <= 0.0031308);
}

fn srgb_decode(value: f32) -> f32 {
  let bounded = clamp(value, 0.0, 1.0);
  return select(pow((bounded + 0.055) / 1.055, 2.4), bounded / 12.92, bounded <= 0.04045);
}

// ---------------------------------------------------------------------------------------------------------
// The display transform.
//
// The network works in display-proxy code values, which are low dynamic range by construction. The frame it
// came from is not: the renderer hands over a linear HDR scene, and what reaches the canvas has to be that
// scene with the network's correction applied, not the proxy the network happened to work in. Three steps
// stand between them, and dropping any of them changes the picture:
//
//   * the tone upgrade rescales the neural result to the original's luminance while keeping its hue and
//     chroma in Oklab, so the correction survives the return to HDR instead of being flattened by it;
//   * the luminance-only blend is what `colorStrength` turns down - at zero the network moves brightness and
//     nothing else;
//   * the display transform is the renderer's own: ACES on the RRT/ODT fit, then sRGB. The canvas shows the
//     same operator the rendered frame would have gone through unaided, which is what makes the two
//     comparable at all.
// ---------------------------------------------------------------------------------------------------------

fn row3(a: vec3<f32>, b: vec3<f32>, c: vec3<f32>, value: vec3<f32>) -> vec3<f32> {
  return vec3<f32>(dot(a, value), dot(b, value), dot(c, value));
}

fn luminance(color: vec3<f32>) -> f32 {
  return dot(color, vec3<f32>(0.212639, 0.715169, 0.072192));
}

fn to_oklab(color: vec3<f32>) -> vec3<f32> {
  var lms = row3(vec3<f32>(0.4122214708, 0.5363325363, 0.0514459929),
                 vec3<f32>(0.2119034982, 0.6806995451, 0.1073969566),
                 vec3<f32>(0.0883024619, 0.2817188376, 0.6299787005), color);
  lms = sign(lms) * pow(abs(lms), vec3<f32>(1.0 / 3.0));
  return row3(vec3<f32>(0.2104542553, 0.7936177850, -0.0040720468),
              vec3<f32>(1.9779984951, -2.4285922050, 0.4505937099),
              vec3<f32>(0.0259040371, 0.7827717662, -0.8086757660), lms);
}

fn from_oklab(lab: vec3<f32>) -> vec3<f32> {
  var lms = row3(vec3<f32>(1.0, 0.3963377774, 0.2158037573),
                 vec3<f32>(1.0, -0.1055613458, -0.0638541728),
                 vec3<f32>(1.0, -0.0894841775, -1.2914855480), lab);
  lms = lms * lms * lms;
  return row3(vec3<f32>(4.0767416621, -3.3077115913, 0.2309699292),
              vec3<f32>(-1.2684380046, 2.6097574011, -0.3413193965),
              vec3<f32>(-0.0041960863, -0.7034186147, 1.7076147010), lms);
}

/// Back through AP1 with the negatives removed: the rescale can leave a colour outside the working gamut.
fn clamp_ap1(color: vec3<f32>) -> vec3<f32> {
  let ap1 = max(row3(vec3<f32>(0.613097, 0.339523, 0.047379),
                     vec3<f32>(0.070194, 0.916354, 0.013452),
                     vec3<f32>(0.020616, 0.109570, 0.869815), color), vec3<f32>(0.0));
  return row3(vec3<f32>(1.705051, -0.621792, -0.083259),
              vec3<f32>(-0.130256, 1.140805, -0.010548),
              vec3<f32>(-0.024003, -0.128969, 1.152972), ap1);
}

/// The lightness of one colour carrying the hue and chroma direction of another.
fn hue_oklab(incorrect: vec3<f32>, correct: vec3<f32>) -> vec3<f32> {
  var result = to_oklab(incorrect);
  let correct_lab = to_oklab(correct);
  let incorrect_chroma = length(result.yz);
  let correct_chroma = length(correct_lab.yz);
  let scale = select(incorrect_chroma / correct_chroma, 1.0, correct_chroma == 0.0);
  result = vec3<f32>(result.x, correct_lab.y * scale, correct_lab.z * scale);
  return clamp_ap1(from_oklab(result));
}

fn upgrade_tone_map(original: vec3<f32>, proxy: vec3<f32>, neural: vec3<f32>) -> vec3<f32> {
  let original_y = luminance(original);
  let proxy_y = luminance(proxy);
  let neural_y = luminance(neural);
  if (neural_y <= 0.00001) { return original; }
  // Below the proxy's luminance the scene is inside the proxy's range and the ratio is a plain rescale;
  // above it, the headroom the proxy's shoulder compressed is added back.
  let ratio = select((neural_y + max(0.0, original_y - proxy_y)) / neural_y,
                     original_y / max(proxy_y, 0.000001), original_y < proxy_y);
  return original + (hue_oklab(neural * ratio, neural) - original);
}

fn aces_fit(value: f32) -> f32 {
  return (value * (value + 0.0245786) - 0.000090537)
       / (value * (0.983729 * value + 0.4329510) + 0.238081);
}

/// The renderer's display transform: into the ACES working space, the RRT/ODT fit, back out, then sRGB.
fn display_transform(scene: vec3<f32>) -> vec3<f32> {
  var color = row3(vec3<f32>(0.59719, 0.35458, 0.04823),
                   vec3<f32>(0.07600, 0.90834, 0.01566),
                   vec3<f32>(0.02840, 0.13383, 0.83777), scene / 0.6);
  color = vec3<f32>(aces_fit(color.r), aces_fit(color.g), aces_fit(color.b));
  color = clamp(row3(vec3<f32>(1.60475, -0.53108, -0.07367),
                     vec3<f32>(-0.10208, 1.10813, -0.00605),
                     vec3<f32>(-0.00327, -0.07276, 1.07602), color), vec3<f32>(0.0), vec3<f32>(1.0));
  return vec3<f32>(srgb_encode(color.r), srgb_encode(color.g), srgb_encode(color.b));
}

// ---------------------------------------------------------------------------------------------------------
// The two conditioning styles, which are a colour grade on the network's output rather than anything the
// network does: an exposure, a smoothstep contrast and a saturation offset, all scaled by the local tone and
// applied in HSL. Style 1 is the cinematic preset, style 2 the natural one.
// ---------------------------------------------------------------------------------------------------------

fn nr_hsl(rgb: vec3<f32>) -> vec3<f32> {
  let high = max(max(rgb.r, rgb.g), rgb.b);
  let low = min(min(rgb.r, rgb.g), rgb.b);
  let sum = high + low;
  let light = sum * 0.5;
  var hue = 0.0;
  var saturation = 0.0;
  if (high > low) {
    let delta = high - low;
    saturation = select(delta / sum, delta / ((2.0 - high) - low), light > 0.5);
    if (high == rgb.r) {
      hue = fma(rgb.g - rgb.b, 1.0 / delta, select(0.0, 6.0, rgb.g < rgb.b)) / 6.0;
    } else if (high == rgb.g) {
      hue = fma(rgb.b - rgb.r, 1.0 / delta, 2.0) / 6.0;
    } else {
      hue = fma(rgb.r - rgb.g, 1.0 / delta, 4.0) / 6.0;
    }
  }
  return vec3<f32>(hue, saturation, light);
}

fn nr_hue(p: f32, q: f32, hue: f32) -> f32 {
  var h = hue;
  if (h < 0.0) { h = h + 1.0; }
  if (h > 1.0) { h = h - 1.0; }
  if (h < bitcast<f32>(0x3e2aaaabu)) { return fma(h, (q - p) * 6.0, p); }
  if (h < 0.5) { return q; }
  if (h < bitcast<f32>(0x3f2aaaabu)) { return fma((bitcast<f32>(0x3f2aaaabu) - h) * (q - p), 6.0, p); }
  return p;
}

fn nr_rgb(hsl: vec3<f32>) -> vec3<f32> {
  if (hsl.y <= 0.0) { return vec3<f32>(hsl.z); }
  let q = select(fma(-hsl.z, hsl.y, hsl.z + hsl.y), hsl.z * (hsl.y + 1.0), hsl.z < 0.5);
  let p = (hsl.z + hsl.z) - q;
  return vec3<f32>(nr_hue(p, q, hsl.x + bitcast<f32>(0x3eaaaaabu)),
                   nr_hue(p, q, hsl.x), nr_hue(p, q, hsl.x - bitcast<f32>(0x3eaaaaabu)));
}

fn nr_style(neural: vec3<f32>, style: f32, tone: f32) -> vec3<f32> {
  // The conditioning lanes accept a tone up to two; the grade caps its own adjustment at one.
  let style_tone = clamp(tone, 0.0, 1.0);
  let exposure = select(0.0, -0.1 * style_tone, style == 1.0);
  let contrast = select(0.0, -0.25 * style_tone, style == 1.0);
  let saturation = select(-0.15 * style_tone, -0.1 * style_tone, style == 1.0);
  var color: vec3<f32>;
  for (var c = 0u; c < 3u; c = c + 1u) {
    let value = clamp(neural[c], 0.0, 1.0);
    let exposed = clamp(value * exp2(exposure), 0.0, 1.0);
    let square = exposed * exposed;
    let curve_delta = fma(square, 3.0 - (exposed + exposed), -exposed);
    let curved = clamp(fma(contrast, curve_delta, exposed), 0.0, 1.0);
    color[c] = exp2(log2(max(0.0, exp2(log2(curved)))));
  }
  let hsl = nr_hsl(color);
  let adjusted = nr_rgb(vec3<f32>(hsl.x, clamp(hsl.y * (saturation + 1.0), 0.0, 1.0), hsl.z));
  let again = nr_hsl(adjusted);
  return clamp(nr_rgb(vec3<f32>(again.x, clamp(exp2(log2(again.y)), 0.0, 1.0), again.z)),
               vec3<f32>(0.0), vec3<f32>(1.0));
}


/// The display proxy: paper-white relative, a soft shoulder above 0.75, sRGB encoded, on the half grid.
fn proxy_component(value: f32) -> f32 {
  let finite = select(0.0, max(value, 0.0), value == value && abs(value) <= 65504.0);
  var relative = finite / max(params.paper_white, 0.05);
  if (relative > 0.75) { relative = 0.75 + 0.25 * (1.0 - exp(-5.770780 * (relative - 0.75))); }
  return round_f16(srgb_encode(relative));
}

/// What the network is given: the code value, centred and scaled down by eight.
fn centre(code: f32) -> f32 { return round_f16(round_f16(round_f16(code) - 0.5) * 0.125); }

// ---------------------------------------------------------------------------------------------------------
// The noise lanes.
//
// Three Box-Muller Gaussians per pixel, evaluated in the order native evaluates them. The order matters
// because the transcendentals are the hardware's approximate ones rather than correctly rounded, which also
// makes these three lanes the one part of a frame a port cannot reproduce bit for bit: log2, cos and sin on
// one vendor's units are not the same function as on another's. Everything downstream of the features is
// exact, which is why the parity harness reads recorded features instead of generating them.
// ---------------------------------------------------------------------------------------------------------

fn hash_uniform(value: u32) -> f32 {
  var mixed = value;
  mixed = (mixed >> ((mixed >> 28u) + 4u)) ^ mixed;
  mixed = mixed * 0x108ef2d9u;
  let integer = ((mixed >> 30u) ^ (mixed >> 8u)) + 1u;
  return f32(integer) * bitcast<f32>(0x33800000u);
}

fn gaussian3(x: u32, y: u32, seed: u32) -> vec3<f32> {
  var base = (x * 0x8da6b343u) ^ (seed * 0x9e3779b9u) ^ (y * 0xd8163841u) ^ 0x243f6a88u;
  base = (base >> ((base >> 28u) + 4u)) ^ base;
  base = base * 0x108ef2d9u;
  base = (base >> 22u) ^ base;
  let u0 = hash_uniform(base * 0x2c9277b5u + 0xac564b05u);
  let u1 = hash_uniform(base * 0xfa6dc5f9u + 0x4712a88eu);
  let u2 = hash_uniform(base * 0xcaa5b80du + 0x21dd796bu);
  let u3 = hash_uniform(base * 0x83232c31u + 0x3463e0acu);
  let radius0 = sqrt(log2(u0) * bitcast<f32>(0x3f317218u) * -2.0);
  let radius1 = sqrt(log2(u2) * bitcast<f32>(0x3f317218u) * -2.0);
  let angle0 = u1 * bitcast<f32>(0x40c90fdbu);
  let angle1 = u3 * bitcast<f32>(0x40c90fdbu);
  return vec3<f32>(round_f16(radius0 * cos(angle0)), round_f16(radius0 * sin(angle0)),
                   round_f16(radius1 * cos(angle1)));
}

@compute @workgroup_size(8, 8)
fn input_features(@builtin(global_invocation_id) id : vec3<u32>) {
  if (id.x >= params.full_width || id.y >= params.full_height) { return; }
  // Outside the valid image the source is mirrored, while the noise still hashes the padded coordinate: the
  // padding has to look like image, but it must not repeat the image's noise.
  let source_x = select(2u * params.valid_width - id.x - 2u, id.x, id.x < params.valid_width);
  let source_y = select(2u * params.valid_height - id.y - 2u, id.y, id.y < params.valid_height);
  let rgb = scene_at(source_x, source_y);
  let code = vec3<f32>(proxy_component(rgb.r), proxy_component(rgb.g), proxy_component(rgb.b));
  let centred = vec3<f32>(centre(code.r), centre(code.g), centre(code.b));
  var previous = centred;
  if (params.history_valid != 0u) {
    let h = reprojected_history(source_x, source_y);
    previous = vec3<f32>(centre(h.r), centre(h.g), centre(h.b));
  }

  let noise = gaussian3(id.x, id.y, params.seed);
  let base = (id.y * params.full_width + id.x) * 16u;
  features[base + 0u] = noise.x;
  features[base + 1u] = noise.y;
  features[base + 2u] = noise.z;
  features[base + 3u] = 1.0;
  features[base + 4u] = centred.r;
  features[base + 5u] = centred.g;
  features[base + 6u] = centred.b;
  features[base + 7u] = previous.r;
  features[base + 8u] = previous.g;
  features[base + 9u] = previous.b;
  features[base + 10u] = params.style / 128.0;
  features[base + 11u] = round_f16(params.local_tone);
  features[base + 12u] = round_f16(select(params.local_structure, 1.0, params.auto_mask > 0.0));
  features[base + 13u] = round_f16(select(-1.0,
      select(params.skin_structure, params.local_structure, params.skin_structure < 0.0),
      params.auto_mask > 0.0));
  features[base + 14u] = round_f16(select(-1.0, params.local_structure, params.auto_mask > 0.0));
  features[base + 15u] = 0.0;
}

/// Truncation toward zero to the half grid. The history is stored truncated, not rounded - it is fed back
/// into the network's input, and rounding it would let a value drift upward frame after frame.
fn truncate_half(value: f32) -> u32 {
  let bits = bitcast<u32>(value);
  let sign = (bits >> 16u) & 0x8000u;
  let exponent = (bits >> 23u) & 0xffu;
  let mantissa = bits & 0x7fffffu;
  if (exponent == 0xffu) { return sign | select(0x7c00u, 0x7e00u, mantissa != 0u); }
  let half_exponent = i32(exponent) - 112;
  if (half_exponent >= 31) { return sign | 0x7c00u; }
  if (half_exponent <= 0) {
    if (half_exponent < -10) { return sign; }
    return sign | ((mantissa | 0x800000u) >> u32(14 - half_exponent));
  }
  return sign | (u32(half_exponent) << 10u) | (mantissa >> 13u);
}

/// Write one pixel of the presented image, in sRGB code values.
///
/// The canvas is bgra8unorm, which is what it wants on most machines. The row index uses the padded pitch,
/// not the image width: copyBufferToTexture requires each row to start on a 256-byte boundary, and writing
/// packed rows into a padded buffer shears the picture diagonally.
fn present(x: u32, y: u32, rgb: vec3<f32>) {
  let quantized = vec3<u32>(clamp(round(rgb * 255.0), vec3<f32>(0.0), vec3<f32>(255.0)));
  image[y * params.image_pitch + x] = quantized.b | (quantized.g << 8u) | (quantized.r << 16u) | 0xff000000u;
}

@compute @workgroup_size(8, 8)
fn compose(@builtin(global_invocation_id) id : vec3<u32>) {
  if (id.x >= params.valid_width || id.y >= params.valid_height) { return; }
  let pixel = id.y * params.valid_width + id.x;
  let field = (id.y * params.full_width + id.x) * 4u;

  let scene = scene_at(id.x, id.y);
  let paper = max(params.paper_white, 0.05);
  let original = scene / paper;
  let code = vec3<f32>(proxy_component(scene.r), proxy_component(scene.g), proxy_component(scene.b));

  // Switching the network off leaves the rendered frame itself on screen, through the same display transform
  // the composed result goes through. That is the honest comparison: only the network's contribution differs.
  if (params.nr_enabled == 0u) {
    let stored = vec3<u32>(truncate_half(code.r), truncate_half(code.g), truncate_half(code.b));
    next_history[pixel * 2u] = stored.r | (stored.g << 16u);
    next_history[pixel * 2u + 1u] = stored.b | (truncate_half(1.0) << 16u);
    present(id.x, id.y, display_transform(scene));
    return;
  }

  // The head's first three channels are a residual added at a quarter of its value, in code space.
  var neural = clamp(code + vec3<f32>(head[field], head[field + 1u], head[field + 2u]) * 0.25,
                     vec3<f32>(0.0), vec3<f32>(1.0));

  if (params.history_valid != 0u) {
    // The fourth channel is a logit: how much of the reprojected history to keep, decided per pixel by the
    // network itself rather than by a hand-written rejection rule.
    let weight = clamp(1.0 / (1.0 + exp2(head[field + 3u] * -1.4426950408889634)) * params.blend_scale,
                       0.0, 1.0);
    let previous = reprojected_history(id.x, id.y);
    neural = neural + (previous - neural) * weight;
  }

  // The history is what the next frame reads back as its own input, so it is the blended code value and
  // nothing downstream of it: no style, no intensity, no display transform.
  let stored = vec3<u32>(truncate_half(neural.r), truncate_half(neural.g), truncate_half(neural.b));
  next_history[pixel * 2u] = stored.r | (stored.g << 16u);
  next_history[pixel * 2u + 1u] = stored.b | (truncate_half(1.0) << 16u);
  let history = vec3<f32>(f16_to_f32(stored.r), f16_to_f32(stored.g), f16_to_f32(stored.b));

  let styled = select(neural, nr_style(history, params.style, params.local_tone), params.style != 0.0);
  var published : vec3<f32>;
  for (var c = 0u; c < 3u; c = c + 1u) {
    if (params.style == 0.0 && params.intensity == 1.0) {
      published[c] = history[c];
    } else {
      // Intensity dials the whole correction back towards the proxy the network started from.
      published[c] = f16_to_f32(truncate_half(
        clamp(fma(params.intensity, styled[c] - code[c], code[c]), 0.0, 1.0)));
    }
  }

  // Back out of code space and into the scene the frame arrived in: the network's correction rescaled to the
  // original's luminance, then as much of its colour as colorStrength asks for, then the display transform.
  let proxy_linear = vec3<f32>(srgb_decode(code.r), srgb_decode(code.g), srgb_decode(code.b));
  let neural_linear = vec3<f32>(srgb_decode(published.r), srgb_decode(published.g),
                                srgb_decode(published.b));
  let upgraded = upgrade_tone_map(original, proxy_linear, neural_linear);
  let original_y = luminance(original);
  let ratio = select(clamp(luminance(upgraded) / original_y, 0.0, 4.0), 1.0, original_y == 0.0);
  let luminance_only = original * ratio;
  let result = (luminance_only + (upgraded - luminance_only) * params.color_strength) * paper;
  present(id.x, id.y, display_transform(result));
}
