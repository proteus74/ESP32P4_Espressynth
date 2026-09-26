import assert from "node:assert/strict";
import { test } from "node:test";
import { COMB_FILTER_DEFAULTS, COMB_FILTER_PORTS, combFilterCv, createCombFilterDsp } from "../../data/static/js/comb-filter-core.js";

const sampleRate = 48000;
function impulse(params, seconds = 0.5) {
  const entry = { ...COMB_FILTER_DEFAULTS, ...params };
  const dsp = createCombFilterDsp(entry, sampleRate);
  const length = Math.round(seconds * sampleRate);
  const inputs = [new Float32Array(length), ...COMB_FILTER_PORTS.map((p) => new Float32Array(length).fill(entry[p]))];
  inputs[0][0] = 1;
  const output = new Float32Array(length);
  dsp.render(inputs, output);
  return { entry, dsp, inputs, output };
}
const close = (actual, expected, tolerance = 1e-6) => assert.ok(Math.abs(actual - expected) <= tolerance, `${actual} != ${expected}`);

test("comb: exact 100ms maximum and one-sample minimum", () => {
  const max = impulse({ time: 10, feedback: 0, mix: 10 }).output;
  close(max[4800], 1);
  assert.equal(max.reduce((sum, x) => sum + Math.abs(x), 0), 1);
  const min = impulse({ time: 0, feedback: 0, mix: 10 }).output;
  close(min[1], 1);
});

test("comb: fractional delay interpolates neighbouring samples", () => {
  const output = impulse({ time: 10.5 / 480, feedback: 0, mix: 10 }).output;
  close(output[10], 0.5);
  close(output[11], 0.5);
});

test("comb: feedback polarity changes echo signs", () => {
  for (const polarity of [0, 10]) {
    const output = impulse({ time: 1, feedback: 5, mix: 10, polarity }).output;
    close(output[480], 1);
    close(output[960], polarity === 0 ? 0.475 : -0.475);
    close(output[1440], 0.475 ** 2);
  }
});

test("comb: mix endpoints, 50% crossfade and final gain", () => {
  const dry = impulse({ mix: 0 }).output;
  close(dry[0], 1);
  close(dry.reduce((sum, x) => sum + Math.abs(x), 0), 1);
  const half = impulse({ time: 1, feedback: 0, mix: 5, gain: 5 }).output;
  close(half[0], 0.25);
  close(half[480], 0.25);
  assert.ok(impulse({ gain: 0 }).output.every((x) => x === 0));
});

test("comb: damping filters feedback, keeps first wet echo unchanged", () => {
  const damped = impulse({ time: 1, feedback: 10, mix: 10, damping: 0 }).output;
  close(damped[480], 1);
  assert.ok(damped[960] > 0 && damped[960] < 0.01);
  assert.ok(damped[961] > 0);
  close(impulse({ time: 1, feedback: 10, mix: 10, damping: 10 }).output[960], 0.95);
});

test("comb: displayed response matches impulse DFT including damping and fractional delay", () => {
  for (const params of [
    { time: 0.5, feedback: 8, mix: 5, gain: 7 },
    { time: 0.51, feedback: 9, mix: 10, polarity: 10, damping: 6 },
  ]) {
    const { dsp, output } = impulse(params, 1);
    const freqs = new Float32Array([0, 100, 200, 1000, 5000, 19000]);
    const magnitudes = new Float32Array(freqs.length);
    dsp.response(freqs, magnitudes);
    freqs.forEach((hz, index) => {
      let real = 0, imaginary = 0;
      output.forEach((value, i) => {
        const phase = 2 * Math.PI * hz * i / sampleRate;
        real += value * Math.cos(phase);
        imaginary -= value * Math.sin(phase);
      });
      close(magnitudes[index], Math.hypot(real, imaginary), 2e-5);
    });
  }
});

test("comb: parameter bounds and maximum feedback remain finite", () => {
  for (const invalid of [NaN, Infinity, -Infinity, "5", null]) close(combFilterCv(invalid, 5), 5);
  close(combFilterCv(-1), 0);
  close(combFilterCv(11), 10);
  for (const polarity of [0, 10]) {
    const { output } = impulse({ time: 0, feedback: 10, mix: 10, polarity }, 2);
    assert.ok(output.every(Number.isFinite));
    assert.ok(output.slice(-100).every((x) => x === 0));
  }
});

test("comb: block boundaries preserve history and parameter transitions stay finite", () => {
  const { entry, inputs, output } = impulse({ time: 0.5, feedback: 10, mix: 10 });
  const dsp = createCombFilterDsp(entry, sampleRate);
  const blocked = new Float32Array(output.length);
  for (let start = 0; start < output.length; start += 256) {
    const end = Math.min(start + 256, output.length);
    dsp.render(inputs.map((input) => input.subarray(start, end)), blocked.subarray(start, end));
  }
  assert.deepEqual(blocked, output);
  inputs[1].fill(10);
  inputs[4].fill(10);
  inputs[5].fill(0);
  dsp.render(inputs, blocked);
  assert.ok(blocked.every(Number.isFinite));
});
