// Behavioural checks of data/static/js/audio-engine.js against the firmware's
// formulas (main/audio/engine/nodes/*.h). Loaded by run.mjs inside Chromium;
// each test renders a small patch offline and compares sample values.
const SR = 48000;
const results = [];

async function newEngine(seconds) {
  const octx = new OfflineAudioContext(1, Math.round(SR * seconds), SR);
  window.AudioContext = function FakeAudioContext() { return octx; };
  const { AudioEngine } = await import("/js/audio-engine.js");
  const engine = new AudioEngine();
  engine.ensureContext();
  return { engine, octx };
}

function out(engine, id, port) {
  const w = engine.nodes.get(id);
  const entry = w.voiced ? w.instances[0] : w;
  const node = engine._resolveOutputFromEntry(entry, port);
  if (!node) throw new Error(`no output ${id}.${port}`);
  return node;
}

async function render(seconds, setup) {
  const { engine, octx } = await newEngine(seconds);
  const tap = await setup(engine, octx);
  tap.connect(octx.destination);
  const buf = await octx.startRendering();
  return { data: buf.getChannelData(0), engine };
}

const at = (data, t) => data[Math.floor(t * SR)];
function peak(data, from = 0.3, to = 0.5) {
  let m = 0;
  for (let i = Math.floor(from * SR); i < Math.floor(to * SR); i += 1) m = Math.max(m, Math.abs(data[i]));
  return m;
}
function mean(data, from = 0.3, to = 0.5) {
  let s = 0;
  const a = Math.floor(from * SR);
  const b = Math.floor(to * SR);
  for (let i = a; i < b; i += 1) s += data[i];
  return s / (b - a);
}
function risingCrossings(data, from, to, level = 0) {
  let n = 0;
  for (let i = Math.floor(from * SR) + 1; i < Math.floor(to * SR); i += 1) {
    if (data[i - 1] < level && data[i] >= level) n += 1;
  }
  return n;
}
function allFinite(data) {
  for (let i = 0; i < data.length; i += 1) if (!Number.isFinite(data[i])) return false;
  return true;
}

async function test(name, fn) {
  try {
    const detail = await fn();
    results.push({ name, ok: true, detail });
  } catch (err) {
    results.push({ name, ok: false, detail: err && err.message ? err.message : String(err) });
  }
}
function expectClose(actual, expected, tol, label = "") {
  if (!(Math.abs(actual - expected) <= tol)) {
    throw new Error(`${label} expected ${expected} +/- ${tol}, got ${actual}`);
  }
  return `${label}${actual.toFixed(4)}`;
}

function constNode(engine, id, value) {
  engine.createNode(id, "const", { value });
}

function sineOsc(engine, id, hz, waveType = "sine") {
  engine.createNode(id, "oscillator", { waveType, frequency: hz, gain: 10 });
  engine.setParam(id, "amplitude", 1, 0);
}

export async function runAll() {
  await test("divide 10/4 = 2.5 (audio mode)", async () => {
    const { data } = await render(0.5, (e) => {
      constNode(e, "a", 10); constNode(e, "b", 4);
      e.createNode("d", "divide", {});
      e.connect("a", "audio", "d", "in1");
      e.connect("b", "audio", "d", "in2");
      return out(e, "d", "value");
    });
    return expectClose(at(data, 0.4), 2.5, 1e-5);
  });
  await test("divide by 0 = 0, by -20 = -0.5", async () => {
    const r0 = await render(0.5, (e) => {
      constNode(e, "a", 10); constNode(e, "b", 0);
      e.createNode("d", "divide", {});
      e.connect("a", "audio", "d", "in1");
      e.connect("b", "audio", "d", "in2");
      return out(e, "d", "value");
    });
    expectClose(at(r0.data, 0.4), 0, 1e-6, "/0 ");
    const r1 = await render(0.5, (e) => {
      constNode(e, "a", 10); constNode(e, "b", -20);
      e.createNode("d", "divide", {});
      e.connect("a", "audio", "d", "in1");
      e.connect("b", "audio", "d", "in2");
      return out(e, "d", "value");
    });
    return expectClose(at(r1.data, 0.4), -0.5, 1e-5, "/-20 ");
  });
  await test("frequency divider snaps to 1/2/4/8/16 and clamps 0..10", async () => {
    const run = async (cv, divider) => (await render(0.3, (e) => {
      constNode(e, "a", cv); constNode(e, "b", divider);
      e.createNode("f", "frequency-divider", { divider: 2 });
      e.connect("a", "audio", "f", "cv");
      e.connect("b", "audio", "f", "divider");
      return out(e, "f", "cv");
    })).data;
    expectClose(at(await run(7, 3.9), 0.2), 5, 0.01, "7V/3.9 ");
    expectClose(at(await run(7, 1), 0.2), 7, 0.01, "7V/1 ");
    return expectClose(at(await run(1, 16), 0.2), 0, 0.01, "1V/16 clamp ");
  });
  await test("logic OR: -5V OR +3V is true", async () => {
    const { data } = await render(0.3, (e) => {
      constNode(e, "a", -5); constNode(e, "b", 3);
      e.createNode("o", "logic-or", {});
      e.connect("a", "audio", "o", "in1");
      e.connect("b", "audio", "o", "in2");
      return out(e, "o", "value");
    });
    return expectClose(at(data, 0.2), 10, 1e-4);
  });
  await test("value mapper extrapolates beyond the point range", async () => {
    const { data } = await render(0.3, (e) => {
      constNode(e, "a", 254);
      e.createNode("m", "value-mapper", { point1In: 0, point1Out: 0, point2In: 127, point2Out: 10 });
      e.connect("a", "audio", "m", "in");
      return out(e, "m", "out");
    });
    return expectClose(at(data, 0.2), 20, 1e-4);
  });
  await test("bitcrusher quantizes the +/-5V domain", async () => {
    const { data } = await render(0.3, (e) => {
      constNode(e, "a", 1.3);
      e.createNode("b", "bitcrusher", { crush: 10 });
      e.connect("a", "audio", "b", "in");
      return out(e, "b", "out");
    });
    return expectClose(at(data, 0.2), 0, 1e-6);
  });
  await test("oscillator: 5V peak, gain CV keeps the 5V scale", async () => {
    const plain = await render(0.5, (e) => { sineOsc(e, "o", 440); return out(e, "o", "value"); });
    expectClose(peak(plain.data), 5, 0.02, "plain ");
    const cv = await render(0.5, (e) => {
      sineOsc(e, "o", 440); constNode(e, "g", 10);
      e.connect("g", "audio", "o", "gain");
      return out(e, "o", "value");
    });
    return expectClose(peak(cv.data), 5, 0.02, "gain cable ");
  });
  await test("oscillator: tune cable keeps the direct-set pitch", async () => {
    const { data } = await render(1.2, (e) => {
      sineOsc(e, "o", 440);
      e.setParam("o", "tuneVolts", 5, 0);
      e.setParam("o", "frequency", 440, 0);
      constNode(e, "t", 5);
      e.connect("t", "audio", "o", "tune");
      return out(e, "o", "value");
    });
    return expectClose(risingCrossings(data, 0.2, 1.2), 440, 2, "Hz ");
  });
  await test("oscillator: pitch cable + tune knob (+1V = one octave up)", async () => {
    const { data } = await render(1.2, (e) => {
      sineOsc(e, "o", 440);
      e.setParam("o", "tuneVolts", 6, 0);
      constNode(e, "p", 5 + 9 / 12); // A4
      e.connect("p", "audio", "o", "pitchCv");
      return out(e, "o", "value");
    });
    return expectClose(risingCrossings(data, 0.2, 1.2), 880, 3, "Hz ");
  });
  await test("oscillator saw starts at -1 after a phase reset (firmware phase)", async () => {
    const { data } = await render(0.3, (e) => {
      sineOsc(e, "o", 5, "sawtooth");
      return out(e, "o", "value");
    });
    return expectClose(data[48], -4.95, 0.3, "1 ms after reset ");
  });
  await test("hammond: single full drawbar peaks at 5V/9", async () => {
    const { data } = await render(0.5, (e) => {
      e.createNode("h", "hammond", { drawbars: [10, 0, 0, 0, 0, 0, 0, 0, 0], gain: 10 });
      e.setParam("h", "amplitude", 1, 0);
      e.setParam("h", "frequency", 200, 0);
      return out(e, "h", "value");
    });
    return expectClose(peak(data), 5 / 9, 0.01);
  });
  await test("lfo triangle phase 0 starts at 0V, phase 90 starts at 5V", async () => {
    const r0 = await render(0.2, (e) => {
      e.createNode("l", "lfo", { waveType: "triangle", rate: 1, amplitude: 10, phase: 0 });
      return out(e, "l", "value");
    });
    expectClose(r0.data[1], 0, 0.05, "phase0 ");
    const r90 = await render(0.2, (e) => {
      e.createNode("l", "lfo", { waveType: "triangle", rate: 1, amplitude: 10, phase: 90 });
      return out(e, "l", "value");
    });
    return expectClose(r90.data[1], 5, 0.05, "phase90 ");
  });
  await test("lfo rate above 50 Hz (max 500)", async () => {
    const { data } = await render(1.1, (e) => {
      e.createNode("l", "lfo", { waveType: "sine", rate: 200, maxRate: 500, amplitude: 10 });
      return out(e, "l", "value");
    });
    return expectClose(risingCrossings(data, 0.1, 1.1, 5), 200, 2, "Hz ");
  });
  await test("lfo output clamped to 0..10V", async () => {
    const { data } = await render(1.1, (e) => {
      e.createNode("l", "lfo", { waveType: "sine", rate: 5, amplitude: 10, zeroPointFixed: true, zeroPoint: 8 });
      return out(e, "l", "value");
    });
    let mx = -1e9;
    let mn = 1e9;
    for (const v of data) { mx = Math.max(mx, v); mn = Math.min(mn, v); }
    expectClose(mx, 10, 0.02, "max ");
    return expectClose(mn, 3, 0.05, "min ");
  });
  await test("filter lowpass 12dB: |H(fc)| = Q (linear Q like the firmware)", async () => {
    const { data } = await render(0.8, (e) => {
      sineOsc(e, "o", 1000);
      e.createNode("f", "filter", { filterType: "lowpass", slope: 12, cutoffMinHz: 1000, cutoffMaxHz: 1000,
        resonanceMinQ: 4, resonanceMaxQ: 4 });
      e.connect("o", "value", "f", "in");
      return out(e, "f", "out");
    });
    return expectClose(peak(data, 0.5, 0.8) / 5, 4, 0.1, "gain ");
  });
  await test("filter lowpass 6dB is a one-pole", async () => {
    const { data } = await render(0.8, (e) => {
      sineOsc(e, "o", 1000);
      e.createNode("f", "filter", { filterType: "lowpass", slope: 6, cutoffMinHz: 1000, cutoffMaxHz: 1000 });
      e.connect("o", "value", "f", "in");
      return out(e, "f", "out");
    });
    const c = Math.exp(-2 * Math.PI * 1000 / SR);
    const w = 2 * Math.PI * 1000 / SR;
    const expected = (1 - c) / Math.hypot(1 - c * Math.cos(w), c * Math.sin(w));
    return expectClose(peak(data, 0.5, 0.8) / 5, expected, 0.01, "gain ");
  });
  await test("filter cutoff CV cable drives the processor", async () => {
    const run = async (volts) => (await render(0.8, (e) => {
      sineOsc(e, "o", 2000);
      e.createNode("f", "filter", { filterType: "lowpass", slope: 24, cutoffMinHz: 100, cutoffMaxHz: 10000,
        resonanceMinQ: 0.707, resonanceMaxQ: 0.707 });
      constNode(e, "c", volts);
      e.connect("o", "value", "f", "in");
      e.connect("c", "audio", "f", "cutoff");
      return out(e, "f", "out");
    })).data;
    const low = peak(await run(0), 0.5, 0.8);
    const high = peak(await run(10), 0.5, 0.8);
    if (!(low < 0.01 && high > 4.5)) throw new Error(`low=${low} high=${high}`);
    return `low=${low.toFixed(4)} high=${high.toFixed(3)}`;
  });
  await test("curtis filter runs, stays finite and resonates", async () => {
    const { data } = await render(0.8, (e) => {
      sineOsc(e, "o", 220, "sawtooth");
      e.createNode("f", "curtis-filter", { filterType: "lowpass", slope: 24, cutoff: 6, resonance: 8 });
      e.connect("o", "value", "f", "in");
      return out(e, "f", "out");
    });
    if (!allFinite(data)) throw new Error("NaN");
    const p = peak(data, 0.4, 0.8);
    if (!(p > 0.5 && p <= 5.01)) throw new Error(`peak ${p}`);
    return `peak ${p.toFixed(3)}`;
  });
  await test("filter frequency response for UI", async () => {
    const { engine } = await render(0.1, (e) => {
      e.createNode("f", "filter", { filterType: "lowpass", slope: 18, cutoff: 5 });
      e.createNode("g", "curtis-filter", { filterType: "bandpass", slope: 24, cutoff: 5 });
      return out(e, "f", "out");
    });
    const a = engine.getFilterFrequencyResponse("f");
    const b = engine.getFilterFrequencyResponse("g");
    if (!a.every(Number.isFinite) || !b.every(Number.isFinite)) throw new Error("non finite");
    if (!(a[0] > 0.9 && a[a.length - 1] < 0.1)) throw new Error(`lp shape ${a[0]} ${a[a.length - 1]}`);
    return `lp ${a[0].toFixed(2)}..${a[a.length - 1].toFixed(4)}`;
  });
  await test("reverb light: wet tail present, bypass passes input", async () => {
    const wet = await render(1.0, (e) => {
      sineOsc(e, "o", 300);
      e.createNode("r", "reverb-light", { roomSize: 0.5, wetLevelDb: 0, dryLevelDb: -96 });
      e.connect("o", "value", "r", "inL");
      e.connect("o", "value", "r", "inR");
      return out(e, "r", "outL");
    });
    if (!allFinite(wet.data)) throw new Error("NaN");
    const p = peak(wet.data, 0.5, 1.0);
    if (!(p > 0.1)) throw new Error(`wet peak ${p}`);
    const byp = await render(0.5, (e) => {
      constNode(e, "c", 3);
      e.createNode("r", "reverb-light", { bypass: true });
      e.connect("c", "audio", "r", "inL");
      return out(e, "r", "outL");
    });
    return expectClose(at(byp.data, 0.4), 3, 1e-5, `wet ${p.toFixed(2)} bypass `);
  });
  await test("delay / chorus / esp-eq / graphic-eq bypass", async () => {
    const check = async (type, inPort, outPort, extra = {}) => {
      const { data } = await render(0.4, (e) => {
        constNode(e, "c", 3);
        e.createNode("x", type, { bypass: true, ...extra });
        e.connect("c", "audio", "x", inPort);
        return out(e, "x", outPort);
      });
      expectClose(at(data, 0.3), 3, 1e-4, `${type} `);
    };
    await check("delay", "inL", "outL");
    await check("chorus", "in", "left");
    await check("esp-eq", "inL", "outL", { filterType: "highpass", frequency: 1000 });
    await check("graphic-eq", "inL", "outL", { gain: 2, gain1: 12 });
    return "ok";
  });
  await test("esp-eq lowpass Q is linear (|H(fc)| = Q)", async () => {
    const { data } = await render(0.8, (e) => {
      sineOsc(e, "o", 1000);
      e.createNode("q", "esp-eq", { filterType: "lowpass", frequency: 1000, q: 4 });
      e.connect("o", "value", "q", "inL");
      return out(e, "q", "outL");
    });
    return expectClose(peak(data, 0.5, 0.8) / 5, 4, 0.1, "gain ");
  });
  await test("fm-oscillator: plain carrier 5V, square duty 25% mean -2.5V", async () => {
    const plain = await render(0.5, (e) => {
      e.createNode("m", "fm-oscillator", { waveType: "sine", frequency: 220, gain: 10 });
      e.setParam("m", "amplitude", 1, 0);
      return out(e, "m", "value");
    });
    expectClose(peak(plain.data), 5, 0.01, "sine peak ");
    const duty = await render(0.6, (e) => {
      e.createNode("m", "fm-oscillator", { waveType: "square", frequency: 100, gain: 10, dutyCycle: 2.5 });
      e.setParam("m", "amplitude", 1, 0);
      return out(e, "m", "value");
    });
    return expectClose(mean(duty.data, 0.1, 0.6), -2.5, 0.05, "square mean ");
  });
  await test("fm-oscillator: phase modulation matches sin(2pi(fc t + I*m(t)))", async () => {
    // fmIndex 10V = kFmIndexMaxPhaseDeviation (4 cycles) per 5V of fmIn, so a
    // constant 0.3125V on fmIn shifts the carrier's phase by a quarter cycle.
    const { data } = await render(0.5, (e) => {
      e.createNode("m", "fm-oscillator", { waveType: "sine", frequency: 1, gain: 10, fmIndex: 10 });
      e.setParam("m", "amplitude", 1, 0);
      constNode(e, "c", 0.3125);
      e.connect("c", "audio", "m", "fmIn");
      return out(e, "m", "value");
    });
    // sin(2pi(t + 0.25)) = cos(2pi t) ~ 1 near t = 0 (processor delay << 1 s)
    return expectClose(at(data, 0.02) / 5, Math.cos(2 * Math.PI * 0.02), 0.03, "value ");
  });
  await test("fm-oscillator supersaw / noise / linear stay finite", async () => {
    for (const waveType of ["supersaw", "noise", "pink-noise", "linear-up", "triangle", "sawtooth-down"]) {
      const { data } = await render(0.3, (e) => {
        e.createNode("m", "fm-oscillator", { waveType, frequency: 110, gain: 10, feedback: 5 });
        e.setParam("m", "amplitude", 1, 0);
        return out(e, "m", "value");
      });
      if (!allFinite(data)) throw new Error(`${waveType} NaN`);
      const p = peak(data, 0.1, 0.3);
      if (!(p > 0.1 && p <= 5.001)) throw new Error(`${waveType} peak ${p}`);
    }
    return "ok";
  });
  await test("output switch holds the unselected output", async () => {
    const { data } = await render(0.8, (e, octx) => {
      constNode(e, "c", 7);
      e.createNode("s", "output-switch", { selectOut2: false });
      e.connect("c", "audio", "s", "in");
      octx.suspend(0.4).then(() => {
        e.setParam("s", "selectOut2", true);
        e.setParam("c", "value", 2, 0);
        octx.resume();
      });
      return out(e, "s", "out1");
    });
    expectClose(at(data, 0.3), 7, 1e-4, "before ");
    return expectClose(at(data, 0.75), 7, 1e-4, "held ");
  });
  await test("smoke: build, wire and remove every node type", async () => {
    const types = ["oscillator", "fm-oscillator", "hammond", "lfo", "const", "up-down-button", "button", "knob",
      "switch", "output-switch", "audio-out", "usb-out", "gain", "linked-input", "linked-output", "filter",
      "curtis-filter", "esp-eq", "graphic-eq", "mixer", "multiplexer", "output-multiplexer", "multiplier", "adder",
      "divide", "frequency-divider", "abs", "logic-and", "logic-or", "logic-xor", "logic-not", "value-mapper",
      "detune", "adsr", "dx-envelope", "bitcrusher", "wave-folder", "envelope-follower", "pan", "sample-hold",
      "burst-generator", "chorus", "delay", "reverb-light", "audio-output-link", "audio-input-link", "vu-meter",
      "oscilloscope", "spectrum"];
    await render(0.2, (e) => {
      types.forEach((t) => e.createNode(`n-${t}`, t, {}));
      for (const [id, params] of [["n-fm-oscillator", ["fmIndex", "feedback", "superSawDetune", "superSawMix", "dutyCycle", "waveType"]],
        ["n-filter", ["cutoff", "resonance", "slope", "filterType", "coeffStride"]],
        ["n-reverb-light", ["roomSize", "damping", "wetLevelDb", "dryLevelDb", "bypass"]]]) {
        params.forEach((p) => e.setParam(id, p, p === "waveType" ? "supersaw" : p === "filterType" ? "notch" : 3));
      }
      e.connect("n-lfo", "value", "n-fm-oscillator", "fmIndex");
      e.connect("n-lfo", "value", "n-fm-oscillator", "fmIn");
      e.disconnect("n-lfo", "value", "n-fm-oscillator", "fmIndex");
      e.connect("n-lfo", "value", "n-filter", "cutoff");
      e.disconnect("n-lfo", "value", "n-filter", "cutoff");
      e.connect("n-lfo", "value", "n-oscillator", "pitchCv");
      e.disconnect("n-lfo", "value", "n-oscillator", "pitchCv");
      e.connect("n-lfo", "value", "n-audio-input-link", "gain3");
      const tap = out(e, "n-fm-oscillator", "value");
      types.forEach((t) => e.removeNode(`n-${t}`));
      return tap;
    });
    return `${types.length} types`;
  });
  await test("voiced filter in global mode follows voice 0", async () => {
    const { engine } = await newEngine(0.1);
    engine.createNode("f", "filter", { filterType: "lowpass" }, 4);
    engine.setParam("f", "global", true);
    const w = engine.nodes.get("f");
    if (w.instances[2].globalPrimary !== w.instances[0]) throw new Error("globalPrimary not set");
    engine.setParam("f", "global", false);
    if (w.instances[2].globalPrimary !== null) throw new Error("globalPrimary not cleared");
    return "ok";
  });
  await test("sample loading off: downloads deferred until loadDeferredSamples()", async () => {
    const { engine } = await newEngine(0.1);
    const fetched = [];
    const realFetch = window.fetch;
    window.fetch = (url) => { fetched.push(String(url)); return Promise.reject(new Error("offline test")); };
    try {
      engine.setSampleLoadingEnabled(false);
      let notified = -1;
      engine.onDeferredSamplesChange = (count) => { notified = count; };
      engine.createNode("w", "wave-player", { sampleFile: "a.wav" });
      engine.createNode("w0", "wave-player", { sampleFile: "" });
      engine.createNode("p", "mda-piano", {});
      engine.createNode("s", "soundfont-player", { path: "soundfonts/x.sf2" });
      if (fetched.length !== 0) throw new Error(`fetched while off: ${fetched}`);
      if (engine.getDeferredSampleCount() !== 3 || notified !== 3) {
        throw new Error(`deferred ${engine.getDeferredSampleCount()} notified ${notified}`);
      }
      engine.removeNode("s");
      if (engine.getDeferredSampleCount() !== 2) throw new Error("removed node still deferred");
      engine.loadDeferredSamples();
      if (fetched.length !== 2 || engine.getDeferredSampleCount() !== 0 || notified !== 0) {
        throw new Error(`after load: fetched ${fetched.length}, deferred ${engine.getDeferredSampleCount()}`);
      }
      return "ok";
    } finally {
      window.fetch = realFetch;
    }
  });
  return results;
}
