import assert from "node:assert/strict";
import { test } from "node:test";
import { OVERDRIVE_DEFAULTS, OVERDRIVE_PORTS, createOverdriveDsp, overdriveModeIndex } from "../../data/static/js/overdrive-core.js";

const sampleRate = 44100;
function run(params, seconds = 0.5, freq = 110, amp = 5) {
  const entry = { ...OVERDRIVE_DEFAULTS, ...params };
  const length = Math.round(seconds * sampleRate);
  const input = Float32Array.from({ length }, (_, i) => amp * Math.sin(2 * Math.PI * freq * i / sampleRate));
  const inputs = [input, ...OVERDRIVE_PORTS.map((p) => new Float32Array(length).fill(entry[p]))];
  const output = new Float32Array(length);
  createOverdriveDsp(sampleRate).render(inputs, output);
  return { input, output };
}
const tail = (a) => a.subarray(a.length >> 1);
const peak = (a) => tail(a).reduce((m, x) => Math.max(m, Math.abs(x)), 0);
const rms = (a) => Math.sqrt(tail(a).reduce((s, x) => s + x * x, 0) / tail(a).length);
const mean = (a) => tail(a).reduce((s, x) => s + x, 0) / tail(a).length;

test("overdrive: mode CV steps into four 2.5V bands", () => {
  assert.deepEqual([0, 2.49, 2.5, 5, 7.5, 10].map(overdriveModeIndex), [0, 0, 1, 2, 3, 3]);
});

test("overdrive: auto-level keeps loudness at input level for every mode, drive and input level", () => {
  for (const amp of [0.5, 5]) {
    for (const mode of [0, 2.5, 5, 7.5]) {
      for (const drive of [0, 5, 10]) {
        const { input, output } = run({ mode, drive, tone: 10 }, 1, 110, amp);
        const ratio = rms(output) / rms(input);
        assert.ok(ratio > 0.8 && ratio < 1.25, `amp ${amp} mode ${mode} drive ${drive}: ratio ${ratio}`);
      }
    }
  }
});

test("overdrive: modes produce clearly different waveforms at full drive", () => {
  const outs = [0, 2.5, 5, 7.5].map((mode) => tail(run({ mode, drive: 10, tone: 10 }, 1).output));
  const corr = (a, b) => {
    let ab = 0, aa = 0, bb = 0;
    for (let i = 0; i < a.length; i += 1) { ab += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
    return ab / Math.sqrt(aa * bb);
  };
  for (let a = 0; a < outs.length; a += 1) {
    for (let b = a + 1; b < outs.length; b += 1) {
      const c = corr(outs[a], outs[b]);
      assert.ok(c < 0.97, `modes ${a}/${b} correlation ${c}`);
    }
  }
});

test("overdrive: mix 0 passes dry signal", () => {
  const { input, output } = run({ mix: 0, drive: 10 });
  output.forEach((y, i) => assert.ok(Math.abs(y - input[i]) < 1e-5));
});

test("overdrive: gain 0 is silent", () => {
  assert.equal(peak(run({ gain: 0 }).output), 0);
});

test("overdrive: bias leaves no DC at the output", () => {
  for (const bias of [0, 10]) assert.ok(Math.abs(mean(run({ bias, drive: 8 }, 2).output)) < 0.05, `bias ${bias}`);
});
