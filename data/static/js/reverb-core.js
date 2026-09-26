// Dattorro plate reverb - 1:1 port of nodes/ReverbNode.h (firmware), used by the
// browser preview's "reverb-light" (UI name "Reverb") node. Keep both in sync.
//
// Input bandwidth lowpass -> four input diffusion allpasses -> "figure-eight" tank of two
// cross-fed halves (modulated allpass -> delay -> damping lowpass -> decay -> allpass ->
// delay), stereo out from seven taps per side. Lengths/taps in samples at 29761 Hz.
// The network runs at half the audio rate (pairs averaged in, wet interpolated back up),
// exactly like the firmware.

const REF_RATE = 29761;
const DECIMATION = 2;
const INPUT_AP = [142, 107, 379, 277];
const INPUT_AP_GAIN = [0.75, 0.75, 0.625, 0.625];
const TANK_MOD_AP = [672, 908];
const TANK_DELAY1 = [4453, 4217];
const TANK_AP2 = [1800, 2656];
const TANK_DELAY2 = [3720, 3163];
const MOD_EXCURSION = 16;
const MOD_RATE_HZ = [0.9, 0.73];
const DECAY_DIFFUSION1 = 0.7;
const INPUT_BANDWIDTH_HZ = 9000; // below the network's Nyquist
const OUTPUT_SCALE = 0.6;
// [line, position, sign]; line 0..5 = L delay1, L ap2, L delay2, R delay1, R ap2, R delay2.
const TAPS_L = [[3, 266, 1], [3, 2974, 1], [4, 1913, -1], [5, 1996, 1], [0, 1990, -1], [1, 187, -1], [2, 1066, -1]];
const TAPS_R = [[0, 353, 1], [0, 3627, 1], [1, 1228, -1], [2, 2673, 1], [3, 2111, -1], [4, 335, -1], [5, 121, -1]];

const flushDenorm = (x) => (x > -1e-15 && x < 1e-15 ? 0 : x);

// Same semantics as the firmware DelayLine: write() appends, readOldest() is the sample
// written `length` writes ago, readExact(d)/readInterpolated(d) count back from the newest.
function makeLine(length) {
  const buffer = new Float32Array(Math.max(1, length));
  let writeIdx = 0;
  const n = buffer.length;
  return {
    length: n,
    write(v) {
      buffer[writeIdx] = v;
      writeIdx = writeIdx + 1 === n ? 0 : writeIdx + 1;
    },
    readOldest() {
      return buffer[writeIdx];
    },
    readExact(d) {
      let idx = writeIdx + n - 1 - Math.min(d, n - 1);
      if (idx >= n) idx -= n;
      return buffer[idx];
    },
    readInterpolated(d) {
      const delay = Math.min(Math.max(d, 0), n - 2);
      const d0 = Math.floor(delay);
      const frac = delay - d0;
      let idx0 = writeIdx + n - 1 - d0;
      if (idx0 >= n) idx0 -= n;
      const idx1 = idx0 === 0 ? n - 1 : idx0 - 1;
      return buffer[idx0] + (buffer[idx1] - buffer[idx0]) * frac;
    },
  };
}

function allpass(line, x, g) {
  const delayed = line.readOldest();
  const v = flushDenorm(x - g * delayed);
  line.write(v);
  return delayed + g * v;
}

export function createReverbCore(audioRate, params = {}) {
  const sampleRate = audioRate / DECIMATION; // network rate
  const scale = sampleRate / REF_RATE;
  const scaled = (n) => Math.floor(n * scale + 0.5);
  const modExcursion = MOD_EXCURSION * scale;
  const modBase = TANK_MOD_AP.map(scaled);
  const s = {
    inputAp: INPUT_AP.map((n) => makeLine(scaled(n))),
    tankModAp: modBase.map((n) => makeLine(n + Math.floor(modExcursion) + 3)),
    tankDelay1: TANK_DELAY1.map((n) => makeLine(scaled(n))),
    tankAp2: TANK_AP2.map((n) => makeLine(scaled(n))),
    tankDelay2: TANK_DELAY2.map((n) => makeLine(scaled(n))),
    modPhase: [0, 0.25],
    modInc: MOD_RATE_HZ.map((hz) => hz / sampleRate),
    tapsL: TAPS_L.map(([line, pos, sign]) => [line, scaled(pos), sign]),
    tapsR: TAPS_R.map(([line, pos, sign]) => [line, scaled(pos), sign]),
    bandwidthCoeff: Math.exp((-2 * Math.PI * INPUT_BANDWIDTH_HZ) / sampleRate),
    bandwidthState: 0,
    dampCoeff: 0,
    dampState: [0, 0],
    decay: 0.5,
    decayDiffusion2: 0.5,
    tankHalfSamples: scaled(TANK_MOD_AP[0] + TANK_DELAY1[0] + TANK_AP2[0] + TANK_DELAY2[0]),
    roomSize: Number(params.roomSize ?? 0.5),
    damping: Number(params.damping ?? 0.5),
    wetGain: 10 ** (Number(params.wetLevelDb ?? -6) / 20),
    dryGain: 10 ** (Number(params.dryLevelDb ?? 0) / 20),
    bypass: params.bypass === true,
    prevWetL: 0,
    prevWetR: 0,
  };
  const tapLines = [s.tankDelay1[0], s.tankAp2[0], s.tankDelay2[0], s.tankDelay1[1], s.tankAp2[1], s.tankDelay2[1]];

  const core = {
    state: s,
    // roomSize 0..1 -> RT60 0.1..8 s; decay is applied twice per tank half.
    recomputeDecay() {
      const rt60 = 0.1 + s.roomSize * 7.9;
      const halfSec = s.tankHalfSamples / sampleRate;
      s.decay = Math.min(0.9995, Math.max(0, 10 ** ((-3 * halfSec) / (2 * rt60))));
      s.decayDiffusion2 = Math.min(0.5, Math.max(0.25, s.decay + 0.15));
    },
    // damping 0..1 -> in-loop lowpass 18 kHz .. 600 Hz (exponential).
    recomputeDamping() {
      const cutoffHz = 18000 * (600 / 18000) ** s.damping;
      s.dampCoeff = Math.exp((-2 * Math.PI * cutoffHz) / sampleRate);
    },
    setRoomSize(v) { s.roomSize = v; core.recomputeDecay(); },
    setDamping(v) { s.damping = v; core.recomputeDamping(); },
    // Stereo in -> stereo out (dry + wet), one block.
    process(inL, inR, outL, outR) {
      if (s.bypass) {
        outL.set(inL);
        outR.set(inR);
        return;
      }
      const frames = outL.length;
      const modDelay = [0, 1].map((side) => {
        const d = modBase[side] - 1 + modExcursion * (0.5 + 0.5 * Math.sin(2 * Math.PI * s.modPhase[side]));
        s.modPhase[side] = (s.modPhase[side] + s.modInc[side] * Math.floor(frames / DECIMATION)) % 1;
        return d;
      });
      const bw = s.bandwidthCoeff;
      const damp = s.dampCoeff;
      const decay = s.decay;
      const dd2 = s.decayDiffusion2;
      const wet = OUTPUT_SCALE * s.wetGain;
      let i = 0;
      for (; i + 1 < frames; i += DECIMATION) {
        const xl0 = inL[i];
        const xr0 = inR[i];
        const xl1 = inL[i + 1];
        const xr1 = inR[i + 1];
        const mono = (xl0 + xr0 + xl1 + xr1) * 0.25;
        s.bandwidthState = flushDenorm(s.bandwidthState + (1 - bw) * (mono - s.bandwidthState));
        let x = s.bandwidthState;
        for (let a = 0; a < 4; a += 1) x = allpass(s.inputAp[a], x, INPUT_AP_GAIN[a]);
        const leftOut = s.tankDelay2[0].readOldest();
        const rightOut = s.tankDelay2[1].readOldest();
        const tankIn0 = x + decay * rightOut;
        const tankIn1 = x + decay * leftOut;
        for (let side = 0; side < 2; side += 1) {
          const mod = s.tankModAp[side];
          const delayed = mod.readInterpolated(modDelay[side]);
          const v = flushDenorm((side === 0 ? tankIn0 : tankIn1) + DECAY_DIFFUSION1 * delayed);
          mod.write(v);
          const a = delayed - DECAY_DIFFUSION1 * v;
          const d1Line = s.tankDelay1[side];
          const d1 = d1Line.readOldest();
          d1Line.write(flushDenorm(a));
          s.dampState[side] = flushDenorm(d1 + damp * (s.dampState[side] - d1));
          const c = allpass(s.tankAp2[side], s.dampState[side] * decay, dd2);
          s.tankDelay2[side].write(flushDenorm(c));
        }
        let yl = 0;
        let yr = 0;
        for (let t = 0; t < 7; t += 1) {
          const tl = s.tapsL[t];
          const tr = s.tapsR[t];
          yl += tl[2] * tapLines[tl[0]].readExact(tl[1]);
          yr += tr[2] * tapLines[tr[0]].readExact(tr[1]);
        }
        yl *= wet;
        yr *= wet;
        outL[i] = xl0 * s.dryGain + (s.prevWetL + yl) * 0.5;
        outR[i] = xr0 * s.dryGain + (s.prevWetR + yr) * 0.5;
        outL[i + 1] = xl1 * s.dryGain + yl;
        outR[i + 1] = xr1 * s.dryGain + yr;
        s.prevWetL = yl;
        s.prevWetR = yr;
      }
      for (; i < frames; i += 1) {
        outL[i] = inL[i] * s.dryGain + s.prevWetL;
        outR[i] = inR[i] * s.dryGain + s.prevWetR;
      }
    },
  };
  core.recomputeDecay();
  core.recomputeDamping();
  return core;
}
