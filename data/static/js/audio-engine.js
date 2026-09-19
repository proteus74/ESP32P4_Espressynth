

import { parseSf2, resolvePreset, decibelsToGain, cents2Hertz } from "./sf2-engine.js";

function clampRange(value, lo, hi) {
  return value < lo ? lo : value > hi ? hi : value;
}

function isNoiseWave(waveType) {
  return waveType === "noise" || waveType === "pink-noise";
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
// Nominal peak amplitude of the AUDIO signal domain (oscillator/Hammond/
// sample-player waveform values), as opposed to CV_MAX_VOLTS (the CV
// domain) - mirrors cv::kAudioPeakVolts in CvConventions.h 1:1.
const AUDIO_PEAK_VOLTS = 5;
const LINK_BUS_COUNT = 8;
const GATE_THRESHOLD_VOLTS = 5;
const ANALYSER_BLACKMAN_COHERENT_GAIN = 0.42;
// fm-oscillator preview: WebAudio's OscillatorNode has no phase-modulation
// input (only frequency/detune AudioParams), so the browser approximates the
// firmware's true phase modulation (see nodes/FmOscillatorNode.h's
// kFmIndexMaxPhaseDeviation) with plain linear frequency modulation instead -
// the modulator signal, scaled by this many Hz at max fmIndex/full-scale
// modulator amplitude, is summed directly into the carrier's osc.frequency.
// Expect the browser preview and real hardware to sound similar but not
// identical for this node; the firmware is the authoritative output.
const FM_INDEX_MAX_HZ_DEVIATION = 2000;
// WebAudio preview approximation for firmware's one-sample phase feedback.
// One delayed raw sample modulates carrier frequency; hardware remains the
// authoritative phase-modulation implementation.
const SELF_FEEDBACK_MAX_HZ_DEVIATION = 2000;
const SUPER_SAW_CENTS = [-28, -14, -7, 7, 14, 28];

function superSawWeights(volts) {
  const mix = clampRange((Number(volts) || 0) / CV_MAX_VOLTS, 0, 1);
  const norm = 1 / (1 + 6 * mix);
  return { center: norm, side: mix * norm };
}



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







function cvToRange(volts, minVal, maxVal, curve) {
  const v = clampRange(volts / CV_MAX_VOLTS, 0, 1);
  if (curve === "log") {
    const lo = minVal <= 0 ? 0.0001 : minVal;
    return lo * (maxVal / lo) ** v;
  }
  return minVal + v * (maxVal - minVal);
}

// Slope in dB per octave -> how many cascaded biquad sections the preview
// engine runs. The firmware resolves this per pole (6 dB each); the browser
// only approximates, rounding up to whole 12 dB sections.
const FILTER_MAX_SECTIONS = 3;

function filterSectionCount(slopeDb) {
  const poles = Math.round((Number(slopeDb) || 12) / 6);
  return clampRange(Math.ceil(poles / 2), 1, FILTER_MAX_SECTIONS);
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
function buildVcaCurve(curve) {
  const samples = new Float32Array(VCA_CURVE_SAMPLES);
  for (let i = 0; i < VCA_CURVE_SAMPLES; i += 1) {
    const v = i / (VCA_CURVE_SAMPLES - 1); 
    samples[i] = curve === "exponential" ? (10 ** v - 1) / 9 : curve === "gainCurve" ? v * 2 : v;
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







function valueMapperDomain(p) {
  const lo = Math.min(p.point1In, p.point2In);
  const hi = Math.max(p.point1In, p.point2In);
  const span = hi - lo;
  const margin = span > 0 ? span * 0.5 : 1;
  return { lo: lo - margin, hi: hi + margin };
}

const VALUE_MAPPER_CURVE_SAMPLES = 1024;
function buildValueMapperCurve(p, domainLo, domainHi) {
  const samples = new Float32Array(VALUE_MAPPER_CURVE_SAMPLES);
  for (let i = 0; i < VALUE_MAPPER_CURVE_SAMPLES; i += 1) {
    const v = domainLo + ((domainHi - domainLo) * i) / (VALUE_MAPPER_CURVE_SAMPLES - 1);
    samples[i] = mapValueMapperRaw(v, p);
  }
  return samples;
}




function eqFilterTypeToBiquad(filterType) {
  return filterType === "peak" ? "peaking" : filterType || "peaking";
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













const LFO_WAVE_PHASE_CORRECTION_CYCLES = { triangle: 0.75 };

function lfoStartOffsetSeconds(waveType, phaseDeg, rate) {
  const correction = LFO_WAVE_PHASE_CORRECTION_CYCLES[waveType] || 0;
  return (phaseDeg / 360 + correction) / rate;
}

function lfoWaveValueAtPhase(waveType, phaseDeg) {
  const phase = ((phaseDeg / 360) % 1 + 1) % 1;
  if (waveType === "square") return phase < 0.5 ? 1 : -1;
  if (waveType === "triangle") return 4 * Math.abs(phase - Math.floor(phase + 0.5)) - 1;
  if (waveType === "sawtooth") return 2 * phase - 1;
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

    input.connect(delay);
    input.connect(dry);
    delay.connect(wet);
    dry.connect(output);
    wet.connect(output);

    return { input, delay, feedbackGain, dry, wet, output };
  }

  
  
  
  _delayMixGains(mix) {
    const m = Math.max(0, Math.min(1, mix));
    return [Math.cos((m * Math.PI) / 2), Math.sin((m * Math.PI) / 2)];
  }

  
  _dampingToHz(damping) {
    return 20000 - Math.max(0, Math.min(1, damping)) * 19800;
  }

  
  
  _roomSizeToSeconds(roomSize) {
    return 0.1 + Math.max(0, Math.min(1, roomSize)) * 7.9;
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
      if (entry.type === "linked-input" && entry.linkId) {
        sourcesByLinkId.set(entry.linkId, entry.gainNode);
      }
    });
    this.nodes.forEach((entry) => {
      if (entry.type !== "linked-output" || !entry.sourceLinkId) return;
      const source = sourcesByLinkId.get(entry.sourceLinkId);
      if (!source) return;
      source.connect(entry.gainNode);
      this._linkedIOEdges.push({ from: source, to: entry.gainNode });
    });
  }

  
  
  
  
  
  
  
  
  
  
  retriggerOscillatorEntry(entry) {
    if (!entry || (entry.type !== "oscillator" && entry.type !== "fm-oscillator")) return;
    const now = this.ctx.currentTime;

    if (entry.type === "fm-oscillator") {
      const oldOscs = [entry.osc, ...entry.sideOscs];
      const newCenter = this.ctx.createOscillator();
      newCenter.type = entry.osc.type;
      newCenter.frequency.value = entry.osc.frequency.value;
      newCenter.connect(entry.normalCarrierGain);
      newCenter.connect(entry.superSawCenterGain);
      const newSides = SUPER_SAW_CENTS.map((_, index) => {
        const side = this.ctx.createOscillator();
        side.type = "sawtooth";
        side.frequency.value = entry.sideOscs[index].frequency.value;
        side.connect(entry.superSawSideGains[index]);
        return side;
      });
      oldOscs.forEach((old) => {
        try {
          entry.pitchWaveShaper.disconnect(old.frequency);
          entry.fmDepthGain.disconnect(old.frequency);
          entry.feedbackGain.disconnect(old.frequency);
        } catch (err) {
        }
      });
      entry.sideOscs.forEach((old, index) => {
        try {
          entry.superSawDetuneScales[index].disconnect(old.detune);
        } catch (err) {
        }
      });
      [newCenter, ...newSides].forEach((fresh) => {
        entry.pitchWaveShaper.connect(fresh.frequency);
        entry.fmDepthGain.connect(fresh.frequency);
        entry.feedbackGain.connect(fresh.frequency);
        fresh.start(now);
      });
      newSides.forEach((side, index) => entry.superSawDetuneScales[index].connect(side.detune));
      oldOscs.forEach((old) => {
        try {
          old.stop(now);
          old.disconnect();
        } catch (err) {
        }
      });
      entry.osc = newCenter;
      entry.sideOscs = newSides;
      return;
    }

    const newOsc = this.ctx.createOscillator();
    newOsc.type = entry.osc.type;
    newOsc.frequency.value = entry.osc.frequency.value;
    newOsc.detune.value = entry.osc.detune.value;
    try {
      entry.pitchWaveShaper.disconnect(entry.osc.frequency);
      entry.pitchWaveShaper.connect(newOsc.frequency);
      entry.feedbackGain.disconnect(entry.osc.frequency);
      entry.feedbackGain.connect(newOsc.frequency);
      if (entry.fmDepthGain) {
        entry.fmDepthGain.disconnect(entry.osc.frequency);
        entry.fmDepthGain.connect(newOsc.frequency);
      }
    } catch (err) {
    }
    newOsc.connect(entry.feedbackDelay);
    if (!isNoiseWave(entry.waveType)) {
      newOsc.connect(entry.ampGain);
    }
    newOsc.start(now);

    try {
      entry.osc.stop(now);
      entry.osc.disconnect();
    } catch (err) {
      
    }

    entry.osc = newOsc;
  }

  
  
  
  
  
  
  
  
  
  
  
  
  
  
  _loadWavePlayerBuffer(entry) {
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
      const g = ((Number(params.gain) || 0) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS;
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

    const filterNode = ctx.createBiquadFilter();
    filterNode.type = "lowpass";
    const fc = region.initialFilterFc <= 13500 ? cents2Hertz(region.initialFilterFc) : 20000;
    filterNode.frequency.value = Math.min(fc, ctx.sampleRate / 2 - 100);
    filterNode.Q.value = clampRange(10 ** (region.initialFilterQ / 10 / 20), 0.1, 20);

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

  
  
  
  
  _createAnalyser(ctx, fftSize) {
    const analyser = ctx.createAnalyser();
    analyser.fftSize = fftSize;
    return { analyser, buffer: new Uint8Array(fftSize) };
  }

  
  
  
  _generateImpulseResponse(ctx, seconds) {
    const length = Math.max(1, Math.floor(ctx.sampleRate * seconds));
    const buffer = ctx.createBuffer(1, length, ctx.sampleRate);
    const data = buffer.getChannelData(0);
    for (let i = 0; i < length; i += 1) {
      const decay = (1 - i / length) ** 2.5;
      data[i] = (Math.random() * 2 - 1) * decay;
    }
    return buffer;
  }

  
  
  
  
  
  
  
  
  
  _createCvGainChain(ctx, curve) {
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
    waveShaper.curve = buildVcaCurve(curve);
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

  
  
  
  
  
  _reciprocalCurve() {
    if (!this._reciprocalCurveCache) {
      const N = 2049;
      const epsilon = 0.02;
      const curve = new Float32Array(N);
      for (let i = 0; i < N; i += 1) {
        let x = (i / (N - 1)) * 2 - 1;
        if (Math.abs(x) < epsilon) {
          x = x < 0 ? -epsilon : epsilon;
        }
        curve[i] = 1 / x;
      }
      this._reciprocalCurveCache = curve;
    }
    return this._reciprocalCurveCache;
  }

  
  
  
  
  
  
  
  
  
  _frequencyDividerCvOffsetCurve() {
    if (!this._frequencyDividerCvOffsetCurveCache) {
      const N = 2049;
      const curve = new Float32Array(N);
      for (let i = 0; i < N; i += 1) {
        const x = (i / (N - 1)) * 2 - 1; 
        let divider = x * 128;
        if (divider < 1) divider = 1;
        if (divider > 128) divider = 128;
        // 1 V/oct CV: dividing frequency by N means subtracting log2(N) V.
        curve[i] = -Math.log2(divider);
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

  
  
  
  
  _buildNode(ctx, type, params) {
    switch (type) {
      case "oscillator": {
        const waveType = params.waveType || "sine";
        const osc = ctx.createOscillator();
        osc.type = isNoiseWave(waveType) ? "sine" : waveType;
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
        const gainCv = this._createCvGainChain(ctx, "linear");
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

        osc.start();
        noiseSource.start();
        pinkNoiseSource.start();
        if (waveType === "noise") {
          noiseSource.connect(ampGain);
        } else if (waveType === "pink-noise") {
          pinkNoiseSource.connect(ampGain);
        } else {
          osc.connect(ampGain);
        }

        return {
          type,
          osc,
          noiseSource,
          pinkNoiseSource,
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
        // Structurally a clone of "oscillator" above (same carrier/pitch-CV
        // machinery, copy-pasted rather than shared to keep each _buildNode
        // case self-contained, matching the firmware's one-node-per-file
        // convention) plus a phase-modulation-FM approximation: see
        // FM_INDEX_MAX_HZ_DEVIATION's comment for why this is linear
        // frequency modulation here, not true phase modulation like the
        // firmware.
        const waveType = params.waveType || "sine";
        const osc = ctx.createOscillator();
        osc.type = waveType === "supersaw" ? "sawtooth" : (isNoiseWave(waveType) ? "sine" : waveType);
        osc.frequency.value = params.frequency ?? 0;
        const sideOscs = SUPER_SAW_CENTS.map(() => {
          const side = ctx.createOscillator();
          side.type = "sawtooth";
          side.frequency.value = params.frequency ?? 0;
          return side;
        });

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

        const periodicSum = ctx.createGain();
        const normalCarrierGain = ctx.createGain();
        const superSawBus = ctx.createGain();
        const superSawEnable = ctx.createGain();
        const superSawCenterGain = ctx.createGain();
        const superSawSideGains = sideOscs.map(() => ctx.createGain());
        normalCarrierGain.gain.value = waveType === "supersaw" || isNoiseWave(waveType) ? 0 : 1;
        superSawEnable.gain.value = waveType === "supersaw" ? 1 : 0;
        osc.connect(normalCarrierGain);
        normalCarrierGain.connect(periodicSum);
        osc.connect(superSawCenterGain);
        superSawCenterGain.connect(superSawBus);
        sideOscs.forEach((side, index) => {
          side.connect(superSawSideGains[index]);
          superSawSideGains[index].connect(superSawBus);
        });
        superSawBus.connect(superSawEnable);
        superSawEnable.connect(periodicSum);
        periodicSum.connect(ampGain);

        const superSawDetuneFallback = ctx.createConstantSource();
        superSawDetuneFallback.offset.value = params.superSawDetune ?? CV_MAX_VOLTS / 2;
        superSawDetuneFallback.start();
        const superSawDetuneFallbackGate = ctx.createGain();
        superSawDetuneFallbackGate.gain.value = 1;
        const superSawDetuneInput = ctx.createGain();
        superSawDetuneFallback.connect(superSawDetuneFallbackGate);
        superSawDetuneFallbackGate.connect(superSawDetuneInput);
        const superSawDetuneScales = SUPER_SAW_CENTS.map((cents, index) => {
          const scale = ctx.createGain();
          scale.gain.value = cents / CV_MAX_VOLTS;
          superSawDetuneInput.connect(scale);
          scale.connect(sideOscs[index].detune);
          return scale;
        });

        const superSawMixFallback = ctx.createConstantSource();
        superSawMixFallback.offset.value = params.superSawMix ?? CV_MAX_VOLTS / 2;
        superSawMixFallback.start();
        const superSawMixFallbackGate = ctx.createGain();
        superSawMixFallbackGate.gain.value = 1;
        const superSawMixInput = ctx.createGain();
        const superSawMixNorm = ctx.createGain();
        superSawMixNorm.gain.value = 1 / CV_MAX_VOLTS;
        superSawMixFallback.connect(superSawMixFallbackGate);
        superSawMixFallbackGate.connect(superSawMixInput);
        superSawMixInput.connect(superSawMixNorm);
        const centerCurve = new Float32Array(1024);
        const sideCurve = new Float32Array(1024);
        for (let i = 0; i < centerCurve.length; i += 1) {
          const m = clampRange(-1 + (2 * i) / (centerCurve.length - 1), 0, 1);
          const weights = superSawWeights(m * CV_MAX_VOLTS);
          centerCurve[i] = weights.center;
          sideCurve[i] = weights.side;
        }
        const superSawCenterShaper = ctx.createWaveShaper();
        const superSawSideShaper = ctx.createWaveShaper();
        superSawCenterShaper.curve = centerCurve;
        superSawSideShaper.curve = sideCurve;
        superSawMixNorm.connect(superSawCenterShaper);
        superSawMixNorm.connect(superSawSideShaper);
        superSawCenterGain.gain.value = 0;
        superSawSideGains.forEach((gain) => { gain.gain.value = 0; });
        superSawCenterShaper.connect(superSawCenterGain.gain);
        superSawSideGains.forEach((gain) => superSawSideShaper.connect(gain.gain));

        const outGain = ctx.createGain();
        outGain.gain.value = ((params.gain ?? CV_MAX_VOLTS) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS;
        const gainCv = this._createCvGainChain(ctx, "linear");
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
        pitchWaveShaper.connect(osc.frequency);
        sideOscs.forEach((side) => pitchWaveShaper.connect(side.frequency));

        // "fmIn" (audio-rate modulator input) + "fmIndex" (0-10V depth CV):
        // fmInScale normalizes the incoming +-AUDIO_PEAK_VOLTS waveform to
        // +-1, fmDepthGain scales that by the current index (in Hz) and sums
        // the result directly into osc.frequency - additive with the
        // pitchWaveShaper connection above, exactly like Web Audio already
        // sums multiple signals into one AudioParam elsewhere in this file.
        const fmInScale = ctx.createGain();
        fmInScale.gain.value = 1 / AUDIO_PEAK_VOLTS;
        const fmDepthGain = ctx.createGain();
        fmDepthGain.gain.value = ((params.fmIndex ?? 0) / CV_MAX_VOLTS) * FM_INDEX_MAX_HZ_DEVIATION;
        fmInScale.connect(fmDepthGain);
        fmDepthGain.connect(osc.frequency);
        sideOscs.forEach((side) => fmDepthGain.connect(side.frequency));

        const fmIndexCv = this._createCvGainChain(ctx, "linear");
        const fmIndexCvScale = ctx.createGain();
        fmIndexCvScale.gain.value = FM_INDEX_MAX_HZ_DEVIATION;
        fmIndexCv.waveShaper.connect(fmIndexCvScale);
        fmIndexCvScale.connect(fmDepthGain.gain);

        const feedbackDelay = ctx.createDelay(1);
        feedbackDelay.delayTime.value = 1 / ctx.sampleRate;
        const feedbackGain = ctx.createGain();
        feedbackGain.gain.value = ((params.feedback ?? 0) / CV_MAX_VOLTS) * SELF_FEEDBACK_MAX_HZ_DEVIATION;
        periodicSum.connect(feedbackDelay);
        feedbackDelay.connect(feedbackGain);
        feedbackGain.connect(osc.frequency);
        sideOscs.forEach((side) => feedbackGain.connect(side.frequency));
        const feedbackCv = this._createCvGainChain(ctx, "linear");
        const feedbackCvScale = ctx.createGain();
        feedbackCvScale.gain.value = SELF_FEEDBACK_MAX_HZ_DEVIATION;
        feedbackCv.waveShaper.connect(feedbackCvScale);
        feedbackCvScale.connect(feedbackGain.gain);

        osc.start();
        sideOscs.forEach((side) => side.start());
        noiseSource.start();
        pinkNoiseSource.start();
        if (waveType === "noise") {
          noiseSource.connect(ampGain);
        } else if (waveType === "pink-noise") {
          pinkNoiseSource.connect(ampGain);
        }

        return {
          type,
          osc,
          sideOscs,
          noiseSource,
          pinkNoiseSource,
          ampGain,
          ampCv,
          periodicSum,
          normalCarrierGain,
          superSawBus,
          superSawEnable,
          superSawCenterGain,
          superSawSideGains,
          superSawDetuneFallback,
          superSawDetuneFallbackGate,
          superSawDetuneInput,
          superSawDetuneScales,
          superSawDetune: params.superSawDetune ?? CV_MAX_VOLTS / 2,
          superSawDetuneCvActive: false,
          superSawMixFallback,
          superSawMixFallbackGate,
          superSawMixInput,
          superSawMixNorm,
          superSawCenterShaper,
          superSawSideShaper,
          superSawMix: params.superSawMix ?? CV_MAX_VOLTS / 2,
          superSawMixCvActive: false,
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
          fmInScale,
          fmDepthGain,
          fmIndexCv,
          fmIndex: params.fmIndex ?? 0,
          fmIndexCvActive: false,
          feedbackDelay,
          feedbackGain,
          feedbackCv,
          feedbackCvScale,
          feedback: params.feedback ?? 0,
          feedbackCvActive: false,
          // No audible effect in this preview (see the setParam "dutyCycle"
          // case's comment) - tracked only so getParam/state round-trips.
          dutyCycle: params.dutyCycle ?? CV_MAX_VOLTS / 2,
        };
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
        const NORM = AUDIO_PEAK_VOLTS / 9; // mirrors kDrawbarNorm - nine unit sines -> <=AUDIO_PEAK_VOLTS at full registration

        const ampGain = ctx.createGain();
        ampGain.gain.value = 0;
        const ampCv = this._createCvGainChain(ctx, "linear");
        ampCv.waveShaper.connect(ampGain.gain);

        const outGain = ctx.createGain();
        outGain.gain.value = ((params.gain ?? CV_MAX_VOLTS) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS;
        const gainCv = this._createCvGainChain(ctx, "linear");
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
      case "lfo": {
        
        
        
        
        
        
        
        
        
        
        const amplitude = params.amplitude ?? CV_MAX_VOLTS;
        const halfSwing = amplitude / 2;
        const center = params.zeroPointFixed ? params.zeroPoint ?? 5 : halfSwing;
        const maxRate = clampRange(Number(params.maxRate ?? 50) || 0, 0, 50);
        const rate = clampRange(Number(params.rate ?? 2) || 0, 0, maxRate);
        const phase = params.phase ?? 0;
        const waveType = params.waveType || "sine";
        const oneShot = params.oneShot === true;
        const lfoOsc = ctx.createOscillator();
        lfoOsc.type = waveType;
        lfoOsc.frequency.value = rate;
        const lfoGain = ctx.createGain();
        lfoGain.gain.value = oneShot ? 0 : halfSwing;
        const centerOffset = ctx.createConstantSource();
        centerOffset.offset.value = oneShot
          ? center + lfoWaveValueAtPhase(waveType, phase) * halfSwing
          : center;
        centerOffset.start();
        
        
        
        
        
        
        
        
        
        const rawOut = ctx.createGain();
        rawOut.gain.value = 1;
        lfoOsc.connect(lfoGain);
        lfoGain.connect(rawOut);
        centerOffset.connect(rawOut);
        const ownGate = ctx.createGain();
        ownGate.gain.value = 1;
        const sharedGate = ctx.createGain();
        sharedGate.gain.value = 0;
        rawOut.connect(ownGate);
        const output = ctx.createGain();
        output.gain.value = 1;
        ownGate.connect(output);
        sharedGate.connect(output);
        
        
        
        
        
        
        
        lfoOsc.start(Math.max(ctx.currentTime, ctx.currentTime - lfoStartOffsetSeconds(waveType, phase, rate)));
        return {
          type,
          lfoOsc,
          lfoGain,
          centerOffset,
          rawOut,
          ownGate,
          sharedGate,
          output,
          _sharedWired: false,
          lfoAmplitude: amplitude,
          lfoMaxRate: maxRate,
          lfoPhase: phase,
          lfoZeroPointFixed: params.zeroPointFixed === true,
          lfoZeroPoint: params.zeroPoint ?? 5,
          lfoOneShot: oneShot,
          lfoOneShotRunning: false,
          lfoOneShotTimer: null,
        };
      }
      case "const":
      case "button":
      case "up-down-button": {
        const constant = ctx.createConstantSource();
        constant.offset.value = params.value ?? 0;
        constant.start();
        return { type, constant };
      }
      case "knob": {
        // One ConstantSourceNode per control (fixed 16, like Multiplexer/
        // Mixer elsewhere in this file - unused ones just stay silent at 0),
        // instead of the single shared node const/button/up-down-button use
        // above - a multi-knob widget's "voltageN" outputs (see main.js's
        // knobPortId) need to be independently wireable into an audio-rate
        // destination (e.g. a Multiplexer input), not all collapse onto one
        // node. `constant` stays an alias for control 0, for any code still
        // reading it directly.
        const constants = [];
        for (let i = 0; i < 16; i += 1) {
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
        return { type, gain1, gain2, output };
      }
      case "output-switch": {
        // Inverted topology from "switch" above: one shared input fans out
        // to two independently-gated outputs instead of two inputs summing
        // into one output.
        const input = ctx.createGain();
        const gain1 = ctx.createGain();
        const gain2 = ctx.createGain();
        gain1.gain.value = params.selectOut2 ? 0 : 1;
        gain2.gain.value = params.selectOut2 ? 1 : 0;
        input.connect(gain1);
        input.connect(gain2);
        return { type, input, gain1, gain2 };
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
        leftGain.gain.value = 1 / AUDIO_PEAK_VOLTS;
        rightGain.gain.value = 1 / AUDIO_PEAK_VOLTS;
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
      case "linked-output": {
        // Transparent, fixed-unity passthrough - the actual fan-out wiring
        // between matching linkId/sourceLinkId pairs happens in
        // _wireLinkedIO(), not here (see createNode()/_removeEntry()).
        const gainNode = ctx.createGain();
        gainNode.gain.value = 1;
        return {
          type,
          gainNode,
          linkId: params.linkId,
          linkName: params.linkName,
          sourceLinkId: params.sourceLinkId,
        };
      }
      case "filter":
      case "curtis-filter": {
        const filterInput = ctx.createGain();
        const isCurtis = type === "curtis-filter";
        // Both filter widgets share this chain: FILTER_MAX_SECTIONS identical
        // biquads in series, with one tap gain per section. The selected slope
        // opens exactly one tap, so the wet path carries 1, 2 or 3 sections.
        const sections = Array.from({ length: FILTER_MAX_SECTIONS }, () => ctx.createBiquadFilter());
        const sectionTaps = sections.map(() => ctx.createGain());
        const filterNode = sections[0];
        const filterNode2 = sections[1];
        const filteredOutput = ctx.createGain();
        const filterOutput = ctx.createGain();
        const dryGain = ctx.createGain();
        const wetGain = ctx.createGain();
        const initialFilterType = params.bypass === true ? "none" : (params.filterType || "lowpass");
        const initialSlope = Number(params.slope) || (isCurtis ? 24 : 12);
        const initialBiquadType = initialFilterType === "none" ? "lowpass" : initialFilterType;
        const initialSections = filterSectionCount(initialSlope);
        filterInput.connect(dryGain);
        dryGain.connect(filterOutput);
        filterInput.connect(sections[0]);
        sections.forEach((section, index) => {
          section.type = initialBiquadType;
          if (index > 0) sections[index - 1].connect(section);
          section.connect(sectionTaps[index]);
          sectionTaps[index].gain.value = index === initialSections - 1 ? 1 : 0;
          sectionTaps[index].connect(filteredOutput);
        });
        filteredOutput.connect(wetGain);
        wetGain.connect(filterOutput);
        dryGain.gain.value = initialFilterType === "none" ? 1 : 0;
        wetGain.gain.value = initialFilterType === "none" ? 0 : 1;
        
        
        
        const entry = {
          type,
          filterInput,
          filterNode,
          filterNode2,
          sections,
          sectionTaps,
          filteredOutput,
          filterOutput,
          dryGain,
          wetGain,
          filterType: initialFilterType,
          cutoff: params.cutoff ?? 5,
          cutoffMinHz: params.cutoffMinHz ?? 20,
          cutoffMaxHz: params.cutoffMaxHz ?? 20000,
          cutoffCurve: params.cutoffCurve ?? "log",
          resonance: params.resonance ?? 5,
          resonanceMinQ: params.resonanceMinQ ?? 0.1,
          resonanceMaxQ: params.resonanceMaxQ ?? 20,
          resonanceCurve: params.resonanceCurve ?? "log",
          slope: initialSlope,
          cutoffCvActive: false,
          resonanceCvActive: false,
        };
        const initialFrequency = cvToRange(entry.cutoff, entry.cutoffMinHz, entry.cutoffMaxHz, entry.cutoffCurve);
        const initialQ = cvToRange(entry.resonance, entry.resonanceMinQ, entry.resonanceMaxQ, entry.resonanceCurve);
        const initialShelfGainDb = shelfGainDbFromCv(entry.resonance) / initialSections;
        sections.forEach((section) => {
          section.frequency.value = initialFrequency;
          section.Q.value = initialQ;
          if (isShelfFilterType(initialFilterType)) section.gain.value = initialShelfGainDb;
        });
        
        
        
        
        
        
        
        
        entry.cutoffCv = this._createCvGainChain(ctx, "linear");
        entry.cutoffCv.waveShaper.curve = buildRangeCurve(entry.cutoffMinHz, entry.cutoffMaxHz, entry.cutoffCurve);
        entry.resonanceCv = this._createCvGainChain(ctx, "linear");
        entry.resonanceCv.waveShaper.curve = buildRangeCurve(entry.resonanceMinQ, entry.resonanceMaxQ, entry.resonanceCurve);
        
        
        
        
        
        
        entry.cutoffOwnGate = ctx.createGain();
        entry.cutoffOwnGate.gain.value = 1;
        entry.cutoffSharedGate = ctx.createGain();
        entry.cutoffSharedGate.gain.value = 0;
        entry.cutoffCv.waveShaper.connect(entry.cutoffOwnGate);
        entry.resonanceOwnGate = ctx.createGain();
        entry.resonanceOwnGate.gain.value = 1;
        entry.resonanceSharedGate = ctx.createGain();
        entry.resonanceSharedGate.gain.value = 0;
        entry.resonanceCv.waveShaper.connect(entry.resonanceOwnGate);
        // Every section tracks the same cutoff/resonance CV - a section that is
        // currently tapped out still has to be in tune for the moment the slope
        // switches onto it.
        sections.forEach((section) => {
          entry.cutoffOwnGate.connect(section.frequency);
          entry.cutoffSharedGate.connect(section.frequency);
          entry.resonanceOwnGate.connect(section.Q);
          entry.resonanceSharedGate.connect(section.Q);
        });
        entry._sharedWired = false;
        return entry;
      }
      case "esp-eq": {
        
        
        
        
        
        
        const filterL = ctx.createBiquadFilter();
        const filterR = ctx.createBiquadFilter();
        [filterL, filterR].forEach((f) => {
          f.type = eqFilterTypeToBiquad(params.filterType);
          f.frequency.value = params.frequency ?? 1000;
          f.Q.value = params.q ?? 1;
          f.gain.value = params.gainDb ?? 0;
        });
        
        
        
        
        const spectrumAnalyser = ctx.createAnalyser();
        spectrumAnalyser.fftSize = 256;
        const binCount = spectrumAnalyser.frequencyBinCount;
        const spectrumBuffer = new Uint8Array(binCount);
        filterL.connect(spectrumAnalyser);
        
        
        
        const responseFreqs = new Float32Array(binCount);
        for (let i = 0; i < binCount; i += 1) {
          responseFreqs[i] = (i * ctx.sampleRate) / 2 / binCount;
        }
        return {
          type,
          inputL: filterL,
          outputL: filterL,
          inputR: filterR,
          outputR: filterR,
          spectrumAnalyser,
          spectrumBuffer,
          responseFreqs,
          responseMag: new Float32Array(binCount),
          responsePhase: new Float32Array(binCount),
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
        const STAGE_COUNT = inputCount - 1;
        const seed = ctx.createConstantSource();
        seed.offset.value = 1;
        seed.start();

        const stages = [];
        let prevOutput = seed;
        for (let i = 0; i < STAGE_COUNT; i += 1) {
          const node = ctx.createGain();
          node.gain.value = 1;
          prevOutput.connect(node);
          stages.push({ node });
          prevOutput = node;
        }
        return { type, seed, stages, output: prevOutput };
      }
      case "adder": {
        
        
        
        
        const sum = ctx.createGain();
        sum.gain.value = 1;
        return { type, sum };
      }
      case "divide": {
        
        
        
        
        
        
        
        
        const seedNum = ctx.createConstantSource();
        seedNum.offset.value = 0;
        seedNum.start();
        const seedDen = ctx.createConstantSource();
        seedDen.offset.value = 1;
        seedDen.start();

        const reciprocal = ctx.createWaveShaper();
        reciprocal.curve = this._reciprocalCurve();
        seedDen.connect(reciprocal);

        const outGain = ctx.createGain();
        outGain.gain.value = 0;
        seedNum.connect(outGain);
        reciprocal.connect(outGain.gain);

        return { type, seedNum, seedDen, reciprocal, outGain, output: outGain };
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

        return { type, seedNum, seedDen, prescale, reciprocal, outGain, output: outGain };
      }
      case "abs": {
        
        
        
        const waveShaper = ctx.createWaveShaper();
        waveShaper.curve = this._absCurve();
        return { type, waveShaper };
      }
      case "value-mapper": {
        
        
        
        
        
        
        
        
        
        const vmParams = {
          point1In: params.point1In ?? 0,
          point1Out: params.point1Out ?? 0,
          point2In: params.point2In ?? 127,
          point2Out: params.point2Out ?? CV_MAX_VOLTS,
          constraintMin: params.constraintMin ?? null,
          constraintMax: params.constraintMax ?? null,
          list: parseValueList(params.listText),
        };
        const domain = valueMapperDomain(vmParams);
        const chain = this._createRangeGainChain(
          ctx, domain.lo, domain.hi, buildValueMapperCurve(vmParams, domain.lo, domain.hi)
        );
        return { type, chain, ...vmParams };
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
          const step = 1 / levels;
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

        return {
          type,
          inputNode,
          lfo,
          depthGainL,
          depthGainR,
          delayL,
          delayR,
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
        };
      }
      case "reverb-light": {
        
        
        
        
        
        
        const inputNodeL = ctx.createGain();
        inputNodeL.gain.value = 1;
        const inputNodeR = ctx.createGain();
        inputNodeR.gain.value = 1;

        const monoSum = ctx.createGain();
        monoSum.gain.value = 0.5;
        inputNodeL.connect(monoSum);
        inputNodeR.connect(monoSum);

        const convolver = ctx.createConvolver();
        convolver.buffer = this._generateImpulseResponse(ctx, this._roomSizeToSeconds(params.roomSize ?? 0.5));
        const dampFilter = ctx.createBiquadFilter();
        dampFilter.type = "lowpass";
        dampFilter.frequency.value = this._dampingToHz(params.damping ?? 0.5);
        const wet = ctx.createGain();
        wet.gain.value = 10 ** ((params.wetLevelDb ?? -6) / 20);
        monoSum.connect(convolver);
        convolver.connect(dampFilter);
        dampFilter.connect(wet);

        const dryL = ctx.createGain();
        dryL.gain.value = 10 ** ((params.dryLevelDb ?? 0) / 20);
        const dryR = ctx.createGain();
        dryR.gain.value = 10 ** ((params.dryLevelDb ?? 0) / 20);
        inputNodeL.connect(dryL);
        inputNodeR.connect(dryR);

        const processedL = ctx.createGain();
        processedL.gain.value = params.bypass ? 0 : 1;
        const processedR = ctx.createGain();
        processedR.gain.value = params.bypass ? 0 : 1;
        const bypassL = ctx.createGain();
        bypassL.gain.value = params.bypass ? 1 : 0;
        const bypassR = ctx.createGain();
        bypassR.gain.value = params.bypass ? 1 : 0;
        const outputL = ctx.createGain();
        outputL.gain.value = 1;
        const outputR = ctx.createGain();
        outputR.gain.value = 1;

        dryL.connect(processedL);
        wet.connect(processedL);
        dryR.connect(processedR);
        wet.connect(processedR);
        inputNodeL.connect(bypassL);
        inputNodeR.connect(bypassR);
        processedL.connect(outputL);
        bypassL.connect(outputL);
        processedR.connect(outputR);
        bypassR.connect(outputR);

        return {
          type,
          roomSize: params.roomSize ?? 0.5,
          bypass: params.bypass === true,
          inputNodeL,
          inputNodeR,
          monoSum,
          convolver,
          dampFilter,
          wet,
          dryL,
          dryR,
          processedL,
          processedR,
          bypassL,
          bypassR,
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
        const linkGainPrescales = type === "audio-input-link"
          ? Array.from({ length: LINK_BUS_COUNT }, (_, i) => {
              const prescale = ctx.createGain();
              prescale.gain.value = 1 / CV_MAX_VOLTS;
              prescale.connect(nodes[i].gain);
              nodes[i].gain.value = gainVolts[i] / CV_MAX_VOLTS;
              return prescale;
            })
          : null;
        return {
          type,
          nodes,
          linkGainVolts: gainVolts,
          linkGainPrescales,
          linkGainConnectionCount: Array(LINK_BUS_COUNT).fill(0),
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
        const gainCv = this._createCvGainChain(ctx, "linear");
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
        const entry = {
          type,
          splitter,
          leftOut,
          rightOut,
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

    if (type === "linked-input" || type === "linked-output") {
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
    entry.inputL.getFrequencyResponse(entry.responseFreqs, entry.responseMag, entry.responsePhase);
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

  _removeEntry(entry) {
    if (!entry) return;
    try {
      if (entry.osc) {
        entry.osc.stop();
        entry.osc.disconnect();
      }
      if (entry.sideOscs) entry.sideOscs.forEach((osc) => { osc.stop(); osc.disconnect(); });
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
      if (entry.fmDepthGain) entry.fmDepthGain.disconnect();
      if (entry.feedbackDelay) entry.feedbackDelay.disconnect();
      if (entry.feedbackGain) entry.feedbackGain.disconnect();
      if (entry.feedbackCvScale) entry.feedbackCvScale.disconnect();
      ["periodicSum", "normalCarrierGain", "superSawBus", "superSawEnable", "superSawCenterGain",
        "superSawDetuneFallbackGate", "superSawDetuneInput", "superSawMixFallbackGate",
        "superSawMixInput", "superSawMixNorm", "superSawCenterShaper", "superSawSideShaper"].forEach((key) => {
        if (entry[key]) entry[key].disconnect();
      });
      if (entry.superSawSideGains) entry.superSawSideGains.forEach((gain) => gain.disconnect());
      if (entry.superSawDetuneScales) entry.superSawDetuneScales.forEach((gain) => gain.disconnect());
      ["superSawDetuneFallback", "superSawMixFallback"].forEach((key) => {
        if (entry[key]) {
          entry[key].stop();
          entry[key].disconnect();
        }
      });
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
      if ((entry.type === "linked-input" || entry.type === "linked-output") && entry.gainNode) {
        entry.gainNode.disconnect();
        this._wireLinkedIO();
      }
      if (entry.analyser) entry.analyser.disconnect();
      if (entry.spectrumAnalyser) entry.spectrumAnalyser.disconnect();
      if (entry.limiter) entry.limiter.disconnect();
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
    const halfSwing = entry.lfoAmplitude / 2;
    const center = entry.lfoZeroPointFixed ? entry.lfoZeroPoint : halfSwing;
    const heldValue = center + lfoWaveValueAtPhase(entry.lfoOsc.type, entry.lfoPhase ?? 0) * halfSwing;
    const now = this.ctx.currentTime;
    entry.lfoGain.gain.cancelScheduledValues(now);
    entry.centerOffset.offset.cancelScheduledValues(now);
    entry.lfoGain.gain.setValueAtTime(running ? halfSwing : 0, now);
    entry.centerOffset.offset.setValueAtTime(running ? center : heldValue, now);
    entry.lfoOneShotRunning = running;
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
      if (!entry || entry.type !== "lfo") return;
      const oldOsc = entry.lfoOsc;
      const newOsc = this.ctx.createOscillator();
      newOsc.type = oldOsc.type;
      newOsc.frequency.value = oldOsc.frequency.value;
      newOsc.connect(entry.lfoGain);
      
      
      
      const phase = entry.lfoPhase ?? 0;
      const rate = newOsc.frequency.value || 2;
      newOsc.start(Math.max(this.ctx.currentTime, this.ctx.currentTime - lfoStartOffsetSeconds(newOsc.type, phase, rate)));
      try {
        oldOsc.disconnect();
        oldOsc.stop();
      } catch (err) {
        
      }
      entry.lfoOsc = newOsc;
      if (entry.lfoOneShot) this._scheduleLfoOneShotStop(entry);
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
          
          
          
          
          
          
          if (!entry.pitchCvActive && !entry.tuneCvActive) {
            applyRamp(entry.osc.frequency);
            entry.sideOscs?.forEach((side) => applyRamp(side.frequency));
          }
        } else if (paramName === "tuneVolts") {
          
          
          
          
          
          applyRampValue(entry.tuneRestConst.offset, value);
        } else if (paramName === "waveType") {
          if (entry.type === "fm-oscillator") {
            if (entry.waveType === "noise") entry.noiseSource.disconnect(entry.ampGain);
            if (entry.waveType === "pink-noise") entry.pinkNoiseSource.disconnect(entry.ampGain);
            entry.normalCarrierGain.gain.value = value === "supersaw" || isNoiseWave(value) ? 0 : 1;
            entry.superSawEnable.gain.value = value === "supersaw" ? 1 : 0;
            if (value === "noise") entry.noiseSource.connect(entry.ampGain);
            else if (value === "pink-noise") entry.pinkNoiseSource.connect(entry.ampGain);
            else entry.osc.type = value === "supersaw" ? "sawtooth" : value;
          } else if (isNoiseWave(value)) {
            if (!isNoiseWave(entry.waveType)) entry.osc.disconnect(entry.ampGain);
            if (entry.waveType === "noise") entry.noiseSource.disconnect(entry.ampGain);
            if (entry.waveType === "pink-noise") entry.pinkNoiseSource.disconnect(entry.ampGain);
            if (value === "noise") entry.noiseSource.connect(entry.ampGain);
            else entry.pinkNoiseSource.connect(entry.ampGain);
          } else {
            entry.osc.type = value;
            if (isNoiseWave(entry.waveType)) {
              if (entry.waveType === "noise") entry.noiseSource.disconnect(entry.ampGain);
              else entry.pinkNoiseSource.disconnect(entry.ampGain);
              entry.osc.connect(entry.ampGain);
            }
          }
          entry.waveType = value;
        } else if (paramName === "detune") {
          applyRamp(entry.osc.detune);
        } else if (paramName === "amplitude") {
          applyRamp(entry.ampGain.gain);
        } else if (paramName === "gain") {
          entry.gain = value;
          
          
          
          if (!entry.gainCvActive) applyRampValue(entry.outGain.gain, ((Number(value) || 0) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS);
        } else if (paramName === "gate") {
          if (value > GATE_THRESHOLD_VOLTS) this.retriggerOscillatorEntry(entry);
        } else if (paramName === "fmIndex") {
          // fm-oscillator only (entry.fmDepthGain doesn't exist on a plain
          // oscillator entry, and this param name is never sent to one).
          entry.fmIndex = value;
          if (!entry.fmIndexCvActive) {
            applyRampValue(entry.fmDepthGain.gain, ((Number(value) || 0) / CV_MAX_VOLTS) * FM_INDEX_MAX_HZ_DEVIATION);
          }
        } else if (paramName === "feedback") {
          entry.feedback = value;
          if (!entry.feedbackCvActive) {
            applyRampValue(entry.feedbackGain.gain, ((Number(value) || 0) / CV_MAX_VOLTS) * SELF_FEEDBACK_MAX_HZ_DEVIATION);
          }
        } else if (paramName === "dutyCycle") {
          // State tracked for preset round-tripping only - no audible
          // effect here. The native WebAudio OscillatorNode's "square" type
          // (see the "fm-oscillator" _buildNode case above) is a fixed 50%
          // pulse with no pulse-width control; reproducing the firmware's
          // exact variable-duty square would need a custom PeriodicWave or
          // AudioWorklet. Same "preview approximates, hardware is
          // authoritative" precedent already documented for FM_INDEX_MAX_HZ_
          // DEVIATION/SELF_FEEDBACK_MAX_HZ_DEVIATION above.
          entry.dutyCycle = value;
        } else if (paramName === "superSawDetune") {
          entry.superSawDetune = value;
          if (!entry.superSawDetuneCvActive) applyRampValue(entry.superSawDetuneFallback.offset, value);
        } else if (paramName === "superSawMix") {
          entry.superSawMix = value;
          if (!entry.superSawMixCvActive) applyRampValue(entry.superSawMixFallback.offset, value);
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
        } else if (paramName === "frequency") {
          // Only when no live pitch/tune cable owns the shared pitch chain -
          // same guard as the oscillator case above, applied to all nine
          // oscillators (each scaled by its own ratio).
          if (!entry.pitchCvActive && !entry.tuneCvActive) {
            entry.oscs.forEach((o, k) => applyRampValue(o.frequency, value * entry.ratios[k]));
          }
        } else if (paramName === "tuneVolts") {
          applyRampValue(entry.tuneRestConst.offset, value);
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
        if (paramName === "linkId") entry.linkId = value;
        else if (paramName === "linkName") entry.linkName = value;
        break;
      case "linked-output":
        if (paramName === "sourceLinkId") entry.sourceLinkId = value;
        break;
      case "filter":
      case "curtis-filter": {
        
        
        
        
        
        
        
        
        const recomputeCutoff = () => {
          if (!entry.cutoffCvActive) {
            const frequency = cvToRange(entry.cutoff, entry.cutoffMinHz, entry.cutoffMaxHz, entry.cutoffCurve);
            entry.sections.forEach((section) => applyRampValue(section.frequency, frequency));
          }
        };
        const recomputeResonance = () => {
          // Shelf types take this control as their gain instead of Q, split
          // across the active sections so the total shelf lift stays put as the
          // slope changes. Only on the manual path - an audio-rate cable on
          // "resonance" still feeds Q, which a shelving biquad ignores.
          if (isShelfFilterType(entry.filterType)) {
            const shelfGainDb = shelfGainDbFromCv(entry.resonance) / filterSectionCount(entry.slope);
            entry.sections.forEach((section) => applyRampValue(section.gain, shelfGainDb));
            return;
          }
          if (!entry.resonanceCvActive) {
            const q = cvToRange(entry.resonance, entry.resonanceMinQ, entry.resonanceMaxQ, entry.resonanceCurve);
            entry.sections.forEach((section) => applyRampValue(section.Q, q));
          }
        };
        if (paramName === "cutoff") {
          entry.cutoff = value;
          recomputeCutoff();
        } else if (paramName === "cutoffMinHz") {
          entry.cutoffMinHz = value;
          entry.cutoffCv.waveShaper.curve = buildRangeCurve(entry.cutoffMinHz, entry.cutoffMaxHz, entry.cutoffCurve);
          recomputeCutoff();
        } else if (paramName === "cutoffMaxHz") {
          entry.cutoffMaxHz = value;
          entry.cutoffCv.waveShaper.curve = buildRangeCurve(entry.cutoffMinHz, entry.cutoffMaxHz, entry.cutoffCurve);
          recomputeCutoff();
        } else if (paramName === "cutoffCurve") {
          entry.cutoffCurve = value;
          entry.cutoffCv.waveShaper.curve = buildRangeCurve(entry.cutoffMinHz, entry.cutoffMaxHz, entry.cutoffCurve);
          recomputeCutoff();
        } else if (paramName === "resonance") {
          entry.resonance = value;
          recomputeResonance();
        } else if (paramName === "resonanceMinQ") {
          entry.resonanceMinQ = value;
          entry.resonanceCv.waveShaper.curve = buildRangeCurve(entry.resonanceMinQ, entry.resonanceMaxQ, entry.resonanceCurve);
          recomputeResonance();
        } else if (paramName === "resonanceMaxQ") {
          entry.resonanceMaxQ = value;
          entry.resonanceCv.waveShaper.curve = buildRangeCurve(entry.resonanceMinQ, entry.resonanceMaxQ, entry.resonanceCurve);
          recomputeResonance();
        } else if (paramName === "resonanceCurve") {
          entry.resonanceCurve = value;
          entry.resonanceCv.waveShaper.curve = buildRangeCurve(entry.resonanceMinQ, entry.resonanceMaxQ, entry.resonanceCurve);
          recomputeResonance();
        } else if (paramName === "filterType") {
          entry.filterType = value;
          applyRampValue(entry.dryGain.gain, value === "none" ? 1 : 0);
          applyRampValue(entry.wetGain.gain, value === "none" ? 0 : 1);
          const biquadType = value === "none" ? "lowpass" : value;
          entry.sections.forEach((section) => { section.type = biquadType; });
          recomputeResonance();
        } else if (paramName === "slope") {
          entry.slope = Number(value) || 12;
          const active = filterSectionCount(entry.slope);
          entry.sectionTaps.forEach((tap, index) => {
            applyRampValue(tap.gain, index === active - 1 ? 1 : 0);
          });
          recomputeResonance();
        }
        break;
      }
      case "esp-eq":
        if (paramName === "frequency") {
          applyRamp(entry.inputL.frequency);
          applyRamp(entry.inputR.frequency);
        } else if (paramName === "q") {
          applyRamp(entry.inputL.Q);
          applyRamp(entry.inputR.Q);
        } else if (paramName === "gainDb") {
          applyRamp(entry.inputL.gain);
          applyRamp(entry.inputR.gain);
        } else if (paramName === "filterType") {
          entry.inputL.type = eqFilterTypeToBiquad(value);
          entry.inputR.type = eqFilterTypeToBiquad(value);
        }
        break;
      case "frequency-divider":
        
        
        
        if (paramName === "divider") {
          const divider = clampRange(Number(value) || 1, 1, 128);
          applyRampValue(entry.seedDen.offset, divider);
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
        const domain = valueMapperDomain(entry);
        entry.chain.prescale.gain.value = 2 / (domain.hi - domain.lo || 1);
        entry.chain.shiftConst.offset.value = -1 - (2 * domain.lo) / (domain.hi - domain.lo || 1);
        entry.chain.waveShaper.curve = buildValueMapperCurve(entry, domain.lo, domain.hi);
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
          if (value > GATE_THRESHOLD_VOLTS) {
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
          if (value > GATE_THRESHOLD_VOLTS) {
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
      case "lfo": {
        if (paramName === "rate") {
          const rate = clampRange(Number(value) || 0, 0, entry.lfoMaxRate ?? 50);
          applyRampValue(entry.lfoOsc.frequency, rate);
        } else if (paramName === "maxRate") {
          entry.lfoMaxRate = clampRange(Number(value) || 0, 0, 50);
          const rate = clampRange(Number(entry.lfoOsc.frequency.value) || 0, 0, entry.lfoMaxRate);
          applyRampValue(entry.lfoOsc.frequency, rate);
        } else if (paramName === "amplitude") {
          entry.lfoAmplitude = value;
        } else if (paramName === "phase") {
          
          
          
          entry.lfoPhase = value;
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
            this._setLfoRunning(entry, !enabled);
          }
        } else if (paramName === "waveType") {
          entry.lfoOsc.type = value;
        }
        if (paramName === "amplitude" || paramName === "phase" || paramName === "zeroPointFixed" || paramName === "zeroPoint" || paramName === "waveType") {
          const halfSwing = entry.lfoAmplitude / 2;
          const center = entry.lfoZeroPointFixed ? entry.lfoZeroPoint : halfSwing;
          if (entry.lfoOneShot && !entry.lfoOneShotRunning) {
            applyRampValue(entry.lfoGain.gain, 0);
            applyRampValue(entry.centerOffset.offset, center + lfoWaveValueAtPhase(entry.lfoOsc.type, entry.lfoPhase ?? 0) * halfSwing);
          } else {
            applyRampValue(entry.lfoGain.gain, halfSwing);
            applyRampValue(entry.centerOffset.offset, center);
          }
        }
        break;
      }
      case "const":
      case "button":
      case "up-down-button":
        if (paramName === "value") applyRamp(entry.constant.offset);
        break;
      case "knob": {
        // "value" (index 0) or "value2".."value16" (see main.js's
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
        if (paramName === "selectOut2") {
          applyRampValue(entry.gain1.gain, value ? 0 : 1);
          applyRampValue(entry.gain2.gain, value ? 1 : 0);
        }
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
      case "wave-folder": {
        const index = ["drive", "symmetry", "gain", "mix"].indexOf(paramName);
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
        if (paramName === "rate") {
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
        if (paramName === "delayTimeMs") {
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
      case "reverb-light":
        if (paramName === "bypass") {
          entry.bypass = value === true || Number(value) >= 5;
          applyRampValue(entry.processedL.gain, entry.bypass ? 0 : 1);
          applyRampValue(entry.processedR.gain, entry.bypass ? 0 : 1);
          applyRampValue(entry.bypassL.gain, entry.bypass ? 1 : 0);
          applyRampValue(entry.bypassR.gain, entry.bypass ? 1 : 0);
        } else if (paramName === "roomSize") {
          entry.roomSize = value;
          entry.convolver.buffer = this._generateImpulseResponse(this.ctx, this._roomSizeToSeconds(value));
        } else if (paramName === "damping") {
          applyRampValue(entry.dampFilter.frequency, this._dampingToHz(value));
        } else if (paramName === "wetLevelDb") {
          applyRampValue(entry.wet.gain, 10 ** (value / 20));
        } else if (paramName === "dryLevelDb") {
          const gain = 10 ** (value / 20);
          applyRampValue(entry.dryL.gain, gain);
          applyRampValue(entry.dryR.gain, gain);
        }
        break;
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
        (wrapper.type === "lfo" || wrapper.type === "gain" || wrapper.type === "filter" || wrapper.type === "curtis-filter")
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
      if (wrapper.type === "lfo") {
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
    if (entry.type === "lfo" && port === "value") return entry.output;
    if (entry.type === "switch" && port === "value") return entry.output;
    if (entry.type === "output-switch") {
      if (port === "out1") return entry.gain1;
      if (port === "out2") return entry.gain2;
    }
    
    
    
    if (entry.type === "const" && port === "audio") return entry.constant;
    if (entry.type === "knob") {
      // "voltage" (bare, index-0 alias) or "voltage1".."voltage16" (see
      // main.js's knobPortId) - each maps to its own ConstantSourceNode (see
      // _buildNode's "knob" case) so a knob can feed an audio-rate
      // destination (e.g. a Multiplexer input) like any other source.
      const match = /^voltage(\d*)$/.exec(port);
      if (match) {
        const index = match[1] ? Number(match[1]) - 1 : 0;
        return entry.constants?.[index] || null;
      }
    }
    if (entry.type === "gain" && port === "out") return entry.curveGainNode;
    if (entry.type === "linked-output" && port === "out") return entry.gainNode;
    if ((entry.type === "filter" || entry.type === "curtis-filter") && port === "out") {
      return entry.filterOutput || entry.filterNode;
    }
    if (entry.type.startsWith("esp-") && (port === "outL" || port === "outR")) return port === "outL" ? entry.outputL : entry.outputR;
    if (entry.type === "mixer" && port === "out") return entry.sum;
    if (entry.type === "multiplexer" && port === "value") return entry.output;
    if (entry.type === "output-multiplexer") return entry.outputGains.get(port) || null;
    if (entry.type === "multiplier" && port === "value") return entry.output;
    if (entry.type === "adder" && port === "value") return entry.sum;
    if (entry.type === "divide" && port === "value") return entry.output;
    if (entry.type === "frequency-divider" && (port === "cv" || port === "hz")) return entry.outGain;
    if (entry.type === "abs" && port === "value") return entry.waveShaper;
    if (entry.type === "value-mapper" && port === "out") return entry.chain.waveShaper;
    if (entry.type === "detune" && port === "out") return entry.out;
    if (entry.type === "adsr" && port === "out") return entry.output;
    if (entry.type === "dx-envelope" && port === "out") return entry.output;
    if (entry.type === "bitcrusher" && port === "out") return entry.processor;
    if (entry.type === "wave-folder" && port === "out") return entry.processor;
    if (entry.type === "pan" && port === "out1") return entry.out1;
    if (entry.type === "pan" && port === "out2") return entry.out2;
    if (entry.type === "sample-hold" && port === "out") return entry.processor;
    if (entry.type === "chorus" && port === "left") return entry.delayL;
    if (entry.type === "chorus" && port === "right") return entry.delayR;
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
    if (entry.type === "linked-input" && port === "in") return entry.gainNode;
    
    
    
    
    if (entry.type === "gain" && port === "amount") return entry.prescale;
    if (entry.type === "gain" && port === "curve") return entry.curveCv.prescale;
    if ((entry.type === "filter" || entry.type === "curtis-filter") && port === "in") return entry.filterInput;
    
    
    
    
    if ((entry.type === "filter" || entry.type === "curtis-filter") && port === "cutoff") return entry.cutoffCv.prescale;
    if ((entry.type === "filter" || entry.type === "curtis-filter") && port === "resonance") return entry.resonanceCv.prescale;
    if (entry.type.startsWith("esp-") && (port === "inL" || port === "inR")) return port === "inL" ? entry.inputL : entry.inputR;
    if (entry.type === "mixer") return entry.inputGains.get(port) || null;
    if (entry.type === "multiplexer") return entry.inputGains.get(port) || null;
    if (entry.type === "adder" && /^in\d+$/.test(port)) return entry.sum;
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
      if (port === "fmIndex") return entry.fmIndexCv.prescale;
      if (port === "feedback") return entry.feedbackCv.prescale;
      if (port === "superSawDetune") return entry.superSawDetuneInput;
      if (port === "superSawMix") return entry.superSawMixInput;
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
      if (entry.type === "value-mapper" && port === "in") return entry.chain.prescale;
    }
    if (entry.type === "detune" && port === "cv") return entry.cvIn;
    if (entry.type === "bitcrusher" && port === "in") return entry.processor;
    if (entry.type === "wave-folder" && ["in", "drive", "symmetry", "gain", "mix"].includes(port)) return entry.inputMerger;
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
    if (toEntry && (toEntry.type === "oscillator" || toEntry.type === "hammond" || toEntry.type === "fm-oscillator" || toEntry.type === "wave-player") && toPort === "gain") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.connect(toEntry.gainCv.prescale);
          toEntry.gainCvActive = true;
          toEntry.outGain.gain.value = 0;
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && (toEntry.type === "oscillator" || toEntry.type === "fm-oscillator") && toPort === "feedback") {
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
    if (toEntry && toEntry.type === "fm-oscillator" && toPort === "fmIndex") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.connect(toEntry.fmIndexCv.prescale);
          toEntry.fmIndexCvActive = true;
          toEntry.fmDepthGain.gain.value = 0;
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && toEntry.type === "fm-oscillator" &&
        (toPort === "superSawDetune" || toPort === "superSawMix")) {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        const input = toPort === "superSawDetune" ? toEntry.superSawDetuneInput : toEntry.superSawMixInput;
        const gate = toPort === "superSawDetune" ? toEntry.superSawDetuneFallbackGate : toEntry.superSawMixFallbackGate;
        try {
          from.connect(input);
          gate.gain.value = 0;
          if (toPort === "superSawDetune") toEntry.superSawDetuneCvActive = true;
          else toEntry.superSawMixCvActive = true;
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
        const restGate = toPort === "pitchCv" ? toEntry.pitchRestGate : toEntry.tuneRestGate;
        const sum = toPort === "pitchCv" ? toEntry.pitchSum : toEntry.tuneSum;
        try {
          restGate.gain.value = 0;
          from.connect(sum);
          if (toPort === "pitchCv") toEntry.pitchCvActive = true;
          else toEntry.tuneCvActive = true;
          // Hammond has nine oscillators (entry.oscs), the single-osc entry
          // just entry.osc - zero whichever this entry actually has, so the
          // CV chain (fanned through footageScale for Hammond) becomes the
          // sole pitch source instead of adding on top of a stale direct-set
          // value (see the pitchRestGate comment in _buildNode).
          if (toEntry.oscs) toEntry.oscs.forEach((o) => { o.frequency.value = 0; });
          else {
            toEntry.osc.frequency.value = 0;
            toEntry.sideOscs?.forEach((o) => { o.frequency.value = 0; });
          }
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
          toEntry.sections.forEach((section) => {
            const target = toPort === "cutoff" ? section.frequency : section.Q;
            target.value = 0;
          });
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
    if (toEntry && (toEntry.type === "oscillator" || toEntry.type === "hammond" || toEntry.type === "fm-oscillator" || toEntry.type === "wave-player") && toPort === "gain") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.disconnect(toEntry.gainCv.prescale);
          toEntry.gainCvActive = false;
          toEntry.outGain.gain.value = ((toEntry.gain ?? CV_MAX_VOLTS) / CV_MAX_VOLTS) * AUDIO_PEAK_VOLTS;
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && (toEntry.type === "oscillator" || toEntry.type === "fm-oscillator") && toPort === "feedback") {
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
    if (toEntry && toEntry.type === "fm-oscillator" && toPort === "fmIndex") {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        try {
          from.disconnect(toEntry.fmIndexCv.prescale);
          toEntry.fmIndexCvActive = false;
          toEntry.fmDepthGain.gain.value = ((toEntry.fmIndex ?? 0) / CV_MAX_VOLTS) * FM_INDEX_MAX_HZ_DEVIATION;
        } catch (err) {

        }
      }
      return;
    }
    if (toEntry && toEntry.type === "fm-oscillator" &&
        (toPort === "superSawDetune" || toPort === "superSawMix")) {
      const from = this._resolveScaledOutput(fromEntry, fromPort, gainScale);
      if (from) {
        const input = toPort === "superSawDetune" ? toEntry.superSawDetuneInput : toEntry.superSawMixInput;
        const gate = toPort === "superSawDetune" ? toEntry.superSawDetuneFallbackGate : toEntry.superSawMixFallbackGate;
        try {
          from.disconnect(input);
          gate.gain.value = 1;
          if (toPort === "superSawDetune") toEntry.superSawDetuneCvActive = false;
          else toEntry.superSawMixCvActive = false;
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
        const restGate = toPort === "pitchCv" ? toEntry.pitchRestGate : toEntry.tuneRestGate;
        const sum = toPort === "pitchCv" ? toEntry.pitchSum : toEntry.tuneSum;
        try {
          from.disconnect(sum);
          restGate.gain.value = 0; // back to closed, see pitchRestGate comment in _buildNode
          if (toPort === "pitchCv") toEntry.pitchCvActive = false;
          else toEntry.tuneCvActive = false;
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
          if (toPort === "cutoff") {
            toEntry.cutoffCvActive = false;
            const frequency = cvToRange(
              toEntry.cutoff, toEntry.cutoffMinHz, toEntry.cutoffMaxHz, toEntry.cutoffCurve
            );
            toEntry.sections.forEach((section) => { section.frequency.value = frequency; });
          } else {
            toEntry.resonanceCvActive = false;
            const q = cvToRange(
              toEntry.resonance, toEntry.resonanceMinQ, toEntry.resonanceMaxQ, toEntry.resonanceCurve
            );
            toEntry.sections.forEach((section) => { section.Q.value = q; });
          }
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

  _wireMultiplierInput(entry, port, sourceNode, connecting) {
    if (!sourceNode) return;
    const idx = parseInt(String(port).replace("in", ""), 10);
    if (Number.isNaN(idx) || idx < 1 || idx > 8) return;

    try {
      if (idx === 1) {
        const stage0 = entry.stages[0];
        if (connecting) {
          entry.seed.disconnect(stage0.node);
          sourceNode.connect(stage0.node);
        } else {
          sourceNode.disconnect(stage0.node);
          entry.seed.connect(stage0.node);
        }
        return;
      }

      const stage = entry.stages[idx - 2];
      if (!stage) return;
      if (connecting) {
        stage.node.gain.value = 1;
        sourceNode.connect(stage.node.gain);
      } else {
        sourceNode.disconnect(stage.node.gain);
        stage.node.gain.value = 1;
      }
    } catch (err) {
      
    }
  }

  _wireDivideInput(entry, port, sourceNode, connecting) {
    if (!sourceNode) return;
    try {
      if (port === "in1") {
        if (connecting) {
          entry.seedNum.disconnect(entry.outGain);
          sourceNode.connect(entry.outGain);
        } else {
          sourceNode.disconnect(entry.outGain);
          entry.seedNum.connect(entry.outGain);
        }
      } else if (port === "in2") {
        if (connecting) {
          entry.seedDen.disconnect(entry.reciprocal);
          sourceNode.connect(entry.reciprocal);
        } else {
          sourceNode.disconnect(entry.reciprocal);
          entry.seedDen.connect(entry.reciprocal);
        }
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
