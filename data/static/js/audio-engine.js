

import { parseSf2, resolvePreset, decibelsToGain, cents2Hertz } from "./sf2-engine.js";
import { WavetableLfoCore, createDefaultBank, sampleBank } from "./wavetable-core.js?v=20260926-knob-2";
import { COMB_FILTER_PORTS, COMB_FILTER_DEFAULTS, combFilterCv, createCombFilterDsp } from "./comb-filter-core.js?v=20260926-comb-1";
import { createReverbCore } from "./reverb-core.js?v=20260926-knob-2";

function clampRange(value, lo, hi) {
  return value < lo ? lo : value > hi ? hi : value;
}

function isNoiseWave(waveType) {
  return waveType === "noise" || waveType === "pink-noise";
}

// Every periodic waveform is a custom PeriodicWave in the firmware's phase
// convention (PhaseOsc::sampleAt in DspPrimitives.h): phase 0 is the start of
// the cycle, the rising saw starts at -1, the falling saw at +1, the triangle
// at its minimum -1 and the square high. WebAudio's built-in "sawtooth"/
// "triangle" are sine series that start at 0 - a quarter/half cycle off, which
// is audible on every note-on phase reset and made the LFO's phase offset land
// elsewhere than on hardware. `phaseCycles` pre-rotates the series (the LFO's
// "phase" param, applied live like PhaseOsc's read-time phase offset).
// Unnormalized so the fundamental matches the firmware's naive waveforms;
// bandlimiting is done by the browser. PeriodicWaves belong to one
// AudioContext, hence the per-context cache (phase 0 only - phased LFO waves
// are built on demand).
const FIRMWARE_WAVE_HARMONICS = 1024;
const firmwareWaveCache = new WeakMap();

function firmwareWaveSeries(waveType, k) {
  switch (waveType) {
    case "square":
      return k % 2 === 1 ? { a: 0, b: 4 / (Math.PI * k) } : null;
    case "triangle":
      return k % 2 === 1 ? { a: -8 / (Math.PI * Math.PI * k * k), b: 0 } : null;
    case "sawtooth":
    case "linear-up":
    case "supersaw":
      return { a: 0, b: -2 / (Math.PI * k) };
    case "sawtooth-down":
    case "linear-down":
      return { a: 0, b: 2 / (Math.PI * k) };
    default:
      return k === 1 ? { a: 0, b: 1 } : null;
  }
}

function getFirmwareWave(ctx, waveType, phaseCycles = 0) {
  const phase = ((Number(phaseCycles) || 0) % 1 + 1) % 1;
  const shape = waveType === "square" || waveType === "triangle" || waveType === "sawtooth" ||
    waveType === "linear-up" || waveType === "supersaw" || waveType === "sawtooth-down" ||
    waveType === "linear-down" ? waveType : "sine";
  let perCtx = firmwareWaveCache.get(ctx);
  if (!perCtx) {
    perCtx = new Map();
    firmwareWaveCache.set(ctx, perCtx);
  }
  if (phase === 0 && perCtx.has(shape)) return perCtx.get(shape);
  const harmonics = shape === "sine" ? 1 : FIRMWARE_WAVE_HARMONICS;
  const real = new Float32Array(harmonics + 1);
  const imag = new Float32Array(harmonics + 1);
  const theta = 2 * Math.PI * phase;
  for (let k = 1; k <= harmonics; k += 1) {
    const term = firmwareWaveSeries(shape, k);
    if (!term) continue;
    // f(p + phase): a*cos(k(w+t)) + b*sin(k(w+t)) re-expanded around w.
    const c = Math.cos(k * theta);
    const s = Math.sin(k * theta);
    real[k] = term.a * c + term.b * s;
    imag[k] = term.b * c - term.a * s;
  }
  const wave = ctx.createPeriodicWave(real, imag, { disableNormalization: true });
  if (phase === 0) perCtx.set(shape, wave);
  return wave;
}

function isLinearWave(waveType) {
  return waveType === "linear-up" || waveType === "linear-down";
}

// Free-running, "linear-up"/"linear-down" repeat exactly like the sawtooth
// shapes - they only differ when one-shot / per-note (see applyLfoLinear and
// applyOscLinear), where they hold their end value instead of falling back.
function setOscWaveType(ctx, osc, waveType, phaseCycles = 0) {
  osc.setPeriodicWave(getFirmwareWave(ctx, waveType, phaseCycles));
}

// True while an LFO is a linear wave in One Shot: then it is not an
// oscillator waveform but a held/one-time ramp (applyLfoLinear). Wavetable
// mode replaces the waveform entirely, so it never counts as linear.
function lfoLinearHold(entry) {
  return !entry.lfoWavetable && isLinearWave(entry.lfoWaveType) && entry.lfoOneShot === true;
}

// Raw -1..+1 value an LFO rests at while One Shot is holding (the cycle
// start, shifted by the phase offset) - the wavetable bank at the current
// morph position for the Wavetable widget, the classic waveform otherwise.
function lfoHeldRaw(entry) {
  if (entry.lfoWavetable) {
    return sampleBank(entry.wtTables, (entry.lfoPhase ?? 0) / 360, (entry.wtMorph ?? 0) / 10);
  }
  return lfoWaveValueAtPhase(entry.lfoWaveType, entry.lfoPhase ?? 0);
}

// Oscillator "linear-up"/"linear-down": one ramp per note (gate), then hold.
// A ConstantSource (entry.linearSource) replaces the oscillator in the signal
// path (like the noise sources); the ramp lasts one cycle of the last direct-
// set frequency (pitch arriving over an audio-rate cable is not readable here,
// so that case uses the last direct-set note, or middle C).
function oscExtraSource(entry, waveType) {
  if (waveType === "noise") return entry.noiseSource;
  if (waveType === "pink-noise") return entry.pinkNoiseSource;
  return isLinearWave(waveType) ? entry.linearSource : null;
}

function applyOscLinear(ctx, entry) {
  const now = ctx.currentTime;
  const hz = entry.osc.frequency.value > 0 ? entry.osc.frequency.value : (entry.directHz > 0 ? entry.directHz : 261.63);
  const up = entry.waveType === "linear-up";
  const offset = entry.linearSource.offset;
  offset.cancelScheduledValues(now);
  offset.setValueAtTime(up ? -1 : 1, now);
  offset.linearRampToValueAtTime(up ? 1 : -1, now + 1 / hz);
}

// LFO "linear-up"/"linear-down" in One Shot: one non-wrapping ramp from the
// last restart, then hold at the end value. lfoGain is muted
// and centerOffset.offset is automated instead. Position is tracked
// (lfoLinearPos0 at lfoLinearT0, running flag) so a parameter change mid-ramp
// re-anchors instead of restarting. Phase offset shifts the mapped position
// (clamped), same as the firmware.
function applyLfoLinear(ctx, entry, restart) {
  const now = ctx.currentTime;
  if (restart) {
    entry.lfoLinearPos0 = 0;
    entry.lfoLinearRunning = true;
  } else if (entry.lfoLinearRunning) {
    entry.lfoLinearPos0 = Math.min(1, entry.lfoLinearPos0 + (now - entry.lfoLinearT0) * entry.lfoLinearRate);
  }
  entry.lfoLinearT0 = now;
  entry.lfoLinearRate = entry.lfoRate;
  const halfSwing = entry.lfoAmplitude / 2;
  const center = entry.lfoZeroPointFixed ? entry.lfoZeroPoint : halfSwing;
  const offset = (entry.lfoPhase ?? 0) / 360;
  const up = entry.lfoWaveType === "linear-up";
  const valueAt = (pos) => {
    const p = clampRange(pos + offset, 0, 1);
    return clampRange(center + (up ? 2 * p - 1 : 1 - 2 * p) * halfSwing, 0, CV_MAX_VOLTS);
  };
  const cur = entry.lfoLinearPos0;
  const target = entry.centerOffset.offset;
  target.cancelScheduledValues(now);
  target.setValueAtTime(valueAt(cur), now);
  entry.lfoGain.gain.cancelScheduledValues(now);
  entry.lfoGain.gain.setValueAtTime(0, now);
  if (entry.lfoLinearRunning && entry.lfoLinearRate > 0 && cur < 1) {
    target.linearRampToValueAtTime(valueAt(1), now + (1 - cur) / entry.lfoLinearRate);
  }
}

function fillPinkNoise(data) {
  let b0 = 0;
  let b1 = 0;
  let b2 = 0;
  for (let i = 0; i < data.length; i += 1) {
    const white = Math.random() * 2 - 1;
    b0 = 0.99765 * b0 + white * 0.099046;
    b1 = 0.963 * b1 + white * 0.2965164;
    b2 = 0.57 * b2 + white * 1.0526913;
    data[i] = clampRange((b0 + b1 + b2 + white * 0.1848) * 0.25, -1, 1);
  }
}




function parseValueList(str) {
  if (typeof str !== "string") return null;
  const parts = str
    .split(",")
    .map((s) => s.trim())
    .filter((s) => s.length > 0);
  if (parts.length < 2) return null;
  const values = parts.map((s) => parseFloat(s));
  if (values.some((v) => Number.isNaN(v))) return null;
  return values;
}




const CV_MAX_VOLTS = 10;
// Mirrors nodes::lfo::kMaxRateHz (LfoNode.h) and main.js's LFO_MAX_RATE_HZ.
const LFO_MAX_RATE_HZ = 500;
const GRAPHIC_EQ_DEFAULT_FREQS = [63, 125, 250, 500, 1000, 2000, 5000, 14000];

function validGraphicEqFrequencies(frequencies) {
  return Array.isArray(frequencies) && frequencies.length === GRAPHIC_EQ_DEFAULT_FREQS.length &&
    frequencies.every((hz, i) => Number.isInteger(Number(hz)) && Number(hz) >= 20 && Number(hz) <= 20000 &&
      (i === 0 || Number(hz) > Number(frequencies[i - 1])));
}
// Nominal peak amplitude of the AUDIO signal domain (oscillator/Hammond/
// sample-player waveform values), as opposed to CV_MAX_VOLTS (the CV
// domain) - mirrors cv::kAudioPeakVolts in CvConventions.h 1:1.
const AUDIO_PEAK_VOLTS = 5;
const LINK_BUS_COUNT = 8;
const GATE_THRESHOLD_VOLTS = 5;
const ANALYSER_BLACKMAN_COHERENT_GAIN = 0.42;
// Plain-oscillator self-feedback preview: the firmware phase-modulates by the
// previous sample (up to kSelfFeedbackMaxPhaseDeviation); a native
// OscillatorNode has no phase input, so one delayed sample modulates its
// frequency instead. (The fm-oscillator runs the exact phase modulation, see
// createFmOscillatorCore.)
const SELF_FEEDBACK_MAX_HZ_DEVIATION = 2000;



// Mirrors nodes/DetuneNode.h's detuneOffsetVolts() / main.js's own copy
// exactly: "amount"'s full 0-10V travel is rescaled to +/-maxOffsetCents
// (5V = 0), then rounded to the stepCents grid (100 = semitone, 1 = cent).
function detuneOffsetVolts(amount, maxOffsetCents, stepCents) {
  const normalized = (amount - CV_MAX_VOLTS / 2) / (CV_MAX_VOLTS / 2);
  const rawCents = normalized * maxOffsetCents;
  const steppedCents = stepCents > 0 ? Math.round(rawCents / stepCents) * stepCents : rawCents;
  return steppedCents / 1200;
}




const PITCH_CV_ANCHOR_NOTE = 60;
const PITCH_CV_VOLTS_PER_OCTAVE = 1;
function hzFromPitchCv(volts) {
  const note = ((volts - CV_MAX_VOLTS / 2) / PITCH_CV_VOLTS_PER_OCTAVE) * 12 + PITCH_CV_ANCHOR_NOTE;
  return 440 * 2 ** ((note - 69) / 12);
}

// ============================================================================
// MDA Piano preview - JS port of components/mda_piano (mdaEPiano.cpp +
// reface_cp_chain.h/reface_cp_fx.h/wahwah.h), matching
// nodes/MdaPianoNode.h's render() 1:1. Firmware is authoritative; this is a
// browser-only approximation using the same sample pack.
//
// Only the "Default" mda-ePiano program is ever selected by this node (no UI
// exposes program/param switching), so every field update()/fillpatch()
// would normally recompute from a patch is folded in as the constants below
// rather than porting the whole program system. Likewise mdaEPiano's own
// pan/tremolo (lmod/rmod) and overdrive are always zero in this integration
// (MdaPianoNode.h never sets them nonzero - the audible tremolo/wah is
// entirely RefaceCpChain's, applied afterward), and the sustain pedal is
// never wired into handleMidi(), so note-off always releases immediately -
// both are omitted here as dead code, not approximated.
// ============================================================================
const MDA_EPIANO_DEFAULT_PARAM = [0.5, 0.5, 0.5, 0.5, 0.5, 0.65, 0.25, 0.5, 1.0, 0.5, 0.146, 0.0];

function mdaVoiceTypeFromCv(volts) {
  const v = Math.min(5, Math.floor(clampRange(volts, 0, CV_MAX_VOLTS) * 0.6));
  return v;
}

function mdaEffectModeFromCv(volts) {
  return Math.min(2, Math.floor(clampRange(volts, 0, CV_MAX_VOLTS) * 0.3));
}

function createMdaEpianoEngine(maxVoices, sampleRate) {
  const Fs = sampleRate;
  const iFs = 1 / Fs;
  const p = MDA_EPIANO_DEFAULT_PARAM;
  const size = Math.trunc(12.0 * p[2] - 6.0);
  const treb = 4.0 * p[3] * p[3] - 1.0;
  const tfrq = 1.0 - Math.exp(-iFs * (p[3] > 0.5 ? 14000.0 : 5000.0));
  const dlfo = 6.283 * iFs * Math.exp(6.22 * p[5] - 2.61);
  let velsens = 1.0 + p[6] + p[6];
  if (p[6] < 0.25) velsens -= 0.75 - 3.0 * p[6];
  const width = 0.03 * p[7];
  const poly = Math.min(maxVoices, 1 + Math.trunc((maxVoices - 1) * p[8]));
  const fine = p[9] - 0.5;
  const random = 0.077 * p[10];
  const muff = 160.0;
  const muffvel = 1.25;
  const volume = 0.2;

  const voices = Array.from({ length: maxVoices }, () => ({
    delta: 0, dbase: 0, frac: 0, pos: 0, end: 0, loop: 0,
    env: 0, dec: 0.99, outl: 0, outr: 0, note: -1,
  }));
  let activevoices = 0;
  let tl = 0, tr = 0, lfo0 = 0, lfo1 = 1;
  let bendf = 1;
  let waves = null;
  let kgrp = [];
  let noteonStep = 3;

  function resetVoices() {
    for (const v of voices) { v.env = 0; v.dec = 0.99; }
    activevoices = 0;
    tl = tr = lfo0 = 0;
    lfo1 = 1;
  }

  function loadInstrument(instrument) {
    waves = instrument.pcm;
    kgrp = instrument.keygroups;
    noteonStep = instrument.noteonStep;
    resetVoices();
  }

  function noteOn(note, velocity) {
    if (!waves || kgrp.length === 0) return;
    if (velocity > 0) {
      let vl;
      if (activevoices < poly) {
        vl = activevoices;
        activevoices += 1;
      } else {
        let quietest = 99;
        vl = 0;
        for (let v = 0; v < poly; v += 1) {
          if (voices[v].env < quietest) { quietest = voices[v].env; vl = v; }
        }
      }
      const V = voices[vl];
      const k0 = (note - 60) * (note - 60);
      let l = fine + random * ((k0 % 13) - 6.5);

      let k = 0;
      while (k + noteonStep < kgrp.length && note > kgrp[k].high + size) k += noteonStep;
      l += note - kgrp[k].root;
      l = 32000.0 * iFs * Math.exp(0.05776227 * l);
      V.dbase = Math.trunc(65536.0 * l);
      V.delta = Math.trunc(V.dbase * bendf);
      V.frac = 0;

      if (velocity > 48) k += 1;
      if (velocity > 80) k += 1;
      V.pos = kgrp[k].pos;
      V.end = kgrp[k].end - 1;
      V.loop = kgrp[k].loop;

      V.env = (3.0 + 2.0 * velsens) * (0.0078 * velocity) ** velsens;
      if (note > 60) V.env *= Math.exp(0.01 * (60 - note));

      const pan = Math.max(12, Math.min(108, note));
      V.outr = volume + volume * width * (pan - 60);
      V.outl = volume + volume - V.outr;
      V.note = note;

      const decNote = Math.max(44, note);
      V.dec = Math.exp(-iFs * Math.exp(-1.0 + 0.03 * decNote - 2.0 * p[0]));
    } else {
      for (let v = 0; v < maxVoices; v += 1) {
        if (voices[v].note === note) {
          voices[v].dec = Math.exp(-iFs * Math.exp(6.0 + 0.01 * note - 5.0 * p[1]));
        }
      }
    }
  }

  function noteOff(note) {
    noteOn(note, 0);
  }

  function setPitchBend(bend14) {
    const b = Math.max(0, Math.min(16383, bend14));
    const semis = (b - 8192) * (2.0 / 8192.0);
    bendf = 2 ** (semis / 12);
    for (let v = 0; v < activevoices; v += 1) voices[v].delta = Math.trunc(voices[v].dbase * bendf);
  }

  function process(outL, outR, frames) {
    if (!waves) {
      outL.fill(0);
      outR.fill(0);
      return;
    }
    for (let frame = 0; frame < frames; frame += 1) {
      let l = 0, r = 0;
      for (let v = 0; v < activevoices; v += 1) {
        const V = voices[v];
        V.frac += V.delta;
        V.pos += Math.floor(V.frac / 65536);
        V.frac %= 65536;
        if (V.pos > V.end) V.pos -= V.loop;

        const s0 = waves[V.pos];
        const s1 = waves[V.pos + 1] ?? s0;
        const interpolated = s0 + (V.frac / 65536) * (s1 - s0);
        const x = V.env * (interpolated / 32768);

        V.env *= V.dec;
        l += V.outl * x;
        r += V.outr * x;
      }

      tl += tfrq * (l - tl);
      tr += tfrq * (r - tr);
      r += treb * (r - tr);
      l += treb * (l - tl);

      lfo0 += dlfo * lfo1;
      lfo1 -= dlfo * lfo0;

      r *= 0.5;
      l *= 0.5;
      outL[frame] = Math.max(-1, Math.min(1, l));
      outR[frame] = Math.max(-1, Math.min(1, r));
    }
    for (let v = 0; v < activevoices; v += 1) {
      // Copy fields, never the reference: the C++ original's
      // voice[v] = voice[--activevoices] is a struct copy. Aliasing two slots
      // onto one object makes the render loop advance it twice per frame
      // (an octave up) and lets the next noteOn overwrite a sounding voice.
      if (voices[v].env < 0.0001) Object.assign(voices[v], voices[--activevoices]);
    }
  }

  return { noteOn, noteOff, setPitchBend, resetVoices, loadInstrument, process };
}

// One-pole envelope-follower-driven wah, ported from wahwah.h (SvfWahCore +
// WahWah). setMidFreqNorm() is never called by RefaceCpChain, so the base
// frequency/sweep-ratio fields just keep wahwah.h's own field-initializer
// defaults (300 Hz base, log(2400/300) sweep) instead of being ported.
function createSvfWahCore() {
  let s1 = 0, s2 = 0, g = 0, k = 0.4, a1 = 0, peakBoost = 1;
  return {
    setParams(cutoff, resoNorm, sr) {
      const c = Math.max(50, Math.min(sr * 0.45, cutoff));
      const reso = Math.max(0, Math.min(1, resoNorm));
      const Q = 2.5 + 9.5 * reso;
      g = Math.tan(Math.PI * c / sr);
      k = 1 / Q;
      a1 = 1 / (1 + g * (g + k));
      peakBoost = 1 + 3 * reso;
    },
    process(x) {
      const hp = (x - (k + g) * s1 - s2) * a1;
      const bp = g * hp + s1;
      const lp = g * bp + s2;
      s1 = g * hp + bp;
      s2 = g * bp + lp;
      const out = (lp * 0.3 + bp * k * peakBoost) * 1.4;
      return Math.tanh(out * 0.7) / 0.7;
    },
    reset() { s1 = 0; s2 = 0; },
  };
}

function createWahWah(sampleRate) {
  const coreL = createSvfWahCore();
  const coreR = createSvfWahCore();
  const sr = sampleRate;
  const freqSmoothCoeff = 1 - Math.exp(-8 / (sr * 0.005));
  const baseFreq = 300;
  const sweepLogRatio = Math.log(2400 / baseFreq);
  let midGainNorm = 0.5;
  let envAmount = 1;
  let freqSmoothL = 300, freqSmoothR = 300;
  let counter = 0;
  return {
    setMidGainNorm(n) { midGainNorm = Math.max(0, Math.min(1, n)); },
    setEnvelopeAmount(n) { envAmount = Math.max(0, Math.min(1, n)); },
    reset() {
      coreL.reset(); coreR.reset();
      freqSmoothL = 300; freqSmoothR = 300;
      counter = 0;
    },
    processStereo(io, envL, envR) {
      if (envAmount <= 0.01) return;
      const dryL = io.l, dryR = io.r;
      if ((counter++ & 7) === 0) {
        const curveL = Math.sqrt(Math.max(0, Math.min(1, envL)));
        const curveR = Math.sqrt(Math.max(0, Math.min(1, envR)));
        const sensGain = 0.3 + 2.2 * envAmount;
        const sweepL = Math.max(0, Math.min(1, curveL * sensGain));
        const sweepR = Math.max(0, Math.min(1, curveR * sensGain));
        const targetFreqL = baseFreq * Math.exp(sweepLogRatio * sweepL);
        const targetFreqR = baseFreq * Math.exp(sweepLogRatio * sweepR);
        freqSmoothL += freqSmoothCoeff * (targetFreqL - freqSmoothL);
        freqSmoothR += freqSmoothCoeff * (targetFreqR - freqSmoothR);
        const resoL = Math.max(0, Math.min(1, midGainNorm + 0.2 * curveL));
        const resoR = Math.max(0, Math.min(1, midGainNorm + 0.2 * curveR));
        coreL.setParams(freqSmoothL, resoL, sr);
        coreR.setParams(freqSmoothR, resoR, sr);
      }
      const satL = Math.tanh(io.l * 1.05) / 1.05;
      const satR = Math.tanh(io.r * 1.05) / 1.05;
      const wetL = coreL.process(satL);
      const wetR = coreR.process(satR);
      const wetMix = 0.55 + 0.4 * envAmount;
      io.l = dryL * (1 - wetMix) + wetL * wetMix;
      io.r = dryR * (1 - wetMix) + wetR * wetMix;
    },
  };
}

// Ported from reface_cp_fx.h's CpTremolo/CpChorus/CpPhaser - short enough to
// keep as plain closures rather than a class per effect.
function createCpTremolo(sampleRate) {
  const sr = sampleRate;
  let phase = 0, depth = 0, rateHz = 0.1, mode = 0;
  return {
    setDepth(n) { depth = Math.max(0, Math.min(1, n)); },
    setRate(n) { rateHz = 0.1 * 10 ** (2 * Math.max(0, Math.min(1, n))); },
    setMode(m) { mode = m === 1 ? 1 : 0; },
    process(io) {
      phase += rateHz / sr;
      if (phase >= 1) phase -= 1;
      const lfo = Math.sin(phase * 2 * Math.PI);
      const a = 0.5 + 0.5 * lfo;
      const b = 0.5 - 0.5 * lfo;
      if (mode === 0) {
        io.l *= Math.max(0, Math.min(1, 1 - depth * a));
        io.r *= Math.max(0, Math.min(1, 1 - depth * b));
      } else {
        const g = Math.max(0, Math.min(1, 1 - depth * a));
        io.l *= g;
        io.r *= g;
      }
    },
  };
}

function createCpChorus(sampleRate) {
  const sr = sampleRate;
  const kBuf = 1024;
  let phaseL = 0, phaseR = 0.25, depthMs = 0, rateHz = 0.1, widx = 0;
  const bufL = new Float32Array(kBuf);
  const bufR = new Float32Array(kBuf);
  function readChan(buf, delaySamp) {
    let rp = widx - delaySamp;
    while (rp < 0) rp += kBuf;
    let i0 = Math.trunc(rp);
    if (i0 >= kBuf) i0 -= kBuf;
    const frac = rp - i0;
    let i1 = i0 + 1;
    if (i1 >= kBuf) i1 = 0;
    return buf[i0] + frac * (buf[i1] - buf[i0]);
  }
  return {
    setDepth(n) { depthMs = Math.max(0, Math.min(1, n)) * 4; },
    setRate(n) { rateHz = 0.1 * 10 ** (1.69897 * Math.max(0, Math.min(1, n))); },
    process(io) {
      const inc = rateHz / sr;
      phaseL += inc; if (phaseL >= 1) phaseL -= 1;
      phaseR += inc; if (phaseR >= 1) phaseR -= 1;
      const baseSamp = 0.012 * sr;
      let modSamp = depthMs * 0.001 * sr;
      if (baseSamp + modSamp > kBuf - 2) modSamp = kBuf - 2 - baseSamp;
      const dl = baseSamp + modSamp * Math.sin(phaseL * 2 * Math.PI);
      const dr = baseSamp + modSamp * Math.sin(phaseR * 2 * Math.PI);
      const dryL = io.l, dryR = io.r;
      bufL[widx] = dryL;
      bufR[widx] = dryR;
      const wetL = readChan(bufL, dl);
      const wetR = readChan(bufR, dr);
      io.l = 0.5 * dryL + 0.5 * wetL;
      io.r = 0.5 * dryR + 0.5 * wetR;
      widx = (widx + 1) % kBuf;
    },
  };
}

function createCpPhaser(sampleRate) {
  const sr = sampleRate;
  const kStages = 4;
  const kTwoPi = 2 * Math.PI;
  let phase = 0, phaseR = 0.25 * kTwoPi;
  let depthNorm = 0.5, rateHz = 0.1;
  let aL = 0.9, aR = 0.9, fb = 0.2, cnt = 0;
  const x1L = new Float32Array(kStages), y1L = new Float32Array(kStages);
  const x1R = new Float32Array(kStages), y1R = new Float32Array(kStages);
  let lastL = 0, lastR = 0;
  return {
    setDepth(n) { depthNorm = Math.max(0, Math.min(1, n)); },
    setSpeed(n) { rateHz = 0.1 * 10 ** (Math.max(0, Math.min(1, n)) * 1.698970004336019); },
    process(io) {
      const inc = kTwoPi * rateHz / sr;
      phase += inc; if (phase >= kTwoPi) phase -= kTwoPi;
      phaseR += inc; if (phaseR >= kTwoPi) phaseR -= kTwoPi;
      if ((cnt++ & 7) === 0) {
        const oct = 0.6 + 1.6 * depthNorm;
        const fcL = 750 * 2 ** (oct * Math.sin(phase));
        const fcR = 750 * 2 ** (oct * Math.sin(phaseR));
        const tL = Math.tan(Math.PI * fcL / sr);
        const tR = Math.tan(Math.PI * fcR / sr);
        aL = (1 - tL) / (1 + tL);
        aR = (1 - tR) / (1 + tR);
        fb = 0.2 + 0.55 * depthNorm;
      }
      const inL = io.l + fb * Math.tanh(lastL);
      const inR = io.r + fb * Math.tanh(lastR);
      let sigL = inL, sigR = inR;
      for (let i = 0; i < kStages; i += 1) {
        const yL = -aL * sigL + x1L[i] + aL * y1L[i];
        x1L[i] = sigL; y1L[i] = yL; sigL = yL;
        const yR = -aR * sigR + x1R[i] + aR * y1R[i];
        x1R[i] = sigR; y1R[i] = yR; sigR = yR;
      }
      lastL = sigL;
      lastR = sigR;
      io.l = 0.5 * io.l + 0.5 * sigL;
      io.r = 0.5 * io.r + 0.5 * sigR;
    },
  };
}

// Ported from reface_cp_chain.h - drive -> [1] trem/wah -> [2] chorus/phaser.
function createRefaceCpChain(sampleRate) {
  const trem = createCpTremolo(sampleRate);
  const wah = createWahWah(sampleRate);
  const chorus = createCpChorus(sampleRate);
  const phaser = createCpPhaser(sampleRate);
  const envAtt = Math.exp(-1 / (sampleRate * 0.008));
  const envRel = Math.exp(-1 / (sampleRate * 0.12));
  let drive = 0, preGain = 1, twMode = 0, cpMode = 0;
  let envL = 0, envR = 0;
  const io = { l: 0, r: 0 };
  return {
    setVoiceType(v) {
      trem.setMode(v === 0 || v === 1 || v === 5 ? 0 : 1); // RDI/RDII/CP=pan, else volume
    },
    setDrive(n) { drive = Math.max(0, Math.min(1, n)); },
    setPreGain(n) { preGain = Math.max(0, Math.min(1, n)); },
    setTremWahMode(m) { twMode = m; },
    setTremWahDepth(n) {
      const d = Math.max(0, Math.min(1, n));
      trem.setDepth(d);
      wah.setEnvelopeAmount(d);
    },
    setTremWahRate(n) {
      const r = Math.max(0, Math.min(1, n));
      trem.setRate(r);
      wah.setMidGainNorm(r);
    },
    setChoPhaMode(m) { cpMode = m; },
    setChoPhaDepth(n) {
      const d = Math.max(0, Math.min(1, n));
      chorus.setDepth(d);
      phaser.setDepth(d);
    },
    setChoPhaSpeed(n) {
      const s = Math.max(0, Math.min(1, n));
      chorus.setRate(s);
      phaser.setSpeed(s);
    },
    process(outL, outR, frames) {
      for (let i = 0; i < frames; i += 1) {
        io.l = outL[i];
        io.r = outR[i];
        if (preGain < 0.9999) { io.l *= preGain; io.r *= preGain; }
        if (drive > 0.0001) {
          const pre = 1 + drive * 11;
          const mk = 0.7 + 0.3 * (1 - drive);
          io.l = Math.tanh(io.l * pre) * mk;
          io.r = Math.tanh(io.r * pre) * mk;
        }
        if (twMode === 1) {
          trem.process(io);
        } else if (twMode === 2) {
          const aL = Math.abs(io.l);
          envL = aL > envL ? envAtt * envL + (1 - envAtt) * aL : envRel * envL + (1 - envRel) * aL;
          const aR = Math.abs(io.r);
          envR = aR > envR ? envAtt * envR + (1 - envAtt) * aR : envRel * envR + (1 - envRel) * aR;
          wah.processStereo(io, envL, envR);
        }
        if (cpMode === 1) chorus.process(io);
        else if (cpMode === 2) phaser.process(io);
        outL[i] = io.l;
        outR[i] = io.r;
      }
    },
  };
}

// Parses the "MDAPACK" binary built by tools/build_mda_piano_pack.py (see
// components/mda_piano/src/MdaPianoBank.cpp for the authoritative format;
// CRC32 is skipped here - the firmware already validates the file it wrote).
function parseMdaPianoPack(buffer) {
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);
  const magic = String.fromCharCode(...bytes.slice(0, 7));
  if (magic !== "MDAPACK") throw new Error("bad MDA piano pack magic");
  const instrumentCount = view.getUint32(12, true);
  const HEADER_SIZE = 24;
  const ENTRY_SIZE = 36;
  const instruments = [];
  for (let i = 0; i < instrumentCount; i += 1) {
    const base = HEADER_SIZE + i * ENTRY_SIZE;
    const name = String.fromCharCode(...bytes.slice(base, base + 8)).replace(/\0+$/, "");
    const noteonStep = view.getUint32(base + 12, true);
    const pcmOffset = view.getUint32(base + 16, true);
    const pcmSamples = view.getUint32(base + 20, true);
    const keygroupOffset = view.getUint32(base + 24, true);
    const keygroupCount = view.getUint32(base + 28, true);
    const pcm = new Int16Array(buffer.slice(pcmOffset, pcmOffset + pcmSamples * 2));
    const keygroups = [];
    for (let k = 0; k < keygroupCount; k += 1) {
      const kb = keygroupOffset + k * 20;
      keygroups.push({
        root: view.getInt32(kb, true),
        high: view.getInt32(kb + 4, true),
        pos: view.getInt32(kb + 8, true),
        end: view.getInt32(kb + 12, true),
        loop: view.getInt32(kb + 16, true),
      });
    }
    instruments.push({ name, noteonStep, pcm, keygroups });
  }
  return instruments;
}

// Order matches nodes/MdaPianoNode.h's port indices 0-11 exactly.
const MDA_PIANO_PORTS = [
  "enableModWheel", "enablePitchWheel", "voiceType", "drive", "tremWahMode",
  "tremWahDepth", "tremWahRate", "choPhaMode", "choPhaDepth", "choPhaSpeed", "preGain", "gain",
];
const MDA_PIANO_PORT_DEFAULTS = [0, 0, 0, 0, 0, 0, 5, 0, 0, 5, 10, 10];







function cvToRange(volts, minVal, maxVal, curve) {
  const v = clampRange(volts / CV_MAX_VOLTS, 0, 1);
  if (curve === "log") {
    const lo = minVal <= 0 ? 0.0001 : minVal;
    return lo * (maxVal / lo) ** v;
  }
  return minVal + v * (maxVal - minVal);
}

// ============================================================================
// Filter / Curtis-filter DSP - 1:1 ports of nodes/FilterNode.h and
// nodes/CurtisFilterNode.h, run per sample inside a ScriptProcessor (see the
// "filter"/"curtis-filter" _buildNode case). Web Audio's BiquadFilterNode could
// not reproduce either: it has no odd-pole (6 dB) slopes, reads lowpass/
// highpass Q in dB, and the CEM3320-style filter is a saturating TPT cascade,
// not a biquad at all.
// ============================================================================
const FILTER_MAX_POLES = 6; // cv::kFilterMaxPoles, 6 dB per pole

function flushDenorm(x) {
  return x > -1e-15 && x < 1e-15 ? 0 : x;
}

function filterPolesFromSlope(slopeDb) {
  const poles = Math.trunc((Number(slopeDb) || 12) / 6);
  return clampRange(poles, 1, FILTER_MAX_POLES);
}

// filterKindFromString() (NodeTypes.h): anything unknown is a lowpass.
const FILTER_KINDS = ["none", "lowpass", "highpass", "bandpass", "notch", "lowshelf", "highshelf", "allpass"];
function normalizeFilterKind(kind) {
  return FILTER_KINDS.includes(kind) ? kind : "lowpass";
}

function filterSupportsOnePole(kind) {
  return kind === "lowpass" || kind === "highpass";
}

// biquadCount()/usesOnePole() in FilterNode.h: LP/HP spend one biquad per two
// poles plus a one-pole stage for an odd pole; the other kinds round up.
function filterBiquadCount(kind, slopeDb) {
  const poles = filterPolesFromSlope(slopeDb);
  if (filterSupportsOnePole(kind)) return Math.trunc(poles / 2);
  return Math.max(1, Math.trunc((poles + 1) / 2));
}

function filterUsesOnePole(kind, slopeDb) {
  return filterSupportsOnePole(kind) && filterPolesFromSlope(slopeDb) % 2 !== 0;
}

function filterCutoffHz(entry, cutoffCv, sampleRate) {
  const hz = cvToRange(cutoffCv, entry.cutoffMinHz, entry.cutoffMaxHz, entry.cutoffCurve);
  return clampRange(hz, 20, sampleRate / 2 - 100);
}

function filterResonanceQ(entry, resonanceCv) {
  return Math.max(0.05, cvToRange(resonanceCv, entry.resonanceMinQ, entry.resonanceMaxQ, entry.resonanceCurve));
}

// Biquad::set*() (DspPrimitives.h, RBJ cookbook) - Q is a plain ratio.
function setBiquadCoeffs(stage, kind, cutoffHz, qOrGainDb, sampleRate) {
  const w0 = (2 * Math.PI * cutoffHz) / sampleRate;
  const sinw0 = Math.sin(w0);
  const cosw0 = Math.cos(w0);
  let b0;
  let b1;
  let b2;
  let a0;
  let a1;
  let a2;
  if (kind === "lowshelf" || kind === "highshelf") {
    const a = 10 ** (qOrGainDb / 40);
    const twoSqrtAAlpha = 2 * Math.sqrt(a) * sinw0 * 0.70710678;
    if (kind === "lowshelf") {
      b0 = a * ((a + 1) - (a - 1) * cosw0 + twoSqrtAAlpha);
      b1 = 2 * a * ((a - 1) - (a + 1) * cosw0);
      b2 = a * ((a + 1) - (a - 1) * cosw0 - twoSqrtAAlpha);
      a0 = (a + 1) + (a - 1) * cosw0 + twoSqrtAAlpha;
      a1 = -2 * ((a - 1) + (a + 1) * cosw0);
      a2 = (a + 1) + (a - 1) * cosw0 - twoSqrtAAlpha;
    } else {
      b0 = a * ((a + 1) + (a - 1) * cosw0 + twoSqrtAAlpha);
      b1 = -2 * a * ((a - 1) + (a + 1) * cosw0);
      b2 = a * ((a + 1) + (a - 1) * cosw0 - twoSqrtAAlpha);
      a0 = (a + 1) - (a - 1) * cosw0 + twoSqrtAAlpha;
      a1 = 2 * ((a - 1) - (a + 1) * cosw0);
      a2 = (a + 1) - (a - 1) * cosw0 - twoSqrtAAlpha;
    }
  } else {
    const alpha = sinw0 / (2 * qOrGainDb);
    a0 = 1 + alpha;
    a1 = -2 * cosw0;
    a2 = 1 - alpha;
    if (kind === "highpass") {
      b0 = (1 + cosw0) / 2;
      b1 = -(1 + cosw0);
      b2 = (1 + cosw0) / 2;
    } else if (kind === "bandpass") {
      b0 = alpha;
      b1 = 0;
      b2 = -alpha;
    } else if (kind === "notch") {
      b0 = 1;
      b1 = -2 * cosw0;
      b2 = 1;
    } else if (kind === "allpass") {
      b0 = 1 - alpha;
      b1 = -2 * cosw0;
      b2 = 1 + alpha;
    } else {
      b0 = (1 - cosw0) / 2;
      b1 = 1 - cosw0;
      b2 = (1 - cosw0) / 2;
    }
  }
  const inv = 1 / a0;
  stage.b0 = b0 * inv;
  stage.b1 = b1 * inv;
  stage.b2 = b2 * inv;
  stage.a1 = a1 * inv;
  stage.a2 = a2 * inv;
}

function createFilterDsp(entry, sampleRate) {
  const stages = Array.from({ length: FILTER_MAX_POLES / 2 }, () => ({ b0: 1, b1: 0, b2: 0, a1: 0, a2: 0, z1: 0, z2: 0 }));
  const dsp = {
    stages,
    onePoleCoeff: 0,
    onePoleZ: 0,
    renderedKind: "none",
    lastCutoffCv: 0,
    lastResonanceCv: 0,
    audioCoeffsInitialized: false,
    // applyCoeffs(): every active stage shares one coefficient set.
    applyCoeffs(kind, cutoffCv, resonanceCv) {
      const cutoff = filterCutoffHz(entry, cutoffCv, sampleRate);
      this.onePoleCoeff = Math.exp((-2 * Math.PI * cutoff) / sampleRate);
      const count = filterBiquadCount(kind, entry.slope);
      const shelf = isShelfFilterType(kind);
      const value = shelf ? shelfGainDbFromCv(resonanceCv) / count : filterResonanceQ(entry, resonanceCv);
      for (let i = 0; i < count; i += 1) setBiquadCoeffs(stages[i], kind, cutoff, value, sampleRate);
    },
    // Slope changes reset every stage but the first, like setParam("slope").
    resetUpperStages() {
      for (let i = 1; i < stages.length; i += 1) {
        stages[i].z1 = 0;
        stages[i].z2 = 0;
      }
    },
    render(input, cutoffBuf, resonanceBuf, output, cutoffFed, resonanceFed) {
      const kind = entry.filterType;
      if (kind === "none") {
        this.renderedKind = "none";
        output.set(input);
        return;
      }
      if (kind !== this.renderedKind) {
        this.renderedKind = kind;
        entry.coeffsDirty = true;
        this.audioCoeffsInitialized = false;
      }
      const count = filterBiquadCount(kind, entry.slope);
      const onePole = filterUsesOnePole(kind, entry.slope);
      const highpass = kind === "highpass";
      const audioFed = cutoffFed || resonanceFed;
      const stride = clampRange(Math.trunc(entry.coeffStride) || 4, 1, 16);
      if (!audioFed && entry.coeffsDirty) {
        this.applyCoeffs(kind, entry.cutoff, entry.resonance);
        entry.coeffsDirty = false;
      }
      for (let i = 0; i < output.length; i += 1) {
        if (audioFed && i % stride === 0) {
          const cutoffCv = cutoffFed ? cutoffBuf[i] : entry.cutoff;
          const resonanceCv = resonanceFed ? resonanceBuf[i] : entry.resonance;
          if (!this.audioCoeffsInitialized || cutoffCv !== this.lastCutoffCv || resonanceCv !== this.lastResonanceCv) {
            this.applyCoeffs(kind, cutoffCv, resonanceCv);
            this.lastCutoffCv = cutoffCv;
            this.lastResonanceCv = resonanceCv;
            this.audioCoeffsInitialized = true;
          }
        }
        let y = input[i];
        for (let k = 0; k < count; k += 1) {
          const st = stages[k];
          const out = st.b0 * y + st.z1;
          st.z1 = flushDenorm(st.b1 * y - st.a1 * out + st.z2);
          st.z2 = flushDenorm(st.b2 * y - st.a2 * out);
          y = out;
        }
        if (onePole) {
          this.onePoleZ = flushDenorm(y * (1 - this.onePoleCoeff) + this.onePoleZ * this.onePoleCoeff);
          y = highpass ? y - this.onePoleZ : this.onePoleZ;
        }
        output[i] = y;
      }
    },
    // Magnitude of the current coefficient set at each frequency (UI curve).
    response(freqs, mag) {
      const kind = entry.filterType;
      if (kind === "none") {
        mag.fill(1);
        return;
      }
      const count = filterBiquadCount(kind, entry.slope);
      const onePole = filterUsesOnePole(kind, entry.slope);
      const st = stages[0];
      const c = this.onePoleCoeff;
      for (let i = 0; i < freqs.length; i += 1) {
        const w = (2 * Math.PI * freqs[i]) / sampleRate;
        const c1 = Math.cos(w);
        const s1 = Math.sin(w);
        const c2 = Math.cos(2 * w);
        const s2 = Math.sin(2 * w);
        let m = 1;
        if (count > 0) {
          const nr = st.b0 + st.b1 * c1 + st.b2 * c2;
          const ni = -(st.b1 * s1 + st.b2 * s2);
          const dr = 1 + st.a1 * c1 + st.a2 * c2;
          const di = -(st.a1 * s1 + st.a2 * s2);
          m = Math.sqrt((nr * nr + ni * ni) / (dr * dr + di * di || 1e-30)) ** count;
        }
        if (onePole) {
          const dr = 1 - c * c1;
          const di = c * s1;
          const den = Math.sqrt(dr * dr + di * di) || 1e-30;
          // LP: (1-c)/(1-c z^-1); HP: c(1-z^-1)/(1-c z^-1)
          const num = kind === "highpass" ? c * Math.sqrt((1 - c1) ** 2 + s1 * s1) : 1 - c;
          m *= num / den;
        }
        mag[i] = m;
      }
    },
  };
  return dsp;
}

function curtisAsymmetricClip(value) {
  const normalized = value / AUDIO_PEAK_VOLTS;
  const drive = normalized >= 0 ? 1.08 : 0.96;
  return AUDIO_PEAK_VOLTS * Math.tanh(normalized * drive);
}

function createCurtisFilterDsp(entry, sampleRate) {
  const stage = new Float64Array(FILTER_MAX_POLES);
  const dsp = {
    g: 0.1,
    feedback: 0,
    renderedKind: "none",
    lastCutoffCv: 0,
    lastResonanceCv: 0,
    audioCoeffsInitialized: false,
    // updateCoefficients() in CurtisFilterNode.h.
    updateCoefficients(cutoffCv, resonanceCv, poles) {
      const hz = filterCutoffHz(entry, cutoffCv, sampleRate);
      const phase = (0.5 * hz) / sampleRate;
      const cosW = Math.cos(2 * Math.PI * phase);
      const tanW = Math.sin(2 * Math.PI * phase) / (Math.abs(cosW) < 0.0001 ? 0.0001 : cosW);
      this.g = clampRange(tanW / (1 + tanW), 0.0001, 0.9995);
      if (isShelfFilterType(entry.filterType)) {
        this.feedback = 0;
      } else {
        const q = filterResonanceQ(entry, resonanceCv);
        const qSpan = entry.resonanceMaxQ - entry.resonanceMinQ;
        const amount = clampRange(qSpan > 0.0001 ? (q - entry.resonanceMinQ) / qSpan : 0, 0, 1);
        this.feedback = amount * (poles >= 4 ? 0.985 : 0.94);
      }
      entry.coeffsDirty = false;
    },
    resetUpperStages() {},
    render(input, cutoffBuf, resonanceBuf, output, cutoffFed, resonanceFed) {
      const kind = entry.filterType;
      if (kind === "none") {
        this.renderedKind = "none";
        output.set(input);
        return;
      }
      if (kind !== this.renderedKind) {
        this.renderedKind = kind;
        entry.coeffsDirty = true;
        this.audioCoeffsInitialized = false;
      }
      const poles = filterPolesFromSlope(entry.slope);
      const audioFed = cutoffFed || resonanceFed;
      const stride = clampRange(Math.trunc(entry.coeffStride) || 4, 1, 16);
      if (!audioFed && entry.coeffsDirty) this.updateCoefficients(entry.cutoff, entry.resonance, poles);
      const shelfGain = isShelfFilterType(kind) ? 10 ** (shelfGainDbFromCv(entry.resonance) / 20) - 1 : 0;
      for (let i = 0; i < output.length; i += 1) {
        if (audioFed && i % stride === 0) {
          const cutoffCv = cutoffFed ? cutoffBuf[i] : entry.cutoff;
          const resonanceCv = resonanceFed ? resonanceBuf[i] : entry.resonance;
          if (!this.audioCoeffsInitialized || cutoffCv !== this.lastCutoffCv || resonanceCv !== this.lastResonanceCv) {
            this.updateCoefficients(cutoffCv, resonanceCv, poles);
            this.lastCutoffCv = cutoffCv;
            this.lastResonanceCv = resonanceCv;
            this.audioCoeffsInitialized = true;
          }
        }
        const x = input[i];
        const driven = curtisAsymmetricClip(x + curtisAsymmetricClip(stage[poles - 1] * this.feedback));
        let chain = driven;
        let previous = driven;
        for (let p = 0; p < poles; p += 1) {
          previous = chain;
          const v = (chain - stage[p]) * this.g;
          const out = v + stage[p];
          stage[p] = out + v;
          chain = out;
        }
        const bandpass = previous - chain;
        const highpass = x - 2 * previous + chain;
        let y;
        switch (kind) {
          case "highpass": y = highpass; break;
          case "bandpass": y = bandpass; break;
          case "notch": y = chain + highpass; break;
          case "allpass": y = x - 4 * bandpass; break;
          case "lowshelf": y = x + shelfGain * chain; break;
          case "highshelf": y = x + shelfGain * highpass; break;
          default: y = chain; break;
        }
        output[i] = curtisAsymmetricClip(y);
      }
    },
    // Small-signal (linear, unclipped) magnitude of the TPT cascade with its
    // resonance feedback - for the widget's curve display only.
    response(freqs, mag) {
      const kind = entry.filterType;
      if (kind === "none") {
        mag.fill(1);
        return;
      }
      const poles = filterPolesFromSlope(entry.slope);
      const g = this.g;
      const k = this.feedback;
      const shelfGain = isShelfFilterType(kind) ? 10 ** (shelfGainDbFromCv(entry.resonance) / 20) - 1 : 0;
      const mul = (a, b) => [a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]];
      const div = (a, b) => {
        const d = b[0] * b[0] + b[1] * b[1] || 1e-30;
        return [(a[0] * b[0] + a[1] * b[1]) / d, (a[1] * b[0] - a[0] * b[1]) / d];
      };
      for (let i = 0; i < freqs.length; i += 1) {
        const w = (2 * Math.PI * freqs[i]) / sampleRate;
        const zi = [Math.cos(w), -Math.sin(w)]; // z^-1
        const den = [1 - (1 - 2 * g) * zi[0], -(1 - 2 * g) * zi[1]];
        const h1 = div([g * (1 + zi[0]), g * zi[1]], den); // one TPT stage
        const s1 = div([2 * g * zi[0], 2 * g * zi[1]], den); // its state
        let hPrev = [1, 0];
        for (let p = 1; p < poles; p += 1) hPrev = mul(hPrev, h1);
        const hLast = mul(hPrev, h1);
        const loop = mul(s1, hPrev);
        const u = div([1, 0], [1 - k * loop[0], -k * loop[1]]);
        const prev = mul(hPrev, u);
        const last = mul(hLast, u);
        const bp = [prev[0] - last[0], prev[1] - last[1]];
        const hp = [1 - 2 * prev[0] + last[0], -2 * prev[1] + last[1]];
        let y;
        switch (kind) {
          case "highpass": y = hp; break;
          case "bandpass": y = bp; break;
          case "notch": y = [last[0] + hp[0], last[1] + hp[1]]; break;
          case "allpass": y = [1 - 4 * bp[0], -4 * bp[1]]; break;
          case "lowshelf": y = [1 + shelfGain * last[0], shelfGain * last[1]]; break;
          case "highshelf": y = [1 + shelfGain * hp[0], shelfGain * hp[1]]; break;
          default: y = last; break;
        }
        mag[i] = Math.hypot(y[0], y[1]);
      }
    },
  };
  return dsp;
}

// ============================================================================
// FM oscillator core - per-sample port of nodes/FmOscillatorNode.h's render()
// together with PhaseOsc (DspPrimitives.h). Phase is kept in cycles [0,1];
// linear ramps saturate at 1 until the next phase reset instead of wrapping.
// ============================================================================
const FM_INDEX_MAX_PHASE_DEVIATION = 4; // kFmIndexMaxPhaseDeviation (cycles)
const FM_SELF_FEEDBACK_MAX_PHASE_DEVIATION = 1; // kSelfFeedbackMaxPhaseDeviation
const SUPER_SAW_CENTS = [-28, -14, -7, 7, 14, 28];
// fm-oscillator CV ports that replace a fallback constant (see _fmCvInput).
const FM_CV_INPUT_PORTS = ["fmIndex", "feedback", "superSawDetune", "superSawMix"];

// PhaseOsc::sampleAt() for a phase in cycles (any value, wrapped here).
function firmwareWaveSample(waveType, phase, dutyThreshold = 0.5) {
  const p = phase - Math.floor(phase);
  switch (waveType) {
    case "square":
      return p < dutyThreshold ? 1 : -1;
    case "triangle":
      return 4 * Math.abs(p - (p < 0.5 ? 0 : 1)) - 1;
    case "sawtooth":
    case "linear-up":
      return 2 * p - 1;
    case "sawtooth-down":
    case "linear-down":
      return 1 - 2 * p;
    default:
      return Math.sin(2 * Math.PI * p);
  }
}

function createFmOscillatorCore(entry, sampleRate) {
  let phase = 0;
  const sidePhases = new Float64Array(SUPER_SAW_CENTS.length);
  let feedbackSample = 0;
  let noiseState = 0x12345678;
  let pinkB0 = 0;
  let pinkB1 = 0;
  let pinkB2 = 0;
  const nextNoise = () => {
    noiseState ^= noiseState << 13;
    noiseState ^= noiseState >>> 17;
    noiseState ^= noiseState << 5;
    return ((noiseState >>> 0) / 4294967295) * 2 - 1;
  };
  const nextPinkNoise = () => {
    const white = nextNoise();
    pinkB0 = 0.99765 * pinkB0 + white * 0.099046;
    pinkB1 = 0.963 * pinkB1 + white * 0.2965164;
    pinkB2 = 0.57 * pinkB2 + white * 1.0526913;
    return clampRange((pinkB0 + pinkB1 + pinkB2 + white * 0.1848) * 0.25, -1, 1);
  };
  return {
    // Graph.h resets only the main phase on note-on; SuperSaw sides run on.
    resetPhase() {
      phase = 0;
    },
    render(inputs, output) {
      const [hzBuf, fmInBuf, fmIndexBuf, feedbackBuf, detuneBuf, mixBuf] = inputs;
      const wave = entry.waveType;
      const invSr = 1 / sampleRate;
      if (wave === "noise" || wave === "pink-noise") {
        const next = wave === "noise" ? nextNoise : nextPinkNoise;
        for (let i = 0; i < output.length; i += 1) output[i] = next();
        return;
      }
      // Block-rate like the firmware (portSample(inst, port, 0)).
      const phaseOffsetScale = (fmIndexBuf[0] / CV_MAX_VOLTS) * FM_INDEX_MAX_PHASE_DEVIATION / AUDIO_PEAK_VOLTS;
      const feedbackAmount = clampRange(feedbackBuf[0] / CV_MAX_VOLTS, 0, 1) * FM_SELF_FEEDBACK_MAX_PHASE_DEVIATION;
      const feedbackActive = feedbackAmount !== 0;
      if (wave === "supersaw") {
        const detune = clampRange(detuneBuf[0] / CV_MAX_VOLTS, 0, 1);
        const mix = clampRange(mixBuf[0] / CV_MAX_VOLTS, 0, 1);
        const norm = 1 / (1 + 6 * mix);
        const sideWeight = mix * norm;
        const ratios = SUPER_SAW_CENTS.map((cents) => 2 ** ((cents * detune) / 1200));
        if (detune === 0) sidePhases.fill(phase);
        for (let i = 0; i < output.length; i += 1) {
          const inc = hzBuf[i] * invSr;
          let offset = fmInBuf[i] * phaseOffsetScale; // 0 while "fmIn" is unpatched
          if (feedbackActive) offset += feedbackSample * feedbackAmount;
          let sample = firmwareWaveSample("sawtooth", phase + offset) * norm;
          phase = (phase + inc) % 1;
          if (phase < 0) phase += 1;
          let sideSum = 0;
          for (let v = 0; v < ratios.length; v += 1) {
            sideSum += firmwareWaveSample("sawtooth", sidePhases[v] + offset);
            let next = (sidePhases[v] + inc * ratios[v]) % 1;
            if (next < 0) next += 1;
            sidePhases[v] = next;
          }
          sample += sideSum * sideWeight;
          if (feedbackActive) feedbackSample = sample;
          output[i] = sample;
        }
        return;
      }
      if (wave === "wavetable") {
        // Morphable wavetable bank, same read as FmOscillatorNode.h's Wavetable branch.
        // Morph is block-rate here (the widget's value); the firmware also takes it per sample.
        const tables = entry.wavetables && entry.wavetables.length > 0 ? entry.wavetables : null;
        const morph = clampRange((Number(entry.morph) || 0) / CV_MAX_VOLTS, 0, 1);
        for (let i = 0; i < output.length; i += 1) {
          let offset = fmInBuf[i] * phaseOffsetScale;
          if (feedbackActive) offset += feedbackSample * feedbackAmount;
          const sample = tables ? sampleBank(tables, phase + offset, morph) : Math.sin(2 * Math.PI * (phase + offset));
          if (feedbackActive) feedbackSample = sample;
          phase = (phase + hzBuf[i] * invSr) % 1;
          if (phase < 0) phase += 1;
          output[i] = sample;
        }
        return;
      }
      const linear = isLinearWave(wave);
      const duty = wave === "square" ? clampRange(entry.dutyCycle / CV_MAX_VOLTS, 0, 1) : 0.5;
      for (let i = 0; i < output.length; i += 1) {
        let offset = fmInBuf[i] * phaseOffsetScale; // 0 while "fmIn" is unpatched
        if (feedbackActive) offset += feedbackSample * feedbackAmount;
        const sample = linear && offset === 0 && phase >= 1
          ? (wave === "linear-up" ? 1 : -1)
          : firmwareWaveSample(wave, phase + offset, duty);
        if (feedbackActive) feedbackSample = sample;
        const inc = hzBuf[i] * invSr;
        if (linear) {
          phase = Math.min(1, phase + Math.max(0, inc));
        } else {
          phase = (phase + inc) % 1;
          if (phase < 0) phase += 1;
        }
        output[i] = sample;
      }
    },
  };
}

function vcaAmountFromCv(volts, curve) {
  const v = clampRange(volts / CV_MAX_VOLTS, 0, 1);
  if (curve === "exponential") return (10 ** v - 1) / 9;
  // Gain/VCA widget's "curve" port (2nd, independent VCA stage): 0-10V ->
  // 0..2, 5V = unity. Mirrors cv::curveGain() in CvConventions.h.
  if (curve === "gainCurve") return v * 2;
  return v;
}








const VCA_CURVE_SAMPLES = 1024;
function buildVcaCurve(curve, scale = 1) {
  const samples = new Float32Array(VCA_CURVE_SAMPLES);
  for (let i = 0; i < VCA_CURVE_SAMPLES; i += 1) {
    const v = i / (VCA_CURVE_SAMPLES - 1); 
    samples[i] = (curve === "exponential" ? (10 ** v - 1) / 9 : curve === "gainCurve" ? v * 2 : v) * scale;
  }
  return samples;
}









const RANGE_CURVE_SAMPLES = 1024;
function buildRangeCurve(minVal, maxVal, curve) {
  const samples = new Float32Array(RANGE_CURVE_SAMPLES);
  for (let i = 0; i < RANGE_CURVE_SAMPLES; i += 1) {
    const v = i / (RANGE_CURVE_SAMPLES - 1); 
    samples[i] = cvToRange(v * CV_MAX_VOLTS, minVal, maxVal, curve);
  }
  return samples;
}










let pitchCvCurveCache = null;
function buildPitchCvCurve() {
  if (pitchCvCurveCache) return pitchCvCurveCache;
  const samples = new Float32Array(RANGE_CURVE_SAMPLES);
  for (let i = 0; i < RANGE_CURVE_SAMPLES; i += 1) {
    const x = (i / (RANGE_CURVE_SAMPLES - 1)) * 2 - 1; 
    const totalVolts = 10 * x + 5;
    samples[i] = hzFromPitchCv(totalVolts);
  }
  pitchCvCurveCache = samples;
  return samples;
}





function mapValueMapperRaw(value, p) {
  if (p.list && p.list.length >= 2) {
    let idx = 0;
    if (p.point2In !== p.point1In) {
      const frac = clampRange((value - p.point1In) / (p.point2In - p.point1In), 0, 1);
      idx = Math.min(p.list.length - 1, Math.max(0, Math.floor(frac * p.list.length)));
    }
    return p.list[idx];
  }
  let mapped;
  if (p.point2In === p.point1In) {
    mapped = p.point1Out;
  } else {
    const slope = (p.point2Out - p.point1Out) / (p.point2In - p.point1In);
    mapped = p.point1Out + (value - p.point1In) * slope;
  }
  if (p.constraintMin !== null && p.constraintMin !== undefined && mapped < p.constraintMin) mapped = p.constraintMin;
  if (p.constraintMax !== null && p.constraintMax !== undefined && mapped > p.constraintMax) mapped = p.constraintMax;
  return mapped;
}












function eqFilterTypeToBiquad(filterType) {
  return filterType === "peak" ? "peaking" : filterType || "peaking";
}

// The firmware's biquads (RBJ cookbook) take Q as a plain ratio for every
// type; Web Audio's BiquadFilterNode instead reads Q in dB for "lowpass"/
// "highpass" (alpha = sin(w0) / (2 * 10^(Q/20))), so those two need the
// ratio converted or the resonance comes out far too weak.
function biquadQ(biquadType, q) {
  if (biquadType === "lowpass" || biquadType === "highpass") return 20 * Math.log10(Math.max(q, 1e-4));
  return q;
}

// A shelving biquad ignores Q and is shaped by its gain instead, so the Filter
// widget's "resonance" control drives the shelf gain for those two types (the
// widget relabels the row accordingly). 0-10V maps to -24..+24 dB.
function isShelfFilterType(filterType) {
  return filterType === "lowshelf" || filterType === "highshelf";
}

function shelfGainDbFromCv(volts) {
  return ((Number(volts) || 0) - 5) * 4.8;
}













function lfoWaveValueAtPhase(waveType, phaseDeg) {
  const phase = ((phaseDeg / 360) % 1 + 1) % 1;
  if (waveType === "square") return phase < 0.5 ? 1 : -1;
  if (waveType === "triangle") return 4 * Math.abs(phase - Math.floor(phase + 0.5)) - 1;
  if (waveType === "sawtooth" || waveType === "linear-up") return 2 * phase - 1;
  if (waveType === "sawtooth-down" || waveType === "linear-down") return 1 - 2 * phase;
  return Math.sin(phase * Math.PI * 2);
}

export class AudioEngine {
  constructor() {
    this.ctx = null;
    this.nodes = new Map();
    this._linkBridged = false;
    this._monitorVoiceIndex = 0;
    this._monitorRoutes = [];
    
    
    
    
    
    
    
    this._wavePlayerBufferCache = new Map();
    
    
    
    this._sf2FileCache = new Map();



    this._mdaPianoBankPromise = null;
    // Browser-only switch (see setSampleLoadingEnabled): while off, sample
    // players (wave-player, soundfont-player, mda-piano) don't download their
    // files; the loads are parked here (entry -> loader) until
    // loadDeferredSamples() runs them.
    this._sampleLoadingEnabled = true;
    this._deferredSampleLoads = new Map();
    this.onDeferredSamplesChange = null;

    this._assetLoadTotal = 0;
    this._assetLoadDone = 0;
    
    
    
    
    
    this._valueMonitorTaps = new Map();
  }

  resetLoadTracking() {
    this._assetLoadTotal = 0;
    this._assetLoadDone = 0;
  }

  getLoadProgress() {
    return { done: this._assetLoadDone, total: this._assetLoadTotal };
  }

  ensureContext() {
    if (!this.ctx) {
      this.ctx = new (window.AudioContext || window.webkitAudioContext)();
      
      
      
      
      
      
      
      this._masterOutputGain = this.ctx.createGain();
      this._masterOutputGain.gain.value = 0;
      this._masterOutputGain.connect(this.ctx.destination);
      this._masterMuted = true;
    }
    return this.ctx;
  }

  
  
  
  
  resume() {
    this.ensureContext().resume();
  }

  isRunning() {
    return !!this.ctx && this.ctx.state === "running";
  }

  
  
  
  
  setMasterMuted(muted) {
    this.ensureContext();
    this._masterOutputGain.gain.value = muted ? 0 : 1;
    this._masterMuted = muted;
  }

  isMasterMuted() {
    return this._masterMuted !== false;
  }

  
  
  
  
  
  // Clamps like espEqMakeFilterPara() (EspEqEffect.h) and converts the
  // firmware's linear Q for the Web Audio biquad (see biquadQ()).
  _applyEspEqParams(entry, glideSeconds) {
    const now = this.ctx.currentTime;
    const biquadType = eqFilterTypeToBiquad(entry.filterType);
    const hz = clampRange(Number(entry.frequency) || 20, 20, this.ctx.sampleRate / 2 - 1);
    const q = biquadQ(biquadType, clampRange(Number(entry.q) || 0.1, 0.1, 20));
    const gainDb = clampRange(Number(entry.gainDb) || 0, -15, 15);
    [entry.filterL, entry.filterR].forEach((filter) => {
      filter.type = biquadType;
      if (glideSeconds > 0) {
        filter.frequency.setTargetAtTime(hz, now, glideSeconds);
        filter.Q.setTargetAtTime(q, now, glideSeconds);
        filter.gain.setTargetAtTime(gainDb, now, glideSeconds);
      } else {
        filter.frequency.value = hz;
        filter.Q.value = q;
        filter.gain.value = gainDb;
      }
    });
  }

  _createDelayChannel(ctx, params) {
    const input = ctx.createGain();
    input.gain.value = 1;

    const delay = ctx.createDelay(2.2);
    delay.delayTime.value = (params.delayTimeMs ?? 300) / 1000;

    const feedbackGain = ctx.createGain();
    feedbackGain.gain.value = params.feedback ?? 0.3;
    delay.connect(feedbackGain);
    feedbackGain.connect(delay);

    const [dryGain, wetGain] = this._delayMixGains(params.mix ?? 0.35);
    const dry = ctx.createGain();
    dry.gain.value = dryGain;
    const wet = ctx.createGain();
    wet.gain.value = wetGain;
    const output = ctx.createGain();
    output.gain.value = 1;

    // Bypass (DelayNode.h): input straight to output, echo path muted.
    const bypass = params.bypass === true;
    const processed = ctx.createGain();
    processed.gain.value = bypass ? 0 : 1;
    const bypassGain = ctx.createGain();
    bypassGain.gain.value = bypass ? 1 : 0;

    input.connect(delay);
    input.connect(dry);
    delay.connect(wet);
    dry.connect(processed);
    wet.connect(processed);
    processed.connect(output);
    input.connect(bypassGain);
    bypassGain.connect(output);

    return { input, delay, feedbackGain, dry, wet, processed, bypassGain, output };
  }

  
  
  
  _delayMixGains(mix) {
    const m = Math.max(0, Math.min(1, mix));
    return [Math.cos((m * Math.PI) / 2), Math.sin((m * Math.PI) / 2)];
  }

  


  
  


  
  
  
  
  
  
  _wireLinkBridge() {
    if (this._linkBridged) return;
    let outputLink = null;
    let inputLink = null;
    this.nodes.forEach((entry) => {
      if (entry.type === "audio-output-link") outputLink = entry;
      if (entry.type === "audio-input-link") inputLink = entry;
    });
    if (outputLink && inputLink) {
      for (let bus = 0; bus < LINK_BUS_COUNT; bus += 1) {
        outputLink.nodes[bus].connect(inputLink.nodes[bus]);
      }
      this._linkBridged = true;
    }
  }

  // Freely-placeable linked-input/linked-output widgets, matched by
  // linkId/sourceLinkId rather than a fixed bus index. Unlike
  // _wireLinkBridge() above (a fixed singleton pair wired once, ever) this
  // rebuilds from scratch on every call, since Web Audio has no "list
  // current connections" API to introspect what's already wired - it must
  // remember and explicitly disconnect what it connected last time.
  _wireLinkedIO() {
    (this._linkedIOEdges || []).forEach(({ from, to }) => {
      try {
        from.disconnect(to);
      } catch (e) {
        // Already disconnected (e.g. its node was torn down) - fine to ignore.
      }
    });
    this._linkedIOEdges = [];

    const sourcesByLinkId = new Map();
    this.nodes.forEach((entry) => {
      if (entry.type.startsWith("linked-input") && entry.linkId) {
        sourcesByLinkId.set(entry.linkId, entry.gainNodes);
      }
    });
    this.nodes.forEach((entry) => {
      if (!entry.type.startsWith("linked-output") || !entry.sourceLinkId) return;
      const sources = sourcesByLinkId.get(entry.sourceLinkId);
      if (!sources) return;
      entry.gainNodes.forEach((target, index) => {
        const source = sources[index];
        if (!source) return;
        source.connect(target);
        this._linkedIOEdges.push({ from: source, to: target });
      });
    });
  }

  
  
  
  
  
  
  
  
  
  
  retriggerOscillatorEntry(entry) {
    if (!entry || (entry.type !== "oscillator" && entry.type !== "fm-oscillator")) return;
    const now = this.ctx.currentTime;

    if (entry.type === "fm-oscillator") {
      entry.core.resetPhase();
      return;
    }

    const newOsc = this.ctx.createOscillator();
    setOscWaveType(this.ctx, newOsc, isNoiseWave(entry.waveType) ? "sine" : entry.waveType);
    newOsc.frequency.value = entry.osc.frequency.value;
    newOsc.detune.value = entry.osc.detune.value;
    try {
      entry.pitchWaveShaper.disconnect(entry.osc.frequency);
      entry.pitchWaveShaper.connect(newOsc.frequency);
      entry.feedbackGain.disconnect(entry.osc.frequency);
      entry.feedbackGain.connect(newOsc.frequency);
    } catch (err) {
    }
    newOsc.connect(entry.feedbackDelay);
    if (!isNoiseWave(entry.waveType) && !isLinearWave(entry.waveType)) {
      newOsc.connect(entry.ampGain);
    }
    newOsc.start(now);

    try {
      entry.osc.stop(now);
      entry.osc.disconnect();
    } catch (err) {
      
    }

    entry.osc = newOsc;
    if (isLinearWave(entry.waveType)) applyOscLinear(this.ctx, entry);
  }

  
  
  
  
  
  
  
  
  
  
  
  
  
  
  // Fetches+parses samples/mda-piano.pack once (shared by every mda-piano
  // instance, same cache-Promise pattern as _loadWavePlayerBuffer/getSf2Info
  // above) via the same generic file-download route the File Explorer/
  // WavePlayer/SoundFontPlayer previews already use to reach LittleFS.
  _loadMdaPianoBank() {
    if (!this._mdaPianoBankPromise) {
      this._assetLoadTotal += 1;
      const path = encodeURIComponent("samples/mda-piano.pack");
      this._mdaPianoBankPromise = fetch(`/api/files/download?volume=flash&path=${path}`)
        .then((res) => (res.ok ? res.arrayBuffer() : Promise.reject(new Error(`HTTP ${res.status}`))))
        .then((buf) => parseMdaPianoPack(buf))
        .catch((err) => {
          console.error("MDA Piano: failed to load samples/mda-piano.pack:", err);
          return null;
        })
        .finally(() => { this._assetLoadDone += 1; });
    }
    return this._mdaPianoBankPromise;
  }

  setSampleLoadingEnabled(enabled) {
    this._sampleLoadingEnabled = enabled !== false;
  }

  isSampleLoadingEnabled() {
    return this._sampleLoadingEnabled;
  }

  // Number of sample players currently waiting for their files.
  getDeferredSampleCount() {
    return this._deferredSampleLoads.size;
  }

  // Downloads everything that was skipped while sample loading was off.
  loadDeferredSamples() {
    const pending = [...this._deferredSampleLoads.values()];
    this._deferredSampleLoads.clear();
    pending.forEach((load) => load());
    this._notifyDeferredSamples();
  }

  _notifyDeferredSamples() {
    if (typeof this.onDeferredSamplesChange === "function") {
      this.onDeferredSamplesChange(this._deferredSampleLoads.size);
    }
  }

  // Runs `load` now, or parks it per entry while sample loading is off (a
  // later request for the same entry, e.g. a new file, replaces it).
  _requestSampleLoad(entry, load) {
    if (this._sampleLoadingEnabled) {
      load();
      return;
    }
    this._deferredSampleLoads.set(entry, load);
    this._notifyDeferredSamples();
  }

  _loadMdaPianoEntry(entry) {
    this._requestSampleLoad(entry, () => {
      this._loadMdaPianoBank().then((instruments) => {
        entry.instruments = instruments;
      });
    });
  }

  _loadWavePlayerBuffer(entry) {
    entry.buffer = null;
    if (!entry.sampleFile) {
      if (this._deferredSampleLoads.delete(entry)) this._notifyDeferredSamples();
      return;
    }
    this._requestSampleLoad(entry, () => this._fetchWavePlayerBuffer(entry));
  }

  _fetchWavePlayerBuffer(entry) {
    entry.buffer = null;
    if (!entry.sampleFile || !this.ctx) return;

    if (!this._wavePlayerBufferCache.has(entry.sampleFile)) {
      const path = encodeURIComponent(`samples/${entry.sampleFile}`);
      const promise = fetch(`/api/files/download?volume=flash&path=${path}`)
        .then((res) => (res.ok ? res.arrayBuffer() : Promise.reject(new Error(`HTTP ${res.status}`))))
        .then((buf) => this.ctx.decodeAudioData(buf))
        .catch((err) => {
          console.error(`WavePlayer: failed to load sample "${entry.sampleFile}":`, err);
          return null;
        });
      this._wavePlayerBufferCache.set(entry.sampleFile, promise);
    }
    this._assetLoadTotal += 1;
    this._wavePlayerBufferCache.get(entry.sampleFile).then((decoded) => {
      entry.buffer = decoded;
      this._assetLoadDone += 1;
    });
  }

  
  
  
  
  
  
  
  _retriggerWavePlayerEntry(entry) {
    if (!entry || entry.type !== "wave-player" || !this.ctx) return;
    if (!entry.buffer) return; 
    const now = this.ctx.currentTime;

    if (entry.source) {
      try {
        entry.source.stop(now);
        entry.source.disconnect();
      } catch (err) {
        
      }
    }

    entry.releaseGain.gain.cancelScheduledValues(now);
    entry.releaseGain.gain.setValueAtTime(1, now);

    const source = this.ctx.createBufferSource();
    source.buffer = entry.buffer;
    source.loop = false;
    source.playbackRate.value = 2 ** ((entry.note - entry.rootNoteMidi) / 12);
    source.connect(entry.releaseGain);
    source.start(now);
    entry.source = source;
  }

  

  _sf2NewChannelState(bank, program) {
    return { bank, program, pitchWheel: 8192, pitchRange: 2, sustain: false, midiRPN: 0xffff, midiData: 0 };
  }

  
  
  
  
  getSf2Info(volume, path) {
    if (!path) return Promise.resolve(null);
    const key = `${volume}:${path}`;
    if (!this._sf2FileCache.has(key)) {
      const url = `/api/files/download?volume=${encodeURIComponent(volume)}&path=${encodeURIComponent(path)}`;
      const promise = fetch(url)
        .then((res) => (res.ok ? res.arrayBuffer() : Promise.reject(new Error(`HTTP ${res.status}`))))
        .then((buf) => parseSf2(buf))
        .catch((err) => {
          console.error(`SoundFontPlayer: failed to load "${path}" (volume=${volume}):`, err);
          return null;
        });
      this._sf2FileCache.set(key, promise);
    }
    return this._sf2FileCache.get(key);
  }

  _loadSf2Entry(entry) {
    entry.parsed = null;
    if (!entry.path) return;
    this._requestSampleLoad(entry, () => this._fetchSf2Entry(entry));
  }

  _fetchSf2Entry(entry) {
    this._assetLoadTotal += 1;
    this.getSf2Info(entry.volume, entry.path).then((parsed) => {
      entry.parsed = parsed;
      this._assetLoadDone += 1;
    });
  }

  
  
  
  setSoundFontParams(id, params) {
    const entry = this.nodes.get(id);
    if (!entry || entry.type !== "soundfont-player") return;
    const fileChanged =
      (params.volume !== undefined && params.volume !== entry.volume) ||
      (params.path !== undefined && params.path !== entry.path);
    if (params.volume !== undefined) entry.volume = params.volume;
    if (params.path !== undefined) entry.path = params.path;
    if (params.maxVoices !== undefined) entry.maxVoices = params.maxVoices;
    if (params.enableCc !== undefined) entry.enableCc = params.enableCc;
    if (params.enableModWheel !== undefined) entry.enableModWheel = params.enableModWheel;
    if (params.enablePitchWheel !== undefined) entry.enablePitchWheel = params.enablePitchWheel;
    if (params.bank !== undefined) entry.bank = params.bank;
    if (params.program !== undefined) entry.program = params.program;
    if (params.gain !== undefined) {
      entry.gain = params.gain;
      const g = entry.gainCvActive ? 0 : ((Number(params.gain) || 0) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS;
      const now = this.ctx.currentTime;
      [entry.leftOut, entry.rightOut].forEach((node) => {
        node.gain.cancelScheduledValues(now);
        node.gain.setValueAtTime(g, now);
      });
    }
    if (params.bank !== undefined || params.program !== undefined) {
      entry.channels.forEach((channel) => {
        channel.bank = entry.bank;
        channel.program = entry.program;
      });
    }
    if (fileChanged) {
      this._sf2StopAllVoices(entry);
      this._loadSf2Entry(entry);
    }
  }

  _sf2StopAllVoices(entry) {
    entry.voices.forEach((voice) => this._sf2DisposeVoice(voice));
    entry.voices = [];
  }

  _sf2DisposeVoice(voice) {
    try {
      voice.source.stop();
    } catch (err) {
      
    }
    try {
      voice.source.disconnect();
      voice.filterNode.disconnect();
      voice.envelopeGain.disconnect();
      voice.pannerNode.disconnect();
    } catch (err) {
      
    }
  }

  _sf2ChannelGainNode(ctx, entry, channelIndex) {
    if (!entry.channelGainNodes[channelIndex]) {
      const gain = ctx.createGain();
      gain.gain.value = 1;
      gain.connect(entry.splitter);
      entry.channelGainNodes[channelIndex] = gain;
    }
    return entry.channelGainNodes[channelIndex];
  }

  
  
  
  
  
  _sf2GetRegionBuffer(ctx, region, sampleData) {
    if (region._buffer) return region._buffer;
    const start = Math.max(0, Math.min(region.offset, sampleData.length));
    const end = Math.max(start, Math.min(region.end, sampleData.length));
    const length = Math.max(1, end - start);
    const rate = region.sample_rate > 0 ? region.sample_rate : ctx.sampleRate;
    const buffer = ctx.createBuffer(1, length, rate);
    buffer.copyToChannel(sampleData.subarray(start, end), 0);
    region._buffer = buffer;
    region._bufferStart = start;
    return buffer;
  }

  _sf2SemitoneOffset(entry, channel, region, key) {
    const note = key + region.transpose + region.tune / 100;
    let semitones = (note - region.pitch_keycenter) * (region.pitch_keytrack / 100);
    if (entry.enablePitchWheel && channel.pitchWheel !== 8192) {
      semitones += (channel.pitchWheel / 16383) * channel.pitchRange * 2 - channel.pitchRange;
    }
    return semitones;
  }

  _sf2StartVoice(ctx, entry, channelIndex, channel, preset, region, key, velocity, now) {
    const buffer = this._sf2GetRegionBuffer(ctx, region, entry.parsed.sampleData);

    const source = ctx.createBufferSource();
    source.buffer = buffer;
    const doLoop = region.loop_mode !== 0 && region.loop_start < region.loop_end;
    if (doLoop) {
      source.loop = true;
      source.loopStart = Math.max(0, (region.loop_start - region._bufferStart) / buffer.sampleRate);
      source.loopEnd = Math.max(source.loopStart, (region.loop_end - region._bufferStart) / buffer.sampleRate);
    }
    source.playbackRate.value = 2 ** (this._sf2SemitoneOffset(entry, channel, region, key) / 12);

    // tsf's voice lowpass: only active for initialFilterFc <= 13500 cents and
    // below 0.499 * sampleRate; its resonance is initialFilterQ centibels,
    // which is exactly what Web Audio's lowpass Q already expects (dB).
    // Inactive -> a 0 dB peaking biquad, i.e. an exact passthrough.
    const filterNode = ctx.createBiquadFilter();
    const fc = region.initialFilterFc <= 13500 ? cents2Hertz(region.initialFilterFc) : ctx.sampleRate;
    if (fc < 0.499 * ctx.sampleRate) {
      filterNode.type = "lowpass";
      filterNode.frequency.value = fc;
      filterNode.Q.value = region.initialFilterQ / 10;
    } else {
      filterNode.type = "peaking";
      filterNode.gain.value = 0;
    }

    const envelopeGain = ctx.createGain();
    const pannerNode = ctx.createStereoPanner();
    pannerNode.pan.value = clampRange(region.pan * 2, -1, 1);

    source.connect(filterNode);
    filterNode.connect(envelopeGain);
    envelopeGain.connect(pannerNode);
    pannerNode.connect(this._sf2ChannelGainNode(ctx, entry, channelIndex));

    const peak = decibelsToGain(-region.attenuation) * velocity;
    if ((entry._sf2DebugVoiceCount = (entry._sf2DebugVoiceCount || 0) + 1) <= 8) {
      console.info(
        `SF2 voice #${entry._sf2DebugVoiceCount}: attenuation=${region.attenuation.toFixed(2)}dB ` +
          `velocity=${velocity.toFixed(3)} peak=${peak.toFixed(4)} pan=${region.pan.toFixed(3)} ` +
          `sampleRate=${region.sample_rate} ctxSampleRate=${ctx.sampleRate} ` +
          `leftOutGain=${entry.leftOut.gain.value.toFixed(3)} rightOutGain=${entry.rightOut.gain.value.toFixed(3)} ` +
          `entryGain=${entry.gain} key=${key} keycenter=${region.pitch_keycenter} ` +
          `keytrack=${region.pitch_keytrack} transpose=${region.transpose} tune=${region.tune.toFixed(2)}`
      );
    }
    const env = region.ampenv;
    const g = envelopeGain.gain;
    g.setValueAtTime(0, now);
    let t = now + Math.max(env.delay, 0);
    g.setValueAtTime(0, t);
    t += Math.max(env.attack, 0.002);
    g.linearRampToValueAtTime(Math.max(peak, 0.0001), t);
    t += Math.max(env.hold, 0);
    g.setValueAtTime(Math.max(peak, 0.0001), t);
    const sustainLevel = Math.max(peak * env.sustain, 0.0001);
    t += Math.max(env.decay, 0.002);
    g.exponentialRampToValueAtTime(sustainLevel, t);

    const voice = {
      channelIndex,
      key,
      group: region.group,
      preset,
      region,
      source,
      filterNode,
      envelopeGain,
      pannerNode,
      startedAt: now,
      releasing: false,
      releaseEndsAt: null,
      heldBySustain: false,
    };
    source.onended = () => {
      const idx = entry.voices.indexOf(voice);
      if (idx >= 0) entry.voices.splice(idx, 1);
      this._sf2DisposeVoice(voice);
    };
    source.start(now);
    return voice;
  }

  _sf2ReleaseVoice(ctx, voice) {
    if (voice.releasing) return;
    voice.releasing = true;
    const now = ctx.currentTime;
    const releaseTime = Math.max(voice.region.ampenv.release, 0.005);
    const g = voice.envelopeGain.gain;
    const current = Math.max(g.value, 0.0001);
    g.cancelScheduledValues(now);
    g.setValueAtTime(current, now);
    g.exponentialRampToValueAtTime(0.0001, now + releaseTime);
    voice.releaseEndsAt = now + releaseTime;
    try {
      voice.source.stop(now + releaseTime + 0.05);
    } catch (err) {
      
    }
  }

  
  
  
  _sf2StealGroup(ctx, entry, channelIndex, preset, group) {
    entry.voices
      .filter((v) => !v.releasing && v.channelIndex === channelIndex && v.preset === preset && v.group === group)
      .forEach((v) => this._sf2QuickFade(ctx, v));
  }

  _sf2QuickFade(ctx, voice) {
    voice.releasing = true;
    const now = ctx.currentTime;
    const g = voice.envelopeGain.gain;
    const current = Math.max(g.value, 0.0001);
    g.cancelScheduledValues(now);
    g.setValueAtTime(current, now);
    g.exponentialRampToValueAtTime(0.0001, now + 0.02);
    try {
      voice.source.stop(now + 0.03);
    } catch (err) {
      
    }
  }

  
  
  
  _sf2StealIfOverCapacity(ctx, entry) {
    if (entry.voices.length < entry.maxVoices) return;
    let victim = null;
    entry.voices.forEach((v) => {
      if (!v.releasing) return;
      if (!victim || (v.releaseEndsAt ?? 0) < (victim.releaseEndsAt ?? 0)) victim = v;
    });
    if (!victim) {
      entry.voices.forEach((v) => {
        if (!victim || v.startedAt < victim.startedAt) victim = v;
      });
    }
    if (victim) this._sf2QuickFade(ctx, victim);
  }

  _sf2NoteOn(ctx, entry, channelIndex, channel, key, velocity) {
    if (velocity <= 0) {
      this._sf2NoteOff(ctx, entry, channelIndex, key);
      return;
    }
    
    
    entry._sf2DebugCount = (entry._sf2DebugCount || 0) + 1;
    if (entry._sf2DebugCount <= 8) {
      console.info(
        `SF2 noteOn #${entry._sf2DebugCount}: ch=${channelIndex} key=${key} parsed=${!!entry.parsed} ` +
          `presetCount=${entry.parsed ? entry.parsed.presets.length : "n/a"} channel.bank=${channel.bank} ` +
          `channel.program=${channel.program} entry.bank=${entry.bank} entry.program=${entry.program} ` +
          `ctxState=${ctx.state}`
      );
    }
    if (!entry.parsed) {
      if (!entry._warnedNoParsed) {
        entry._warnedNoParsed = true;
        console.warn("SoundFont Player: note received but the .sf2 file has not finished loading (or failed to load) - staying silent.");
      }
      return;
    }
    const isDrum = channelIndex === 9;
    const preset = resolvePreset(entry.parsed.presets, channel.bank, channel.program, isDrum);
    if (!preset) {
      if (!entry._warnedNoPreset) {
        entry._warnedNoPreset = true;
        console.warn(`SoundFont Player: no preset found for bank ${channel.bank} program ${channel.program} (isDrum=${isDrum}) - staying silent.`);
      }
      return;
    }
    const midiVelocity = Math.max(0, Math.min(127, Math.round(velocity * 127)));
    const now = ctx.currentTime;
    let matched = 0;
    preset.regions.forEach((region) => {
      if (key < region.lokey || key > region.hikey || midiVelocity < region.lovel || midiVelocity > region.hivel) return;
      matched += 1;
      if (region.group) this._sf2StealGroup(ctx, entry, channelIndex, preset, region.group);
      this._sf2StealIfOverCapacity(ctx, entry);
      try {
        const voice = this._sf2StartVoice(ctx, entry, channelIndex, channel, preset, region, key, velocity, now);
        entry.voices.push(voice);
        if (entry._sf2DebugCount <= 8) {
          console.info(
            `SF2 noteOn #${entry._sf2DebugCount}: voice started, playbackRate=${voice.source.playbackRate.value}, ` +
              `peakGain=${voice.envelopeGain.gain.value}, bufferLength=${voice.source.buffer.length}, ` +
              `bufferSampleRate=${voice.source.buffer.sampleRate}`
          );
        }
      } catch (err) {
        
        
        
        if (!region._warnedFailed) {
          region._warnedFailed = true;
          console.error(`SoundFont Player: failed to start voice for region (key ${key}, sampleRate ${region.sample_rate}, offset ${region.offset}, end ${region.end}):`, err);
        }
      }
    });
    if (matched === 0 && !entry._warnedNoRegion) {
      entry._warnedNoRegion = true;
      console.warn(
        `SoundFont Player: preset "${preset.name}" (bank ${channel.bank} program ${channel.program}) has ` +
          `${preset.regions.length} region(s), none covering key ${key} velocity ${midiVelocity} - staying silent.`
      );
    }
  }

  _sf2NoteOff(ctx, entry, channelIndex, key) {
    const channel = entry.channels[channelIndex];
    entry.voices
      .filter((v) => !v.releasing && v.channelIndex === channelIndex && v.key === key)
      .forEach((v) => {
        if (channel && channel.sustain) {
          v.heldBySustain = true;
          return;
        }
        this._sf2ReleaseVoice(ctx, v);
      });
  }

  _sf2SetSustain(ctx, entry, channelIndex, isDown) {
    const channel = entry.channels[channelIndex];
    if (!channel) return;
    channel.sustain = isDown;
    if (isDown) return;
    entry.voices
      .filter((v) => !v.releasing && v.channelIndex === channelIndex && v.heldBySustain)
      .forEach((v) => this._sf2ReleaseVoice(ctx, v));
  }

  _sf2AllNotesOff(ctx, entry, channelIndex) {
    entry.voices.filter((v) => !v.releasing && v.channelIndex === channelIndex).forEach((v) => this._sf2ReleaseVoice(ctx, v));
  }

  _sf2AllSoundOff(entry, channelIndex) {
    entry.voices.filter((v) => v.channelIndex === channelIndex).forEach((v) => this._sf2QuickFade(this.ctx, v));
  }

  _sf2ControlChange(entry, channelIndex, channel, control, value) {
    switch (control) {
      case 0: 
        channel.bank = 0x8000 | value;
        return;
      case 32: 
        channel.bank = (channel.bank & 0x8000 ? (channel.bank & 0x7f) << 7 : 0) | value;
        return;
      case 6: 
        channel.midiData = (channel.midiData & 0x7f) | (value << 7);
        this._sf2ApplyRpnData(channel);
        return;
      case 38: 
        channel.midiData = (channel.midiData & 0x3f80) | value;
        this._sf2ApplyRpnData(channel);
        return;
      case 101: 
        channel.midiRPN = ((channel.midiRPN === 0xffff ? 0 : channel.midiRPN) & 0x7f) | (value << 7);
        return;
      case 100: 
        channel.midiRPN = ((channel.midiRPN === 0xffff ? 0 : channel.midiRPN) & 0x3f80) | value;
        return;
      case 98: 
      case 99: 
        channel.midiRPN = 0xffff;
        return;
      case 64: 
        this._sf2SetSustain(this.ctx, entry, channelIndex, value >= 64);
        return;
      case 120: 
        this._sf2AllSoundOff(entry, channelIndex);
        return;
      case 123: 
        this._sf2AllNotesOff(this.ctx, entry, channelIndex);
        return;
      default:
    }
  }

  _sf2ApplyRpnData(channel) {
    
    
    if (channel.midiRPN === 0) {
      channel.pitchRange = (channel.midiData >> 7) + 0.01 * (channel.midiData & 0x7f);
    }
  }

  
  
  
  
  handleSoundFontMidi(id, data) {
    const wrapper = this.nodes.get(id);
    const entry = wrapper && wrapper.voiced ? wrapper.instances[0] : wrapper;
    if (!entry || entry.type !== "soundfont-player" || !entry.parsed) return;
    const ctx = this.ensureContext();
    const channelIndex = Math.max(0, Math.min(15, data.channel ?? 0));
    const channel = entry.channels[channelIndex];
    if (!channel) return;

    if (data.midiType === "note_on") {
      if (data.velocity > 0) this._sf2NoteOn(ctx, entry, channelIndex, channel, data.note, data.velocity / 127);
      else this._sf2NoteOff(ctx, entry, channelIndex, data.note);
    } else if (data.midiType === "note_off") {
      this._sf2NoteOff(ctx, entry, channelIndex, data.note);
    } else if (data.midiType === "control_change") {
      const isModWheel = channelIndex === 0 && data.control === 1;
      if (isModWheel ? entry.enableModWheel : entry.enableCc) {
        this._sf2ControlChange(entry, channelIndex, channel, data.control, data.value);
      }
    } else if (data.midiType === "program_change") {
      if (data.program !== undefined) channel.program = data.program;
    } else if (data.midiType === "pitchwheel") {
      
      
      
      if (entry.enablePitchWheel && data.value !== undefined) {
        channel.pitchWheel = Math.max(0, Math.min(16383, data.value + 8192));
      }
    }
  }

  // Mirrors nodes/MdaPianoNode.h's handleMidi() - note on/off go straight to
  // the ported engine, mod wheel only updates entry.modWheel (consumed by
  // the effects-chain trem/wah depth in the "mda-piano" onaudioprocess
  // above, exactly like the firmware's state.modWheel), pitch bend goes to
  // the engine's own setPitchBend(). Sustain pedal is intentionally not
  // wired here either - MdaPianoNode.h's handleMidi() never wires it in the
  // firmware, so note-off always releases immediately there too.
  handleMdaPianoMidi(id, data) {
    const wrapper = this.nodes.get(id);
    const entry = wrapper && wrapper.voiced ? wrapper.instances[0] : wrapper;
    if (!entry || entry.type !== "mda-piano") return;

    if (data.midiType === "note_on") {
      if (data.velocity > 0) entry.engine.noteOn(data.note, data.velocity);
      else entry.engine.noteOff(data.note);
    } else if (data.midiType === "note_off") {
      entry.engine.noteOff(data.note);
    } else if (data.midiType === "control_change") {
      if (entry.enableModWheel && data.control === 1) {
        entry.modWheel = (data.value ?? 0) / 127;
      }
    } else if (data.midiType === "pitchwheel") {
      if (entry.enablePitchWheel && data.value !== undefined) {
        entry.engine.setPitchBend(Math.max(0, Math.min(16383, data.value + 8192)));
      }
    }
  }

  
  
  
  
  _createAnalyser(ctx, fftSize) {
    const analyser = ctx.createAnalyser();
    analyser.fftSize = fftSize;
    return { analyser, buffer: new Uint8Array(fftSize) };
  }

  
  
  


  
  
  
  
  
  
  
  
  
  _createCvGainChain(ctx, curve, scale = 1) {
    const prescale = ctx.createGain();
    prescale.gain.value = 2 / CV_MAX_VOLTS;
    const shiftConst = ctx.createConstantSource();
    shiftConst.offset.value = -1;
    shiftConst.start();
    const sumNode = ctx.createGain();
    sumNode.gain.value = 1;
    prescale.connect(sumNode);
    shiftConst.connect(sumNode);
    const waveShaper = ctx.createWaveShaper();
    waveShaper.curve = buildVcaCurve(curve, scale);
    sumNode.connect(waveShaper);
    return { prescale, shiftConst, sumNode, waveShaper };
  }

  
  
  
  
  
  _createRangeGainChain(ctx, domainLo, domainHi, curve) {
    const span = domainHi - domainLo || 1; 
    const prescale = ctx.createGain();
    prescale.gain.value = 2 / span;
    const shiftConst = ctx.createConstantSource();
    shiftConst.offset.value = -1 - (2 * domainLo) / span;
    shiftConst.start();
    const sumNode = ctx.createGain();
    sumNode.gain.value = 1;
    prescale.connect(sumNode);
    shiftConst.connect(sumNode);
    const waveShaper = ctx.createWaveShaper();
    waveShaper.curve = curve;
    sumNode.connect(waveShaper);
    return { prescale, shiftConst, sumNode, waveShaper };
  }

  
  
  
  
  

  
  
  
  
  
  
  
  
  
  _frequencyDividerCvOffsetCurve() {
    if (!this._frequencyDividerCvOffsetCurveCache) {
      const N = 2049;
      const curve = new Float32Array(N);
      for (let i = 0; i < N; i += 1) {
        const x = (i / (N - 1)) * 2 - 1; 
        // 1 V/oct CV: dividing frequency by N means subtracting log2(N) V.
        // The divider snaps to 1/2/4/8/16 exactly like the firmware's
        // frequencyDividerPitchOffset() (ArithmeticNodes.h).
        const divider = x * 128;
        curve[i] = -(divider <= 1.5 ? 0 : divider <= 3 ? 1 : divider <= 6 ? 2 : divider <= 12 ? 3 : 4);
      }
      this._frequencyDividerCvOffsetCurveCache = curve;
    }
    return this._frequencyDividerCvOffsetCurveCache;
  }

  
  
  _absCurve() {
    if (!this._absCurveCache) {
      const N = 1025;
      const curve = new Float32Array(N);
      for (let i = 0; i < N; i += 1) {
        const x = (i / (N - 1)) * 2 - 1;
        curve[i] = Math.abs(x);
      }
      this._absCurveCache = curve;
    }
    return this._absCurveCache;
  }

  // Logic gates (AND/OR/XOR/NOT): >0V reads as logical 1, matching the
  // firmware's nodes::arithmetic render*() functions (ArithmeticNodes.h).
  // Raw volts feed these waveshapers directly, same as _absCurve() above -
  // every curve here is flat on both sides of its transition point, so a
  // CV well outside -1..1 still clamps to the correct boolean plateau
  // instead of losing information the way a magnitude-preserving curve
  // (like _absCurve) would.
  _logicStepCurve() {
    if (!this._logicStepCurveCache) {
      const N = 2049;
      const curve = new Float32Array(N);
      for (let i = 0; i < N; i += 1) {
        const x = (i / (N - 1)) * 2 - 1;
        curve[i] = x > 0 ? CV_MAX_VOLTS : 0;
      }
      this._logicStepCurveCache = curve;
    }
    return this._logicStepCurveCache;
  }

  _logicInvertStepCurve() {
    if (!this._logicInvertStepCurveCache) {
      const N = 2049;
      const curve = new Float32Array(N);
      for (let i = 0; i < N; i += 1) {
        const x = (i / (N - 1)) * 2 - 1;
        curve[i] = x > 0 ? 0 : CV_MAX_VOLTS;
      }
      this._logicInvertStepCurveCache = curve;
    }
    return this._logicInvertStepCurveCache;
  }

  // XOR: booleanized inputs are differenced (bitA - bitB), which lands on
  // exactly -CV_MAX_VOLTS/0/+CV_MAX_VOLTS - true iff nonzero (either sign).
  _logicNonzeroCurve() {
    if (!this._logicNonzeroCurveCache) {
      const N = 2049;
      const curve = new Float32Array(N);
      for (let i = 0; i < N; i += 1) {
        const x = (i / (N - 1)) * 2 - 1;
        curve[i] = x !== 0 ? CV_MAX_VOLTS : 0;
      }
      this._logicNonzeroCurveCache = curve;
    }
    return this._logicNonzeroCurveCache;
  }

  
  
  
  
  _buildNode(ctx, type, params) {
    switch (type) {
      case "oscillator": {
        const waveType = params.waveType || "sine";
        const osc = ctx.createOscillator();
        setOscWaveType(ctx, osc, isNoiseWave(waveType) ? "sine" : waveType);
        osc.frequency.value = params.frequency ?? 0;

        const noiseBuffer = ctx.createBuffer(1, ctx.sampleRate * 2, ctx.sampleRate);
        const noiseData = noiseBuffer.getChannelData(0);
        for (let i = 0; i < noiseData.length; i += 1) {
          noiseData[i] = Math.random() * 2 - 1;
        }
        const noiseSource = ctx.createBufferSource();
        noiseSource.buffer = noiseBuffer;
        noiseSource.loop = true;

        const pinkNoiseBuffer = ctx.createBuffer(1, ctx.sampleRate * 2, ctx.sampleRate);
        fillPinkNoise(pinkNoiseBuffer.getChannelData(0));
        const pinkNoiseSource = ctx.createBufferSource();
        pinkNoiseSource.buffer = pinkNoiseBuffer;
        pinkNoiseSource.loop = true;

        const ampGain = ctx.createGain();
        ampGain.gain.value = 0;

        
        
        
        
        
        
        
        
        const ampCv = this._createCvGainChain(ctx, "linear");
        ampCv.waveShaper.connect(ampGain.gain);

        
        
        
        
        
        
        
        const outGain = ctx.createGain();
        outGain.gain.value = ((params.gain ?? CV_MAX_VOLTS) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS;
        // A "gain" cable replaces outGain's own value (zeroed on connect), so
        // the CV path carries the same AUDIO_PEAK_VOLTS rescale.
        const gainCv = this._createCvGainChain(ctx, "linear", AUDIO_PEAK_VOLTS);
        gainCv.waveShaper.connect(outGain.gain);
        ampGain.connect(outGain);

        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        const pitchSum = ctx.createGain();
        pitchSum.gain.value = 1;
        const pitchRestConst = ctx.createConstantSource();
        pitchRestConst.offset.value = CV_MAX_VOLTS / 2; 
        pitchRestConst.start();
        const pitchRestGate = ctx.createGain();
        // Closed by default: osc.frequency.value already carries the correct
        // direct-set Hz (see params.frequency below and applyRamp() in
        // _setParamOnEntry's "frequency" case) whenever no live pitchCv/tune
        // cable is connected. Web Audio SUMS an AudioParam's own value with
        // any connected signal, so leaving this open by default used to add
        // the rest constant's decoded frequency (~261.6 Hz, note 60) on top
        // of the intended pitch - e.g. A4 (440 Hz) came out ~703 Hz. Opened
        // (gain=1) only while a cable is actually connected - see
        // connectEdge()/disconnectEdge() below, which zero out
        // osc.frequency.value at that point so the CV chain becomes the sole
        // source instead.
        pitchRestGate.gain.value = 0;
        pitchRestConst.connect(pitchRestGate);
        pitchRestGate.connect(pitchSum);

        const tuneSum = ctx.createGain();
        tuneSum.gain.value = 1;
        const tuneRestConst = ctx.createConstantSource();
        tuneRestConst.offset.value = params.tuneVolts ?? CV_MAX_VOLTS / 2;
        tuneRestConst.start();
        const tuneRestGate = ctx.createGain();
        tuneRestGate.gain.value = 0; // see pitchRestGate above
        tuneRestConst.connect(tuneRestGate);
        tuneRestGate.connect(tuneSum);

        const pitchPreScale = ctx.createGain();
        pitchPreScale.gain.value = 0.1; 
        pitchSum.connect(pitchPreScale);
        tuneSum.connect(pitchPreScale);
        const pitchFinalShift = ctx.createConstantSource();
        pitchFinalShift.offset.value = -1; 
        pitchFinalShift.start();
        const pitchShaperInput = ctx.createGain();
        pitchShaperInput.gain.value = 1;
        pitchPreScale.connect(pitchShaperInput);
        pitchFinalShift.connect(pitchShaperInput);
        const pitchWaveShaper = ctx.createWaveShaper();
        pitchWaveShaper.curve = buildPitchCvCurve();
        pitchShaperInput.connect(pitchWaveShaper);
        pitchWaveShaper.connect(osc.frequency);

        const feedbackDelay = ctx.createDelay(1);
        feedbackDelay.delayTime.value = 1 / ctx.sampleRate;
        const feedbackGain = ctx.createGain();
        feedbackGain.gain.value = ((params.feedback ?? 0) / CV_MAX_VOLTS) * SELF_FEEDBACK_MAX_HZ_DEVIATION;
        osc.connect(feedbackDelay);
        feedbackDelay.connect(feedbackGain);
        feedbackGain.connect(osc.frequency);
        const feedbackCv = this._createCvGainChain(ctx, "linear");
        const feedbackCvScale = ctx.createGain();
        feedbackCvScale.gain.value = SELF_FEEDBACK_MAX_HZ_DEVIATION;
        feedbackCv.waveShaper.connect(feedbackCvScale);
        feedbackCvScale.connect(feedbackGain.gain);

        // Linear waves: per-note ramp source, see applyOscLinear. Rests at the
        // ramp's end value until the first gate.
        const linearSource = ctx.createConstantSource();
        linearSource.offset.value = waveType === "linear-down" ? -1 : 1;
        linearSource.start();
        osc.start();
        noiseSource.start();
        pinkNoiseSource.start();
        if (waveType === "noise") {
          noiseSource.connect(ampGain);
        } else if (waveType === "pink-noise") {
          pinkNoiseSource.connect(ampGain);
        } else if (isLinearWave(waveType)) {
          linearSource.connect(ampGain);
        } else {
          osc.connect(ampGain);
        }

        return {
          type,
          osc,
          directHz: params.frequency ?? 0,
          extraNodes: [pitchSum, pitchRestGate, tuneSum, tuneRestGate, pitchPreScale, pitchShaperInput, pitchWaveShaper],
          extraSources: [linearSource, pitchRestConst, tuneRestConst, pitchFinalShift],
          noiseSource,
          pinkNoiseSource,
          linearSource,
          ampGain,
          ampCv,
          waveType,
          outGain,
          gainCv,
          gain: params.gain ?? CV_MAX_VOLTS,
          gainCvActive: false,
          pitchSum,
          pitchRestConst,
          pitchRestGate,
          tuneSum,
          tuneRestConst,
          tuneRestGate,
          pitchWaveShaper,
          pitchCvActive: false,
          tuneCvActive: false,
          feedbackDelay,
          feedbackGain,
          feedbackCv,
          feedbackCvScale,
          feedback: params.feedback ?? 0,
          feedbackCvActive: false,
        };
      }
      case "fm-oscillator": {
        // Per-sample port of nodes/FmOscillatorNode.h in a ScriptProcessor
        // (createFmOscillatorCore): true phase modulation (fmIn * fmIndex,
        // up to kFmIndexMaxPhaseDeviation cycles), one-sample phase feedback,
        // variable square duty cycle, SuperSaw and the saturating linear
        // ramps - none of which a native OscillatorNode can do. The CV plumbing
        // around it (amplitude/gain/pitch/tune chains) is the same as the
        // plain oscillator's. Merger channels: 0 Hz, 1 fmIn, 2 fmIndex,
        // 3 feedback, 4 superSawDetune, 5 superSawMix (all raw volts).
        const waveType = params.waveType || "sine";
        const inputMerger = ctx.createChannelMerger(6);
        const processor = ctx.createScriptProcessor(256, 6, 1);
        inputMerger.connect(processor);

        // Hz: direct-set frequency (freqConst) or the pitch-CV chain below.
        const freqSum = ctx.createGain();
        freqSum.connect(inputMerger, 0, 0);
        const freqConst = ctx.createConstantSource();
        freqConst.offset.value = params.frequency ?? 0;
        freqConst.start();
        freqConst.connect(freqSum);

        const fmInScale = ctx.createGain(); // "fmIn" port: raw volts
        fmInScale.connect(inputMerger, 0, 1);

        // fallback constant -> gate -> input -> merger; a cable closes the gate.
        const makeCvInput = (channel, initial) => {
          const fallback = ctx.createConstantSource();
          fallback.offset.value = initial;
          fallback.start();
          const gate = ctx.createGain();
          const input = ctx.createGain();
          fallback.connect(gate);
          gate.connect(input);
          input.connect(inputMerger, 0, channel);
          return { fallback, gate, input };
        };
        const fmIndexIn = makeCvInput(2, params.fmIndex ?? 0);
        const feedbackIn = makeCvInput(3, params.feedback ?? 0);
        const superSawDetuneIn = makeCvInput(4, params.superSawDetune ?? CV_MAX_VOLTS / 2);
        const superSawMixIn = makeCvInput(5, params.superSawMix ?? CV_MAX_VOLTS / 2);

        const ampGain = ctx.createGain();
        ampGain.gain.value = 0;
        processor.connect(ampGain);
        const ampCv = this._createCvGainChain(ctx, "linear");
        ampCv.waveShaper.connect(ampGain.gain);

        const outGain = ctx.createGain();
        outGain.gain.value = ((params.gain ?? CV_MAX_VOLTS) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS;
        // A "gain" cable replaces outGain's own value (zeroed on connect), so
        // the CV path carries the same AUDIO_PEAK_VOLTS rescale.
        const gainCv = this._createCvGainChain(ctx, "linear", AUDIO_PEAK_VOLTS);
        gainCv.waveShaper.connect(outGain.gain);
        ampGain.connect(outGain);

        const pitchSum = ctx.createGain();
        pitchSum.gain.value = 1;
        const pitchRestConst = ctx.createConstantSource();
        pitchRestConst.offset.value = CV_MAX_VOLTS / 2;
        pitchRestConst.start();
        const pitchRestGate = ctx.createGain();
        pitchRestGate.gain.value = 0; // see "oscillator" case above for why
        pitchRestConst.connect(pitchRestGate);
        pitchRestGate.connect(pitchSum);

        const tuneSum = ctx.createGain();
        tuneSum.gain.value = 1;
        const tuneRestConst = ctx.createConstantSource();
        tuneRestConst.offset.value = params.tuneVolts ?? CV_MAX_VOLTS / 2;
        tuneRestConst.start();
        const tuneRestGate = ctx.createGain();
        tuneRestGate.gain.value = 0;
        tuneRestConst.connect(tuneRestGate);
        tuneRestGate.connect(tuneSum);

        const pitchPreScale = ctx.createGain();
        pitchPreScale.gain.value = 0.1;
        pitchSum.connect(pitchPreScale);
        tuneSum.connect(pitchPreScale);
        const pitchFinalShift = ctx.createConstantSource();
        pitchFinalShift.offset.value = -1;
        pitchFinalShift.start();
        const pitchShaperInput = ctx.createGain();
        pitchShaperInput.gain.value = 1;
        pitchPreScale.connect(pitchShaperInput);
        pitchFinalShift.connect(pitchShaperInput);
        const pitchWaveShaper = ctx.createWaveShaper();
        pitchWaveShaper.curve = buildPitchCvCurve();
        pitchShaperInput.connect(pitchWaveShaper);
        pitchWaveShaper.connect(freqSum);

        const entry = {
          type,
          inputMerger,
          processor,
          freqConst,
          directHz: params.frequency ?? 0,
          wavetables: Array.isArray(params.wavetables) ? params.wavetables.map((t) => Float32Array.from(t)) : null,
          morph: params.morph ?? 0,
          fmInScale,
          fmIndexIn,
          feedbackIn,
          superSawDetuneIn,
          superSawMixIn,
          ampGain,
          ampCv,
          waveType,
          outGain,
          gainCv,
          gain: params.gain ?? CV_MAX_VOLTS,
          gainCvActive: false,
          pitchSum,
          pitchRestConst,
          pitchRestGate,
          tuneSum,
          tuneRestConst,
          tuneRestGate,
          pitchWaveShaper,
          pitchCvActive: false,
          tuneCvActive: false,
          fmIndex: params.fmIndex ?? 0,
          fmIndexCvActive: false,
          feedback: params.feedback ?? 0,
          feedbackCvActive: false,
          superSawDetune: params.superSawDetune ?? CV_MAX_VOLTS / 2,
          superSawDetuneCvActive: false,
          superSawMix: params.superSawMix ?? CV_MAX_VOLTS / 2,
          superSawMixCvActive: false,
          dutyCycle: params.dutyCycle ?? CV_MAX_VOLTS / 2,
          extraNodes: [freqSum, fmInScale, pitchSum, pitchRestGate, tuneSum, tuneRestGate, pitchPreScale,
            pitchShaperInput, pitchWaveShaper,
            ...[fmIndexIn, feedbackIn, superSawDetuneIn, superSawMixIn].flatMap((cv) => [cv.gate, cv.input])],
          extraSources: [freqConst, pitchRestConst, tuneRestConst, pitchFinalShift,
            fmIndexIn.fallback, feedbackIn.fallback, superSawDetuneIn.fallback, superSawMixIn.fallback],
        };
        entry.core = createFmOscillatorCore(entry, ctx.sampleRate);
        processor.onaudioprocess = (event) => {
          const inputs = [];
          for (let c = 0; c < 6; c += 1) inputs.push(event.inputBuffer.getChannelData(c));
          entry.core.render(inputs, event.outputBuffer.getChannelData(0));
        };
        return entry;
      }
      case "hammond": {
        // Nine drawbar oscillators sharing the SAME pitch-CV chain as
        // "oscillator" above (copied verbatim: pitchSum/pitchRestConst/
        // pitchRestGate/tuneSum/tuneRestConst/tuneRestGate/pitchPreScale/
        // pitchFinalShift/pitchShaperInput/pitchWaveShaper), except
        // pitchWaveShaper's Hz-carrying output fans out through nine
        // per-drawbar ratio-scaling gains instead of connecting to one
        // oscillator directly - see nodes/HammondNode.h for why each
        // drawbar needs its own ratio (independent per-drawbar phase, no
        // shared sub-octave accumulator, arbitrary user-editable ratios).
        const RATIOS =
          Array.isArray(params.ratios) && params.ratios.length === 9
            ? params.ratios
            : [0.5, 1.5, 1, 2, 3, 4, 5, 6, 8];
        // kDrawbarNorm is AUDIO_PEAK_VOLTS / 9; the AUDIO_PEAK_VOLTS part is
        // already applied by outGain (same as the oscillator), so only 1/9 here.
        const NORM = 1 / 9;

        const ampGain = ctx.createGain();
        ampGain.gain.value = 0;
        const ampCv = this._createCvGainChain(ctx, "linear");
        ampCv.waveShaper.connect(ampGain.gain);

        const outGain = ctx.createGain();
        outGain.gain.value = ((params.gain ?? CV_MAX_VOLTS) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS;
        // A "gain" cable replaces outGain's own value (zeroed on connect), so
        // the CV path carries the same AUDIO_PEAK_VOLTS rescale.
        const gainCv = this._createCvGainChain(ctx, "linear", AUDIO_PEAK_VOLTS);
        gainCv.waveShaper.connect(outGain.gain);

        const norm = ctx.createGain();
        norm.gain.value = NORM;
        norm.connect(ampGain);
        ampGain.connect(outGain);

        // Pitch chain - identical to the "oscillator" case above.
        const pitchSum = ctx.createGain();
        pitchSum.gain.value = 1;
        const pitchRestConst = ctx.createConstantSource();
        pitchRestConst.offset.value = CV_MAX_VOLTS / 2;
        pitchRestConst.start();
        const pitchRestGate = ctx.createGain();
        pitchRestGate.gain.value = 0;
        pitchRestConst.connect(pitchRestGate);
        pitchRestGate.connect(pitchSum);

        const tuneSum = ctx.createGain();
        tuneSum.gain.value = 1;
        const tuneRestConst = ctx.createConstantSource();
        tuneRestConst.offset.value = params.tuneVolts ?? CV_MAX_VOLTS / 2;
        tuneRestConst.start();
        const tuneRestGate = ctx.createGain();
        tuneRestGate.gain.value = 0;
        tuneRestConst.connect(tuneRestGate);
        tuneRestGate.connect(tuneSum);

        const pitchPreScale = ctx.createGain();
        pitchPreScale.gain.value = 0.1;
        pitchSum.connect(pitchPreScale);
        tuneSum.connect(pitchPreScale);
        const pitchFinalShift = ctx.createConstantSource();
        pitchFinalShift.offset.value = -1;
        pitchFinalShift.start();
        const pitchShaperInput = ctx.createGain();
        pitchShaperInput.gain.value = 1;
        pitchPreScale.connect(pitchShaperInput);
        pitchFinalShift.connect(pitchShaperInput);
        const pitchWaveShaper = ctx.createWaveShaper();
        pitchWaveShaper.curve = buildPitchCvCurve();
        pitchShaperInput.connect(pitchWaveShaper);
        // NOTE: does not connect to a single osc.frequency here - fans out
        // through nine per-drawbar ratio gains below instead.

        const oscs = [];
        const footageScale = [];
        const drawbarGains = [];
        const drawbarCv = [];
        const drawbars = [];
        for (let k = 0; k < 9; k += 1) {
          const wheel = ctx.createOscillator();
          wheel.type = "sine";
          wheel.frequency.value = 0; // driven by _setParamOnEntry's "frequency" case below, or the CV chain once connected

          const fs = ctx.createGain();
          fs.gain.value = RATIOS[k];
          pitchWaveShaper.connect(fs);
          fs.connect(wheel.frequency);

          const g = ctx.createGain();
          const v = (params.drawbars && params.drawbars[k]) ?? (k < 3 ? CV_MAX_VOLTS : 0);
          g.gain.value = v / CV_MAX_VOLTS; // linear vcaGain, matches cv::vcaGain(v, false)
          const cvChain = this._createCvGainChain(ctx, "linear");
          cvChain.waveShaper.connect(g.gain);

          wheel.connect(g);
          g.connect(norm);
          wheel.start();

          oscs.push(wheel);
          footageScale.push(fs);
          drawbarGains.push(g);
          drawbarCv.push(cvChain);
          drawbars.push(v);
        }

        return {
          type,
          oscs,
          directHz: 0,
          extraNodes: [pitchSum, pitchRestGate, tuneSum, tuneRestGate, pitchPreScale, pitchShaperInput, pitchWaveShaper],
          extraSources: [pitchRestConst, tuneRestConst, pitchFinalShift],
          footageScale,
          drawbarGains,
          drawbarCv,
          drawbars,
          drawbarCvActive: new Array(9).fill(false),
          ratios: [...RATIOS],
          norm,
          ampGain,
          ampCv,
          outGain,
          gainCv,
          gain: params.gain ?? CV_MAX_VOLTS,
          gainCvActive: false,
          pitchSum,
          pitchRestConst,
          pitchRestGate,
          tuneSum,
          tuneRestConst,
          tuneRestGate,
          pitchWaveShaper,
          pitchCvActive: false,
          tuneCvActive: false,
        };
      }
      case "lfo":
      case "wavetable": {
        // "wavetable" is its own widget but reuses this LFO chain completely
        // (rate, gain, one-shot hold, global mirroring, reset) - only the raw
        // -1..+1 source is the wavetable worklet instead of lfoOsc (see
        // _setLfoWavetable). Its entry keeps type "wavetable".
        
        
        
        
        
        
        
        
        
        
        const amplitude = clampRange(Number(params.gain ?? CV_MAX_VOLTS), 0, CV_MAX_VOLTS);
        const halfSwing = amplitude / 2;
        const center = params.zeroPointFixed ? params.zeroPoint ?? 5 : halfSwing;
        const maxRate = clampRange(Number(params.maxRate ?? 50) || 0, 0, LFO_MAX_RATE_HZ);
        const rate = clampRange(Number(params.rate ?? 2) || 0, 0, maxRate);
        const phase = params.phase ?? 0;
        const waveType = params.waveType || "sine";
        const oneShot = params.oneShot === true;
        const wavetable = type === "wavetable";
        const wtTables = Array.isArray(params.wavetables) && params.wavetables.length > 0
          ? params.wavetables.map((t) => Float32Array.from(t))
          : createDefaultBank();
        const wtMorph = clampRange(Number(params.morph ?? 0) || 0, 0, CV_MAX_VOLTS);
        const lfoOsc = ctx.createOscillator();
        setOscWaveType(ctx, lfoOsc, waveType, phase / 360);
        lfoOsc.frequency.value = rate;
        const lfoGain = ctx.createGain();
        lfoGain.gain.value = oneShot ? 0 : halfSwing;
        const centerOffset = ctx.createConstantSource();
        centerOffset.offset.value = oneShot
          ? center + (wavetable
            ? sampleBank(wtTables, phase / 360, wtMorph / 10)
            : lfoWaveValueAtPhase(waveType, phase)) * halfSwing
          : center;
        centerOffset.start();
        
        
        
        
        
        
        
        
        
        // lfoSum -> clamp -> rawOut: LfoNode.h clamps every output sample to
        // 0..CV_MAX_VOLTS (a fixed zero point plus a large amplitude would
        // otherwise swing past the CV window).
        const lfoSum = ctx.createGain();
        lfoSum.gain.value = 1;
        lfoOsc.connect(lfoGain);
        lfoGain.connect(lfoSum);
        centerOffset.connect(lfoSum);
        const clamp = this._createRangeGainChain(ctx, 0, CV_MAX_VOLTS, buildRangeCurve(0, CV_MAX_VOLTS, "linear"));
        lfoSum.connect(clamp.prescale);
        const rawOut = ctx.createGain();
        rawOut.gain.value = 1;
        clamp.waveShaper.connect(rawOut);
        const ownGate = ctx.createGain();
        ownGate.gain.value = 1;
        const sharedGate = ctx.createGain();
        sharedGate.gain.value = 0;
        rawOut.connect(ownGate);
        const output = ctx.createGain();
        output.gain.value = 1;
        ownGate.connect(output);
        sharedGate.connect(output);
        
        
        
        
        
        
        
        // The phase offset lives in the PeriodicWave itself (see
        // getFirmwareWave), so a plain start is already phase-exact.
        lfoOsc.start();
        const lfoEntry = {
          type,
          lfoOsc,
          lfoGain,
          centerOffset,
          extraNodes: [lfoSum, clamp.prescale, clamp.sumNode, clamp.waveShaper],
          extraSources: [clamp.shiftConst],
          rawOut,
          ownGate,
          sharedGate,
          output,
          _sharedWired: false,
          lfoWaveType: waveType,
          lfoRate: rate,
          lfoLinearPos0: 0,
          lfoLinearT0: 0,
          lfoLinearRate: rate,
          lfoLinearRunning: !oneShot,
          lfoAmplitude: amplitude,
          lfoMaxRate: maxRate,
          lfoPhase: phase,
          lfoZeroPointFixed: params.zeroPointFixed === true,
          lfoZeroPoint: params.zeroPoint ?? 5,
          lfoOneShot: oneShot,
          lfoOneShotRunning: false,
          lfoOneShotTimer: null,
          // Wavetable widget (see _setLfoWavetable): wtSource replaces lfoOsc
          // as the raw signal feeding lfoGain.
          lfoWavetable: false,
          wtTables,
          wtMorph,
          wtSource: null,
          wtSink: null,
          wtCore: null,
          wtPending: false,
        };
        if (wavetable) this._setLfoWavetable(lfoEntry, true);
        if (lfoLinearHold(lfoEntry)) {
          // One-shot starts held at the ramp start until the first reset.
          lfoEntry.lfoLinearRunning = false;
          applyLfoLinear(ctx, lfoEntry, false);
        }
        return lfoEntry;
      }
      case "const":
      case "up-down-button": {
        const constant = ctx.createConstantSource();
        constant.offset.value = params.value ?? 0;
        constant.start();
        return { type, constant };
      }
      case "button": {
        const constants = [];
        for (let i = 0; i < 128; i += 1) {
          const constant = ctx.createConstantSource();
          constant.offset.value = i === 0 ? (params.value ?? 0) : 0;
          constant.start();
          constants.push(constant);
        }
        return { type, constant: constants[0], constants };
      }
      case "knob": {
        // One ConstantSourceNode per knob/layer output (16 knobs x 8 layers).
        // Mixer elsewhere in this file - unused ones just stay silent at 0),
        // instead of the single shared node const/button/up-down-button use
        // above - a multi-knob widget's "voltageN" outputs (see main.js's
        // knobPortId) need to be independently wireable into an audio-rate
        // destination (e.g. a Multiplexer input), not all collapse onto one
        // node. `constant` stays an alias for control 0, for any code still
        // reading it directly.
        const constants = [];
        for (let i = 0; i < 128; i += 1) {
          const c = ctx.createConstantSource();
          c.offset.value = i === 0 ? (params.value ?? 0) : 0;
          c.start();
          constants.push(c);
        }
        return { type, constant: constants[0], constants };
      }
      case "switch": {
        
        
        
        const gain1 = ctx.createGain();
        const gain2 = ctx.createGain();
        gain1.gain.value = params.selectIn2 ? 0 : 1;
        gain2.gain.value = params.selectIn2 ? 1 : 0;
        const output = ctx.createGain();
        output.gain.value = 1;
        gain1.connect(output);
        gain2.connect(output);
        return { type, gain1, gain2, output, extraNodes: [gain1, gain2] };
      }
      case "output-switch": {
        // One input, two outputs - mirrors OutputSwitchNode.h: the selected
        // output follows the input, the other one HOLDS the last value it
        // carried (held1/held2), it does not drop to 0V.
        const input = ctx.createGain();
        const processor = ctx.createScriptProcessor(256, 1, 2);
        input.connect(processor);
        const splitter = ctx.createChannelSplitter(2);
        processor.connect(splitter);
        const gain1 = ctx.createGain();
        const gain2 = ctx.createGain();
        splitter.connect(gain1, 0);
        splitter.connect(gain2, 1);
        const entry = {
          type,
          input,
          processor,
          splitter,
          gain1,
          gain2,
          extraNodes: [gain1, gain2],
          selectOut2: params.selectOut2 === true,
          held1: Number(params.held1) || 0,
          held2: Number(params.held2) || 0,
        };
        processor.onaudioprocess = (event) => {
          const src = event.inputBuffer.getChannelData(0);
          const out1 = event.outputBuffer.getChannelData(0);
          const out2 = event.outputBuffer.getChannelData(1);
          if (entry.selectOut2) {
            out2.set(src);
            out1.fill(entry.held1);
            entry.held2 = src[src.length - 1];
          } else {
            out1.set(src);
            out2.fill(entry.held2);
            entry.held1 = src[src.length - 1];
          }
        };
        return entry;
      }
      case "audio-out":
      case "usb-out":
      case "es8311-out":
      case "wm8960-out": {
        const leftGain = ctx.createGain();
        const rightGain = ctx.createGain();
        // leftGain/rightGain double as the AUDIO_PEAK_VOLTS -> native +/-1.0
        // rescale point (mirrors OutputNodes.h's "* cv::kInvAudioPeakVolts"
        // right before its SoftLimiter) - folded into the same trim multiply
        // instead of a separate node, since GainNode.gain already multiplies
        // whatever's connected either way.
        leftGain.gain.value = (Number(params.left ?? 1) || 0) / AUDIO_PEAK_VOLTS;
        rightGain.gain.value = (Number(params.right ?? 1) || 0) / AUDIO_PEAK_VOLTS;
        const merger = ctx.createChannelMerger(2);
        const outputGain = ctx.createGain();
        outputGain.gain.value = Math.max(0, Math.min(1, (params.gain ?? 10) / 10));
        const muteGain = ctx.createGain();
        muteGain.gain.value = params.muted ? 0 : 1;

        
        
        
        
        
        const limiter = ctx.createDynamicsCompressor();
        limiter.threshold.value = -1;
        limiter.knee.value = 0;
        limiter.ratio.value = 20;
        limiter.attack.value = 0.003;
        limiter.release.value = 0.25;

        leftGain.connect(merger, 0, 0);
        rightGain.connect(merger, 0, 1);
        merger.connect(limiter);
        limiter.connect(outputGain);
        outputGain.connect(muteGain);
        if (type === "audio-out") muteGain.connect(this._masterOutputGain);

        
        
        
        
        const leftTap = this._createAnalyser(ctx, 256);
        leftGain.connect(leftTap.analyser);
        const rightTap = this._createAnalyser(ctx, 256);
        rightGain.connect(rightTap.analyser);

        return {
          type,
          leftGain,
          rightGain,
          merger,
          limiter,
          outputGain,
          muteGain,
          leftAnalyser: leftTap.analyser,
          leftBuffer: leftTap.buffer,
          rightAnalyser: rightTap.analyser,
          rightBuffer: rightTap.buffer,
        };
      }
      case "gain": {
        // "amount" (Output Gain, always linear) -> gainNode -> "curve" (2nd
        // independent VCA stage, 5V = unity/0..2x) -> curveGainNode -> out.
        const gainNode = ctx.createGain();
        const curveGainNode = ctx.createGain();
        gainNode.connect(curveGainNode);

        const cv = this._createCvGainChain(ctx, "linear");
        const ownGate = ctx.createGain();
        ownGate.gain.value = 1;
        const sharedGate = ctx.createGain();
        sharedGate.gain.value = 0;
        cv.waveShaper.connect(ownGate);
        ownGate.connect(gainNode.gain);
        sharedGate.connect(gainNode.gain);

        const curveCv = this._createCvGainChain(ctx, "gainCurve");
        const curveOwnGate = ctx.createGain();
        curveOwnGate.gain.value = 1;
        const curveSharedGate = ctx.createGain();
        curveSharedGate.gain.value = 0;
        curveCv.waveShaper.connect(curveOwnGate);
        curveOwnGate.connect(curveGainNode.gain);
        curveSharedGate.connect(curveGainNode.gain);

        const entry = {
          type,
          gainNode,
          curveGainNode,
          amount: params.amount ?? CV_MAX_VOLTS,
          curve: typeof params.curve === "number" ? params.curve : CV_MAX_VOLTS / 2,
          cvActive: false,
          curveCvActive: false,
          globalFollower: false,
          _sharedWired: false,
          ownGate,
          sharedGate,
          prescale: cv.prescale,
          shiftConst: cv.shiftConst,
          sumNode: cv.sumNode,
          waveShaper: cv.waveShaper,
          curveOwnGate,
          curveSharedGate,
          curveCv,
        };
        gainNode.gain.value = vcaAmountFromCv(entry.amount, "linear");
        curveGainNode.gain.value = vcaAmountFromCv(entry.curve, "gainCurve");
        return entry;
      }
      case "linked-input":
      case "linked-output":
      case "linked-input-4":
      case "linked-output-4":
      case "linked-input-8":
      case "linked-output-8": {
        // Transparent, fixed-unity passthrough - the actual fan-out wiring
        // between matching linkId/sourceLinkId pairs happens in
        // _wireLinkedIO(), not here (see createNode()/_removeEntry()).
        const linkCount = params.linkCount || (type.endsWith("-8") ? 8 : (type.endsWith("-4") ? 4 : 1));
        const gainNodes = Array.from({ length: linkCount }, () => {
          const gainNode = ctx.createGain();
          gainNode.gain.value = 1;
          return gainNode;
        });
        return {
          type,
          gainNode: gainNodes[0],
          gainNodes,
          linkId: params.linkId,
          linkName: params.linkName,
          sourceLinkId: params.sourceLinkId,
        };
      }
      case "comb-filter": {
        const filterInput = ctx.createGain();
        const inputMerger = ctx.createChannelMerger(7);
        filterInput.connect(inputMerger, 0, 0);
        const processor = ctx.createScriptProcessor(256, 7, 1);
        inputMerger.connect(processor);
        const filterOutput = ctx.createGain();
        processor.connect(filterOutput);
        const entry = { type, filterInput, inputMerger, processor, filterOutput,
          seeds: [], controlConnectionCounts: Array(6).fill(0) };
        COMB_FILTER_PORTS.forEach((port, index) => {
          entry[port] = combFilterCv(params[port], COMB_FILTER_DEFAULTS[port]);
          const seed = ctx.createConstantSource();
          seed.offset.value = entry[port];
          seed.connect(inputMerger, 0, index + 1);
          seed.start();
          entry.seeds.push(seed);
        });
        entry.dsp = createCombFilterDsp(entry, ctx.sampleRate);
        processor.onaudioprocess = (event) => {
          const inputs = Array.from({ length: 7 }, (_, i) => event.inputBuffer.getChannelData(i));
          entry.dsp.render(inputs, event.outputBuffer.getChannelData(0));
        };
        entry.spectrumAnalyser = ctx.createAnalyser();
        entry.spectrumAnalyser.fftSize = 2048;
        entry.spectrumBuffer = new Uint8Array(entry.spectrumAnalyser.frequencyBinCount);
        filterOutput.connect(entry.spectrumAnalyser);
        const maxHz = Math.min(20000, ctx.sampleRate / 2 - 1);
        entry.responseFreqs = Float32Array.from({ length: 1024 }, (_, i) => 20 * (maxHz / 20) ** (i / 1023));
        entry.responseMag = new Float32Array(entry.responseFreqs.length);
        return entry;
      }
      case "filter":
      case "curtis-filter": {
        // filterInput -> merger ch0, cutoff CV -> ch1, resonance CV -> ch2 ->
        // ScriptProcessor running createFilterDsp()/createCurtisFilterDsp()
        // -> filterOutput. The CV chains now pass raw volts (clamped 0-10V,
        // same as cvToRange() does on hardware); the processor maps them.
        const isCurtis = type === "curtis-filter";
        const filterInput = ctx.createGain();
        const inputMerger = ctx.createChannelMerger(3);
        filterInput.connect(inputMerger, 0, 0);
        const cutoffCvSum = ctx.createGain();
        const resonanceCvSum = ctx.createGain();
        cutoffCvSum.connect(inputMerger, 0, 1);
        resonanceCvSum.connect(inputMerger, 0, 2);
        const processor = ctx.createScriptProcessor(256, 3, 1);
        inputMerger.connect(processor);
        const filterOutput = ctx.createGain();
        processor.connect(filterOutput);
        // curtisLegacyFilterKind()/curtisLegacySlopeDb() (NodeTypes.h): the
        // CEM3320 filter once stored type+slope as "lp24"/"lp12"/"bp12"/"hp12".
        const legacySlope = isCurtis ? { lp24: 24, lp12: 12, bp12: 12, hp12: 12 }[params.filterType] : undefined;
        const legacyKind = isCurtis ? { lp24: "lowpass", lp12: "lowpass", bp12: "bandpass", hp12: "highpass" }[params.filterType] : undefined;
        const entry = {
          type,
          filterInput,
          inputMerger,
          processor,
          filterOutput,
          cutoffCvSum,
          resonanceCvSum,
          filterType: params.bypass === true ? "none" : normalizeFilterKind(legacyKind || params.filterType),
          cutoff: params.cutoff ?? 5,
          cutoffMinHz: params.cutoffMinHz ?? 20,
          cutoffMaxHz: params.cutoffMaxHz ?? 20000,
          cutoffCurve: params.cutoffCurve ?? "log",
          resonance: params.resonance ?? 5,
          resonanceMinQ: params.resonanceMinQ ?? 0.1,
          resonanceMaxQ: params.resonanceMaxQ ?? 20,
          resonanceCurve: params.resonanceCurve ?? "log",
          slope: filterPolesFromSlope(legacySlope ?? params.slope ?? (isCurtis ? 24 : 12)) * 6,
          coeffStride: clampRange(Math.trunc(Number(params.coeffStride ?? 4)) || 4, 1, 16),
          coeffsDirty: true,
          cutoffCvActive: false,
          resonanceCvActive: false,
          globalPrimary: null,
        };
        entry.dsp = isCurtis ? createCurtisFilterDsp(entry, ctx.sampleRate) : createFilterDsp(entry, ctx.sampleRate);
        processor.onaudioprocess = (event) => {
          const src = entry.globalPrimary || entry;
          entry.dsp.render(
            event.inputBuffer.getChannelData(0),
            event.inputBuffer.getChannelData(1),
            event.inputBuffer.getChannelData(2),
            event.outputBuffer.getChannelData(0),
            src.cutoffCvActive,
            src.resonanceCvActive,
          );
        };
        entry.cutoffCv = this._createCvGainChain(ctx, "linear", CV_MAX_VOLTS);
        entry.resonanceCv = this._createCvGainChain(ctx, "linear", CV_MAX_VOLTS);
        // Own/shared gates: in Global mode every voice follows voice 0's CV
        // (see _setGlobalMode), like the firmware's `src = *inst.primary`.
        entry.cutoffOwnGate = ctx.createGain();
        entry.cutoffOwnGate.gain.value = 1;
        entry.cutoffSharedGate = ctx.createGain();
        entry.cutoffSharedGate.gain.value = 0;
        entry.cutoffCv.waveShaper.connect(entry.cutoffOwnGate);
        entry.cutoffOwnGate.connect(cutoffCvSum);
        entry.cutoffSharedGate.connect(cutoffCvSum);
        entry.resonanceOwnGate = ctx.createGain();
        entry.resonanceOwnGate.gain.value = 1;
        entry.resonanceSharedGate = ctx.createGain();
        entry.resonanceSharedGate.gain.value = 0;
        entry.resonanceCv.waveShaper.connect(entry.resonanceOwnGate);
        entry.resonanceOwnGate.connect(resonanceCvSum);
        entry.resonanceSharedGate.connect(resonanceCvSum);
        entry._sharedWired = false;
        entry.extraNodes = [cutoffCvSum, resonanceCvSum];

        // Inline spectrum + filter-curve preview (32 bands is plenty for a
        // small widget panel - same idea as "esp-eq" below).
        entry.spectrumAnalyser = ctx.createAnalyser();
        entry.spectrumAnalyser.fftSize = 64;
        entry.spectrumBuffer = new Uint8Array(entry.spectrumAnalyser.frequencyBinCount);
        filterOutput.connect(entry.spectrumAnalyser);
        entry.responseFreqs = new Float32Array(entry.spectrumAnalyser.frequencyBinCount);
        for (let i = 0; i < entry.responseFreqs.length; i += 1) {
          entry.responseFreqs[i] = (i * ctx.sampleRate) / 2 / entry.responseFreqs.length;
        }
        entry.responseMag = new Float32Array(entry.responseFreqs.length);
        entry.responsePhase = new Float32Array(entry.responseFreqs.length);

        return entry;
      }
      case "esp-eq": {
        // One biquad per channel, parameters clamped like espEqMakeFilterPara()
        // (EspEqEffect.h). inputX -> filterX -> processedX -> outputX, plus an
        // inputX -> bypassX -> outputX passthrough for "bypass".
        const bypass = params.bypass === true;
        const make = () => {
          const input = ctx.createGain();
          const filter = ctx.createBiquadFilter();
          const processed = ctx.createGain();
          const bypassGain = ctx.createGain();
          const output = ctx.createGain();
          processed.gain.value = bypass ? 0 : 1;
          bypassGain.gain.value = bypass ? 1 : 0;
          input.connect(filter);
          filter.connect(processed);
          processed.connect(output);
          input.connect(bypassGain);
          bypassGain.connect(output);
          return { input, filter, processed, bypassGain, output };
        };
        const chanL = make();
        const chanR = make();
        const entry = {
          type,
          inputL: chanL.input,
          filterL: chanL.filter,
          processedL: chanL.processed,
          bypassL: chanL.bypassGain,
          outputL: chanL.output,
          inputR: chanR.input,
          filterR: chanR.filter,
          processedR: chanR.processed,
          bypassR: chanR.bypassGain,
          outputR: chanR.output,
          bypass,
          filterType: params.filterType || "peak",
          frequency: params.frequency ?? 1000,
          q: params.q ?? 1,
          gainDb: params.gainDb ?? 0,
        };
        this._applyEspEqParams(entry, 0);
        const spectrumAnalyser = ctx.createAnalyser();
        spectrumAnalyser.fftSize = 256;
        const binCount = spectrumAnalyser.frequencyBinCount;
        chanL.filter.connect(spectrumAnalyser);
        const responseFreqs = new Float32Array(binCount);
        for (let i = 0; i < binCount; i += 1) {
          responseFreqs[i] = (i * ctx.sampleRate) / 2 / binCount;
        }
        return Object.assign(entry, {
          spectrumAnalyser,
          spectrumBuffer: new Uint8Array(binCount),
          responseFreqs,
          responseMag: new Float32Array(binCount),
          responsePhase: new Float32Array(binCount),
          extraNodes: [chanL.filter, chanR.filter],
        });
      }
      case "graphic-eq": {
        // 8 fixed-Q peaking bands cascaded in series per channel - mirrors
        // the firmware's single esp_ae_eq handle with filter_num=8
        // (GraphicEqEffect.h). Frequency is settings-dialog-only (not CV);
        // each band's gain IS CV-controllable (see EFFECT_CV_PARAMS in
        // main.js: 0-10V, 5V = 0 dB center, mapped to dB before it reaches
        // setParam - same convention as esp-eq's own "gainDb").
        const bandCount = 8;
        const freqs = validGraphicEqFrequencies(params.frequencies)
          ? params.frequencies.map(Number)
          : [...GRAPHIC_EQ_DEFAULT_FREQS];
        const gains = Array.from({ length: bandCount }, (_, i) => Number(params[`gain${i + 1}`]) || 0);
        // graphicEqMakeFilterPara() clamps: fc 20..Nyquist-1, gain +/-15 dB.
        const makeBands = () => freqs.map((hz, i) => {
          const f = ctx.createBiquadFilter();
          f.type = "peaking";
          f.frequency.value = clampRange(hz, 20, ctx.sampleRate / 2 - 1);
          f.Q.value = 1.41;
          f.gain.value = clampRange(gains[i], -15, 15);
          return f;
        });
        const bandsL = makeBands();
        const bandsR = makeBands();
        for (let i = 1; i < bandCount; i += 1) {
          bandsL[i - 1].connect(bandsL[i]);
          bandsR[i - 1].connect(bandsR[i]);
        }
        const outputGainL = ctx.createGain();
        const outputGainR = ctx.createGain();
        outputGainL.gain.value = clampRange((Number(params.gain ?? CV_MAX_VOLTS) || 0) / CV_MAX_VOLTS, 0, 1);
        outputGainR.gain.value = outputGainL.gain.value;
        bandsL[bandCount - 1].connect(outputGainL);
        bandsR[bandCount - 1].connect(outputGainR);
        // Bypass is a full passthrough in processGraphicEq() - it skips the
        // output gain as well, not just the bands.
        const bypass = params.bypass === true;
        const makeRouting = (firstBand, outputGain) => {
          const input = ctx.createGain();
          const bypassGain = ctx.createGain();
          const processed = ctx.createGain();
          const output = ctx.createGain();
          processed.gain.value = bypass ? 0 : 1;
          bypassGain.gain.value = bypass ? 1 : 0;
          input.connect(firstBand);
          outputGain.connect(processed);
          processed.connect(output);
          input.connect(bypassGain);
          bypassGain.connect(output);
          return { input, bypassGain, processed, output };
        };
        const routeL = makeRouting(bandsL[0], outputGainL);
        const routeR = makeRouting(bandsR[0], outputGainR);

        const geqSpectrumAnalyser = ctx.createAnalyser();
        geqSpectrumAnalyser.fftSize = 1024;
        const geqBinCount = geqSpectrumAnalyser.frequencyBinCount;
        const geqSpectrumBuffer = new Uint8Array(geqBinCount);
        routeL.output.connect(geqSpectrumAnalyser);
        const geqResponseCount = 128;
        const geqResponseFreqs = new Float32Array(geqResponseCount);
        const geqMinHz = 20;
        const geqMaxHz = Math.min(ctx.sampleRate / 2 - 1, 20000);
        for (let i = 0; i < geqResponseCount; i += 1) {
          geqResponseFreqs[i] = geqMinHz * Math.pow(geqMaxHz / geqMinHz, i / (geqResponseCount - 1));
        }

        return {
          type,
          bandsL,
          bandsR,
          inputL: routeL.input,
          inputR: routeR.input,
          outputL: routeL.output,
          outputR: routeR.output,
          processedL: routeL.processed,
          processedR: routeR.processed,
          bypassL: routeL.bypassGain,
          bypassR: routeR.bypassGain,
          outputGainL,
          outputGainR,
          gainDb: gains,
          freqHz: freqs,
          bypass,
          spectrumAnalyser: geqSpectrumAnalyser,
          spectrumBuffer: geqSpectrumBuffer,
          responseFreqs: geqResponseFreqs,
          responseMag: new Float32Array(geqResponseCount),
          responsePhase: new Float32Array(geqResponseCount),
          bandMagScratch: new Float32Array(geqResponseCount),
        };
      }
      case "mixer": {
        
        
        
        const sum = ctx.createGain();
        sum.gain.value = params.outputGain ?? 1;
        const inputGains = new Map();
        for (let i = 1; i <= 8; i += 1) {
          const g = ctx.createGain();
          g.gain.value = 1;
          g.connect(sum);
          inputGains.set(`in${i}`, g);
        }
        return { type, sum, inputGains };
      }
      case "multiplexer": {
        // N (fixed 16, like Mixer above - unused ones just stay silent)
        // hard-gated input gains summed into one output - only the gain
        // matching the currently active index is ever 1, the rest 0 (same
        // hard-switch idea as "switch" above, generalized from 2 to 16 ways).
        // See _setParam's "multiplexer"/"select" case for how the gate moves.
        const output = ctx.createGain();
        output.gain.value = 1;
        const inputGains = new Map();
        const selected = Math.max(0, Math.min(15, Math.round(params.select ?? 0)));
        for (let i = 1; i <= 16; i += 1) {
          const g = ctx.createGain();
          g.gain.value = i - 1 === selected ? 1 : 0;
          g.connect(output);
          inputGains.set(`in${i}`, g);
        }
        return {
          type,
          output,
          inputGains,
          selected,
          softFadeMs: Math.max(0, Math.min(100, Number(params.softFadeMs ?? 5))),
        };
      }
      case "output-multiplexer": {
        // Inverted topology from "multiplexer" above: one shared input fans
        // out to 16 pre-created output gains, only the selected one gated to
        // 1 - no fade (see NodeState.h's OutputMultiplexerState comment,
        // switching is instant by design).
        const input = ctx.createGain();
        input.gain.value = 1;
        const outputGains = new Map();
        const selected = Math.max(0, Math.min(15, Math.round(params.select ?? 0)));
        for (let i = 1; i <= 16; i += 1) {
          const g = ctx.createGain();
          g.gain.value = i - 1 === selected ? 1 : 0;
          input.connect(g);
          outputGains.set(`out${i}`, g);
        }
        return { type, input, outputGains, selected };
      }
      case "multiplier": {






        const inputCount = Math.max(2, Math.min(8, params.inputCount ?? 2));
        const values = params.values || [];
        const STAGE_COUNT = inputCount - 1;
        // Neutral element (1x) - also this node's only live-updatable
        // fallback for "in1" (see setParam's "multiplier" case): a Const
        // wired into in1 while another input is genuinely audio-rate must
        // still reach the real sum, not just this fixed default.
        const seed = ctx.createConstantSource();
        seed.offset.value = values[0] ?? 1;
        seed.start();

        const stages = [];
        let prevOutput = seed;
        for (let i = 0; i < STAGE_COUNT; i += 1) {
          const node = ctx.createGain();
          node.gain.value = values[i + 1] ?? 1;
          prevOutput.connect(node);
          stages.push({ node });
          prevOutput = node;
        }
        return { type, seed, stages, inputCount, controlConnectionCounts: new Array(inputCount).fill(0), output: prevOutput };
      }
      case "adder": {
        // One ConstantSourceNode per input, always summed into `sum`
        // (WebAudio auto-mixes every connection to one node). Each starts
        // at its last known scalar value so a control-rate source (e.g. a
        // Const) wired alongside a genuinely audio-rate input still
        // contributes correctly, instead of silently reading as 0 once
        // this node is forced into audio mode (see setParam's "adder"
        // case and _connectEntries/_disconnectEntries's seed swap).
        const inputCount = Math.max(2, Math.min(8, params.inputCount ?? 2));
        const values = params.values || [];
        const seeds = [];
        for (let i = 0; i < inputCount; i += 1) {
          const seed = ctx.createConstantSource();
          seed.offset.value = values[i] ?? 0;
          seed.start();
          seeds.push(seed);
        }
        const sum = ctx.createGain();
        sum.gain.value = 1;
        seeds.forEach((seed) => seed.connect(sum));
        return { type, sum, seeds, inputCount, controlConnectionCounts: new Array(inputCount).fill(0) };
      }
      case "divide": {
        // Exact per-sample port of renderDivide() (ArithmeticNodes.h): any
        // numerator/denominator range, and a zero denominator outputs 0 (a
        // WaveShaper reciprocal only covered |den| <= 1). seedNum/seedDen are
        // the fallback values while an input carries no audio-rate cable.
        const divideValues = params.values || [];
        const inputMerger = ctx.createChannelMerger(2);
        const processor = ctx.createScriptProcessor(256, 2, 1);
        const seedNum = ctx.createConstantSource();
        seedNum.offset.value = divideValues[0] ?? 0;
        seedNum.start();
        const seedDen = ctx.createConstantSource();
        seedDen.offset.value = divideValues[1] ?? 1;
        seedDen.start();
        seedNum.connect(inputMerger, 0, 0);
        seedDen.connect(inputMerger, 0, 1);
        inputMerger.connect(processor);
        processor.onaudioprocess = (event) => {
          const num = event.inputBuffer.getChannelData(0);
          const den = event.inputBuffer.getChannelData(1);
          const output = event.outputBuffer.getChannelData(0);
          for (let i = 0; i < output.length; i += 1) {
            output[i] = den[i] === 0 ? 0 : num[i] / den[i];
          }
        };
        return { type, seedNum, seedDen, inputMerger, processor, output: processor, controlConnectionCounts: [0, 0] };
      }
      case "frequency-divider": {
        
        
        
        
        
        
        
        
        
        
        
        const seedNum = ctx.createConstantSource();
        seedNum.offset.value = 0;
        seedNum.start();
        const seedDen = ctx.createConstantSource();
        // Prescale converts the raw divider value to the curve's 0..1 input.
        // Dividing the seed here as well made every divider effectively 1.
        seedDen.offset.value = params.divider ?? 2;
        seedDen.start();

        const prescale = ctx.createGain();
        prescale.gain.value = 1 / 128;
        const reciprocal = ctx.createWaveShaper();
        reciprocal.curve = this._frequencyDividerCvOffsetCurve();
        seedDen.connect(prescale);
        prescale.connect(reciprocal);

        const outGain = ctx.createGain();
        outGain.gain.value = 1;
        seedNum.connect(outGain);
        reciprocal.connect(outGain);
        // Output clamped to 0..CV_MAX_VOLTS like applyFrequencyDividerOffset().
        const clamp = this._createRangeGainChain(ctx, 0, CV_MAX_VOLTS, buildRangeCurve(0, CV_MAX_VOLTS, "linear"));
        outGain.connect(clamp.prescale);

        return {
          type, seedNum, seedDen, prescale, reciprocal, outGain, output: clamp.waveShaper,
          extraNodes: [clamp.prescale, clamp.sumNode, clamp.waveShaper],
          extraSources: [clamp.shiftConst],
        };
      }
      case "abs": {



        const waveShaper = ctx.createWaveShaper();
        waveShaper.curve = this._absCurve();
        return { type, waveShaper };
      }
      case "logic-and": {
        // Each input is booleanized-and-inverted first (0 if >0V/true, 10V
        // if <=0V/false), summed, then run through the same invert curve
        // again: sum stays exactly 0 only when every input was true, so
        // that final step reads as "all inputs true" - see
        // _logicInvertStepCurve()'s doc comment above.
        const inputCount = Math.max(2, Math.min(8, params.inputCount ?? 2));
        const seeds = [];
        const perInputShapers = [];
        for (let i = 0; i < inputCount; i += 1) {
          const seed = ctx.createConstantSource();
          seed.offset.value = CV_MAX_VOLTS; // unconnected input defaults to "true" - neutral for AND
          seed.start();
          const shaper = ctx.createWaveShaper();
          shaper.curve = this._logicInvertStepCurve();
          seed.connect(shaper);
          seeds.push(seed);
          perInputShapers.push(shaper);
        }
        const sum = ctx.createGain();
        sum.gain.value = 1;
        perInputShapers.forEach((s) => s.connect(sum));
        const output = ctx.createWaveShaper();
        output.curve = this._logicInvertStepCurve();
        sum.connect(output);
        return { type, seeds, perInputShapers, sum, output, inputCount };
      }
      case "logic-or": {
        // Each input is booleanized on its own (0V / 10V) before summing -
        // summing raw volts first let a negative input cancel a positive one
        // (e.g. -5V OR +3V read false). Unconnected inputs contribute 0
        // (false), the correct OR default.
        const sum = ctx.createGain();
        sum.gain.value = 1;
        const perInputShapers = Array.from({ length: 8 }, () => {
          const shaper = ctx.createWaveShaper();
          shaper.curve = this._logicStepCurve();
          shaper.connect(sum);
          return shaper;
        });
        const output = ctx.createWaveShaper();
        output.curve = this._logicStepCurve();
        sum.connect(output);
        return { type, sum, perInputShapers, output };
      }
      case "logic-xor": {
        const seedA = ctx.createConstantSource();
        seedA.offset.value = 0;
        seedA.start();
        const seedB = ctx.createConstantSource();
        seedB.offset.value = 0;
        seedB.start();
        const bitA = ctx.createWaveShaper();
        bitA.curve = this._logicStepCurve();
        const bitB = ctx.createWaveShaper();
        bitB.curve = this._logicStepCurve();
        seedA.connect(bitA);
        seedB.connect(bitB);
        const negBitB = ctx.createGain();
        negBitB.gain.value = -1;
        bitB.connect(negBitB);
        const diff = ctx.createGain();
        diff.gain.value = 1;
        bitA.connect(diff);
        negBitB.connect(diff);
        const output = ctx.createWaveShaper();
        output.curve = this._logicNonzeroCurve();
        diff.connect(output);
        return { type, seedA, seedB, bitA, bitB, output };
      }
      case "logic-not": {
        const waveShaper = ctx.createWaveShaper();
        waveShaper.curve = this._logicInvertStepCurve();
        return { type, waveShaper };
      }
      case "value-mapper": {
        // Per-sample mappedValue() (ValueMapperNode.h) - the old WaveShaper
        // curve only covered the point range +/-50% and clamped beyond it,
        // while the firmware extrapolates the line (up to the constraints).
        const input = ctx.createGain();
        const processor = ctx.createScriptProcessor(256, 1, 1);
        input.connect(processor);
        const entry = {
          type,
          input,
          processor,
          point1In: params.point1In ?? 0,
          point1Out: params.point1Out ?? 0,
          point2In: params.point2In ?? 127,
          point2Out: params.point2Out ?? CV_MAX_VOLTS,
          constraintMin: params.constraintMin ?? null,
          constraintMax: params.constraintMax ?? null,
          list: parseValueList(params.listText),
        };
        processor.onaudioprocess = (event) => {
          const src = event.inputBuffer.getChannelData(0);
          const out = event.outputBuffer.getChannelData(0);
          for (let i = 0; i < out.length; i += 1) out[i] = mapValueMapperRaw(src[i], entry);
        };
        return entry;
      }
      case "detune": {
        const cvIn = ctx.createGain();
        cvIn.gain.value = 1;
        const amount = clampRange(params.amount ?? CV_MAX_VOLTS / 2, 0, CV_MAX_VOLTS);
        const maxOffsetCents = params.maxOffsetCents ?? 1200;
        const stepCents = params.stepCents ?? 100;
        const deltaConst = ctx.createConstantSource();
        deltaConst.offset.value = detuneOffsetVolts(amount, maxOffsetCents, stepCents);
        deltaConst.start();
        const out = ctx.createGain();
        out.gain.value = 1;
        cvIn.connect(out);
        deltaConst.connect(out);
        return { type, cvIn, deltaConst, out, amount, maxOffsetCents, stepCents };
      }
      case "adsr": {
        
        
        
        
        const constant = ctx.createConstantSource();
        constant.offset.value = 1;
        constant.start();
        const envGain = ctx.createGain();
        envGain.gain.value = 0;
        constant.connect(envGain);
        
        
        
        
        const velocityGain = ctx.createGain();
        velocityGain.gain.value = 1;
        envGain.connect(velocityGain);
        
        
        
        
        
        
        const cvOut = ctx.createGain();
        cvOut.gain.value = CV_MAX_VOLTS;
        velocityGain.connect(cvOut);
        const output = ctx.createGain();
        output.gain.value = 1;
        cvOut.connect(output);
        return {
          type,
          constant,
          envGain,
          velocityGain,
          cvOut,
          output,
          attack: params.attack ?? 0.1,
          decay: params.decay ?? 0.1,
          sustain: params.sustain ?? 0.7,
          release: params.release ?? 0.2,
          // ctx.currentTime the release ramp finishes at, or null while
          // held/never released - see isVoiceStillSounding().
          releaseEndsAt: null,
        };
      }
      case "dx-envelope": {
        const constant = ctx.createConstantSource();
        constant.offset.value = 1;
        constant.start();
        const envGain = ctx.createGain();
        envGain.gain.value = clampRange((params.level4 ?? 0) / CV_MAX_VOLTS, 0, 1);
        constant.connect(envGain);
        const velocityGain = ctx.createGain();
        velocityGain.gain.value = 1;
        envGain.connect(velocityGain);
        const cvOut = ctx.createGain();
        cvOut.gain.value = CV_MAX_VOLTS;
        velocityGain.connect(cvOut);
        const output = ctx.createGain();
        output.gain.value = 1;
        cvOut.connect(output);
        return {
          type,
          constant,
          envGain,
          velocityGain,
          cvOut,
          output,
          level1: clampRange(params.level1 ?? 10, 0, CV_MAX_VOLTS),
          level2: clampRange(params.level2 ?? 8, 0, CV_MAX_VOLTS),
          level3: clampRange(params.level3 ?? 7, 0, CV_MAX_VOLTS),
          level4: clampRange(params.level4 ?? 0, 0, CV_MAX_VOLTS),
          time1: Math.max(params.time1 ?? 0.1, 0.001),
          time2: Math.max(params.time2 ?? 0.25, 0.001),
          time3: Math.max(params.time3 ?? 0.4, 0.001),
          time4: Math.max(params.time4 ?? 0.6, 0.001),
          releaseEndsAt: null,
        };
      }
      case "bitcrusher": {
        const processor = ctx.createScriptProcessor(256, 1, 1);
        const entry = {
          type,
          processor,
          crush: Math.max(0, Math.min(10, Number(params.crush ?? 5))),
          bypass: params.bypass === true,
          heldSample: 0,
          holdCounter: 0,
        };
        processor.onaudioprocess = (event) => {
          const input = event.inputBuffer.getChannelData(0);
          const output = event.outputBuffer.getChannelData(0);
          const crush = entry.crush;
          if (entry.bypass || crush <= 0) {
            output.set(input);
            return;
          }
          const amount = crush / 10;
          const bits = Math.round(16 - 14 * amount);
          const holdSamples = Math.round(1 + 31 * amount * amount);
          const levels = (2 ** (bits - 1)) - 1;
          // Quantizes the +/-AUDIO_PEAK_VOLTS audio domain, like
          // BitcrusherNode.h (a +/-1 step would crush 5x too finely).
          const step = AUDIO_PEAK_VOLTS / levels;
          for (let i = 0; i < input.length; i += 1) {
            if (entry.holdCounter === 0) {
              entry.heldSample = Math.round(input[i] / step) * step;
              entry.holdCounter = holdSamples;
            }
            output[i] = entry.heldSample;
            entry.holdCounter -= 1;
          }
        };
        return entry;
      }
      case "wave-folder": {
        // Keep signal and every parameter on separate merger channels. This
        // lets Drive/Symmetry/Gain/Mix be modulated at audio rate.
        const inputMerger = ctx.createChannelMerger(5);
        const processor = ctx.createScriptProcessor(256, 5, 1);
        const controlSources = ["drive", "symmetry", "gain", "mix"].map((name, index) => {
          const source = ctx.createConstantSource();
          const fallback = [0, CV_MAX_VOLTS / 2, CV_MAX_VOLTS, CV_MAX_VOLTS][index];
          source.offset.value = clampRange(Number(params[name] ?? fallback), 0, CV_MAX_VOLTS);
          source.connect(inputMerger, 0, index + 1);
          source.start();
          return source;
        });
        inputMerger.connect(processor);
        const entry = {
          type,
          inputMerger,
          processor,
          controlSources,
          controlValues: controlSources.map((source) => source.offset.value),
          controlConnectionCounts: [0, 0, 0, 0],
        };
        processor.onaudioprocess = (event) => {
          const input = event.inputBuffer.getChannelData(0);
          const driveCv = event.inputBuffer.getChannelData(1);
          const symmetryCv = event.inputBuffer.getChannelData(2);
          const gainCv = event.inputBuffer.getChannelData(3);
          const mixCv = event.inputBuffer.getChannelData(4);
          const output = event.outputBuffer.getChannelData(0);
          const fold = (sample) => {
            // Audio in the editor uses the same nominal +/-5 V domain as
            // the firmware.  Keeping this limit in volts is essential: a
            // +/-1 Web Audio normalization here would fold a normal signal
            // five times too often and make the preview disagree with the
            // hardware WaveFolderNode.
            const limit = AUDIO_PEAK_VOLTS;
            const period = limit * 4;
            let phase = (sample + limit) % period;
            if (phase < 0) phase += period;
            return limit - Math.abs(phase - limit * 2);
          };
          for (let i = 0; i < output.length; i += 1) {
            const drive = 1 + 7 * clampRange(driveCv[i], 0, CV_MAX_VOLTS) / CV_MAX_VOLTS;
            const bias = clampRange(symmetryCv[i], 0, CV_MAX_VOLTS) - CV_MAX_VOLTS / 2;
            const gain = clampRange(gainCv[i], 0, CV_MAX_VOLTS) / CV_MAX_VOLTS;
            const mix = clampRange(mixCv[i], 0, CV_MAX_VOLTS) / CV_MAX_VOLTS;
            const dry = input[i];
            const wet = fold(dry * drive + bias);
            output[i] = (dry + (wet - dry) * mix) * gain;
          }
        };
        return entry;
      }
      case "envelope-follower": {
        // Mirrors the firmware's nodes::envelopefollower::render()
        // (EnvelopeFollowerNode.h) 1:1: rectify -> attack/release-switched
        // one-pole smoothing, carried in `envelope` across onaudioprocess
        // calls (module-scope cvToRange() above matches cv::cvToRange()'s
        // log-curve mapping exactly).
        const inputMerger = ctx.createChannelMerger(4);
        const processor = ctx.createScriptProcessor(256, 4, 1);
        const controlSources = ["attack", "release", "preGain"].map((name, index) => {
          const source = ctx.createConstantSource();
          source.offset.value = clampRange(Number(params[name] ?? CV_MAX_VOLTS / 2), 0, CV_MAX_VOLTS);
          source.connect(inputMerger, 0, index + 1);
          source.start();
          return source;
        });
        inputMerger.connect(processor);
        const entry = {
          type,
          inputMerger,
          processor,
          controlSources,
          controlValues: controlSources.map((source) => source.offset.value),
          controlConnectionCounts: [0, 0, 0],
        };
        let envelope = 0;
        processor.onaudioprocess = (event) => {
          const input = event.inputBuffer.getChannelData(0);
          const attackCv = event.inputBuffer.getChannelData(1);
          const releaseCv = event.inputBuffer.getChannelData(2);
          const preGainCv = event.inputBuffer.getChannelData(3);
          const output = event.outputBuffer.getChannelData(0);
          for (let i = 0; i < output.length; i += 1) {
            const attackMs = cvToRange(clampRange(attackCv[i], 0, CV_MAX_VOLTS), 1, 500, "log");
            const releaseMs = cvToRange(clampRange(releaseCv[i], 0, CV_MAX_VOLTS), 10, 2000, "log");
            const preGain = cvToRange(clampRange(preGainCv[i], 0, CV_MAX_VOLTS), 0.1, 10, "log");
            const sampleAbs = Math.abs(input[i] * preGain);
            const ms = sampleAbs > envelope ? attackMs : releaseMs;
            const coeff = 1 - Math.exp(-1 / (ms * 0.001 * ctx.sampleRate));
            envelope = clampRange(envelope + coeff * (sampleAbs - envelope), 0, CV_MAX_VOLTS);
            output[i] = envelope;
          }
        };
        return entry;
      }
      case "pan": {
        // Equal-power pan law, mirrors the firmware PanNode.h sin/cos split.
        // ScriptProcessor has one output pin with 2 channels; a splitter +
        // per-channel gain gives out1/out2 their own connectable node, same
        // shape as chorus's separate delayL/delayR.
        const inputMerger = ctx.createChannelMerger(2);
        const processor = ctx.createScriptProcessor(256, 2, 2);
        const panSource = ctx.createConstantSource();
        panSource.offset.value = clampRange(Number(params.pan ?? CV_MAX_VOLTS / 2), 0, CV_MAX_VOLTS);
        panSource.connect(inputMerger, 0, 1);
        panSource.start();
        inputMerger.connect(processor);
        const splitter = ctx.createChannelSplitter(2);
        processor.connect(splitter);
        const out1 = ctx.createGain();
        const out2 = ctx.createGain();
        splitter.connect(out1, 0);
        splitter.connect(out2, 1);
        const entry = {
          type,
          inputMerger,
          processor,
          splitter,
          out1,
          out2,
          panSource,
          panValue: panSource.offset.value,
          panConnectionCount: 0,
        };
        processor.onaudioprocess = (event) => {
          const input = event.inputBuffer.getChannelData(0);
          const panCv = event.inputBuffer.getChannelData(1);
          const outL = event.outputBuffer.getChannelData(0);
          const outR = event.outputBuffer.getChannelData(1);
          for (let i = 0; i < outL.length; i += 1) {
            const phase = clampRange(panCv[i], 0, CV_MAX_VOLTS) / CV_MAX_VOLTS * (Math.PI / 2);
            const dry = input[i];
            outL[i] = dry * Math.cos(phase);
            outR[i] = dry * Math.sin(phase);
          }
        };
        return entry;
      }
      case "sample-hold": {
        // ChannelMerger keeps In, Trigger and Threshold independent. A
        // ScriptProcessor has just one input, so connecting directly to it
        // would mix all three signals before we could compare them.
        const inputMerger = ctx.createChannelMerger(3);
        const processor = ctx.createScriptProcessor(256, 3, 1);
        const thresholdSource = ctx.createConstantSource();
        thresholdSource.offset.value = clampRange(Number(params.threshold ?? 5), 0, CV_MAX_VOLTS);
        thresholdSource.connect(inputMerger, 0, 2);
        thresholdSource.start();
        inputMerger.connect(processor);
        const entry = {
          type,
          inputMerger,
          processor,
          thresholdSource,
          threshold: thresholdSource.offset.value,
          thresholdConnectionCount: 0,
          heldValue: 0,
          triggerWasAbove: false,
        };
        processor.onaudioprocess = (event) => {
          const input = event.inputBuffer.getChannelData(0);
          const trigger = event.inputBuffer.getChannelData(1);
          const threshold = event.inputBuffer.getChannelData(2);
          const output = event.outputBuffer.getChannelData(0);
          for (let i = 0; i < output.length; i += 1) {
            const triggerAbove = trigger[i] >= threshold[i];
            if (triggerAbove && !entry.triggerWasAbove) entry.heldValue = input[i];
            output[i] = entry.heldValue;
            entry.triggerWasAbove = triggerAbove;
          }
        };
        return entry;
      }
      case "burst-generator": {
        // Mirrors the firmware's nodes::burstgenerator::render()
        // (BurstGeneratorNode.h) 1:1: on a rising trigger edge, schedules
        // either one delayed pulse (mode >= 5V) or a decaying burst of
        // "count" pulses spaced "time" ms apart, tracked as a small
        // sample-countdown queue (swap-removed as entries fire).
        const inputMerger = ctx.createChannelMerger(5);
        const processor = ctx.createScriptProcessor(256, 5, 1);
        const burstDefaults = { mode: 0, count: 5, time: 5, decay: 5 };
        const controlSources = ["mode", "count", "time", "decay"].map((name, index) => {
          const source = ctx.createConstantSource();
          source.offset.value = clampRange(Number(params[name] ?? burstDefaults[name]), 0, CV_MAX_VOLTS);
          source.connect(inputMerger, 0, index + 1);
          source.start();
          return source;
        });
        inputMerger.connect(processor);
        const entry = {
          type,
          inputMerger,
          processor,
          controlSources,
          controlValues: controlSources.map((source) => source.offset.value),
          controlConnectionCounts: [0, 0, 0, 0],
        };
        const kMaxPending = 8;
        const kOutPulseSamples = Math.floor(0.005 * ctx.sampleRate);
        let prevTriggerAbove = false;
        const pendingSamples = new Array(kMaxPending).fill(0);
        const pendingAmplitude = new Array(kMaxPending).fill(0);
        let pendingCount = 0;
        let outSamplesLeft = 0;
        let outAmplitude = 0;
        processor.onaudioprocess = (event) => {
          const input = event.inputBuffer.getChannelData(0);
          const modeCv = event.inputBuffer.getChannelData(1);
          const countCv = event.inputBuffer.getChannelData(2);
          const timeCv = event.inputBuffer.getChannelData(3);
          const decayCv = event.inputBuffer.getChannelData(4);
          const output = event.outputBuffer.getChannelData(0);
          for (let i = 0; i < output.length; i += 1) {
            const delayMode = clampRange(modeCv[i], 0, CV_MAX_VOLTS) >= CV_MAX_VOLTS / 2;
            const triggerAbove = input[i] >= CV_MAX_VOLTS / 2;
            if (triggerAbove && !prevTriggerAbove) {
              if (delayMode) {
                const delayMs = cvToRange(clampRange(timeCv[i], 0, CV_MAX_VOLTS), 1, 2000, "log");
                pendingSamples[0] = Math.floor(delayMs * 0.001 * ctx.sampleRate);
                pendingAmplitude[0] = CV_MAX_VOLTS;
                pendingCount = 1;
              } else {
                const pulses = Math.floor(
                  cvToRange(clampRange(countCv[i], 0, CV_MAX_VOLTS), 1, kMaxPending, "linear") + 0.5
                );
                const intervalMs = cvToRange(clampRange(timeCv[i], 0, CV_MAX_VOLTS), 5, 300, "log");
                const decay = cvToRange(clampRange(decayCv[i], 0, CV_MAX_VOLTS), 0, 0.9, "linear");
                const intervalSamples = Math.floor(intervalMs * 0.001 * ctx.sampleRate);
                let amplitude = CV_MAX_VOLTS;
                for (let p = 0; p < pulses; p += 1) {
                  pendingSamples[p] = intervalSamples * p;
                  pendingAmplitude[p] = amplitude;
                  amplitude *= 1 - decay;
                }
                pendingCount = pulses;
              }
            }
            prevTriggerAbove = triggerAbove;

            for (let p = 0; p < pendingCount; ) {
              if (pendingSamples[p] === 0) {
                outSamplesLeft = kOutPulseSamples;
                outAmplitude = pendingAmplitude[p];
                pendingCount -= 1;
                pendingSamples[p] = pendingSamples[pendingCount];
                pendingAmplitude[p] = pendingAmplitude[pendingCount];
              } else {
                pendingSamples[p] -= 1;
                p += 1;
              }
            }

            output[i] = outSamplesLeft > 0 ? outAmplitude : 0;
            if (outSamplesLeft > 0) outSamplesLeft -= 1;
          }
        };
        return entry;
      }
      case "chorus": {




        const inputNode = ctx.createGain();
        inputNode.gain.value = 1;

        const delayL = ctx.createDelay(1);
        const delayR = ctx.createDelay(1);
        delayL.delayTime.value = params.offset ?? 0.02;
        delayR.delayTime.value = params.offset ?? 0.02;
        inputNode.connect(delayL);
        inputNode.connect(delayR);

        const lfo = ctx.createOscillator();
        lfo.type = "sine";
        lfo.frequency.value = params.rate ?? 1.5;
        lfo.start();

        const depthGainL = ctx.createGain();
        depthGainL.gain.value = params.depth ?? 0.003;
        const depthGainR = ctx.createGain();
        depthGainR.gain.value = -(params.depth ?? 0.003);
        lfo.connect(depthGainL);
        lfo.connect(depthGainR);
        depthGainL.connect(delayL.delayTime);
        depthGainR.connect(delayR.delayTime);

        // Bypass: ChorusNode.h passes the input straight to both outputs.
        const bypass = params.bypass === true;
        const processedL = ctx.createGain();
        const processedR = ctx.createGain();
        const bypassL = ctx.createGain();
        const bypassR = ctx.createGain();
        const outputL = ctx.createGain();
        const outputR = ctx.createGain();
        processedL.gain.value = bypass ? 0 : 1;
        processedR.gain.value = bypass ? 0 : 1;
        bypassL.gain.value = bypass ? 1 : 0;
        bypassR.gain.value = bypass ? 1 : 0;
        delayL.connect(processedL);
        delayR.connect(processedR);
        inputNode.connect(bypassL);
        inputNode.connect(bypassR);
        processedL.connect(outputL);
        bypassL.connect(outputL);
        processedR.connect(outputR);
        bypassR.connect(outputR);

        return {
          type,
          inputNode,
          lfo,
          depthGainL,
          depthGainR,
          delayL,
          delayR,
          processedL,
          processedR,
          bypassL,
          bypassR,
          outputL,
          outputR,
          bypass,
        };
      }
      case "delay": {
        
        
        const chanL = this._createDelayChannel(ctx, params);
        const chanR = this._createDelayChannel(ctx, params);
        return {
          type,
          inputL: chanL.input,
          delayL: chanL.delay,
          feedbackGainL: chanL.feedbackGain,
          dryGainL: chanL.dry,
          wetGainL: chanL.wet,
          outputL: chanL.output,
          inputR: chanR.input,
          delayR: chanR.delay,
          feedbackGainR: chanR.feedbackGain,
          dryGainR: chanR.dry,
          wetGainR: chanR.wet,
          outputR: chanR.output,
          processedL: chanL.processed,
          processedR: chanR.processed,
          bypassL: chanL.bypassGain,
          bypassR: chanR.bypassGain,
          bypass: params.bypass === true,
        };
      }
      case "reverb-light": {
        // Dattorro plate (reverb-core.js), a 1:1 port of renderReverbLight() in
        // nodes/ReverbNode.h: mono sum in, stereo dry/wet out.
        const inputNodeL = ctx.createGain();
        const inputNodeR = ctx.createGain();
        const inputMerger = ctx.createChannelMerger(2);
        inputNodeL.connect(inputMerger, 0, 0);
        inputNodeR.connect(inputMerger, 0, 1);
        const processor = ctx.createScriptProcessor(256, 2, 2);
        inputMerger.connect(processor);
        const splitter = ctx.createChannelSplitter(2);
        processor.connect(splitter);
        const outputL = ctx.createGain();
        const outputR = ctx.createGain();
        splitter.connect(outputL, 0);
        splitter.connect(outputR, 1);

        const rv = createReverbCore(ctx.sampleRate, params);
        processor.onaudioprocess = (event) => {
          rv.process(
            event.inputBuffer.getChannelData(0),
            event.inputBuffer.getChannelData(1),
            event.outputBuffer.getChannelData(0),
            event.outputBuffer.getChannelData(1),
          );
        };
        return {
          type,
          reverb: rv,
          inputNodeL,
          inputNodeR,
          inputMerger,
          processor,
          splitter,
          outputL,
          outputR,
        };
      }
      case "audio-output-link":
      case "audio-input-link": {
        const gainVolts = Array.from({ length: LINK_BUS_COUNT }, (_, i) =>
          clampRange(Number(params.gains?.[i] ?? CV_MAX_VOLTS) || 0, 0, CV_MAX_VOLTS)
        );
        const nodes = Array.from({ length: LINK_BUS_COUNT }, () => {
          const node = ctx.createGain();
          node.gain.value = 1;
          return node;
        });
        // Level CV per bus: cv::vcaGain(v, false), i.e. clamped to 0..1.
        const linkGainChains = type === "audio-input-link"
          ? Array.from({ length: LINK_BUS_COUNT }, (_, i) => {
              const chain = this._createCvGainChain(ctx, "linear");
              chain.waveShaper.connect(nodes[i].gain);
              nodes[i].gain.value = gainVolts[i] / CV_MAX_VOLTS;
              return chain;
            })
          : [];
        return {
          type,
          nodes,
          linkGainVolts: gainVolts,
          linkGainPrescales: type === "audio-input-link" ? linkGainChains.map((chain) => chain.prescale) : null,
          linkGainConnectionCount: Array(LINK_BUS_COUNT).fill(0),
          extraNodes: linkGainChains.flatMap((chain) => [chain.sumNode, chain.waveShaper]),
          extraSources: linkGainChains.map((chain) => chain.shiftConst),
        };
      }
      case "vu-meter": {
        const analyser = ctx.createAnalyser();
        analyser.fftSize = 256;
        const buffer = new Float32Array(analyser.fftSize);
        return { type, analyser, buffer };
      }
      case "oscilloscope": {
        
        
        
        
        
        
        
        // Keep substantially more samples than the scope renders.  That gives
        // the renderer enough data on both sides of a trigger edge to show a
        // contiguous time window instead of wrapping the analyser buffer.
        const fftSize = 4096;
        const analyser1 = ctx.createAnalyser();
        analyser1.fftSize = fftSize;
        const analyser2 = ctx.createAnalyser();
        analyser2.fftSize = fftSize;
        return {
          type,
          analyser1,
          analyser2,
          buffer1: new Float32Array(fftSize),
          buffer2: new Float32Array(fftSize),
        };
      }
      case "spectrum": {
        
        
        
        
        
        
        
        const analyser = ctx.createAnalyser();
        analyser.fftSize = 256;
        analyser.minDecibels = -100;
        analyser.maxDecibels = 20;
        analyser.smoothingTimeConstant = 0.75;
        const dbBuffer = new Float32Array(analyser.frequencyBinCount);
        const voltageBuffer = new Float32Array(analyser.frequencyBinCount);
        return { type, analyser, dbBuffer, voltageBuffer };
      }
      case "wave-player": {
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        
        const releaseGain = ctx.createGain();
        releaseGain.gain.value = 1;
        const ampGain = ctx.createGain();
        ampGain.gain.value = 1;
        releaseGain.connect(ampGain);
        // Combines the fixed rescale to the audio domain's peak (mirrors
        // WavePlayerNode.h's "* cv::kAudioPeakVolts") with the "gain" port's
        // 0-10V CV, same one-node pattern as Oscillator's/Hammond's/FM-
        // Oscillator's own outGain - kept separate from ampGain so a
        // connected amplitude cable (which zeroes ampGain's own base value
        // and drives it additively instead, see connectEdge() below) doesn't
        // also wipe out this scale.
        const outGain = ctx.createGain();
        outGain.gain.value = ((params.gain ?? CV_MAX_VOLTS) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS;
        ampGain.connect(outGain);
        const gainCv = this._createCvGainChain(ctx, "linear", AUDIO_PEAK_VOLTS);
        gainCv.waveShaper.connect(outGain.gain);
        const splitter = ctx.createChannelSplitter(2);
        outGain.connect(splitter);
        const leftOut = ctx.createGain();
        const rightOut = ctx.createGain();
        splitter.connect(leftOut, 0, 0);
        splitter.connect(rightOut, 1, 0);

        const entry = {
          type,
          releaseGain,
          ampGain,
          outGain,
          gain: params.gain ?? CV_MAX_VOLTS,
          gainCv,
          gainCvActive: false,
          splitter,
          leftOut,
          rightOut,
          source: null,
          buffer: null,
          note: 60,
          rootNoteMidi: params.rootNoteMidi ?? 60,
          sampleFile: params.sampleFile || "",
        };
        this._loadWavePlayerBuffer(entry);
        return entry;
      }
      case "soundfont-player": {
        
        
        
        
        
        
        
        
        const splitter = ctx.createChannelSplitter(2);
        const leftOut = ctx.createGain();
        const rightOut = ctx.createGain();
        const initialGain = ((Number(params.gain ?? CV_MAX_VOLTS) || 0) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS;
        leftOut.gain.value = initialGain;
        rightOut.gain.value = initialGain;
        splitter.connect(leftOut, 0, 0);
        splitter.connect(rightOut, 1, 0);
        // "gain" cable: replaces the knob value (base gains zeroed on
        // connect), same AUDIO_PEAK_VOLTS-scaled path as the oscillator's.
        const gainCv = this._createCvGainChain(ctx, "linear", AUDIO_PEAK_VOLTS);
        gainCv.waveShaper.connect(leftOut.gain);
        gainCv.waveShaper.connect(rightOut.gain);
        const entry = {
          type,
          splitter,
          leftOut,
          rightOut,
          gainCv,
          gainCvActive: false,
          gain: params.gain ?? CV_MAX_VOLTS,
          volume: params.volume || "flash",
          path: params.path || "",
          bank: params.bank ?? 0,
          program: params.program ?? 0,
          maxVoices: params.maxVoices ?? 32,
          enableCc: params.enableCc === true,
          enableModWheel: params.enableModWheel === true,
          enablePitchWheel: params.enablePitchWheel === true,
          parsed: null,
          channels: Array.from({ length: 16 }, () => this._sf2NewChannelState(params.bank ?? 0, params.program ?? 0)),
          channelGainNodes: new Array(16).fill(null),
          voices: [],
        };
        this._loadSf2Entry(entry);
        return entry;
      }
      case "mda-piano": {
        // See the "MDA Piano preview" block near the top of this file for
        // the ported engine/effects-chain. All 12 CV ports are read once per
        // block (channel [0] only), exactly matching MdaPianoNode.h's own
        // portSample(inst, port, 0) block-rate reads - only "gain" differs
        // slightly (read per-sample here for a click-free knob response
        // instead of the firmware's dedicated one-pole smoother; a minor,
        // deliberate simplification for a preview-only signal path).
        const maxVoices = Math.max(4, Math.min(128, Math.round(Number(params.maxVoices) || 32)));
        const inputMerger = ctx.createChannelMerger(MDA_PIANO_PORTS.length);
        const seeds = MDA_PIANO_PORTS.map((name, index) => {
          const raw = params[name];
          const value = typeof raw === "boolean" ? (raw ? 1 : 0) : Number(raw ?? MDA_PIANO_PORT_DEFAULTS[index]) || 0;
          const seed = ctx.createConstantSource();
          seed.offset.value = clampRange(value, 0, CV_MAX_VOLTS);
          seed.connect(inputMerger, 0, index);
          seed.start();
          return seed;
        });
        // 2048, not the 256 the other ScriptProcessor nodes use: this one
        // renders up to 32 sampled voices plus the effects chain per block,
        // and a 5.3 ms main-thread deadline drops whole blocks (audible as
        // clicks). Costs ~43 ms preview latency.
        const processor = ctx.createScriptProcessor(2048, MDA_PIANO_PORTS.length, 2);
        inputMerger.connect(processor);
        const splitter = ctx.createChannelSplitter(2);
        processor.connect(splitter);
        const leftOut = ctx.createGain();
        const rightOut = ctx.createGain();
        splitter.connect(leftOut, 0, 0);
        splitter.connect(rightOut, 1, 0);

        const entry = {
          type,
          inputMerger,
          processor,
          splitter,
          leftOut,
          rightOut,
          seeds,
          controlConnectionCounts: new Array(MDA_PIANO_PORTS.length).fill(0),
          controlValues: seeds.map((s) => s.offset.value),
          maxVoices,
          engine: createMdaEpianoEngine(maxVoices, ctx.sampleRate),
          effects: createRefaceCpChain(ctx.sampleRate),
          instruments: null,
          currentInstrumentIndex: -1,
          modWheel: 1,
        };
        this._loadMdaPianoEntry(entry);

        processor.onaudioprocess = (event) => {
          const outL = event.outputBuffer.getChannelData(0);
          const outR = event.outputBuffer.getChannelData(1);
          if (!entry.instruments) {
            outL.fill(0);
            outR.fill(0);
            return;
          }
          const ch = (i) => event.inputBuffer.getChannelData(i)[0];
          entry.enableModWheel = ch(0) >= 0.5;
          entry.enablePitchWheel = ch(1) >= 0.5;
          const selectedInstrument = mdaVoiceTypeFromCv(ch(2));
          if (selectedInstrument !== entry.currentInstrumentIndex) {
            const instrument = entry.instruments[selectedInstrument];
            if (instrument) {
              entry.engine.loadInstrument(instrument);
              entry.currentInstrumentIndex = selectedInstrument;
            }
          }
          entry.effects.setVoiceType(selectedInstrument);
          entry.effects.setDrive(ch(3) * 0.1);
          entry.effects.setTremWahMode(mdaEffectModeFromCv(ch(4)));
          entry.effects.setTremWahDepth(ch(5) * 0.1 * entry.modWheel);
          entry.effects.setTremWahRate(ch(6) * 0.1);
          entry.effects.setChoPhaMode(mdaEffectModeFromCv(ch(7)));
          entry.effects.setChoPhaDepth(ch(8) * 0.1);
          entry.effects.setChoPhaSpeed(ch(9) * 0.1);
          entry.effects.setPreGain(ch(10) * 0.1);

          const frames = outL.length;
          entry.engine.process(outL, outR, frames);
          entry.effects.process(outL, outR, frames);

          const gainCv = event.inputBuffer.getChannelData(11);
          for (let i = 0; i < frames; i += 1) {
            const g = vcaAmountFromCv(gainCv[i], "linear") * AUDIO_PEAK_VOLTS * 3;
            outL[i] *= g;
            outR[i] *= g;
          }
        };
        return entry;
      }
      default:
        return null;
    }
  }

  
  
  
  
  createNode(id, type, params = {}, voiceCount = 1) {
    const ctx = this.ensureContext();

    if (voiceCount <= 1) {
      this.nodes.set(id, this._buildNode(ctx, type, params));
    } else {
      const instances = [];
      for (let v = 0; v < voiceCount; v += 1) {
        instances.push(this._buildNode(ctx, type, params));
      }
      
      
      
      
      
      this.nodes.set(id, { type, voiced: true, instances, global: false });
    }

    if (type === "audio-output-link" || type === "audio-input-link") {
      this._wireLinkBridge();
    }

    if (type.startsWith("linked-input") || type.startsWith("linked-output")) {
      this._wireLinkedIO();
    }
  }

  _getLevelFromEntry(entry, channel) {
    if (!entry) return 0;
    let analyser = null;
    let buffer = null;
    if (entry.type === "vu-meter") {
      analyser = entry.analyser;
      buffer = entry.buffer;
    } else if (entry.type === "audio-out" || entry.type === "usb-out" || entry.type === "es8311-out" ||
             entry.type === "wm8960-out") {
      analyser = channel === "right" ? entry.rightAnalyser : entry.leftAnalyser;
      buffer = channel === "right" ? entry.rightBuffer : entry.leftBuffer;
    }
    if (!analyser) return 0;

    const floatSamples = buffer instanceof Float32Array;
    if (floatSamples) analyser.getFloatTimeDomainData(buffer);
    else analyser.getByteTimeDomainData(buffer);
    let sumSquares = 0;
    for (let i = 0; i < buffer.length; i += 1) {
      const v = floatSamples ? buffer[i] : (buffer[i] - 128) / 128;
      sumSquares += v * v;
    }
    return Math.sqrt(sumSquares / buffer.length);
  }

  
  
  
  
  
  
  getLevel(id, channel) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return 0;
    if (wrapper.voiced) {
      if (wrapper.instances.length === 0) return 0;
      const sum = wrapper.instances.reduce(
        (acc, entry) => acc + this._getLevelFromEntry(entry, channel),
        0,
      );
      return sum / wrapper.instances.length;
    }
    return this._getLevelFromEntry(wrapper, channel);
  }

  
  
  
  
  
  getWaveform(id, channel) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return null;
    const entry = wrapper.voiced ? wrapper.instances[0] : wrapper;
    if (!entry || entry.type !== "oscilloscope") return null;
    if (channel === "in2") {
      entry.analyser2.getFloatTimeDomainData(entry.buffer2);
      return entry.buffer2;
    }
    entry.analyser1.getFloatTimeDomainData(entry.buffer1);
    return entry.buffer1;
  }

  
  
  
  getSpectrum(id) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return null;
    const entry = wrapper.voiced ? wrapper.instances[0] : wrapper;
    if (!entry || entry.type !== "spectrum") return null;
    entry.analyser.getFloatFrequencyData(entry.dbBuffer);
    for (let i = 0; i < entry.dbBuffer.length; i += 1) {
      const db = entry.dbBuffer[i];
      entry.voltageBuffer[i] = Number.isFinite(db)
        ? (2 / ANALYSER_BLACKMAN_COHERENT_GAIN) * 10 ** (db / 20)
        : 0;
    }
    return entry.voltageBuffer;
  }

  
  
  getEspEqSpectrum(id) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return null;
    const entry = wrapper.voiced ? wrapper.instances[0] : wrapper;
    if (!entry || entry.type !== "esp-eq") return null;
    entry.spectrumAnalyser.getByteFrequencyData(entry.spectrumBuffer);
    return entry.spectrumBuffer;
  }

  
  
  
  getEspEqFrequencyResponse(id) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return null;
    const entry = wrapper.voiced ? wrapper.instances[0] : wrapper;
    if (!entry || entry.type !== "esp-eq") return null;
    if (entry.bypass) {
      entry.responseMag.fill(1);
      return entry.responseMag;
    }
    entry.filterL.getFrequencyResponse(entry.responseFreqs, entry.responseMag, entry.responsePhase);
    return entry.responseMag;
  }

  getFilterSpectrum(id) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return null;
    const entry = wrapper.voiced ? wrapper.instances[0] : wrapper;
    if (!entry || !["filter", "curtis-filter", "comb-filter"].includes(entry.type)) return null;
    entry.spectrumAnalyser.getByteFrequencyData(entry.spectrumBuffer);
    return entry.spectrumBuffer;
  }

  // Magnitude of the processor's current coefficients (see the dsp objects'
  // response()), sampled at the widget's responseFreqs.
  getFilterFrequencyResponse(id) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return null;
    const entry = wrapper.voiced ? wrapper.instances[0] : wrapper;
    if (!entry || !["filter", "curtis-filter", "comb-filter"].includes(entry.type)) return null;
    if (entry.type !== "comb-filter" && entry.coeffsDirty && !entry.cutoffCvActive && !entry.resonanceCvActive) {
      // Not rendered yet since the last change - show the new settings.
      if (entry.type === "curtis-filter") {
        entry.dsp.updateCoefficients(entry.cutoff, entry.resonance, filterPolesFromSlope(entry.slope));
      } else if (entry.filterType !== "none") {
        entry.dsp.applyCoeffs(entry.filterType, entry.cutoff, entry.resonance);
      }
      entry.coeffsDirty = false;
    }
    entry.dsp.response(entry.responseFreqs, entry.responseMag);
    return entry.responseMag;
  }

  getGraphicEqSpectrum(id) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return null;
    const entry = wrapper.voiced ? wrapper.instances[0] : wrapper;
    if (!entry || entry.type !== "graphic-eq") return null;
    entry.spectrumAnalyser.getByteFrequencyData(entry.spectrumBuffer);
    return entry.spectrumBuffer;
  }

  // 8 DIFFERENT bands cascaded in series - unlike Filter's identical
  // sections, each band's magnitude must be sampled and multiplied in
  // separately (bypass shows a flat line, same reasoning as Filter's own
  // "none" case above).
  getGraphicEqFrequencyResponse(id) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return null;
    const entry = wrapper.voiced ? wrapper.instances[0] : wrapper;
    if (!entry || entry.type !== "graphic-eq") return null;
    entry.responseMag.fill(1);
    if (!entry.bypass) {
      entry.bandsL.forEach((band) => {
        band.getFrequencyResponse(entry.responseFreqs, entry.bandMagScratch, entry.responsePhase);
        for (let i = 0; i < entry.responseMag.length; i += 1) entry.responseMag[i] *= entry.bandMagScratch[i];
      });
    }
    return entry.responseMag;
  }

  
  
  
  
  
  
  
  
  
  
  
  
  getPortValue(id, port) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return null;
    const entry = wrapper.voiced ? wrapper.instances[0] : wrapper;
    const node = this._resolveOutputFromEntry(entry, port);
    if (!node) return null;
    const key = `${id}:${port}`;
    let tap = this._valueMonitorTaps.get(key);
    if (!tap || tap.node !== node) {
      if (tap) tap.analyser.disconnect();
      const analyser = this.ctx.createAnalyser();
      analyser.fftSize = 32;
      node.connect(analyser);
      tap = { node, analyser, buffer: new Float32Array(analyser.fftSize) };
      this._valueMonitorTaps.set(key, tap);
    }
    tap.analyser.getFloatTimeDomainData(tap.buffer);
    return tap.buffer[tap.buffer.length - 1];
  }

  // Per-widget oscilloscope preview (e.g. "oscillator"/"fm-oscillator" inline
  // scope panel) - same lazy-tap pattern as getPortValue above but with a
  // larger fftSize so the caller gets a full waveform, not just one sample.
  // Reuses _valueMonitorTaps so removeNode()'s `${id}:` prefix cleanup below
  // also disconnects this tap.
  getOscilloscopeWaveform(id, port = "value") {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return null;
    const entry = wrapper.voiced ? wrapper.instances[0] : wrapper;
    const node = this._resolveOutputFromEntry(entry, port);
    if (!node) return null;
    const key = `${id}:scope:${port}`;
    let tap = this._valueMonitorTaps.get(key);
    if (!tap || tap.node !== node) {
      if (tap) tap.analyser.disconnect();
      const analyser = this.ctx.createAnalyser();
      analyser.fftSize = 2048;
      node.connect(analyser);
      tap = { node, analyser, buffer: new Float32Array(analyser.fftSize) };
      this._valueMonitorTaps.set(key, tap);
    }
    tap.analyser.getFloatTimeDomainData(tap.buffer);
    return tap.buffer;
  }

  _removeEntry(entry) {
    if (!entry) return;
    if (this._deferredSampleLoads.delete(entry)) this._notifyDeferredSamples();
    // Generic cleanup lists (each node on its own, so one already-stopped
    // source can't abort the rest): extraSources get stop()+disconnect(),
    // extraNodes just disconnect().
    (entry.extraSources || []).forEach((node) => {
      try { node.stop(); } catch (err) { /* already stopped */ }
      try { node.disconnect(); } catch (err) { /* already disconnected */ }
    });
    (entry.extraNodes || []).forEach((node) => {
      try { node.disconnect(); } catch (err) { /* already disconnected */ }
    });
    try {
      if (entry.osc) {
        entry.osc.stop();
        entry.osc.disconnect();
      }
      if (entry.noiseSource) {
        entry.noiseSource.stop();
        entry.noiseSource.disconnect();
      }
      if (entry.pinkNoiseSource) {
        entry.pinkNoiseSource.stop();
        entry.pinkNoiseSource.disconnect();
      }
      if (entry.ampGain) entry.ampGain.disconnect();
      if (entry.lfoOsc) {
        entry.lfoOsc.stop();
        entry.lfoOsc.disconnect();
      }
      if (entry.lfoOneShotTimer) clearTimeout(entry.lfoOneShotTimer);
      if (entry.type === "wavetable") this._destroyLfoWavetableSource(entry);
      if (entry.lfoGain) entry.lfoGain.disconnect();
      if (entry.centerOffset) {
        entry.centerOffset.stop();
        entry.centerOffset.disconnect();
      }
      if (entry.leftGain) entry.leftGain.disconnect();
      if (entry.rightGain) entry.rightGain.disconnect();
      if (entry.merger) entry.merger.disconnect();
      if (entry.muteGain) entry.muteGain.disconnect();
      if (entry.leftAnalyser) entry.leftAnalyser.disconnect();
      if (entry.rightAnalyser) entry.rightAnalyser.disconnect();
      if (entry.gainNode) entry.gainNode.disconnect();
      if (entry.curveGainNode) entry.curveGainNode.disconnect();
      if (entry.curveOwnGate) entry.curveOwnGate.disconnect();
      if (entry.curveSharedGate) entry.curveSharedGate.disconnect();
      if (entry.curveCv) {
        entry.curveCv.waveShaper.disconnect();
        entry.curveCv.sumNode.disconnect();
        entry.curveCv.prescale.disconnect();
        entry.curveCv.shiftConst.stop();
        entry.curveCv.shiftConst.disconnect();
      }
      if (entry.filterNode) entry.filterNode.disconnect();
      if (entry.filterNode2) entry.filterNode2.disconnect();
      if (entry.filterInput) entry.filterInput.disconnect();
      if (entry.filteredOutput && entry.filteredOutput !== entry.filterNode) entry.filteredOutput.disconnect();
      if (entry.filterOutput && entry.filterOutput !== entry.filterNode) entry.filterOutput.disconnect();
      if (entry.dryGain) entry.dryGain.disconnect();
      if (entry.wetGain) entry.wetGain.disconnect();
      if (entry.sections) entry.sections.forEach((s) => s.disconnect());
      if (entry.sectionTaps) entry.sectionTaps.forEach((t) => t.disconnect());
      if (entry.inputMap) entry.inputMap.forEach((g) => g.disconnect());
      if (entry.sum) entry.sum.disconnect();
      if (entry.inputGains) entry.inputGains.forEach((g) => g.disconnect());
      if (entry.seed) {
        entry.seed.stop();
        entry.seed.disconnect();
      }
      if (entry.stages) entry.stages.forEach((s) => s.node.disconnect());
      if (entry.seeds) entry.seeds.forEach((s) => { s.stop(); s.disconnect(); });
      if (entry.perInputShapers) entry.perInputShapers.forEach((s) => s.disconnect());
      if (entry.seedA) { entry.seedA.stop(); entry.seedA.disconnect(); }
      if (entry.seedB) { entry.seedB.stop(); entry.seedB.disconnect(); }
      if (entry.bitA) entry.bitA.disconnect();
      if (entry.bitB) entry.bitB.disconnect();
      if (entry.negBitB) entry.negBitB.disconnect();
      if (entry.diff) entry.diff.disconnect();
      if (entry.seedNum) {
        entry.seedNum.stop();
        entry.seedNum.disconnect();
      }
      if (entry.seedDen) {
        entry.seedDen.stop();
        entry.seedDen.disconnect();
      }
      if (entry.reciprocal) entry.reciprocal.disconnect();
      if (entry.outGain) entry.outGain.disconnect();
      if (entry.constants) {
        // "knob" entries own 16 ConstantSourceNodes (see _buildNode's "knob"
        // case) - entry.constant is only an alias for constants[0], stop
        // that array instead of double-freeing the same node twice.
        entry.constants.forEach((c) => {
          c.stop();
          c.disconnect();
        });
      } else if (entry.constant) {
        entry.constant.stop();
        entry.constant.disconnect();
      }
      if (entry.envGain) entry.envGain.disconnect();
      if (entry.velocityGain) entry.velocityGain.disconnect();
      if (entry.cvOut) entry.cvOut.disconnect();
      if (entry.inputNode) entry.inputNode.disconnect();
      
      
      
      
      ["prescale", "sumNode", "waveShaper", "ownGate", "sharedGate", "rawOut"].forEach((key) => {
        if (entry[key]) entry[key].disconnect();
      });
      if (entry.shiftConst) {
        entry.shiftConst.stop();
        entry.shiftConst.disconnect();
      }
      if (entry.ampCv) {
        entry.ampCv.waveShaper.disconnect();
        entry.ampCv.sumNode.disconnect();
        entry.ampCv.prescale.disconnect();
        entry.ampCv.shiftConst.stop();
        entry.ampCv.shiftConst.disconnect();
      }
      if (entry.gainCv) {
        entry.gainCv.waveShaper.disconnect();
        entry.gainCv.sumNode.disconnect();
        entry.gainCv.prescale.disconnect();
        entry.gainCv.shiftConst.stop();
        entry.gainCv.shiftConst.disconnect();
      }
      if (entry.fmIndexCv) {
        entry.fmIndexCv.waveShaper.disconnect();
        entry.fmIndexCv.sumNode.disconnect();
        entry.fmIndexCv.prescale.disconnect();
        entry.fmIndexCv.shiftConst.stop();
        entry.fmIndexCv.shiftConst.disconnect();
      }
      if (entry.feedbackCv) {
        entry.feedbackCv.waveShaper.disconnect();
        entry.feedbackCv.sumNode.disconnect();
        entry.feedbackCv.prescale.disconnect();
        entry.feedbackCv.shiftConst.stop();
        entry.feedbackCv.shiftConst.disconnect();
      }
      if (entry.fmInScale) entry.fmInScale.disconnect();
      if (entry.feedbackDelay) entry.feedbackDelay.disconnect();
      if (entry.feedbackGain) entry.feedbackGain.disconnect();
      if (entry.feedbackCvScale) entry.feedbackCvScale.disconnect();
      if (entry.oscs) entry.oscs.forEach((o) => { o.stop(); o.disconnect(); });
      if (entry.footageScale) entry.footageScale.forEach((g) => g.disconnect());
      if (entry.drawbarGains) entry.drawbarGains.forEach((g) => g.disconnect());
      if (entry.drawbarCv) {
        entry.drawbarCv.forEach((cv) => {
          cv.waveShaper.disconnect();
          cv.sumNode.disconnect();
          cv.prescale.disconnect();
          cv.shiftConst.stop();
          cv.shiftConst.disconnect();
        });
      }
      
      
      
      
      
      ["cutoffCv", "resonanceCv"].forEach((key) => {
        const cv = entry[key];
        if (!cv) return;
        cv.waveShaper.disconnect();
        cv.sumNode.disconnect();
        cv.prescale.disconnect();
        cv.shiftConst.stop();
        cv.shiftConst.disconnect();
      });
      ["cutoffOwnGate", "cutoffSharedGate", "resonanceOwnGate", "resonanceSharedGate"].forEach((key) => {
        if (entry[key]) entry[key].disconnect();
      });
      if (entry.lfo) {
        entry.lfo.stop();
        entry.lfo.disconnect();
      }
      if (entry.depthGainL) entry.depthGainL.disconnect();
      if (entry.depthGainR) entry.depthGainR.disconnect();
      if (entry.processor) {
        entry.processor.onaudioprocess = null;
        entry.processor.disconnect();
      }
      if (entry.thresholdSource) {
        entry.thresholdSource.stop();
        entry.thresholdSource.disconnect();
      }
      if (entry.panSource) {
        entry.panSource.stop();
        entry.panSource.disconnect();
      }
      if (entry.splitter) entry.splitter.disconnect();
      if (entry.out1) entry.out1.disconnect();
      if (entry.out2) entry.out2.disconnect();
      if (entry.inputMerger) entry.inputMerger.disconnect();
      if (entry.delayL) entry.delayL.disconnect();
      if (entry.delayR) entry.delayR.disconnect();
      
      
      
      
      
      [
        "inputL", "inputR",
        "feedbackGainL", "feedbackGainR",
        "dryGainL", "dryGainR",
        "wetGainL", "wetGainR",
        "outputL", "outputR",
        "inputNodeL", "inputNodeR",
        "preDelayL", "preDelayR",
        "convolverL", "convolverR",
        "dampFilterL", "dampFilterR",
        "dryL", "dryR",
        "wetL", "wetR",
        "processedL", "processedR",
        "bypassL", "bypassR",
        "monoSum", "convolver", "dampFilter", "wet",
      ].forEach((key) => {
        if (entry[key]) entry[key].disconnect();
      });
      if (entry.convolver) entry.convolver.disconnect();
      if (entry.dampFilter) entry.dampFilter.disconnect();
      if (entry.dry) entry.dry.disconnect();
      if (entry.wet) entry.wet.disconnect();
      if (entry.output) entry.output.disconnect();
      if (entry.input) entry.input.disconnect();
      if (entry.outputGains) entry.outputGains.forEach((g) => g.disconnect());
      if (
        (entry.type === "audio-output-link" || entry.type === "audio-input-link") &&
        entry.nodes
      ) {
        entry.nodes.forEach((node) => node.disconnect());
        if (entry.linkGainPrescales) entry.linkGainPrescales.forEach((node) => node.disconnect());
        this._linkBridged = false;
      }
      if ((entry.type.startsWith("linked-input") || entry.type.startsWith("linked-output")) && entry.gainNodes) {
        entry.gainNodes.forEach((node) => node.disconnect());
        this._wireLinkedIO();
      }
      if (entry.analyser) entry.analyser.disconnect();
      if (entry.spectrumAnalyser) entry.spectrumAnalyser.disconnect();
      if (entry.limiter) entry.limiter.disconnect();
      if (entry.bandsL) entry.bandsL.forEach((b) => b.disconnect());
      if (entry.bandsR) entry.bandsR.forEach((b) => b.disconnect());
      if (entry.outputGainL) entry.outputGainL.disconnect();
      if (entry.outputGainR) entry.outputGainR.disconnect();
      if (entry.type === "wave-player") {
        if (entry.source) {
          entry.source.stop();
          entry.source.disconnect();
        }
        
        if (entry.releaseGain) entry.releaseGain.disconnect();
        if (entry.splitter) entry.splitter.disconnect();
        if (entry.leftOut) entry.leftOut.disconnect();
        if (entry.rightOut) entry.rightOut.disconnect();
      }
      if (entry.type === "soundfont-player") {
        this._sf2StopAllVoices(entry);
        if (entry.channelGainNodes) entry.channelGainNodes.forEach((g) => g && g.disconnect());
        if (entry.splitter) entry.splitter.disconnect();
        if (entry.leftOut) entry.leftOut.disconnect();
        if (entry.rightOut) entry.rightOut.disconnect();
      }
      if (entry.type === "mda-piano") {
        if (entry.splitter) entry.splitter.disconnect();
        if (entry.leftOut) entry.leftOut.disconnect();
        if (entry.rightOut) entry.rightOut.disconnect();
      }
      if (entry._voiceCompGains) {
        Object.values(entry._voiceCompGains).forEach((g) => g.disconnect());
      }
    } catch (err) {
      
    }
  }

  
  
  
  
  getVoiceCount(id) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return 0;
    return wrapper.voiced ? wrapper.instances.length : 1;
  }

  _setLfoRunning(entry, running) {
    if (!entry || !this.ctx) return;
    if (lfoLinearHold(entry)) {
      // Linear ramps hold on their own at the end - just (un)pause the ramp.
      applyLfoLinear(this.ctx, entry, false);
      entry.lfoLinearRunning = running;
      applyLfoLinear(this.ctx, entry, false);
      entry.lfoOneShotRunning = running;
      return;
    }
    const halfSwing = entry.lfoAmplitude / 2;
    const center = entry.lfoZeroPointFixed ? entry.lfoZeroPoint : halfSwing;
    const heldValue = center + lfoHeldRaw(entry) * halfSwing;
    const now = this.ctx.currentTime;
    entry.lfoGain.gain.cancelScheduledValues(now);
    entry.centerOffset.offset.cancelScheduledValues(now);
    entry.lfoGain.gain.setValueAtTime(running ? halfSwing : 0, now);
    entry.centerOffset.offset.setValueAtTime(running ? center : heldValue, now);
    entry.lfoOneShotRunning = running;
  }

  // --- Wavetable widget (LFO chain with a wavetable source) ---------------
  //
  // The wavetable source replaces lfoOsc as the raw -1..+1 signal into
  // lfoGain; everything downstream (gain, One Shot hold via
  // _setLfoRunning, Global via rawOut/sharedGate) is untouched. Preferred
  // source is the "wavetable-lfo" AudioWorklet (morph is a real AudioParam
  // there). AudioWorklet only exists in secure contexts - the device UI is
  // normally served over plain http:// from a LAN address - so a
  // ScriptProcessor running the same WavetableLfoCore is the fallback.

  _loadWavetableWorklet() {
    const ctx = this.ensureContext();
    if (!ctx.audioWorklet || typeof AudioWorkletNode === "undefined") return null;
    if (!this._wavetableWorkletPromise) {
      this._wavetableWorkletPromise = ctx.audioWorklet
        .addModule(new URL("./wavetable-worklet.js?v=20260926-knob-2", import.meta.url))
        .then(() => true)
        .catch(() => false);
    }
    return this._wavetableWorkletPromise;
  }

  _setLfoWavetable(entry, enabled) {
    if (!entry || entry.type !== "wavetable" || entry.lfoWavetable === enabled) return;
    entry.lfoWavetable = enabled;
    if (enabled) {
      try {
        entry.lfoOsc.disconnect(entry.lfoGain);
      } catch (err) {
      }
      if (entry.wtSource) entry.wtSource.connect(entry.lfoGain);
      else this._createLfoWavetableSource(entry);
    } else {
      if (entry.wtSource) {
        try {
          entry.wtSource.disconnect(entry.lfoGain);
        } catch (err) {
        }
      }
      entry.lfoOsc.connect(entry.lfoGain);
    }
  }

  _createLfoWavetableSource(entry) {
    const ctx = this.ensureContext();
    if (entry.wtSource || entry.wtPending) return;
    const attach = (source) => {
      entry.wtSource = source;
      this._syncLfoWavetableParams(entry);
      this._sendLfoWavetableTables(entry);
      if (entry.lfoWavetable) source.connect(entry.lfoGain);
    };
    const buildFallback = () => {
      const core = new WavetableLfoCore(ctx.sampleRate);
      core.setTables(entry.wtTables);
      const processor = ctx.createScriptProcessor(256, 1, 1);
      processor.onaudioprocess = (event) => {
        core.process(event.outputBuffer.getChannelData(0), entry.lfoRate ?? 0, entry.wtMorph ?? 0, (entry.lfoPhase ?? 0) / 360);
      };
      // A ScriptProcessor only runs while pulled towards the destination -
      // an LFO feeding nothing but AudioParams would otherwise stall.
      const sink = ctx.createGain();
      sink.gain.value = 0;
      processor.connect(sink);
      sink.connect(ctx.destination);
      entry.wtCore = core;
      entry.wtSink = sink;
      attach(processor);
    };
    const workletReady = this._loadWavetableWorklet();
    if (!workletReady) {
      buildFallback();
      return;
    }
    entry.wtPending = true;
    workletReady.then((ok) => {
      entry.wtPending = false;
      if (entry.wtSource || entry.removed) return;
      if (!ok) {
        buildFallback();
        return;
      }
      try {
        attach(new AudioWorkletNode(ctx, "wavetable-lfo", {
          numberOfInputs: 0,
          numberOfOutputs: 1,
          outputChannelCount: [1],
        }));
      } catch (err) {
        buildFallback();
      }
    });
  }

  _destroyLfoWavetableSource(entry) {
    entry.removed = true;
    if (entry.wtSource) {
      if (entry.wtSource.port) entry.wtSource.port.onmessage = null;
      if ("onaudioprocess" in entry.wtSource) entry.wtSource.onaudioprocess = null;
      try {
        entry.wtSource.disconnect();
      } catch (err) {
      }
    }
    if (entry.wtSink) entry.wtSink.disconnect();
    entry.wtSource = null;
    entry.wtSink = null;
    entry.wtCore = null;
  }

  _syncLfoWavetableParams(entry) {
    const source = entry && entry.wtSource;
    if (!source || !source.parameters || !this.ctx) return;
    const now = this.ctx.currentTime;
    source.parameters.get("frequency").setTargetAtTime(entry.lfoRate ?? 0, now, 0.01);
    source.parameters.get("morph").setTargetAtTime(entry.wtMorph ?? 0, now, 0.01);
    const phase = source.parameters.get("phaseOffset");
    phase.cancelScheduledValues(now);
    phase.setValueAtTime((entry.lfoPhase ?? 0) / 360, now);
  }

  _sendLfoWavetableTables(entry) {
    if (entry.wtCore) entry.wtCore.setTables(entry.wtTables);
    else if (entry.wtSource && entry.wtSource.port) entry.wtSource.port.postMessage({ type: "tables", tables: entry.wtTables });
  }

  _resetLfoWavetablePhase(entry) {
    if (entry.wtCore) entry.wtCore.reset();
    else if (entry.wtSource && entry.wtSource.port) entry.wtSource.port.postMessage({ type: "reset" });
  }

  _scheduleLfoOneShotStop(entry) {
    if (entry.lfoOneShotTimer) clearTimeout(entry.lfoOneShotTimer);
    this._setLfoRunning(entry, true);
    const rate = Math.max(0, Number(entry.lfoOsc.frequency.value) || 0);
    if (rate <= 0) return;
    entry.lfoOneShotTimer = setTimeout(() => {
      entry.lfoOneShotTimer = null;
      this._setLfoRunning(entry, false);
    }, 1000 / rate);
  }

  
  
  
  
  
  
  
  
  
  
  
  resetLfoPhase(id, voiceIndex) {
    const wrapper = this.nodes.get(id);
    if (!wrapper || !this.ctx) return;
    const resetOne = (entry) => {
      if (!entry || (entry.type !== "lfo" && entry.type !== "wavetable")) return;
      const oldOsc = entry.lfoOsc;
      const newOsc = this.ctx.createOscillator();
      setOscWaveType(this.ctx, newOsc, entry.lfoWaveType, (entry.lfoPhase ?? 0) / 360);
      newOsc.frequency.value = oldOsc.frequency.value;
      // For the Wavetable widget the wavetable source (not the oscillator) drives
      // lfoGain - restart its phase instead.
      if (!entry.lfoWavetable) newOsc.connect(entry.lfoGain);
      this._resetLfoWavetablePhase(entry);
      newOsc.start();
      try {
        oldOsc.disconnect();
        oldOsc.stop();
      } catch (err) {
        
      }
      entry.lfoOsc = newOsc;
      if (lfoLinearHold(entry)) {
        // Re-arm: ramp restarts from its start and holds at the end (no timer needed).
        applyLfoLinear(this.ctx, entry, true);
        entry.lfoOneShotRunning = true;
      } else if (entry.lfoOneShot) this._scheduleLfoOneShotStop(entry);
      else this._setLfoRunning(entry, true);
    };
    if (wrapper.voiced) {
      if (voiceIndex !== undefined && wrapper.instances[voiceIndex]) {
        resetOne(wrapper.instances[voiceIndex]);
      } else {
        wrapper.instances.forEach(resetOne);
      }
    } else {
      resetOne(wrapper);
    }
  }

  removeNode(id) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return;
    if (wrapper.voiced) {
      wrapper.instances.forEach((entry) => this._removeEntry(entry));
    } else {
      this._removeEntry(wrapper);
    }
    this._monitorRoutes = this._monitorRoutes.filter((route) => route.fromId !== id && route.toId !== id);
    
    
    
    for (const key of this._valueMonitorTaps.keys()) {
      if (key.startsWith(`${id}:`)) {
        this._valueMonitorTaps.get(key).analyser.disconnect();
        this._valueMonitorTaps.delete(key);
      }
    }
    this.nodes.delete(id);
  }

  _setParamOnEntry(entry, paramName, value, glideSeconds = 0.01) {
    if (!entry || !this.ctx) return;
    const now = this.ctx.currentTime;

    
    
    
    
    const applyRampValue = (audioParam, val, duration = glideSeconds) => {
      if (duration <= 0) {
        audioParam.cancelScheduledValues(now);
        audioParam.setValueAtTime(val, now);
      } else {
        audioParam.setTargetAtTime(val, now, duration);
      }
    };
    const applyLinearRampValue = (audioParam, val, duration) => {
      audioParam.cancelScheduledValues(now);
      if (duration <= 0) {
        audioParam.setValueAtTime(val, now);
      } else {
        audioParam.setValueAtTime(audioParam.value, now);
        audioParam.linearRampToValueAtTime(val, now + duration);
      }
    };
    const applyRamp = (audioParam) => applyRampValue(audioParam, value);

    switch (entry.type) {
      case "oscillator":
      case "fm-oscillator":
        if (paramName === "frequency") {
          entry.directHz = Number(value) || 0;
          if (!entry.pitchCvActive && !entry.tuneCvActive) {
            applyRamp(entry.freqConst ? entry.freqConst.offset : entry.osc.frequency);
          } else {
            this._refreshPitchChain(entry);
          }
        } else if (paramName === "tuneVolts") {
          entry.tuneRestConst.offset.value = Number(value);
          if (entry.pitchCvActive || entry.tuneCvActive) this._refreshPitchChain(entry);
        } else if (paramName === "waveType") {
          if (entry.type === "fm-oscillator") {
            // Read per block by createFmOscillatorCore.
            entry.waveType = value;
            break;
          }
          // Noise / linear ramps are separate sources wired straight into
          // ampGain; every other wave plays through the oscillator itself.
          const fromExtra = oscExtraSource(entry, entry.waveType);
          const toExtra = oscExtraSource(entry, value);
          if (!isNoiseWave(value)) setOscWaveType(this.ctx, entry.osc, value);
          if (fromExtra !== toExtra) {
            (fromExtra ?? entry.osc).disconnect(entry.ampGain);
            (toExtra ?? entry.osc).connect(entry.ampGain);
          }
          entry.waveType = value;
          if (isLinearWave(value)) applyOscLinear(this.ctx, entry);
        } else if (paramName === "detune") {
          // Plain oscillator only (detuneMul); FmOscillatorNode.h has none.
          if (entry.osc) applyRamp(entry.osc.detune);
        } else if (paramName === "amplitude") {
          applyRamp(entry.ampGain.gain);
        } else if (paramName === "gain") {
          entry.gain = value;
          if (!entry.gainCvActive) applyRampValue(entry.outGain.gain, ((Number(value) || 0) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS);
        } else if (paramName === "gate") {
          if (value > GATE_THRESHOLD_VOLTS) this.retriggerOscillatorEntry(entry);
        } else if (entry.type === "fm-oscillator") {
          // fallback constants feeding createFmOscillatorCore (raw volts)
          const cvInput = {
            fmIndex: [entry.fmIndexIn, "fmIndexCvActive"],
            feedback: [entry.feedbackIn, "feedbackCvActive"],
            superSawDetune: [entry.superSawDetuneIn, "superSawDetuneCvActive"],
            superSawMix: [entry.superSawMixIn, "superSawMixCvActive"],
          }[paramName];
          if (cvInput) {
            entry[paramName] = value;
            if (!entry[cvInput[1]]) applyRampValue(cvInput[0].fallback.offset, Number(value) || 0);
          } else if (paramName === "dutyCycle") {
            entry.dutyCycle = Number(value) || 0;
          } else if (paramName === "morph") {
            entry.morph = Number(value) || 0;
          } else if (paramName === "wavetables") {
            entry.wavetables = Array.isArray(value) ? value.map((t) => Float32Array.from(t)) : null;
          }
        } else if (paramName === "feedback") {
          entry.feedback = value;
          if (!entry.feedbackCvActive) {
            applyRampValue(entry.feedbackGain.gain, ((Number(value) || 0) / CV_MAX_VOLTS) * SELF_FEEDBACK_MAX_HZ_DEVIATION);
          }
        }
        break;
      case "hammond": {
        const db = /^drawbar([1-9])$/.exec(paramName);
        const rt = /^ratio([1-9])$/.exec(paramName);
        if (db) {
          const k = Number(db[1]) - 1;
          entry.drawbars[k] = value;
          // Only touch the AudioParam while no cable owns it - exactly the
          // oscillator gain case's `if (!entry.gainCvActive)` guard above.
          if (!entry.drawbarCvActive[k]) {
            applyRampValue(entry.drawbarGains[k].gain, (Number(value) || 0) / CV_MAX_VOLTS);
          }
        } else if (rt) {
          // Settings-dialog field, not a live-modulation target - a direct
          // value set (no ramp) is correct here.
          const k = Number(rt[1]) - 1;
          entry.ratios[k] = value;
          entry.footageScale[k].gain.value = value;
          if (!entry.pitchCvActive && !entry.tuneCvActive) {
            entry.oscs[k].frequency.value = (entry.directHz ?? 0) * value;
          }
        } else if (paramName === "frequency") {
          // Only when no live pitch/tune cable owns the shared pitch chain -
          // same guard as the oscillator case above, applied to all nine
          // oscillators (each scaled by its own ratio).
          entry.directHz = Number(value) || 0;
          if (!entry.pitchCvActive && !entry.tuneCvActive) {
            entry.oscs.forEach((o, k) => applyRampValue(o.frequency, value * entry.ratios[k]));
          } else {
            this._refreshPitchChain(entry);
          }
        } else if (paramName === "tuneVolts") {
          entry.tuneRestConst.offset.value = Number(value);
          if (entry.pitchCvActive || entry.tuneCvActive) this._refreshPitchChain(entry);
        } else if (paramName === "amplitude") {
          applyRamp(entry.ampGain.gain);
        } else if (paramName === "gain") {
          entry.gain = value;
          if (!entry.gainCvActive) applyRampValue(entry.outGain.gain, ((Number(value) || 0) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS);
        }
        // No "gate" case on purpose: free-running tonewheels are never
        // retriggered (see nodes/HammondNode.h - no phase reset on note-on).
        break;
      }
      case "audio-out":
    case "usb-out":
    case "es8311-out":
    case "wm8960-out":
        if (paramName === "muted") {
          entry.muteGain.gain.setTargetAtTime(value ? 0 : 1, now, 0.01);
        } else if (paramName === "gain") {
          entry.outputGain.gain.setTargetAtTime(Math.max(0, Math.min(1, (Number(value) || 0) / 10)), now, 0.01);
        } else if (paramName === "left" || paramName === "right") {
          const target = paramName === "left" ? entry.leftGain : entry.rightGain;
          applyRampValue(target.gain, (Number(value) || 0) / AUDIO_PEAK_VOLTS);
        }
        break;
      case "gain": {
        
        
        
        
        
        
        
        if (paramName === "amount") {
          entry.amount = value;
          if (!entry.cvActive && !entry.globalFollower) applyRampValue(entry.gainNode.gain, vcaAmountFromCv(entry.amount, "linear"));
        } else if (paramName === "curve") {
          entry.curve = value;
          if (!entry.curveCvActive && !entry.globalFollower) applyRampValue(entry.curveGainNode.gain, vcaAmountFromCv(entry.curve, "gainCurve"));
        }
        break;
      }
      case "linked-input":
      case "linked-input-4":
      case "linked-input-8":
        if (paramName === "linkId") entry.linkId = value;
        else if (paramName === "linkName") entry.linkName = value;
        break;
      case "linked-output":
      case "linked-output-4":
      case "linked-output-8":
        if (paramName === "sourceLinkId") entry.sourceLinkId = value;
        break;
      case "comb-filter": {
        const index = COMB_FILTER_PORTS.indexOf(paramName);
        if (index >= 0) {
          entry[paramName] = combFilterCv(value, COMB_FILTER_DEFAULTS[paramName]);
          entry.seeds[index].offset.value = entry[paramName];
        }
        break;
      }
      case "filter":
      case "curtis-filter": {
        // Everything is read per block by the processor (see createFilterDsp/
        // createCurtisFilterDsp); a change only has to mark the coefficients
        // dirty, like the firmware's setParam().
        const num = Number(value);
        if (["cutoff", "cutoffMinHz", "cutoffMaxHz", "resonance", "resonanceMinQ", "resonanceMaxQ"].includes(paramName)) {
          if (Number.isFinite(num)) entry[paramName] = num;
        } else if (paramName === "cutoffCurve" || paramName === "resonanceCurve") {
          entry[paramName] = value === "log" ? "log" : "linear";
        } else if (paramName === "filterType") {
          entry.filterType = normalizeFilterKind(value);
        } else if (paramName === "slope") {
          entry.slope = filterPolesFromSlope(num) * 6;
          entry.dsp.resetUpperStages();
          entry.dsp.audioCoeffsInitialized = false;
        } else if (paramName === "coeffStride") {
          entry.coeffStride = clampRange(Math.trunc(num) || 1, 1, 16);
          break;
        } else {
          break;
        }
        entry.coeffsDirty = true;
        entry.dsp.audioCoeffsInitialized = false;
        break;
      }
      case "esp-eq":
        if (paramName === "bypass") {
          entry.bypass = value === true || Number(value) >= 0.5;
          applyRampValue(entry.processedL.gain, entry.bypass ? 0 : 1);
          applyRampValue(entry.processedR.gain, entry.bypass ? 0 : 1);
          applyRampValue(entry.bypassL.gain, entry.bypass ? 1 : 0);
          applyRampValue(entry.bypassR.gain, entry.bypass ? 1 : 0);
        } else if (paramName === "frequency" || paramName === "q" || paramName === "gainDb" || paramName === "filterType") {
          entry[paramName] = value;
          this._applyEspEqParams(entry, glideSeconds);
        }
        break;
      case "graphic-eq": {
        const bandGainMatch = /^gain(\d)$/.exec(paramName);
        const bandFreqMatch = /^freq(\d)$/.exec(paramName);
        if (paramName === "bypass") {
          entry.bypass = value === true || Number(value) >= 0.5;
          applyRampValue(entry.processedL.gain, entry.bypass ? 0 : 1);
          applyRampValue(entry.processedR.gain, entry.bypass ? 0 : 1);
          applyRampValue(entry.bypassL.gain, entry.bypass ? 1 : 0);
          applyRampValue(entry.bypassR.gain, entry.bypass ? 1 : 0);
        } else if (paramName === "gain") {
          const g = clampRange((Number(value) || 0) / CV_MAX_VOLTS, 0, 1);
          applyRampValue(entry.outputGainL.gain, g);
          applyRampValue(entry.outputGainR.gain, g);
        } else if (bandGainMatch) {
          const i = Number(bandGainMatch[1]) - 1;
          entry.gainDb[i] = Number(value) || 0;
          const gainDb = clampRange(entry.gainDb[i], -15, 15);
          applyRampValue(entry.bandsL[i].gain, gainDb);
          applyRampValue(entry.bandsR[i].gain, gainDb);
        } else if (bandFreqMatch) {
          const i = Number(bandFreqMatch[1]) - 1;
          entry.freqHz[i] = Number(value) || entry.freqHz[i];
          const hz = clampRange(entry.freqHz[i], 20, this.ctx.sampleRate / 2 - 1);
          applyRampValue(entry.bandsL[i].frequency, hz);
          applyRampValue(entry.bandsR[i].frequency, hz);
        }
        break;
      }
      case "frequency-divider":
        
        
        
        if (paramName === "divider") {
          const divider = clampRange(Number(value) || 1, 1, 128);
          applyRampValue(entry.seedDen.offset, divider, 0);
        }
        break;
      case "value-mapper": {
        
        
        
        
        
        entry.point1In = paramName === "point1In" ? value : entry.point1In;
        entry.point1Out = paramName === "point1Out" ? value : entry.point1Out;
        entry.point2In = paramName === "point2In" ? value : entry.point2In;
        entry.point2Out = paramName === "point2Out" ? value : entry.point2Out;
        entry.constraintMin = paramName === "constraintMin" ? value : entry.constraintMin;
        entry.constraintMax = paramName === "constraintMax" ? value : entry.constraintMax;
        if (paramName === "listText") entry.list = parseValueList(value);
        break;
      }
      case "detune":
        if (paramName === "amount") {
          entry.amount = clampRange(Number(value) ?? CV_MAX_VOLTS / 2, 0, CV_MAX_VOLTS);
        } else if (paramName === "maxOffsetCents") {
          entry.maxOffsetCents = Number(value) || 0;
        } else if (paramName === "stepCents") {
          entry.stepCents = Number(value) || 0;
        } else {
          break;
        }
        applyRampValue(entry.deltaConst.offset, detuneOffsetVolts(entry.amount, entry.maxOffsetCents, entry.stepCents));
        break;
      case "mixer":
        if (paramName === "outputGain") {
          applyRamp(entry.sum.gain);
        } else if (paramName.startsWith("gain")) {
          
          
          
          const g = entry.inputGains.get(`in${paramName.slice(4)}`);
          if (g) applyRamp(g.gain);
        }
        break;
      case "multiplexer":
        if (paramName === "select") {
          const selected = Math.max(0, Math.min(15, Math.round(value)));
          entry.selected = selected;
          for (let i = 1; i <= 16; i += 1) {
            const g = entry.inputGains.get(`in${i}`);
            if (g) applyLinearRampValue(g.gain, i - 1 === selected ? 1 : 0, entry.softFadeMs / 1000);
          }
        } else if (paramName === "softFadeMs") {
          entry.softFadeMs = Math.max(0, Math.min(100, Number(value) || 0));
        }
        break;
      case "output-multiplexer":
        if (paramName === "select") {
          const selected = Math.max(0, Math.min(15, Math.round(value)));
          entry.selected = selected;
          for (let i = 1; i <= 16; i += 1) {
            const g = entry.outputGains.get(`out${i}`);
            if (g) applyLinearRampValue(g.gain, i - 1 === selected ? 1 : 0, 0);
          }
        }
        break;
      case "adsr":
        if (paramName === "attack" || paramName === "decay" || paramName === "release") {
          entry[paramName] = Math.max(value, 0.001);
        } else if (paramName === "sustain") {
          entry.sustain = value;
        } else if (paramName === "velocity") {
          entry.velocityGain.gain.value = clampRange(value / CV_MAX_VOLTS, 0, 1);
        } else if (paramName === "gate") {
          const g = entry.envGain.gain;
          g.cancelScheduledValues(now);
          g.setValueAtTime(g.value, now);
          if (value >= GATE_THRESHOLD_VOLTS) {
            entry.releaseEndsAt = null;
            g.linearRampToValueAtTime(1, now + entry.attack);
            g.linearRampToValueAtTime(entry.sustain, now + entry.attack + entry.decay);
          } else {
            entry.releaseEndsAt = now + entry.release;
            g.linearRampToValueAtTime(0, now + entry.release);
          }
        }
        break;
      case "dx-envelope":
        if (/^level[1-4]$/.test(paramName)) {
          entry[paramName] = clampRange(Number(value) || 0, 0, CV_MAX_VOLTS);
        } else if (/^time[1-4]$/.test(paramName)) {
          entry[paramName] = Math.max(Number(value) || 0, 0.001);
        } else if (paramName === "velocity") {
          entry.velocityGain.gain.value = clampRange(value / CV_MAX_VOLTS, 0, 1);
        } else if (paramName === "gate") {
          const gain = entry.envGain.gain;
          gain.cancelScheduledValues(now);
          gain.setValueAtTime(gain.value, now);
          if (value >= GATE_THRESHOLD_VOLTS) {
            let segmentEnd = now + entry.time1;
            gain.linearRampToValueAtTime(entry.level1 / CV_MAX_VOLTS, segmentEnd);
            segmentEnd += entry.time2;
            gain.linearRampToValueAtTime(entry.level2 / CV_MAX_VOLTS, segmentEnd);
            segmentEnd += entry.time3;
            gain.linearRampToValueAtTime(entry.level3 / CV_MAX_VOLTS, segmentEnd);
            entry.releaseEndsAt = null;
          } else {
            gain.linearRampToValueAtTime(entry.level4 / CV_MAX_VOLTS, now + entry.time4);
            entry.releaseEndsAt = entry.level4 > 0 ? Number.POSITIVE_INFINITY : now + entry.time4;
          }
        }
        break;
      case "lfo":
      case "wavetable": {
        if (paramName === "rate") {
          const rate = clampRange(Number(value) || 0, 0, entry.lfoMaxRate ?? 50);
          entry.lfoRate = rate;
          applyRampValue(entry.lfoOsc.frequency, rate);
          this._syncLfoWavetableParams(entry);
        } else if (paramName === "maxRate") {
          entry.lfoMaxRate = clampRange(Number(value) || 0, 0, LFO_MAX_RATE_HZ);
          const rate = clampRange(Number(entry.lfoRate) || 0, 0, entry.lfoMaxRate);
          entry.lfoRate = rate;
          applyRampValue(entry.lfoOsc.frequency, rate);
          this._syncLfoWavetableParams(entry);
        } else if (paramName === "wavetables") {
          if (Array.isArray(value) && value.length > 0) {
            entry.wtTables = value.map((t) => Float32Array.from(t));
            this._sendLfoWavetableTables(entry);
          }
        } else if (paramName === "morph") {
          entry.wtMorph = clampRange(Number(value) || 0, 0, CV_MAX_VOLTS);
          if (entry.wtSource && entry.wtSource.parameters) applyRampValue(entry.wtSource.parameters.get("morph"), entry.wtMorph);
        } else if (paramName === "gain") {
          entry.lfoAmplitude = clampRange(Number(value) || 0, 0, CV_MAX_VOLTS);
        } else if (paramName === "phase") {
          // Takes effect immediately, like LfoNode.h's read-time phase
          // offset: swapping the wave keeps the oscillator's running phase.
          entry.lfoPhase = value;
          setOscWaveType(this.ctx, entry.lfoOsc, entry.lfoWaveType, (Number(value) || 0) / 360);
          this._syncLfoWavetableParams(entry);
        } else if (paramName === "zeroPointFixed") {
          entry.lfoZeroPointFixed = value === true || value === 1;
        } else if (paramName === "zeroPoint") {
          entry.lfoZeroPoint = value;
        } else if (paramName === "oneShot") {
          const enabled = value === true || value === 1;
          if (enabled !== entry.lfoOneShot) {
            entry.lfoOneShot = enabled;
            if (entry.lfoOneShotTimer) {
              clearTimeout(entry.lfoOneShotTimer);
              entry.lfoOneShotTimer = null;
            }
            if (enabled && isLinearWave(entry.lfoWaveType)) {
              // Entering One Shot with a linear wave: held at the ramp start until the next reset.
              entry.lfoLinearPos0 = 0;
              entry.lfoLinearT0 = now;
              entry.lfoLinearRunning = false;
            }
            this._setLfoRunning(entry, !enabled);
          }
        } else if (paramName === "waveType") {
          if (lfoLinearHold(entry)) {
            // Leaving the held ramp: drop it before normal LFO automation resumes.
            entry.centerOffset.offset.cancelScheduledValues(now);
            entry.lfoGain.gain.cancelScheduledValues(now);
          }
          setOscWaveType(this.ctx, entry.lfoOsc, value, (entry.lfoPhase ?? 0) / 360);
          entry.lfoWaveType = value;
          if (lfoLinearHold(entry)) {
            // Picking a linear wave in One Shot: held at its start until the next reset.
            entry.lfoLinearPos0 = 0;
            entry.lfoLinearT0 = now;
            entry.lfoLinearRunning = false;
          }
        }
        if (paramName === "gain" || paramName === "phase" || paramName === "zeroPointFixed" || paramName === "zeroPoint" || paramName === "waveType" ||
            paramName === "wavetables" || paramName === "morph") {
          const halfSwing = entry.lfoAmplitude / 2;
          const center = entry.lfoZeroPointFixed ? entry.lfoZeroPoint : halfSwing;
          if (entry.lfoOneShot && !entry.lfoOneShotRunning) {
            applyRampValue(entry.lfoGain.gain, 0);
            applyRampValue(entry.centerOffset.offset, center + lfoHeldRaw(entry) * halfSwing);
          } else {
            applyRampValue(entry.lfoGain.gain, halfSwing);
            applyRampValue(entry.centerOffset.offset, center);
          }
        }
        if (lfoLinearHold(entry) &&
            ["rate", "maxRate", "gain", "phase", "zeroPointFixed", "zeroPoint", "waveType"].includes(paramName)) {
          applyLfoLinear(this.ctx, entry, false);
        }
        break;
      }
      case "const":
      case "up-down-button":
        if (paramName === "value") applyRamp(entry.constant.offset);
        break;
      case "button": {
        const match = /^value(\d*)$/.exec(paramName);
        if (match) {
          const index = match[1] ? Number(match[1]) - 1 : 0;
          const node = entry.constants?.[index];
          if (node) applyRampValue(node.offset, value);
        }
        break;
      }
      case "knob": {
        // "value" (index 0) or "value2".."value128" (see main.js's
        // knobValueParamName) - each control owns its own ConstantSourceNode
        // (see _buildNode's "knob" case), unlike const/button/up-down-button
        // above which only ever have one.
        const match = /^value(\d*)$/.exec(paramName);
        if (match) {
          const index = match[1] ? Number(match[1]) - 1 : 0;
          const node = entry.constants?.[index];
          if (node) applyRampValue(node.offset, value);
        }
        break;
      }
      case "switch":
        if (paramName === "selectIn2") {
          applyRampValue(entry.gain1.gain, value ? 0 : 1);
          applyRampValue(entry.gain2.gain, value ? 1 : 0);
        }
        break;
      case "output-switch":
        if (paramName === "selectOut2") entry.selectOut2 = value === true || Number(value) >= 0.5;
        break;
      case "audio-input-link": {
        const match = /^gain([1-8])$/.exec(paramName);
        if (!match) break;
        const channel = Number(match[1]) - 1;
        const gainVolts = clampRange(Number(value) || 0, 0, CV_MAX_VOLTS);
        entry.linkGainVolts[channel] = gainVolts;
        if (entry.linkGainConnectionCount[channel] === 0) {
          applyRampValue(entry.nodes[channel].gain, gainVolts / CV_MAX_VOLTS);
        }
        break;
      }
      case "bitcrusher":
        if (paramName === "crush") entry.crush = Math.max(0, Math.min(10, Number(value) || 0));
        else if (paramName === "bypass") entry.bypass = value === true || Number(value) >= 0.5;
        break;
      case "adder": {
        // Keeps a control-rate source (e.g. a Const) wired into one input
        // reaching the real sum once this node is forced into audio mode
        // by a genuinely audio-rate cable on another input - see the
        // "adder" _buildNode case and _connectEntries/_disconnectEntries's
        // seed swap (controlConnectionCounts[idx] === 0 means this index
        // isn't currently carrying a real audio-rate connection).
        const idx = parseInt(String(paramName).replace("in", ""), 10) - 1;
        if (idx >= 0 && idx < entry.inputCount && entry.controlConnectionCounts[idx] === 0) {
          applyRampValue(entry.seeds[idx].offset, Number(value) || 0);
        }
        break;
      }
      case "mda-piano": {
        const idx = MDA_PIANO_PORTS.indexOf(paramName);
        if (idx >= 0 && entry.controlConnectionCounts[idx] === 0) {
          const v = typeof value === "boolean" ? (value ? 1 : 0) : Number(value) || 0;
          const safe = clampRange(v, 0, CV_MAX_VOLTS);
          entry.controlValues[idx] = safe;
          applyRampValue(entry.seeds[idx].offset, safe);
        }
        break;
      }
      case "multiplier": {
        // Same reasoning as "adder" above.
        const idx = parseInt(String(paramName).replace("in", ""), 10) - 1;
        if (idx < 0 || idx >= entry.inputCount || entry.controlConnectionCounts[idx] !== 0) break;
        if (idx === 0) applyRampValue(entry.seed.offset, Number(value) || 1);
        else applyRampValue(entry.stages[idx - 1].node.gain, Number(value) || 1);
        break;
      }
      case "divide": {
        // Same reasoning as "adder" above.
        if (paramName === "in1" && entry.controlConnectionCounts[0] === 0) {
          applyRampValue(entry.seedNum.offset, Number(value) || 0);
        } else if (paramName === "in2" && entry.controlConnectionCounts[1] === 0) {
          applyRampValue(entry.seedDen.offset, Number(value) || 1);
        }
        break;
      }
      case "wave-folder": {
        const index = ["drive", "symmetry", "gain", "mix"].indexOf(paramName);
        if (index >= 0) {
          const safe = clampRange(Number(value) || 0, 0, CV_MAX_VOLTS);
          entry.controlValues[index] = safe;
          if (entry.controlConnectionCounts[index] === 0) applyRampValue(entry.controlSources[index].offset, safe);
        }
        break;
      }
      case "envelope-follower": {
        const index = ["attack", "release", "preGain"].indexOf(paramName);
        if (index >= 0) {
          const safe = clampRange(Number(value) || 0, 0, CV_MAX_VOLTS);
          entry.controlValues[index] = safe;
          if (entry.controlConnectionCounts[index] === 0) applyRampValue(entry.controlSources[index].offset, safe);
        }
        break;
      }
      case "burst-generator": {
        const index = ["mode", "count", "time", "decay"].indexOf(paramName);
        if (index >= 0) {
          const safe = clampRange(Number(value) || 0, 0, CV_MAX_VOLTS);
          entry.controlValues[index] = safe;
          if (entry.controlConnectionCounts[index] === 0) applyRampValue(entry.controlSources[index].offset, safe);
        }
        break;
      }
      case "pan":
        if (paramName === "pan") {
          entry.panValue = clampRange(Number(value) || 0, 0, CV_MAX_VOLTS);
          if (entry.panConnectionCount === 0) applyRampValue(entry.panSource.offset, entry.panValue);
        }
        break;
      case "sample-hold":
        if (paramName === "threshold") {
          entry.threshold = clampRange(Number(value) || 0, 0, CV_MAX_VOLTS);
          if (entry.thresholdConnectionCount === 0) applyRampValue(entry.thresholdSource.offset, entry.threshold);
        }
        break;
      case "chorus":
        if (paramName === "bypass") {
          entry.bypass = value === true || Number(value) >= 0.5;
          applyRampValue(entry.processedL.gain, entry.bypass ? 0 : 1);
          applyRampValue(entry.processedR.gain, entry.bypass ? 0 : 1);
          applyRampValue(entry.bypassL.gain, entry.bypass ? 1 : 0);
          applyRampValue(entry.bypassR.gain, entry.bypass ? 1 : 0);
        } else if (paramName === "rate") {
          applyRamp(entry.lfo.frequency);
        } else if (paramName === "offset") {
          applyRamp(entry.delayL.delayTime);
          applyRamp(entry.delayR.delayTime);
        } else if (paramName === "depth") {
          applyRampValue(entry.depthGainL.gain, value);
          applyRampValue(entry.depthGainR.gain, -value);
        }
        break;
      case "delay":
        if (paramName === "bypass") {
          entry.bypass = value === true || Number(value) >= 0.5;
          applyRampValue(entry.processedL.gain, entry.bypass ? 0 : 1);
          applyRampValue(entry.processedR.gain, entry.bypass ? 0 : 1);
          applyRampValue(entry.bypassL.gain, entry.bypass ? 1 : 0);
          applyRampValue(entry.bypassR.gain, entry.bypass ? 1 : 0);
        } else if (paramName === "delayTimeMs") {
          applyRampValue(entry.delayL.delayTime, value / 1000);
          applyRampValue(entry.delayR.delayTime, value / 1000);
        } else if (paramName === "feedback") {
          applyRamp(entry.feedbackGainL.gain);
          applyRamp(entry.feedbackGainR.gain);
        } else if (paramName === "mix") {
          const [dryGain, wetGain] = this._delayMixGains(value);
          applyRampValue(entry.dryGainL.gain, dryGain);
          applyRampValue(entry.wetGainL.gain, wetGain);
          applyRampValue(entry.dryGainR.gain, dryGain);
          applyRampValue(entry.wetGainR.gain, wetGain);
        }
        break;
      case "wave-player":
        if (paramName === "note") {
          entry.note = value;
          if (entry.source) {
            
            
            entry.source.playbackRate.value = 2 ** ((entry.note - entry.rootNoteMidi) / 12);
          }
        } else if (paramName === "gate") {
          if (value > GATE_THRESHOLD_VOLTS) {
            this._retriggerWavePlayerEntry(entry);
          } else {
            
            
            
            entry.releaseGain.gain.cancelScheduledValues(now);
            entry.releaseGain.gain.setValueAtTime(entry.releaseGain.gain.value, now);
            entry.releaseGain.gain.linearRampToValueAtTime(0, now + 0.02);
          }
        } else if (paramName === "rootNoteMidi") {
          entry.rootNoteMidi = value;
        } else if (paramName === "sampleFile") {
          entry.sampleFile = value;
          this._loadWavePlayerBuffer(entry);
        } else if (paramName === "amplitude") {
          applyRamp(entry.ampGain.gain);
        } else if (paramName === "gain") {
          entry.gain = value;
          if (!entry.gainCvActive) applyRampValue(entry.outGain.gain, ((Number(value) || 0) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS);
        }
        break;
      case "reverb-light": {
        // Read per block by the processor (see the "reverb-light" _buildNode
        // case), exactly like setParamReverbLight() in ReverbNode.h.
        const rv = entry.reverb;
        const num = Number(value);
        if (paramName === "bypass") {
          rv.state.bypass = value === true || num >= 0.5;
        } else if (paramName === "roomSize" && Number.isFinite(num)) {
          rv.setRoomSize(num);
        } else if (paramName === "damping" && Number.isFinite(num)) {
          rv.setDamping(num);
        } else if (paramName === "wetLevelDb" && Number.isFinite(num)) {
          rv.state.wetGain = 10 ** (num / 20);
        } else if (paramName === "dryLevelDb" && Number.isFinite(num)) {
          rv.state.dryGain = 10 ** (num / 20);
        }
        break;
      }
      default:
        break;
    }
  }

  
  
  setParam(id, paramName, value, glideSeconds = 0.01) {
    const wrapper = this.nodes.get(id);
    if (!wrapper) return;
    if (wrapper.voiced) {
      
      
      
      if (
        paramName === "global" &&
        (wrapper.type === "lfo" || wrapper.type === "wavetable" || wrapper.type === "gain" || wrapper.type === "filter" || wrapper.type === "curtis-filter")
      ) {
        this._setGlobalMode(wrapper, value === true);
        return;
      }
      wrapper.instances.forEach((entry) => this._setParamOnEntry(entry, paramName, value, glideSeconds));
    } else {
      this._setParamOnEntry(wrapper, paramName, value, glideSeconds);
    }
  }

  
  
  
  
  
  
  
  
  _setGlobalMode(wrapper, isGlobal) {
    if (!this.ctx || wrapper.global === isGlobal) return;
    wrapper.global = isGlobal;
    const primary = wrapper.instances[0];
    const now = this.ctx.currentTime;
    for (let v = 1; v < wrapper.instances.length; v += 1) {
      const entry = wrapper.instances[v];
      if (wrapper.type === "lfo" || wrapper.type === "wavetable") {
        if (isGlobal && !entry._sharedWired) {
          primary.rawOut.connect(entry.sharedGate);
          entry._sharedWired = true;
        }
        entry.ownGate.gain.setTargetAtTime(isGlobal ? 0 : 1, now, 0.005);
        entry.sharedGate.gain.setTargetAtTime(isGlobal ? 1 : 0, now, 0.005);
      } else if (wrapper.type === "gain") {
        if (isGlobal && !entry._sharedWired) {
          primary.waveShaper.connect(entry.sharedGate);
          primary.curveCv.waveShaper.connect(entry.curveSharedGate);
          entry._sharedWired = true;
        }
        entry.ownGate.gain.setTargetAtTime(isGlobal ? 0 : 1, now, 0.005);
        entry.sharedGate.gain.setTargetAtTime(isGlobal ? 1 : 0, now, 0.005);
        entry.curveOwnGate.gain.setTargetAtTime(isGlobal ? 0 : 1, now, 0.005);
        entry.curveSharedGate.gain.setTargetAtTime(isGlobal ? 1 : 0, now, 0.005);

        entry.globalFollower = isGlobal;
        if (isGlobal) {
          entry.gainNode.gain.setTargetAtTime(0, now, 0.005);
          entry.curveGainNode.gain.setTargetAtTime(0, now, 0.005);
        } else {
          if (!entry.cvActive) entry.gainNode.gain.setTargetAtTime(vcaAmountFromCv(entry.amount, "linear"), now, 0.005);
          if (!entry.curveCvActive) entry.curveGainNode.gain.setTargetAtTime(vcaAmountFromCv(entry.curve, "gainCurve"), now, 0.005);
        }
      } else if (wrapper.type === "filter" || wrapper.type === "curtis-filter") {
        if (isGlobal && !entry._sharedWired) {
          primary.cutoffCv.waveShaper.connect(entry.cutoffSharedGate);
          primary.resonanceCv.waveShaper.connect(entry.resonanceSharedGate);
          entry._sharedWired = true;
        }
        entry.globalPrimary = isGlobal ? primary : null;
        entry.coeffsDirty = true;
        entry.dsp.audioCoeffsInitialized = false;
        entry.cutoffOwnGate.gain.setTargetAtTime(isGlobal ? 0 : 1, now, 0.005);
        entry.cutoffSharedGate.gain.setTargetAtTime(isGlobal ? 1 : 0, now, 0.005);
        entry.resonanceOwnGate.gain.setTargetAtTime(isGlobal ? 0 : 1, now, 0.005);
        entry.resonanceSharedGate.gain.setTargetAtTime(isGlobal ? 1 : 0, now, 0.005);
      }
    }
  }

  
  
  setVoiceParam(id, voiceIndex, paramName, value, glideSeconds = 0.01) {
    const wrapper = this.nodes.get(id);
    const entry = wrapper && wrapper.voiced ? wrapper.instances[voiceIndex] : null;
    if (entry) this._setParamOnEntry(entry, paramName, value, glideSeconds);
  }

  // True while any connected ADSR instance at this voice index is still
  // ramping out its release (or held) - i.e. picking this slot for a new
  // note-on right now would audibly cut off a still-sounding voice. Mirrors
  // VoiceAllocator's use of Graph::voiceStillSounding on the C++ side (see
  // allocateVoice() in main.js). Not tied to a specific midi-in widget: like
  // the C++ side, voice index is treated as a single shared namespace across
  // the whole patch.
  isVoiceStillSounding(voiceIndex) {
    if (!this.ctx) return false;
    const now = this.ctx.currentTime;
    for (const wrapper of this.nodes.values()) {
      if (!wrapper.voiced || (wrapper.type !== "adsr" && wrapper.type !== "dx-envelope")) continue;
      const entry = wrapper.instances[voiceIndex];
      if (entry && entry.releaseEndsAt !== null && entry.releaseEndsAt > now) return true;
    }
    return false;
  }

  _resolveOutputFromEntry(entry, port) {
    if (!entry) return null;
    if (entry.type === "oscillator" && port === "value") return entry.outGain;
    if (entry.type === "fm-oscillator" && port === "value") return entry.outGain;
    if (entry.type === "hammond" && port === "value") return entry.outGain;
    if ((entry.type === "lfo" || entry.type === "wavetable") && port === "value") return entry.output;
    if (entry.type === "switch" && port === "value") return entry.output;
    if (entry.type === "output-switch") {
      if (port === "out1") return entry.gain1;
      if (port === "out2") return entry.gain2;
    }
    
    
    
    if (entry.type === "const" && port === "audio") return entry.constant;
    if (entry.type === "knob") {
      // Layer 1 uses voltageN; later layers use voltageNLayerM.
      const match = /^voltage(\d*)(?:Layer(\d+))?$/.exec(port);
      if (match) {
        const knobIndex = match[1] ? Number(match[1]) - 1 : 0;
        const layerIndex = match[2] ? Number(match[2]) - 1 : 0;
        const index = layerIndex * 16 + knobIndex;
        return entry.constants?.[index] || null;
      }
    }
    if (entry.type === "button") {
      const match = /^value(\d+)(?:Layer(\d+))?$/.exec(port);
      if (match) {
        const buttonIndex = Number(match[1]) - 1;
        const layerIndex = match[2] ? Number(match[2]) - 1 : 0;
        return entry.constants?.[layerIndex * 16 + buttonIndex] || null;
      }
    }
    if (entry.type === "gain" && port === "out") return entry.curveGainNode;
    if (entry.type.startsWith("linked-output")) {
      if (port === "out") return entry.gainNodes?.[0] || null;
      const match = /^out([1-8])$/.exec(port);
      if (match) return entry.gainNodes?.[Number(match[1]) - 1] || null;
    }
    if (["filter", "curtis-filter", "comb-filter"].includes(entry.type) && port === "out") {
      return entry.filterOutput || entry.filterNode;
    }
    if (entry.type.startsWith("esp-") && (port === "outL" || port === "outR")) return port === "outL" ? entry.outputL : entry.outputR;
    if (entry.type === "graphic-eq" && (port === "outL" || port === "outR")) return port === "outL" ? entry.outputL : entry.outputR;
    if (entry.type === "mixer" && port === "out") return entry.sum;
    if (entry.type === "multiplexer" && port === "value") return entry.output;
    if (entry.type === "output-multiplexer") return entry.outputGains.get(port) || null;
    if (entry.type === "multiplier" && port === "value") return entry.output;
    if (entry.type === "adder" && port === "value") return entry.sum;
    if (entry.type === "divide" && port === "value") return entry.output;
    if (entry.type === "frequency-divider" && (port === "cv" || port === "hz")) return entry.output;
    if (entry.type === "abs" && port === "value") return entry.waveShaper;
    if (entry.type === "logic-and" && port === "value") return entry.output;
    if (entry.type === "logic-or" && port === "value") return entry.output;
    if (entry.type === "logic-xor" && port === "value") return entry.output;
    if (entry.type === "logic-not" && port === "value") return entry.waveShaper;
    if (entry.type === "value-mapper" && port === "out") return entry.processor;
    if (entry.type === "detune" && port === "out") return entry.out;
    if (entry.type === "adsr" && port === "out") return entry.output;
    if (entry.type === "dx-envelope" && port === "out") return entry.output;
    if (entry.type === "bitcrusher" && port === "out") return entry.processor;
    if (entry.type === "wave-folder" && port === "out") return entry.processor;
    if (entry.type === "envelope-follower" && port === "out") return entry.processor;
    if (entry.type === "burst-generator" && port === "out") return entry.processor;
    if (entry.type === "pan" && port === "out1") return entry.out1;
    if (entry.type === "pan" && port === "out2") return entry.out2;
    if (entry.type === "sample-hold" && port === "out") return entry.processor;
    if (entry.type === "chorus" && port === "left") return entry.outputL;
    if (entry.type === "chorus" && port === "right") return entry.outputR;
    if (entry.type === "delay" || entry.type === "reverb-light") {
      if (port === "outL") return entry.outputL;
      if (port === "outR") return entry.outputR;
    }
    if (entry.type === "audio-input-link") {
      const match = /^out([1-8])$/.exec(port);
      if (match) return entry.nodes[Number(match[1]) - 1];
      if (port === "outL") return entry.nodes[0]; // legacy preset compatibility
      if (port === "outR") return entry.nodes[1];
    }
    if (entry.type === "wave-player" && port === "left") return entry.leftOut;
    if (entry.type === "wave-player" && port === "right") return entry.rightOut;
    if (entry.type === "soundfont-player" && port === "left") return entry.leftOut;
    if (entry.type === "soundfont-player" && port === "right") return entry.rightOut;
    if (entry.type === "mda-piano" && port === "left") return entry.leftOut;
    if (entry.type === "mda-piano" && port === "right") return entry.rightOut;
    return null;
  }

  _resolveInputFromEntry(entry, port) {
    if (!entry) return null;
    if (entry.type === "audio-out" || entry.type === "usb-out" || entry.type === "es8311-out" ||
      entry.type === "wm8960-out") {
      if (port === "left") return entry.leftGain;
      if (port === "right") return entry.rightGain;
    }
    if (entry.type === "gain" && port === "in") return entry.gainNode;
    if (entry.type.startsWith("linked-input")) {
      if (port === "in") return entry.gainNodes?.[0] || null;
      const match = /^in([1-8])$/.exec(port);
      if (match) return entry.gainNodes?.[Number(match[1]) - 1] || null;
    }
    
    
    
    
    if (entry.type === "gain" && port === "amount") return entry.prescale;
    if (entry.type === "gain" && port === "curve") return entry.curveCv.prescale;
    if (["filter", "curtis-filter", "comb-filter"].includes(entry.type) && port === "in") return entry.filterInput;
    
    
    
    
    if ((entry.type === "filter" || entry.type === "curtis-filter") && port === "cutoff") return entry.cutoffCv.prescale;
    if ((entry.type === "filter" || entry.type === "curtis-filter") && port === "resonance") return entry.resonanceCv.prescale;
    if (entry.type.startsWith("esp-") && (port === "inL" || port === "inR")) return port === "inL" ? entry.inputL : entry.inputR;
    if (entry.type === "graphic-eq" && (port === "inL" || port === "inR")) return port === "inL" ? entry.inputL : entry.inputR;
    if (entry.type === "mixer") return entry.inputGains.get(port) || null;
    if (entry.type === "multiplexer") return entry.inputGains.get(port) || null;
    if (entry.type === "switch") {
      if (port === "in1") return entry.gain1;
      if (port === "in2") return entry.gain2;
    }
    if (entry.type === "output-switch" && port === "in") return entry.input;
    if (entry.type === "output-multiplexer" && port === "in") return entry.input;
    if (entry.type === "oscillator") {
      
      
      
      
      if (port === "amplitude") return entry.ampCv.prescale;
      
      
      
      if (port === "gain") return entry.gainCv.prescale;
      
      
      
      
      if (port === "pitchCv") return entry.pitchSum;
      if (port === "tune") return entry.tuneSum;
      if (port === "feedback") return entry.feedbackCv.prescale;
    }
    if (entry.type === "fm-oscillator") {
      if (port === "amplitude") return entry.ampCv.prescale;
      if (port === "gain") return entry.gainCv.prescale;
      if (port === "pitchCv") return entry.pitchSum;
      if (port === "tune") return entry.tuneSum;
      if (port === "fmIn") return entry.fmInScale;
      if (port === "fmIndex") return entry.fmIndexIn.input;
      if (port === "feedback") return entry.feedbackIn.input;
      if (port === "superSawDetune") return entry.superSawDetuneIn.input;
      if (port === "superSawMix") return entry.superSawMixIn.input;
    }
    if (entry.type === "hammond") {
      if (port === "amplitude") return entry.ampCv.prescale;
      if (port === "gain") return entry.gainCv.prescale;
      if (port === "pitchCv") return entry.pitchSum;
      if (port === "tune") return entry.tuneSum;
      const m = /^drawbar([1-9])$/.exec(port);
      if (m) return entry.drawbarCv[Number(m[1]) - 1].prescale;
    }
    if (entry.type === "frequency-divider" || entry.type === "abs" || entry.type === "value-mapper") {
      
      
      
      if (entry.type === "frequency-divider" && (port === "cv" || port === "hz")) return entry.outGain;
      if (entry.type === "frequency-divider" && port === "divider") return entry.prescale;
      if (entry.type === "abs" && port === "in") return entry.waveShaper;
      if (entry.type === "value-mapper" && port === "in") return entry.input;
    }
    if (entry.type === "logic-or" && /^in\d+$/.test(port)) return entry.perInputShapers[Number(port.slice(2)) - 1] || null;
    if (entry.type === "logic-not" && port === "in") return entry.waveShaper;
    if (entry.type === "detune" && port === "cv") return entry.cvIn;
    if (entry.type === "bitcrusher" && port === "in") return entry.processor;
    if (entry.type === "wave-folder" && ["in", "drive", "symmetry", "gain", "mix"].includes(port)) return entry.inputMerger;
    if (entry.type === "envelope-follower" && ["in", "attack", "release", "preGain"].includes(port)) return entry.inputMerger;
    if (entry.type === "burst-generator" && ["in", "mode", "count", "time", "decay"].includes(port)) return entry.inputMerger;
    if (entry.type === "pan" && (port === "in" || port === "pan")) return entry.inputMerger;
    if (entry.type === "sample-hold" && (port === "in" || port === "trigger" || port === "threshold")) return entry.inputMerger;
    if (entry.type === "chorus" && port === "in") return entry.inputNode;
    if (entry.type === "delay") {
      if (port === "inL") return entry.inputL;
      if (port === "inR") return entry.inputR;
    }
    if (entry.type === "reverb-light") {
      if (port === "inL") return entry.inputNodeL;
      if (port === "inR") return entry.inputNodeR;
    }
    if (entry.type === "audio-output-link") {
      const match = /^in([1-8])$/.exec(port);
      if (match) return entry.nodes[Number(match[1]) - 1];
      if (port === "inL") return entry.nodes[0]; // legacy preset compatibility
      if (port === "inR") return entry.nodes[1];
    }
    if (entry.type === "audio-input-link") {
      const match = /^gain([1-8])$/.exec(port);
      if (match) return entry.linkGainPrescales[Number(match[1]) - 1];
    }
    if (entry.type === "vu-meter" && port === "in") return entry.analyser;
    if (entry.type === "oscilloscope") {
      if (port === "in1") return entry.analyser1;
      if (port === "in2") return entry.analyser2;
    }
    if (entry.type === "spectrum" && port === "in") return entry.analyser;
    if (entry.type === "wave-player" && port === "amplitude") return entry.ampGain.gain;
    if (entry.type === "soundfont-player" && port === "gain") return entry.gainCv.prescale;
    return null;
  }

  
  
  
  
  
  
  
  
  
  _resolveScaledOutput(entry, port, gainScale) {
    const raw = this._resolveOutputFromEntry(entry, port);
    if (!raw || gainScale >= 1) return raw;

    entry._voiceCompGains = entry._voiceCompGains || {};
    if (!entry._voiceCompGains[port]) {
      const compGain = this.ctx.createGain();
      compGain.gain.value = gainScale;
      raw.connect(compGain);
      entry._voiceCompGains[port] = compGain;
    }
    return entry._voiceCompGains[port];
  }

  _connectEntries(fromEntry, fromPort, toEntry, toPort, gainScale = 1) {
    if (toEntry?.type === "comb-filter" && COMB_FILTER_PORTS.includes(toPort)) {
      this._wireCombFilterInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), true);
      return;
    }
    if (toEntry && toEntry.type === "wave-folder") {
      const inputIndex = { in: 0, drive: 1, symmetry: 2, gain: 3, mix: 4 }[toPort];
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from === undefined || !from || inputIndex === undefined) return;
      try {
        if (inputIndex > 0 && toEntry.controlConnectionCounts[inputIndex - 1]++ === 0) {
          toEntry.controlSources[inputIndex - 1].disconnect(toEntry.inputMerger, 0, inputIndex);
        }
        from.connect(toEntry.inputMerger, 0, inputIndex);
      } catch (err) {
        if (inputIndex > 0 && --toEntry.controlConnectionCounts[inputIndex - 1] <= 0) {
          toEntry.controlConnectionCounts[inputIndex - 1] = 0;
          toEntry.controlSources[inputIndex - 1].connect(toEntry.inputMerger, 0, inputIndex);
        }
      }
      return;
    }
    if (toEntry && toEntry.type === "sample-hold") {
      const inputIndex = { in: 0, trigger: 1, threshold: 2 }[toPort];
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from === undefined || !from || inputIndex === undefined) return;
      try {
        if (inputIndex === 2 && toEntry.thresholdConnectionCount++ === 0) {
          toEntry.thresholdSource.disconnect(toEntry.inputMerger, 0, 2);
        }
        from.connect(toEntry.inputMerger, 0, inputIndex);
      } catch (err) {
        if (inputIndex === 2 && --toEntry.thresholdConnectionCount <= 0) {
          toEntry.thresholdConnectionCount = 0;
          toEntry.thresholdSource.connect(toEntry.inputMerger, 0, 2);
        }
      }
      return;
    }
    if (toEntry && toEntry.type === "envelope-follower") {
      const inputIndex = { in: 0, attack: 1, release: 2, preGain: 3 }[toPort];
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from === undefined || !from || inputIndex === undefined) return;
      try {
        if (inputIndex > 0 && toEntry.controlConnectionCounts[inputIndex - 1]++ === 0) {
          toEntry.controlSources[inputIndex - 1].disconnect(toEntry.inputMerger, 0, inputIndex);
        }
        from.connect(toEntry.inputMerger, 0, inputIndex);
      } catch (err) {
        if (inputIndex > 0 && --toEntry.controlConnectionCounts[inputIndex - 1] <= 0) {
          toEntry.controlConnectionCounts[inputIndex - 1] = 0;
          toEntry.controlSources[inputIndex - 1].connect(toEntry.inputMerger, 0, inputIndex);
        }
      }
      return;
    }
    if (toEntry && toEntry.type === "burst-generator") {
      const inputIndex = { in: 0, mode: 1, count: 2, time: 3, decay: 4 }[toPort];
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from === undefined || !from || inputIndex === undefined) return;
      try {
        if (inputIndex > 0 && toEntry.controlConnectionCounts[inputIndex - 1]++ === 0) {
          toEntry.controlSources[inputIndex - 1].disconnect(toEntry.inputMerger, 0, inputIndex);
        }
        from.connect(toEntry.inputMerger, 0, inputIndex);
      } catch (err) {
        if (inputIndex > 0 && --toEntry.controlConnectionCounts[inputIndex - 1] <= 0) {
          toEntry.controlConnectionCounts[inputIndex - 1] = 0;
          toEntry.controlSources[inputIndex - 1].connect(toEntry.inputMerger, 0, inputIndex);
        }
      }
      return;
    }
    if (toEntry && toEntry.type === "pan") {
      const inputIndex = { in: 0, pan: 1 }[toPort];
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from === undefined || !from || inputIndex === undefined) return;
      try {
        if (inputIndex === 1 && toEntry.panConnectionCount++ === 0) {
          toEntry.panSource.disconnect(toEntry.inputMerger, 0, 1);
        }
        from.connect(toEntry.inputMerger, 0, inputIndex);
      } catch (err) {
        if (inputIndex === 1 && --toEntry.panConnectionCount <= 0) {
          toEntry.panConnectionCount = 0;
          toEntry.panSource.connect(toEntry.inputMerger, 0, 1);
        }
      }
      return;
    }
    if (toEntry && toEntry.type === "adder") {
      this._wireAdderInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), true);
      return;
    }
    if (toEntry && toEntry.type === "mda-piano" && MDA_PIANO_PORTS.includes(toPort)) {
      this._wireMdaPianoInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), true);
      return;
    }
    if (toEntry && toEntry.type === "multiplier") {
      this._wireMultiplierInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), true);
      return;
    }
    if (toEntry && toEntry.type === "divide") {
      this._wireDivideInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), true);
      return;
    }
    if (toEntry && toEntry.type === "frequency-divider") {
      this._wireFrequencyDividerInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), true);
      return;
    }
    if (toEntry && toEntry.type === "logic-and") {
      this._wireLogicAndInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), true);
      return;
    }
    if (toEntry && toEntry.type === "logic-xor") {
      this._wireLogicXorInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), true);
      return;
    }
    if (toEntry && toEntry.type === "audio-input-link" && /^gain[1-8]$/.test(toPort)) {
      const channel = Number(toPort.slice(4)) - 1;
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.connect(toEntry.linkGainPrescales[channel]);
          toEntry.linkGainConnectionCount[channel] += 1;
          toEntry.nodes[channel].gain.value = 0;
        } catch (err) {
        }
      }
      return;
    }
    if (toEntry && toEntry.type === "wave-player" && toPort === "amplitude") {
      
      
      
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          toEntry.ampGain.gain.value = 0;
          from.connect(toEntry.ampGain.gain);
        } catch (err) {
          
        }
      }
      return;
    }
    if (toEntry && (toEntry.type === "oscillator" || toEntry.type === "hammond" || toEntry.type === "fm-oscillator") && toPort === "amplitude") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          toEntry.ampGain.gain.value = 0;
          from.connect(toEntry.ampCv.prescale);
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && (toEntry.type === "oscillator" || toEntry.type === "hammond" || toEntry.type === "fm-oscillator" || toEntry.type === "wave-player" || toEntry.type === "soundfont-player") && toPort === "gain") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.connect(toEntry.gainCv.prescale);
          toEntry.gainCvActive = true;
          const now = this.ctx.currentTime;
          (toEntry.outGain ? [toEntry.outGain] : [toEntry.leftOut, toEntry.rightOut]).forEach((node) => {
            node.gain.cancelScheduledValues(now);
            node.gain.value = 0;
          });
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && toEntry.type === "oscillator" && toPort === "feedback") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.connect(toEntry.feedbackCv.prescale);
          toEntry.feedbackCvActive = true;
          toEntry.feedbackGain.gain.value = 0;
        } catch (err) {
        }
      }
      return;
    }
    if (toEntry && toEntry.type === "fm-oscillator" && FM_CV_INPUT_PORTS.includes(toPort)) {
      // A cable replaces the port's fallback constant (gate closed).
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      const cv = this._fmCvInput(toEntry, toPort);
      if (from && cv) {
        try {
          from.connect(cv.input);
          cv.gate.gain.value = 0;
          toEntry[`${toPort}CvActive`] = true;
        } catch (err) {
        }
      }
      return;
    }
    if (toEntry && toEntry.type === "hammond" && /^drawbar[1-9]$/.test(toPort)) {
      const k = Number(toPort.slice(7)) - 1;
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.connect(toEntry.drawbarCv[k].prescale);
          toEntry.drawbarCvActive[k] = true;
          toEntry.drawbarGains[k].gain.value = 0;
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && toEntry.type === "gain" && toPort === "amount") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.connect(toEntry.prescale);
          toEntry.cvActive = true;
          toEntry.gainNode.gain.value = 0;
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && toEntry.type === "gain" && toPort === "curve") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.connect(toEntry.curveCv.prescale);
          toEntry.curveCvActive = true;
          toEntry.curveGainNode.gain.value = 0;
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && (toEntry.type === "oscillator" || toEntry.type === "hammond" || toEntry.type === "fm-oscillator") && (toPort === "pitchCv" || toPort === "tune")) {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        const sum = toPort === "pitchCv" ? toEntry.pitchSum : toEntry.tuneSum;
        try {
          from.connect(sum);
          if (toPort === "pitchCv") toEntry.pitchCvActive = true;
          else toEntry.tuneCvActive = true;
          this._refreshPitchChain(toEntry);
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && (toEntry.type === "filter" || toEntry.type === "curtis-filter") && (toPort === "cutoff" || toPort === "resonance")) {
      
      
      
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        const cv = toPort === "cutoff" ? toEntry.cutoffCv : toEntry.resonanceCv;
        try {
          from.connect(cv.prescale);
          if (toPort === "cutoff") toEntry.cutoffCvActive = true;
          else toEntry.resonanceCvActive = true;
          toEntry.dsp.audioCoeffsInitialized = false;
        } catch (err) {
          
        }
      }
      return;
    }

  const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
    const to = this._resolveInputFromEntry(toEntry, toPort);
    if (from && to) {
      try {
        from.connect(to);
      } catch (err) {
        
      }
    }
  }

  _disconnectEntries(fromEntry, fromPort, toEntry, toPort, gainScale = 1) {
    if (toEntry?.type === "comb-filter" && COMB_FILTER_PORTS.includes(toPort)) {
      this._wireCombFilterInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), false);
      return;
    }
    if (toEntry && toEntry.type === "wave-folder") {
      const inputIndex = { in: 0, drive: 1, symmetry: 2, gain: 3, mix: 4 }[toPort];
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from === undefined || !from || inputIndex === undefined) return;
      try {
        from.disconnect(toEntry.inputMerger, 0, inputIndex);
        if (inputIndex > 0 && --toEntry.controlConnectionCounts[inputIndex - 1] <= 0) {
          toEntry.controlConnectionCounts[inputIndex - 1] = 0;
          toEntry.controlSources[inputIndex - 1].connect(toEntry.inputMerger, 0, inputIndex);
        }
      } catch (err) {
      }
      return;
    }
    if (toEntry && toEntry.type === "sample-hold") {
      const inputIndex = { in: 0, trigger: 1, threshold: 2 }[toPort];
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from === undefined || !from || inputIndex === undefined) return;
      try {
        from.disconnect(toEntry.inputMerger, 0, inputIndex);
        if (inputIndex === 2 && --toEntry.thresholdConnectionCount <= 0) {
          toEntry.thresholdConnectionCount = 0;
          toEntry.thresholdSource.connect(toEntry.inputMerger, 0, 2);
        }
      } catch (err) {
      }
      return;
    }
    if (toEntry && toEntry.type === "envelope-follower") {
      const inputIndex = { in: 0, attack: 1, release: 2, preGain: 3 }[toPort];
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from === undefined || !from || inputIndex === undefined) return;
      try {
        from.disconnect(toEntry.inputMerger, 0, inputIndex);
        if (inputIndex > 0 && --toEntry.controlConnectionCounts[inputIndex - 1] <= 0) {
          toEntry.controlConnectionCounts[inputIndex - 1] = 0;
          toEntry.controlSources[inputIndex - 1].connect(toEntry.inputMerger, 0, inputIndex);
        }
      } catch (err) {
      }
      return;
    }
    if (toEntry && toEntry.type === "burst-generator") {
      const inputIndex = { in: 0, mode: 1, count: 2, time: 3, decay: 4 }[toPort];
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from === undefined || !from || inputIndex === undefined) return;
      try {
        from.disconnect(toEntry.inputMerger, 0, inputIndex);
        if (inputIndex > 0 && --toEntry.controlConnectionCounts[inputIndex - 1] <= 0) {
          toEntry.controlConnectionCounts[inputIndex - 1] = 0;
          toEntry.controlSources[inputIndex - 1].connect(toEntry.inputMerger, 0, inputIndex);
        }
      } catch (err) {
      }
      return;
    }
    if (toEntry && toEntry.type === "pan") {
      const inputIndex = { in: 0, pan: 1 }[toPort];
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from === undefined || !from || inputIndex === undefined) return;
      try {
        from.disconnect(toEntry.inputMerger, 0, inputIndex);
        if (inputIndex === 1 && --toEntry.panConnectionCount <= 0) {
          toEntry.panConnectionCount = 0;
          toEntry.panSource.connect(toEntry.inputMerger, 0, 1);
        }
      } catch (err) {
      }
      return;
    }
    if (toEntry && toEntry.type === "adder") {
      this._wireAdderInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), false);
      return;
    }
    if (toEntry && toEntry.type === "mda-piano" && MDA_PIANO_PORTS.includes(toPort)) {
      this._wireMdaPianoInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), false);
      return;
    }
    if (toEntry && toEntry.type === "multiplier") {
      this._wireMultiplierInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), false);
      return;
    }
    if (toEntry && toEntry.type === "divide") {
      this._wireDivideInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), false);
      return;
    }
    if (toEntry && toEntry.type === "frequency-divider") {
      this._wireFrequencyDividerInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), false);
      return;
    }
    if (toEntry && toEntry.type === "logic-and") {
      this._wireLogicAndInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), false);
      return;
    }
    if (toEntry && toEntry.type === "logic-xor") {
      this._wireLogicXorInput(toEntry, toPort, this._resolveScaledOutput(fromEntry, fromPort, gainScale), false);
      return;
    }
    if (toEntry && toEntry.type === "audio-input-link" && /^gain[1-8]$/.test(toPort)) {
      const channel = Number(toPort.slice(4)) - 1;
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.disconnect(toEntry.linkGainPrescales[channel]);
          toEntry.linkGainConnectionCount[channel] = Math.max(0, toEntry.linkGainConnectionCount[channel] - 1);
          if (toEntry.linkGainConnectionCount[channel] === 0) {
            toEntry.nodes[channel].gain.value = toEntry.linkGainVolts[channel] / CV_MAX_VOLTS;
          }
        } catch (err) {
        }
      }
      return;
    }
    if (toEntry && toEntry.type === "wave-player" && toPort === "amplitude") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.disconnect(toEntry.ampGain.gain);
          toEntry.ampGain.gain.value = 1; 
        } catch (err) {
          
        }
      }
      return;
    }
    if (toEntry && (toEntry.type === "oscillator" || toEntry.type === "hammond" || toEntry.type === "fm-oscillator") && toPort === "amplitude") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.disconnect(toEntry.ampCv.prescale);
          toEntry.ampGain.gain.value = 1;
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && (toEntry.type === "oscillator" || toEntry.type === "hammond" || toEntry.type === "fm-oscillator" || toEntry.type === "wave-player" || toEntry.type === "soundfont-player") && toPort === "gain") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.disconnect(toEntry.gainCv.prescale);
          toEntry.gainCvActive = false;
          const g = ((toEntry.gain ?? CV_MAX_VOLTS) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS;
          (toEntry.outGain ? [toEntry.outGain] : [toEntry.leftOut, toEntry.rightOut]).forEach((node) => {
            node.gain.value = g;
          });
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && toEntry.type === "oscillator" && toPort === "feedback") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.disconnect(toEntry.feedbackCv.prescale);
          toEntry.feedbackCvActive = false;
          toEntry.feedbackGain.gain.value = ((toEntry.feedback ?? 0) / CV_MAX_VOLTS) * SELF_FEEDBACK_MAX_HZ_DEVIATION;
        } catch (err) {
        }
      }
      return;
    }
    if (toEntry && toEntry.type === "fm-oscillator" && FM_CV_INPUT_PORTS.includes(toPort)) {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      const cv = this._fmCvInput(toEntry, toPort);
      if (from && cv) {
        try {
          from.disconnect(cv.input);
          cv.gate.gain.value = 1;
          cv.fallback.offset.value = Number(toEntry[toPort]) || 0;
          toEntry[`${toPort}CvActive`] = false;
        } catch (err) {
        }
      }
      return;
    }
    if (toEntry && toEntry.type === "hammond" && /^drawbar[1-9]$/.test(toPort)) {
      const k = Number(toPort.slice(7)) - 1;
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.disconnect(toEntry.drawbarCv[k].prescale);
          toEntry.drawbarCvActive[k] = false;
          toEntry.drawbarGains[k].gain.value = (toEntry.drawbars[k] ?? 0) / CV_MAX_VOLTS;
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && toEntry.type === "gain" && toPort === "amount") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.disconnect(toEntry.prescale);
          toEntry.cvActive = false;
          toEntry.gainNode.gain.value = vcaAmountFromCv(toEntry.amount, "linear");
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && toEntry.type === "gain" && toPort === "curve") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.disconnect(toEntry.curveCv.prescale);
          toEntry.curveCvActive = false;
          toEntry.curveGainNode.gain.value = vcaAmountFromCv(toEntry.curve, "gainCurve");
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && (toEntry.type === "oscillator" || toEntry.type === "hammond" || toEntry.type === "fm-oscillator") && (toPort === "pitchCv" || toPort === "tune")) {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        const sum = toPort === "pitchCv" ? toEntry.pitchSum : toEntry.tuneSum;
        try {
          from.disconnect(sum);
          if (toPort === "pitchCv") toEntry.pitchCvActive = false;
          else toEntry.tuneCvActive = false;
          this._refreshPitchChain(toEntry);
        } catch (err) {
          
        }
      }
      return;
    }
    if (toEntry && (toEntry.type === "filter" || toEntry.type === "curtis-filter") && (toPort === "cutoff" || toPort === "resonance")) {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        const cv = toPort === "cutoff" ? toEntry.cutoffCv : toEntry.resonanceCv;
        try {
          from.disconnect(cv.prescale);
          if (toPort === "cutoff") toEntry.cutoffCvActive = false;
          else toEntry.resonanceCvActive = false;
          toEntry.coeffsDirty = true;
        } catch (err) {
          
        }
      }
      return;
    }

    const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
    const to = this._resolveInputFromEntry(toEntry, toPort);
    if (from && to) {
      try {
        from.disconnect(to);
      } catch (err) {
        
      }
    }
  }

  
  
  
  
  
  
  connect(fromId, fromPort, toId, toPort) {
    const fromWrapper = this.nodes.get(fromId);
    const toWrapper = this.nodes.get(toId);
    if (!fromWrapper || !toWrapper) return;

    const fromVoiced = !!fromWrapper.voiced;
    const toVoiced = !!toWrapper.voiced;

    if (fromVoiced && toVoiced) {
      const count = Math.min(fromWrapper.instances.length, toWrapper.instances.length);
      for (let v = 0; v < count; v += 1) {
        this._connectEntries(fromWrapper.instances[v], fromPort, toWrapper.instances[v], toPort, 1);
      }
    } else if (fromVoiced && !toVoiced) {
      if (toWrapper.type === "vu-meter" || toWrapper.type === "oscilloscope") {
        const index = Math.min(this._monitorVoiceIndex, fromWrapper.instances.length - 1);
        this._connectEntries(fromWrapper.instances[index], fromPort, toWrapper, toPort, 1);
        this._monitorRoutes.push({ fromId, fromPort, toId, toPort, connectedIndex: index });
        return;
      }
      
      
      
      
      
      
      
      const scale = 1 / Math.sqrt(fromWrapper.instances.length);
      fromWrapper.instances.forEach((entry) => this._connectEntries(entry, fromPort, toWrapper, toPort, scale));
    } else if (!fromVoiced && toVoiced) {
      toWrapper.instances.forEach((entry) => this._connectEntries(fromWrapper, fromPort, entry, toPort, 1));
    } else {
      this._connectEntries(fromWrapper, fromPort, toWrapper, toPort, 1);
    }
  }

  disconnect(fromId, fromPort, toId, toPort) {
    const fromWrapper = this.nodes.get(fromId);
    const toWrapper = this.nodes.get(toId);
    if (!fromWrapper || !toWrapper) return;

    const fromVoiced = !!fromWrapper.voiced;
    const toVoiced = !!toWrapper.voiced;

    if (fromVoiced && toVoiced) {
      const count = Math.min(fromWrapper.instances.length, toWrapper.instances.length);
      for (let v = 0; v < count; v += 1) {
        this._disconnectEntries(fromWrapper.instances[v], fromPort, toWrapper.instances[v], toPort, 1);
      }
    } else if (fromVoiced && !toVoiced) {
      if (toWrapper.type === "vu-meter" || toWrapper.type === "oscilloscope") {
        const routeIndex = this._monitorRoutes.findIndex(
          (route) => route.fromId === fromId && route.fromPort === fromPort &&
            route.toId === toId && route.toPort === toPort,
        );
        const connectedIndex = routeIndex >= 0 ? this._monitorRoutes[routeIndex].connectedIndex : 0;
        this._disconnectEntries(fromWrapper.instances[connectedIndex], fromPort, toWrapper, toPort, 1);
        if (routeIndex >= 0) this._monitorRoutes.splice(routeIndex, 1);
        return;
      }
      
      
      
      const scale = 1 / Math.sqrt(fromWrapper.instances.length);
      fromWrapper.instances.forEach((entry) => this._disconnectEntries(entry, fromPort, toWrapper, toPort, scale));
    } else if (!fromVoiced && toVoiced) {
      toWrapper.instances.forEach((entry) => this._disconnectEntries(fromWrapper, fromPort, entry, toPort, 1));
    } else {
      this._disconnectEntries(fromWrapper, fromPort, toWrapper, toPort, 1);
    }
  }

  setMonitorVoice(voiceIndex) {
    this._monitorVoiceIndex = voiceIndex;
    this._monitorRoutes.forEach((route) => {
      const fromWrapper = this.nodes.get(route.fromId);
      const toWrapper = this.nodes.get(route.toId);
      if (!fromWrapper || !fromWrapper.voiced || !toWrapper) return;
      const nextIndex = Math.min(voiceIndex, fromWrapper.instances.length - 1);
      if (nextIndex === route.connectedIndex) return;
      this._disconnectEntries(fromWrapper.instances[route.connectedIndex], route.fromPort, toWrapper, route.toPort, 1);
      this._connectEntries(fromWrapper.instances[nextIndex], route.fromPort, toWrapper, route.toPort, 1);
      route.connectedIndex = nextIndex;
    });
  }

  // Pitch source of oscillator/fm-oscillator/hammond, mirroring the
  // firmware's hz = hzFromPitchCv(pitchCv + (tune - 5V)): with no pitch/tune
  // cable the direct-set Hz (setParam "frequency") drives the oscillator(s);
  // as soon as either port has a cable the CV chain is the sole source and
  // the port WITHOUT a cable contributes its current value through its rest
  // constant - the note's pitch volts (recovered from the last direct-set Hz)
  // or the widget's tune volts.
  _refreshPitchChain(entry) {
    const cvDriven = entry.pitchCvActive || entry.tuneCvActive;
    const tuneVolts = entry.tuneRestConst.offset.value;
    const directHz = entry.directHz ?? 0;
    const pitchVolts = directHz > 0
      ? (12 * Math.log2(directHz / 440) + 69 - PITCH_CV_ANCHOR_NOTE) / 12 * PITCH_CV_VOLTS_PER_OCTAVE +
        CV_MAX_VOLTS / 2 - (tuneVolts - CV_MAX_VOLTS / 2)
      : CV_MAX_VOLTS / 2;
    entry.pitchRestConst.offset.value = pitchVolts;
    entry.pitchRestGate.gain.value = cvDriven && !entry.pitchCvActive ? 1 : 0;
    entry.tuneRestGate.gain.value = cvDriven && !entry.tuneCvActive ? 1 : 0;
    const hz = cvDriven ? 0 : directHz;
    if (entry.oscs) entry.oscs.forEach((o, k) => { o.frequency.value = hz * entry.ratios[k]; });
    else if (entry.freqConst) entry.freqConst.offset.value = hz;
    else if (entry.osc) entry.osc.frequency.value = hz;
  }

  _fmCvInput(entry, port) {
    return { fmIndex: entry.fmIndexIn, feedback: entry.feedbackIn,
      superSawDetune: entry.superSawDetuneIn, superSawMix: entry.superSawMixIn }[port] || null;
  }

  _wireAdderInput(entry, port, sourceNode, connecting) {
    if (!sourceNode) return;
    const idx = parseInt(String(port).replace("in", ""), 10) - 1;
    if (Number.isNaN(idx) || idx < 0 || idx >= entry.inputCount) return;
    const counts = entry.controlConnectionCounts;
    const seed = entry.seeds[idx];
    try {
      if (connecting) {
        if (counts[idx]++ === 0) seed.disconnect(entry.sum);
        sourceNode.connect(entry.sum);
      } else {
        sourceNode.disconnect(entry.sum);
        if (--counts[idx] <= 0) {
          counts[idx] = 0;
          seed.connect(entry.sum);
        }
      }
    } catch (err) {

    }
  }

  _wireCombFilterInput(entry, port, sourceNode, connecting) {
    const index = COMB_FILTER_PORTS.indexOf(port);
    if (!sourceNode || index < 0) return;
    const seed = entry.seeds[index];
    const count = entry.controlConnectionCounts[index];
    if (connecting) {
      sourceNode.connect(entry.inputMerger, 0, index + 1);
      if (count === 0) seed.disconnect(entry.inputMerger, 0, index + 1);
      entry.controlConnectionCounts[index] = count + 1;
    } else {
      sourceNode.disconnect(entry.inputMerger, 0, index + 1);
      entry.controlConnectionCounts[index] = Math.max(0, count - 1);
      if (count === 1) seed.connect(entry.inputMerger, 0, index + 1);
    }
  }

  _wireMdaPianoInput(entry, port, sourceNode, connecting) {
    if (!sourceNode) return;
    const idx = MDA_PIANO_PORTS.indexOf(port);
    if (idx < 0) return;
    const counts = entry.controlConnectionCounts;
    const seed = entry.seeds[idx];
    try {
      if (connecting) {
        if (counts[idx]++ === 0) seed.disconnect(entry.inputMerger, 0, idx);
        sourceNode.connect(entry.inputMerger, 0, idx);
      } else {
        sourceNode.disconnect(entry.inputMerger, 0, idx);
        if (--counts[idx] <= 0) {
          counts[idx] = 0;
          seed.connect(entry.inputMerger, 0, idx);
        }
      }
    } catch (err) {

    }
  }

  _wireMultiplierInput(entry, port, sourceNode, connecting) {
    if (!sourceNode) return;
    const idx = parseInt(String(port).replace("in", ""), 10);
    if (Number.isNaN(idx) || idx < 1 || idx > entry.inputCount) return;
    const counts = entry.controlConnectionCounts;

    try {
      if (idx === 1) {
        const stage0 = entry.stages[0];
        if (connecting) {
          if (counts[0]++ === 0) entry.seed.disconnect(stage0.node);
          sourceNode.connect(stage0.node);
        } else {
          sourceNode.disconnect(stage0.node);
          if (--counts[0] <= 0) {
            counts[0] = 0;
            entry.seed.connect(stage0.node);
          }
        }
        return;
      }

      const stage = entry.stages[idx - 2];
      if (!stage) return;
      if (connecting) {
        if (counts[idx - 1]++ === 0) stage.node.gain.value = 1;
        sourceNode.connect(stage.node.gain);
      } else {
        sourceNode.disconnect(stage.node.gain);
        if (--counts[idx - 1] <= 0) {
          counts[idx - 1] = 0;
          stage.node.gain.value = 1;
        }
      }
    } catch (err) {

    }
  }

  _wireDivideInput(entry, port, sourceNode, connecting) {
    if (!sourceNode) return;
    const idx = port === "in1" ? 0 : port === "in2" ? 1 : -1;
    if (idx < 0) return;
    const counts = entry.controlConnectionCounts;
    const seed = idx === 0 ? entry.seedNum : entry.seedDen;
    try {
      if (connecting) {
        if (counts[idx]++ === 0) seed.disconnect(entry.inputMerger, 0, idx);
        sourceNode.connect(entry.inputMerger, 0, idx);
      } else {
        sourceNode.disconnect(entry.inputMerger, 0, idx);
        if (--counts[idx] <= 0) {
          counts[idx] = 0;
          seed.connect(entry.inputMerger, 0, idx);
        }
      }
    } catch (err) {

    }
  }

  _wireLogicAndInput(entry, port, sourceNode, connecting) {
    if (!sourceNode) return;
    const idx = parseInt(String(port).replace("in", ""), 10);
    if (Number.isNaN(idx) || idx < 1 || idx > entry.inputCount) return;
    const shaper = entry.perInputShapers[idx - 1];
    const seed = entry.seeds[idx - 1];
    if (!shaper || !seed) return;
    try {
      if (connecting) {
        seed.disconnect(shaper);
        sourceNode.connect(shaper);
      } else {
        sourceNode.disconnect(shaper);
        seed.connect(shaper);
      }
    } catch (err) {

    }
  }

  _wireLogicXorInput(entry, port, sourceNode, connecting) {
    if (!sourceNode) return;
    const target = port === "in1" ? entry.bitA : port === "in2" ? entry.bitB : null;
    const seed = port === "in1" ? entry.seedA : port === "in2" ? entry.seedB : null;
    if (!target || !seed) return;
    try {
      if (connecting) {
        seed.disconnect(target);
        sourceNode.connect(target);
      } else {
        sourceNode.disconnect(target);
        seed.connect(target);
      }
    } catch (err) {

    }
  }






  _wireFrequencyDividerInput(entry, port, sourceNode, connecting) {
    if (!sourceNode) return;
    try {
      if (port === "cv" || port === "hz") {
        if (connecting) {
          entry.seedNum.disconnect(entry.outGain);
          sourceNode.connect(entry.outGain);
        } else {
          sourceNode.disconnect(entry.outGain);
          entry.seedNum.connect(entry.outGain);
        }
      } else if (port === "divider") {
        if (connecting) {
          entry.seedDen.disconnect(entry.prescale);
          sourceNode.connect(entry.prescale);
        } else {
          sourceNode.disconnect(entry.prescale);
          entry.seedDen.connect(entry.prescale);
        }
      }
    } catch (err) {
      
    }
  }
}
