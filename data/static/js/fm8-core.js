// FM8 preview engine: JavaScript port of components/msfa (the DX7 core from Google's
// "music-synthesizer-for-android", Apache-2.0, as used by Dexed/MicroDexed and by the
// device's nodes/Fm8Node.h via msfa_bridge). Same fixed-point tables and arithmetic, so the
// browser preview sounds like the device. Integer math is done on JS numbers: every
// intermediate stays below 2^53 except a few products that are shifted right by >= 24 bits
// straight away (noted inline), where the lost low bits do not reach the result.
//
// Patch layout = Fm8Patch.h / fm8.js: 155 voice bytes, [155] operator on/off mask,
// [156] post low-pass cutoff (99 = off), [157] resonance.

const LG_N = 6;
const N = 1 << LG_N; // msfa renders 64-sample blocks
const Q24 = 1 << 24;
export const FM8_PATCH_BYTES = 158;

// ---------------------------------------------------------------------------------------------
// Tables (sin.cpp, exp2.cpp, freqlut.cpp) - built once per sample rate.

const SIN_LG = 10;
const SIN_N = 1 << SIN_LG;
const sintab = new Int32Array(SIN_N << 1); // [dy, y0] pairs
const EXP2_LG = 10;
const EXP2_N = 1 << EXP2_LG;
const exp2tab = new Int32Array(EXP2_N << 1);
const FREQ_LG = 10;
const FREQ_N = 1 << FREQ_LG;
const FREQ_SHIFT = 24 - FREQ_LG;
const MAX_LOGFREQ_INT = 20;
const freqlut = new Int32Array(FREQ_N + 1);
let tablesSampleRate = 0;
let envSrMultiplier = Q24;
let lfoUnit = 0;
let pitchEnvUnit = 0;

function initTables(sampleRate) {
  if (tablesSampleRate === sampleRate) return;
  tablesSampleRate = sampleRate;
  for (let i = 0; i < SIN_N; i += 1) sintab[(i << 1) + 1] = Math.round(Math.sin((2 * Math.PI * i) / SIN_N) * Q24);
  for (let i = 0; i < SIN_N - 1; i += 1) sintab[i << 1] = sintab[(i << 1) + 3] - sintab[(i << 1) + 1];
  sintab[(SIN_N << 1) - 2] = -sintab[(SIN_N << 1) - 1];

  for (let i = 0; i < EXP2_N; i += 1) exp2tab[(i << 1) + 1] = Math.floor(2 ** 30 * 2 ** (i / EXP2_N) + 0.5);
  for (let i = 0; i < EXP2_N - 1; i += 1) exp2tab[i << 1] = exp2tab[(i << 1) + 3] - exp2tab[(i << 1) + 1];
  exp2tab[(EXP2_N << 1) - 2] = 2 ** 31 - exp2tab[(EXP2_N << 1) - 1];

  let y = 2 ** (24 + MAX_LOGFREQ_INT) / sampleRate;
  const inc = 2 ** (1 / FREQ_N);
  for (let i = 0; i < FREQ_N + 1; i += 1) {
    freqlut[i] = Math.floor(y + 0.5);
    y *= inc;
  }

  envSrMultiplier = Math.floor((44100 / sampleRate) * Q24);
  lfoUnit = Math.floor((N * 25190424) / sampleRate + 0.5);
  pitchEnvUnit = Math.floor((N * Q24) / (21.3 * sampleRate) + 0.5);
}

// phase: 2^24 per cycle (int32, wraps). Returns Q24 sine.
function sinLookup(phase) {
  const lowbits = phase & 0x3fff;
  const pi = (phase >> 13) & ((SIN_N - 1) << 1);
  return sintab[pi + 1] + Math.floor((sintab[pi] * lowbits) / 16384);
}

// Q24 in, Q24 out (2^x).
function exp2Lookup(x) {
  const lowbits = x & 0x3fff;
  const xi = (x >> 13) & ((EXP2_N - 1) << 1);
  const y = exp2tab[xi + 1] + Math.floor((exp2tab[xi] * lowbits) / 16384);
  return Math.floor(y / 2 ** (6 - (x >> 24)));
}

// Q24 log2(Hz) -> phase delta per sample (2^24 per cycle).
function freqLookup(logfreq) {
  const ix = (logfreq & 0xffffff) >> FREQ_SHIFT;
  const y0 = freqlut[ix];
  const y1 = freqlut[ix + 1];
  const lowbits = logfreq & ((1 << FREQ_SHIFT) - 1);
  const y = y0 + Math.floor(((y1 - y0) * lowbits) / (1 << FREQ_SHIFT));
  return Math.floor(y / 2 ** (MAX_LOGFREQ_INT - (logfreq >> 24)));
}

// ---------------------------------------------------------------------------------------------
// Envelope (env.cpp)

const levellut = [0, 5, 9, 13, 17, 20, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 42, 43, 45, 46];
function scaleoutlevel(outlevel) {
  return outlevel >= 20 ? 28 + outlevel : levellut[outlevel];
}

class Env {
  constructor() {
    this.rates = [0, 0, 0, 0];
    this.levels = [0, 0, 0, 0];
    this.outlevel = 0;
    this.rateScaling = 0;
    this.level = 0;
    this.targetlevel = 0;
    this.rising = false;
    this.ix = 0;
    this.inc = 0;
    this.down = true;
  }

  init(r, l, ol, rateScaling) {
    for (let i = 0; i < 4; i += 1) {
      this.rates[i] = r[i];
      this.levels[i] = l[i];
    }
    this.outlevel = ol;
    this.rateScaling = rateScaling;
    this.level = 0;
    this.down = true;
    this.advance(0);
  }

  getsample() {
    if (this.ix < 3 || (this.ix < 4 && !this.down)) {
      if (this.rising) {
        const jumptarget = 1716;
        if (this.level < jumptarget << 16) this.level = jumptarget << 16;
        this.level += (((17 << 24) - this.level) >> 24) * this.inc;
        if (this.level >= this.targetlevel) {
          this.level = this.targetlevel;
          this.advance(this.ix + 1);
        }
      } else {
        this.level -= this.inc;
        if (this.level <= this.targetlevel) {
          this.level = this.targetlevel;
          this.advance(this.ix + 1);
        }
      }
    }
    return this.level;
  }

  keydown(d) {
    if (this.down !== d) {
      this.down = d;
      this.advance(d ? 0 : 3);
    }
  }

  advance(newix) {
    this.ix = newix;
    if (this.ix < 4) {
      let actuallevel = scaleoutlevel(this.levels[this.ix]) >> 1;
      actuallevel = (actuallevel << 6) + this.outlevel - 4256;
      actuallevel = actuallevel < 16 ? 16 : actuallevel;
      this.targetlevel = actuallevel << 16;
      this.rising = this.targetlevel > this.level;
      let qrate = (this.rates[this.ix] * 41) >> 6;
      qrate += this.rateScaling;
      qrate = Math.min(qrate, 63);
      this.inc = (4 + (qrate & 3)) << (2 + LG_N + (qrate >> 2));
      this.inc = Math.floor((this.inc * envSrMultiplier) / Q24);
    }
  }

  update(r, l, ol, rateScaling) {
    for (let i = 0; i < 4; i += 1) {
      this.rates[i] = r[i];
      this.levels[i] = l[i];
    }
    this.outlevel = ol;
    this.rateScaling = rateScaling;
    if (this.down) {
      let actuallevel = scaleoutlevel(this.levels[2]) >> 1;
      actuallevel = (actuallevel << 6) - 4256;
      actuallevel = actuallevel < 16 ? 16 : actuallevel;
      this.targetlevel = actuallevel << 16;
      this.advance(2);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Pitch envelope (pitchenv.cpp)

const pitchenvRate = [
  1, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
  12, 13, 13, 14, 14, 15, 16, 16, 17, 18, 18, 19, 20, 21, 22, 23, 24,
  25, 26, 27, 28, 30, 31, 33, 34, 36, 37, 38, 39, 41, 42, 44, 46, 47,
  49, 51, 53, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 79, 82,
  85, 88, 91, 94, 98, 102, 106, 110, 115, 120, 125, 130, 135, 141, 147,
  153, 159, 165, 171, 178, 185, 193, 202, 211, 232, 243, 254, 255,
];
const pitchenvTab = [
  -128, -116, -104, -95, -85, -76, -68, -61, -56, -52, -49, -46, -43,
  -41, -39, -37, -35, -33, -32, -31, -30, -29, -28, -27, -26, -25, -24,
  -23, -22, -21, -20, -19, -18, -17, -16, -15, -14, -13, -12, -11, -10,
  -9, -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
  11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
  28, 29, 30, 31, 32, 33, 34, 35, 38, 40, 43, 46, 49, 53, 58, 65, 73,
  82, 92, 103, 115, 127,
];

class PitchEnv {
  constructor() {
    this.rates = [0, 0, 0, 0];
    this.levels = [0, 0, 0, 0];
    this.level = 0;
    this.targetlevel = 0;
    this.rising = false;
    this.ix = 0;
    this.inc = 0;
    this.down = true;
  }

  set(r, l) {
    for (let i = 0; i < 4; i += 1) {
      this.rates[i] = r[i];
      this.levels[i] = l[i];
    }
    this.level = pitchenvTab[l[3]] << 19;
    this.down = true;
    this.advance(0);
  }

  getsample() {
    if (this.ix < 3 || (this.ix < 4 && !this.down)) {
      if (this.rising) {
        this.level += this.inc;
        if (this.level >= this.targetlevel) {
          this.level = this.targetlevel;
          this.advance(this.ix + 1);
        }
      } else {
        this.level -= this.inc;
        if (this.level <= this.targetlevel) {
          this.level = this.targetlevel;
          this.advance(this.ix + 1);
        }
      }
    }
    return this.level;
  }

  keydown(d) {
    if (this.down !== d) {
      this.down = d;
      this.advance(d ? 0 : 3);
    }
  }

  advance(newix) {
    this.ix = newix;
    if (this.ix < 4) {
      this.targetlevel = pitchenvTab[this.levels[this.ix]] << 19;
      this.rising = this.targetlevel > this.level;
      this.inc = pitchenvRate[this.rates[this.ix]] * pitchEnvUnit;
    }
  }
}

// ---------------------------------------------------------------------------------------------
// LFO (lfo.cpp) - phase is an unsigned 32-bit accumulator.

class Lfo {
  constructor() {
    this.phase = 0;
    this.delta = 0;
    this.waveform = 0;
    this.randstate = 0;
    this.sync = false;
    this.delaystate = 0;
    this.delayinc = 0;
    this.delayinc2 = 0;
  }

  reset(p, offset) {
    const rate = p[offset]; // LFS
    let sr = rate === 0 ? 1 : (165 * rate) >> 6;
    sr *= sr < 160 ? 11 : 11 + ((sr - 160) >> 4);
    this.delta = (lfoUnit * sr) >>> 0;
    let a = 99 - p[offset + 1]; // LFD
    if (a === 99) {
      this.delayinc = 0xffffffff;
      this.delayinc2 = 0xffffffff;
    } else {
      a = (16 + (a & 15)) << (1 + (a >> 4));
      this.delayinc = (lfoUnit * a) >>> 0;
      a &= 0xff80;
      a = Math.max(0x80, a);
      this.delayinc2 = (lfoUnit * a) >>> 0;
    }
    this.waveform = p[offset + 5]; // LFW
    this.sync = p[offset + 4] !== 0; // LKS
  }

  // 0..1 in Q24
  getsample() {
    this.phase = (this.phase + this.delta) >>> 0;
    const p = this.phase;
    switch (this.waveform) {
      case 0: { // triangle
        let x = p >>> 7;
        x ^= -(p >>> 31);
        x &= Q24 - 1;
        return x;
      }
      case 1: // sawtooth down
        return (~p ^ 0x80000000) >>> 8;
      case 2: // sawtooth up
        return (p ^ 0x80000000) >>> 8;
      case 3: // square
        return (~p >>> 7) & Q24;
      case 4: // sine
        return (1 << 23) + (sinLookup(p >>> 8) >> 1);
      case 5: { // s&h
        if (p < this.delta) this.randstate = (this.randstate * 179 + 17) & 0xff;
        const x = this.randstate ^ 0x80;
        return (x + 1) << 16;
      }
      default:
        return 1 << 23;
    }
  }

  // 0..1 in Q24
  getdelay() {
    const delta = this.delaystate < 2 ** 31 ? this.delayinc : this.delayinc2;
    const d = this.delaystate + delta;
    if (d > 0xffffffff) return Q24;
    this.delaystate = d;
    if (d < 2 ** 31) return 0;
    return (d >>> 7) & (Q24 - 1);
  }

  keydown() {
    if (this.sync) this.phase = 0x7fffffff;
    this.delaystate = 0;
  }
}

// ---------------------------------------------------------------------------------------------
// Operator kernels + algorithm router (fm_op_kernel.cpp, fm_core.cpp)

const OUT_BUS_ADD = 1 << 2;
const ALGORITHMS = [
  [0xc1, 0x11, 0x11, 0x14, 0x01, 0x14], [0x01, 0x11, 0x11, 0x14, 0xc1, 0x14],
  [0xc1, 0x11, 0x14, 0x01, 0x11, 0x14], [0xc1, 0x11, 0x94, 0x01, 0x11, 0x14],
  [0xc1, 0x14, 0x01, 0x14, 0x01, 0x14], [0xc1, 0x94, 0x01, 0x14, 0x01, 0x14],
  [0xc1, 0x11, 0x05, 0x14, 0x01, 0x14], [0x01, 0x11, 0xc5, 0x14, 0x01, 0x14],
  [0x01, 0x11, 0x05, 0x14, 0xc1, 0x14], [0x01, 0x05, 0x14, 0xc1, 0x11, 0x14],
  [0xc1, 0x05, 0x14, 0x01, 0x11, 0x14], [0x01, 0x05, 0x05, 0x14, 0xc1, 0x14],
  [0xc1, 0x05, 0x05, 0x14, 0x01, 0x14], [0xc1, 0x05, 0x11, 0x14, 0x01, 0x14],
  [0x01, 0x05, 0x11, 0x14, 0xc1, 0x14], [0xc1, 0x11, 0x02, 0x25, 0x05, 0x14],
  [0x01, 0x11, 0x02, 0x25, 0xc5, 0x14], [0x01, 0x11, 0x11, 0xc5, 0x05, 0x14],
  [0xc1, 0x14, 0x14, 0x01, 0x11, 0x14], [0x01, 0x05, 0x14, 0xc1, 0x14, 0x14],
  [0x01, 0x14, 0x14, 0xc1, 0x14, 0x14], [0xc1, 0x14, 0x14, 0x14, 0x01, 0x14],
  [0xc1, 0x14, 0x14, 0x01, 0x14, 0x04], [0xc1, 0x14, 0x14, 0x14, 0x04, 0x04],
  [0xc1, 0x14, 0x14, 0x04, 0x04, 0x04], [0xc1, 0x05, 0x14, 0x01, 0x14, 0x04],
  [0x01, 0x05, 0x14, 0xc1, 0x14, 0x04], [0x04, 0xc1, 0x11, 0x14, 0x01, 0x14],
  [0xc1, 0x14, 0x01, 0x14, 0x04, 0x04], [0x04, 0xc1, 0x11, 0x14, 0x04, 0x04],
  [0xc1, 0x14, 0x04, 0x04, 0x04, 0x04], [0xc4, 0x04, 0x04, 0x04, 0x04, 0x04],
];

// gain for sample i is gain1 + (1 + i) / 64 * (gain2 - gain1); y * gain < 2^52, exact.
function kernelCompute(output, input, phase0, freq, gain1, gain2, add) {
  const dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
  let gain = gain1;
  let phase = phase0;
  for (let i = 0; i < N; i += 1) {
    gain += dgain;
    const y = sinLookup((phase + input[i]) | 0);
    const y1 = Math.floor((y * gain) / Q24);
    if (add) output[i] += y1;
    else output[i] = y1;
    phase = (phase + freq) | 0;
  }
}

function kernelComputePure(output, phase0, freq, gain1, gain2, add) {
  const dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
  let gain = gain1;
  let phase = phase0;
  for (let i = 0; i < N; i += 1) {
    gain += dgain;
    const y = sinLookup(phase);
    const y1 = Math.floor((y * gain) / Q24);
    if (add) output[i] += y1;
    else output[i] = y1;
    phase = (phase + freq) | 0;
  }
}

function kernelComputeFb(output, phase0, freq, gain1, gain2, fbBuf, fbShift, add) {
  const dgain = (gain2 - gain1 + (N >> 1)) >> LG_N;
  let gain = gain1;
  let phase = phase0;
  let y0 = fbBuf[0];
  let y = fbBuf[1];
  for (let i = 0; i < N; i += 1) {
    gain += dgain;
    const scaledFb = (y0 + y) >> (fbShift + 1);
    y0 = y;
    y = sinLookup((phase + scaledFb) | 0);
    y = Math.floor((y * gain) / Q24);
    if (add) output[i] += y;
    else output[i] = y;
    phase = (phase + freq) | 0;
  }
  fbBuf[0] = y0;
  fbBuf[1] = y;
}

const busBuf = [new Int32Array(N), new Int32Array(N)];

function coreRender(output, params, algorithm, fbBuf, fbShift) {
  const kLevelThresh = 1120;
  const alg = ALGORITHMS[algorithm];
  const hasContents = [true, false, false];
  for (let op = 0; op < 6; op += 1) {
    const flags = alg[op];
    let add = (flags & OUT_BUS_ADD) !== 0;
    const param = params[op];
    const inbus = (flags >> 4) & 3;
    const outbus = flags & 3;
    const outptr = outbus === 0 ? output : busBuf[outbus - 1];
    const gain1 = param.gainOut;
    const gain2 = exp2Lookup(param.levelIn - 14 * Q24);
    param.gainOut = gain2;
    if (gain1 >= kLevelThresh || gain2 >= kLevelThresh) {
      if (!hasContents[outbus]) add = false;
      if (inbus === 0 || !hasContents[inbus]) {
        if ((flags & 0xc0) === 0xc0 && fbShift < 16) {
          kernelComputeFb(outptr, param.phase, param.freq, gain1, gain2, fbBuf, fbShift, add);
        } else {
          kernelComputePure(outptr, param.phase, param.freq, gain1, gain2, add);
        }
      } else {
        kernelCompute(outptr, busBuf[inbus - 1], param.phase, param.freq, gain1, gain2, add);
      }
      hasContents[outbus] = true;
    } else if (!add) {
      hasContents[outbus] = false;
    }
    param.phase = (param.phase + (param.freq << LG_N)) | 0;
  }
}

// ---------------------------------------------------------------------------------------------
// Note (dx7note.cpp)

const FEEDBACK_BITDEPTH = 8;
const coarsemul = [
  -16777216, 0, 16777216, 26591258, 33554432, 38955489, 43368474, 47099600,
  50331648, 53182516, 55732705, 58039632, 60145690, 62083076, 63876816,
  65546747, 67108864, 68576247, 69959732, 71268397, 72509921, 73690858,
  74816848, 75892776, 76922906, 77910978, 78860292, 79773775, 80654032,
  81503396, 82323963, 83117622,
];
const velocityData = [
  0, 70, 86, 97, 106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163,
  166, 170, 173, 174, 178, 181, 184, 186, 189, 190, 194, 196, 198, 200, 202,
  205, 206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229, 230, 232,
  233, 235, 237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251,
  252, 253, 254,
];
const expScaleData = [
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 16, 19, 23, 27, 33, 39, 47, 56, 66,
  80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250,
];
const pitchmodsenstab = [0, 10, 20, 33, 55, 92, 153, 255];
const ampmodsenstab = [0, 4342338, 7171437, 16777216];

function midinoteToLogfreq(midinote) {
  const base = 50857777; // (1 << 24) * (log(440) / log(2) - 69/12)
  const step = Math.floor(Q24 / 12);
  return base + step * midinote;
}

function oscFreq(midinote, mode, coarse, fine, detune) {
  let logfreq;
  if (mode === 0) {
    logfreq = midinoteToLogfreq(midinote);
    const detuneRatio = (0.0209 * Math.exp(-0.396 * (logfreq / Q24))) / 7;
    logfreq = Math.trunc(logfreq + detuneRatio * logfreq * (detune - 7));
    logfreq += coarsemul[coarse & 31];
    if (fine) logfreq += Math.floor(24204406.323123 * Math.log(1 + 0.01 * fine) + 0.5);
  } else {
    logfreq = (4458616 * ((coarse & 3) * 100 + fine)) >> 3;
    logfreq += detune > 7 ? 13457 * (detune - 7) : 0;
  }
  return logfreq;
}

function scaleVelocity(velocity, sensitivity) {
  const clamped = Math.max(0, Math.min(127, velocity));
  const velValue = velocityData[clamped >> 1] - 239;
  return ((sensitivity * velValue + 7) >> 3) << 4;
}

function scaleRate(midinote, sensitivity) {
  const x = Math.min(31, Math.max(0, Math.trunc(midinote / 3) - 7));
  return (sensitivity * x) >> 3;
}

function scaleCurve(group, depth, curve) {
  let scale;
  if (curve === 0 || curve === 3) {
    scale = (group * depth * 329) >> 12;
  } else {
    const rawExp = expScaleData[Math.min(group, expScaleData.length - 1)];
    scale = (rawExp * depth * 329) >> 15;
  }
  if (curve < 2) scale = -scale;
  return scale;
}

function scaleLevel(midinote, breakPt, leftDepth, rightDepth, leftCurve, rightCurve) {
  const offset = midinote - breakPt - 17;
  if (offset >= 0) return scaleCurve(Math.trunc((offset + 1) / 3), rightDepth, rightCurve);
  return scaleCurve(Math.trunc(-(offset - 1) / 3), leftDepth, leftCurve);
}

class Dx7Note {
  constructor() {
    this.env = [];
    this.params = [];
    for (let op = 0; op < 6; op += 1) {
      this.env.push(new Env());
      this.params.push({ levelIn: 0, gainOut: 0, freq: 0, phase: 0 });
    }
    this.pitchenv = new PitchEnv();
    this.basepitch = [0, 0, 0, 0, 0, 0];
    this.fbBuf = [0, 0];
    this.fbShift = 16;
    this.ampmodsens = [0, 0, 0, 0, 0, 0];
    this.opMode = [0, 0, 0, 0, 0, 0];
    this.ampmoddepth = 0;
    this.algorithm = 0;
    this.pitchmoddepth = 0;
    this.pitchmodsens = 0;
    this.rates = [0, 0, 0, 0];
    this.levels = [0, 0, 0, 0];
  }

  _opParams(patch, op, midinote, velocity) {
    const off = op * 21;
    for (let i = 0; i < 4; i += 1) {
      this.rates[i] = patch[off + i];
      this.levels[i] = patch[off + 4 + i];
    }
    let outlevel = scaleoutlevel(patch[off + 16]);
    outlevel += scaleLevel(midinote, patch[off + 8], patch[off + 9], patch[off + 10], patch[off + 11], patch[off + 12]);
    outlevel = Math.min(127, outlevel);
    outlevel <<= 5;
    outlevel += scaleVelocity(velocity, patch[off + 15]);
    outlevel = Math.max(0, outlevel);
    const rateScaling = scaleRate(midinote, patch[off + 13]);
    this.basepitch[op] = oscFreq(midinote, patch[off + 17], patch[off + 18], patch[off + 19], patch[off + 20]);
    this.opMode[op] = patch[off + 17];
    this.ampmodsens[op] = ampmodsenstab[patch[off + 14] & 3];
    return { outlevel, rateScaling };
  }

  _globals(patch) {
    this.algorithm = patch[134];
    const feedback = patch[135];
    this.fbShift = feedback !== 0 ? FEEDBACK_BITDEPTH - feedback : 16;
    this.pitchmoddepth = (patch[139] * 165) >> 6;
    this.pitchmodsens = pitchmodsenstab[patch[143] & 7];
    this.ampmoddepth = (patch[140] * 165) >> 6;
  }

  init(patch, midinote, velocity) {
    for (let op = 0; op < 6; op += 1) {
      const { outlevel, rateScaling } = this._opParams(patch, op, midinote, velocity);
      this.env[op].init(this.rates, this.levels, outlevel, rateScaling);
    }
    for (let i = 0; i < 4; i += 1) {
      this.rates[i] = patch[126 + i];
      this.levels[i] = patch[130 + i];
    }
    this.pitchenv.set(this.rates, this.levels);
    this._globals(patch);
  }

  update(patch, midinote, velocity) {
    for (let op = 0; op < 6; op += 1) {
      const { outlevel, rateScaling } = this._opParams(patch, op, midinote, velocity);
      this.env[op].update(this.rates, this.levels, outlevel, rateScaling);
    }
    this._globals(patch);
  }

  keyup() {
    for (let op = 0; op < 6; op += 1) this.env[op].keydown(false);
    this.pitchenv.keydown(false);
  }

  compute(buf, lfoVal, lfoDelay, ctrls) {
    // ==== PITCH ====
    const pmd = this.pitchmoddepth * lfoDelay; // < 2^32
    const senslfo = this.pitchmodsens * (lfoVal - (1 << 23));
    // pmd * senslfo may reach 2^63: double rounding is far below the >> 39 that follows.
    let pmod1 = Math.abs(Math.floor((pmd * senslfo) / 2 ** 39));
    const pmod2 = Math.abs(Math.floor((ctrls.pitchMod * senslfo) / 2 ** 14));
    let pitchMod = Math.max(pmod1, pmod2);
    pitchMod = this.pitchenv.getsample() + pitchMod * (senslfo < 0 ? -1 : 1);

    // ---- PITCH BEND ----
    let pb = ctrls.pitch - 0x2000;
    if (pb !== 0) {
      if (ctrls.pitchStep === 0) {
        pb = Math.trunc(((pb << 11) * ctrls.pitchRange) / 12);
      } else {
        const stp = Math.trunc(12 / ctrls.pitchStep);
        pb = Math.trunc((pb * stp) / 8191);
        pb = (pb * Math.trunc(8191 / stp)) << 11;
      }
    }
    const pitchBase = pb + ctrls.masterTune;
    pitchMod += pitchBase;

    // ==== AMP MOD ====
    lfoVal = Q24 - lfoVal;
    let amod1 = Math.floor((this.ampmoddepth * lfoDelay) / 256);
    amod1 = Math.floor((amod1 * lfoVal) / Q24);
    const amod2 = Math.floor((ctrls.ampMod * lfoVal) / 128);
    let amdMod = Math.max(amod1, amod2);

    // ==== EG AMP MOD ====
    const amod3 = (ctrls.egMod + 1) << 17;
    amdMod = Math.max(Q24 - amod3, amdMod);

    // ==== OP RENDER ====
    for (let op = 0; op < 6; op += 1) {
      const param = this.params[op];
      if (!(ctrls.opSwitch & (1 << op))) {
        this.env[op].getsample(); // advance the envelope even if it is not playing
        param.levelIn = 0;
      } else {
        param.freq = freqLookup(this.basepitch[op] + (this.opMode[op] ? pitchBase : pitchMod));
        let level = this.env[op].getsample();
        if (this.ampmodsens[op] !== 0) {
          const sensamp = Math.floor((amdMod * this.ampmodsens[op]) / Q24);
          const pt = Math.trunc(Math.exp((sensamp / 262144) * 0.07 + 12.2)) >>> 0;
          // level * (pt << 4) can exceed 2^53; the >> 28 that follows hides the rounding.
          const ldiff = Math.floor((level * pt * 16) / 2 ** 28);
          level -= ldiff;
        }
        param.levelIn = level;
      }
    }
    coreRender(buf, this.params, this.algorithm, this.fbBuf, this.fbShift);
  }
}

// ---------------------------------------------------------------------------------------------
// Voice (msfa_bridge.cpp) + widget state (nodes/Fm8Node.h)

const CV_MAX_VOLTS = 10;
const AUDIO_PEAK_VOLTS = 5;
const kBlock = N;

class Voice {
  constructor() {
    this.note = new Dx7Note();
    this.lfo = new Lfo();
    this.ctrls = {
      pitch: 0x2000, pitchRange: 2, pitchStep: 0, masterTune: 0,
      ampMod: 0, pitchMod: 0, egMod: 127, opSwitch: 0x3f,
    };
    this.keyDown = false;
    // Fm8State
    this.gate = false;
    this.idle = true;
    this.midiNote = 60; // as played (before tune)
    this.velocity = 100; // 1..127
    this.silentBlocks = 0;
    this.appliedVersion = 0;
    this.appliedOctaves = 0;
    this.lp = 0;
    this.bp = 0;
    this.filterCutoffByte = 0xff;
    this.filterResByte = 0xff;
    this.filterF = 0;
    this.filterQ = 0;
    this.buf = new Int32Array(kBlock);
  }

  noteOn(patch, midiNote, velocity) {
    this.note.init(patch, midiNote, Math.max(1, Math.min(127, velocity)));
    this.lfo.reset(patch, 137);
    this.lfo.keydown();
    this.ctrls.opSwitch = patch[155] !== 0 ? patch[155] & 0x3f : 0x3f;
    this.keyDown = true;
  }

  noteOff() {
    if (!this.keyDown) return;
    this.note.keyup();
    this.keyDown = false;
  }

  update(patch, midiNote, velocity) {
    this.note.update(patch, midiNote, Math.max(1, Math.min(127, velocity)));
    this.lfo.reset(patch, 137);
    this.ctrls.opSwitch = patch[155] !== 0 ? patch[155] & 0x3f : 0x3f;
  }

  // One 64-sample block into out[offset..], +-1.0. Returns the block's peak magnitude.
  renderBlock(out, offset, pitchBend14, pitchRangeSemis) {
    this.ctrls.pitch = pitchBend14;
    this.ctrls.pitchRange = pitchRangeSemis;
    const buf = this.buf;
    buf.fill(0);
    const lfoValue = this.lfo.getsample();
    const lfoDelay = this.lfo.getdelay();
    this.note.compute(buf, lfoValue, lfoDelay, this.ctrls);
    let peak = 0;
    for (let i = 0; i < kBlock; i += 1) {
      // msfa output is ~24 bit; Dexed scales it to 16 bit with >> 13 and saturates.
      let v = buf[i] >> 13;
      v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
      const f = v / 32768;
      out[offset + i] = f;
      const a = f < 0 ? -f : f;
      if (a > peak) peak = a;
    }
    return peak;
  }
}

function tuneOffsetVolts(tuneVolts) {
  const v = Math.max(0, Math.min(CV_MAX_VOLTS, tuneVolts));
  return v - CV_MAX_VOLTS * 0.5;
}

export function fm8KeysToPatch(keys, out = new Uint8Array(FM8_PATCH_BYTES)) {
  for (let i = 0; i < 155; i += 1) out[i] = Math.max(0, Math.min(255, Math.round(Number(keys[`d${i}`]) || 0)));
  const ops = Number(keys.ops);
  out[155] = Number.isFinite(ops) ? ops & 0x3f : 0x3f;
  out[156] = Math.max(0, Math.min(99, Math.round(Number(keys.d156 ?? 99))));
  out[157] = Math.max(0, Math.min(99, Math.round(Number(keys.d157 ?? 0))));
  return out;
}

// One FM8 widget: a pool of voice slots (the firmware's per-voice NodeInstances), one shared
// patch. Slot = the midi-in voice index; slot 0 doubles as the mono/CV-gate voice.
export function createFm8Engine(sampleRate, options = {}) {
  initTables(sampleRate);
  const maxSlots = Math.max(1, Math.min(32, Math.round(options.maxSlots ?? 16)));
  const patch = new Uint8Array(FM8_PATCH_BYTES);
  fm8KeysToPatch(options.keys || {}, patch);
  let patchVersion = 1;
  const slots = [];
  for (let i = 0; i < maxSlots; i += 1) slots.push(new Voice());
  const state = {
    tuneVolts: Number(options.tune ?? 5),
    gainCv: Number(options.gain ?? CV_MAX_VOLTS),
    // Loudness like the device's voiced->global fan-in (Edge::scale = 1/sqrt(N)).
    fanInScale: 1,
  };
  let scratch = new Float32Array(0);

  function tuneOctaves() {
    return Math.trunc(tuneOffsetVolts(state.tuneVolts));
  }

  function tuneBend14() {
    const rest = tuneOffsetVolts(state.tuneVolts) - tuneOctaves();
    const bend = 0x2000 + Math.round(rest * 8192);
    return Math.max(0, Math.min(0x3fff, bend));
  }

  function effectiveNote(slot) {
    const n = slot.midiNote + 12 * tuneOctaves();
    return Math.max(0, Math.min(127, n));
  }

  function gateOn(slotIndex, midiNote, velocity127) {
    const s = slots[slotIndex];
    if (!s) return;
    s.midiNote = Math.max(0, Math.min(127, Math.round(midiNote)));
    s.velocity = Math.max(1, Math.min(127, Math.round(velocity127)));
    s.noteOn(patch, effectiveNote(s), s.velocity);
    s.appliedVersion = patchVersion;
    s.gate = true;
    s.idle = false;
    s.silentBlocks = 0;
    s.lp = 0;
    s.bp = 0;
    s.appliedOctaves = tuneOctaves();
  }

  function gateOff(slotIndex) {
    const s = slots[slotIndex];
    if (!s) return;
    s.gate = false;
    if (!s.idle) s.noteOff();
  }

  // Post-voice 2-pole low-pass (Chamberlin SVF), see Fm8Node.h applyFilter().
  function applyFilter(s, out, frames) {
    const cutoff = patch[156];
    if (cutoff >= 99) {
      s.lp = 0;
      s.bp = 0;
      return;
    }
    const res = patch[157];
    if (cutoff !== s.filterCutoffByte || res !== s.filterResByte) {
      const fc = 30 * ((sampleRate * 0.17) / 30) ** (cutoff / 98);
      s.filterF = 2 * Math.sin((Math.PI * fc) / sampleRate);
      s.filterQ = 1 / (0.6 + (res / 99) * 9);
      s.filterCutoffByte = cutoff;
      s.filterResByte = res;
    }
    const f = s.filterF;
    const q = s.filterQ;
    let lp = s.lp;
    let bp = s.bp;
    for (let i = 0; i < frames; i += 1) {
      lp += f * bp;
      const hp = out[i] - lp - q * bp;
      bp += f * hp;
      out[i] = lp;
    }
    s.lp = lp;
    s.bp = bp;
  }

  // Adds all sounding slots into out[0..frames). frames must be a multiple of 64.
  function process(out, frames) {
    const octaves = tuneOctaves();
    const bend = tuneBend14();
    const gain = (Math.max(0, Math.min(CV_MAX_VOLTS, state.gainCv)) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS * state.fanInScale;
    if (scratch.length < frames) scratch = new Float32Array(frames);
    const voiceOut = scratch;
    for (let v = 0; v < slots.length; v += 1) {
      const s = slots[v];
      if (s.idle) continue;
      if (s.appliedVersion !== patchVersion || octaves !== s.appliedOctaves) {
        s.update(patch, effectiveNote(s), s.velocity);
        s.appliedVersion = patchVersion;
        s.appliedOctaves = octaves;
      }
      let peak = 0;
      for (let done = 0; done < frames; done += kBlock) {
        const p = s.renderBlock(voiceOut, done, bend, 12);
        if (p > peak) peak = p;
      }
      for (let i = 0; i < frames; i += 1) voiceOut[i] *= gain;
      applyFilter(s, voiceOut, frames);
      for (let i = 0; i < frames; i += 1) out[i] += voiceOut[i];
      const blocks = frames / kBlock;
      if (peak < 1e-5) s.silentBlocks = Math.min(65535, s.silentBlocks + blocks);
      else s.silentBlocks = 0;
      // Released and silent: stop rendering. A held note that has decayed stops after ~1 s.
      if ((!s.gate && s.silentBlocks > 8) || s.silentBlocks > 400) s.idle = true;
    }
  }

  return {
    state,
    slots,
    setPatchKeys(keys) {
      fm8KeysToPatch(keys, patch);
      patchVersion += 1;
    },
    setPatchKey(key, value) {
      const m = /^d(\d+)$/.exec(key);
      if (m) {
        const idx = Number(m[1]);
        if (idx >= 0 && idx < FM8_PATCH_BYTES) {
          patch[idx] = Math.max(0, Math.min(255, Math.round(Number(value) || 0)));
          patchVersion += 1;
        }
      } else if (key === "ops") {
        patch[155] = Number(value) & 0x3f;
        patchVersion += 1;
      }
    },
    setTune(volts) { state.tuneVolts = Number(volts) || 0; },
    setGain(volts) { state.gainCv = Number(volts) || 0; },
    setFanIn(voiceCount) { state.fanInScale = 1 / Math.sqrt(Math.max(1, voiceCount)); },
    gateOn,
    gateOff,
    process,
    anySounding() { return slots.some((s) => !s.idle); },
  };
}
