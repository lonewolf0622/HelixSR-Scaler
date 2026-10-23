// The shapes the graph is built on: the padded field, the six pooling levels, the window phase cycle, and the
// byte layout of a block's weight tensor. All of it mirrors src/nr_graph.cpp; see docs/network.md for why the
// field rule is what it is. None of this is a free choice - the field size decides which tokens exist, and
// therefore the result inside the valid rectangle as well.

export const alignUp = (value, alignment) => Math.ceil(value / alignment) * alignment;

/**
 * Every level halves its input and rounds up to 4, and the decoder doubles the chain back up, so a dimension
 * needs enough headroom for all of those halvings to be exact. That is what the padded field provides: the
 * valid size aligned to two to the power of the number of size reductions the graph makes on that axis. Six
 * are the halvings; level 0 adds a seventh when it is not a whole number of 8-pixel windows.
 */
function fieldAlignment(valid) {
  let reductions = 0;
  let size = valid;
  for (let level = 0; level < 6; ++level) {
    const half = alignUp(Math.floor((size + 1) / 2), 4);
    if (half < size) reductions += 1;
    if (level === 0 && half % 8 !== 0) reductions += 1;
    size = half;
  }
  return 1 << reductions;
}

// Local optional performance mode: a smaller field changes the token set and
// model context. The original 320-pixel floor remains the default.
export function geometryFromValid(validWidth, validHeight, minimumField = 320) {
  if (minimumField !== 320 && minimumField !== 128) throw new Error('unsupported minimum field');
  const alignWidth = fieldAlignment(validWidth);
  const alignHeight = fieldAlignment(validHeight);
  let fullWidth = Math.max(minimumField, alignUp(validWidth, alignWidth));
  let fullHeight = Math.max(minimumField, alignUp(validHeight, alignHeight));
  // One more alignment step on the width when both axes are a multiple of four alignments. The rule has no
  // stated reason; it has to be reproduced because it moves the window grid.
  if (fullWidth % (4 * alignWidth) === 0 && fullHeight % (4 * alignHeight) === 0) fullWidth += alignWidth;

  const levels = [];
  let width = fullWidth;
  let height = fullHeight;
  for (let level = 0; level < 6; ++level) {
    width = alignUp(Math.floor((width + 1) / 2), 4);
    height = alignUp(Math.floor((height + 1) / 2), 4);
    levels.push({ width, height, rows: width * height });
  }
  if (levels[0].width % 8 || levels[0].height % 8) {
    throw new Error(`unsupported size ${validWidth}x${validHeight}: level 0 ` +
                    `(${levels[0].width}x${levels[0].height}) is not a whole number of 8-pixel windows; ` +
                    'use at least 33 pixels on each axis');
  }
  const vitTokens = levels[5].rows;
  return {
    validWidth, validHeight, fullWidth, fullHeight, levels,
    fullRows: fullWidth * fullHeight,
    vitTokens,
    paddedVitTokens: (vitTokens + 63) & ~63,
  };
}

// The four window views, as origin offsets (-shiftX, -shiftY). Every resolution level runs its own cycle, one
// step per block at that level in visit order, and a decoder stage continues the count its encoder stage left.
const PHASES = [[0, 0], [4, 4], [4, 0], [0, 4]];
export const windowPhase = (index) => PHASES[index & 3];

/** Level 6 is the un-pooled field (blocks 0 and 70); 0..5 are the pooled levels. */
export class WindowPhases {
  constructor() { this.counters = new Int32Array(7); }
  take(level) { return this.counters[level]++; }
  reset() { this.counters.fill(0); }
}

const standardHidden = (channels) => {
  if (channels === 32 || channels === 64 || channels === 128 || channels === 256) return 128;
  throw new Error(`no fused layout for ${channels} channels`);
};

/**
 * Byte offsets inside one block's weight tensor. A block is FFN -> QKV -> window attention -> projection, and
 * the tensor holds the matrices in that order with two 16-byte pads whose purpose is not visible.
 */
export function fusedLayout(channels, base = 0) {
  const hidden = standardHidden(channels);
  const heads = channels / 32;
  const expertFfn = channels >= 64;
  const expertCount = expertFfn ? channels / 32 : 0;
  const expandBytes = expertFfn ? expertCount * channels * 128 : channels * hidden;
  const ffnWeightBytes = expertFfn ? expandBytes + expertCount * 128 * 32 + expertCount * 32 * channels
                                   : expandBytes + hidden * channels;
  const l = { hidden, heads, expertFfn, expertCount, expand: base, contractWeights: base + expandBytes };
  l.ffnCosSkip = base + ffnWeightBytes + 16;
  l.qkv = l.ffnCosSkip + channels * 2 + 16;
  l.relative = l.qkv + channels * channels * 3;
  l.scale = l.relative + heads * 8192;
  l.projection = l.scale + alignUp(heads * 4, 16);
  l.attnCosSkip = l.projection + channels * channels;
  l.endWithoutPadding = l.attnCosSkip + channels * 2;
  return l;
}

/** Block 0: the same block with a 16 -> 32 f16 input adapter in front of it. */
export const preFusedLayout = () => ({
  hidden: 128, heads: 1, expertFfn: false, expertCount: 0,
  expand: 0, contractWeights: 4096, inputAdapter: 8208, ffnCosSkip: 9232, qkv: 9312,
  relative: 12384, scale: 20576, projection: 20592, attnCosSkip: 21616, endWithoutPadding: 21680,
});

/** The first block of a decoder stage: the 2x upsample weight and the skip scale sit before the QKV. */
export function upsampleFusedLayout(inputChannels, channels) {
  if (inputChannels !== channels * 2) throw new Error('upsample layout expects 2x input channels');
  const hidden = standardHidden(channels);
  const heads = channels / 32;
  const narrowPadding = channels === 32 ? 16 : 0;
  const expertFfn = channels >= 64;
  const expertCount = expertFfn ? channels / 32 : 0;
  const expandBytes = expertFfn ? expertCount * channels * 128 : channels * hidden;
  const ffnWeightBytes = expertFfn ? expandBytes + expertCount * 128 * 32 + expertCount * 32 * channels
                                   : expandBytes + hidden * channels;
  const l = { hidden, heads, expertFfn, expertCount, expand: 0, contractWeights: expandBytes };
  l.upsampleWeight = ffnWeightBytes;
  l.ffnCosSkip = l.upsampleWeight + inputChannels * channels + narrowPadding;
  l.transitionScale = l.ffnCosSkip + channels * 2 + narrowPadding;
  l.qkv = l.transitionScale + channels * 2;
  l.relative = l.qkv + channels * channels * 3;
  l.scale = l.relative + heads * 8192;
  l.projection = l.scale + alignUp(heads * 4, 16);
  l.attnCosSkip = l.projection + channels * channels;
  l.endWithoutPadding = l.attnCosSkip + channels * 2;
  return l;
}

/** Block 70: the post blend's two scales in front, the 32 -> 4 f16 head at the end. */
export const postFusedLayout = () => ({
  hidden: 128, heads: 1, expertFfn: false, expertCount: 0,
  expand: 0, contractWeights: 4096, ffnCosSkip: 8208, inputScale: 8272, adapterScale: 8336,
  qkv: 8400, relative: 11472, scale: 19664, projection: 19680, attnCosSkip: 20704,
  postWeights: 20784, endWithoutPadding: 21808,
});
