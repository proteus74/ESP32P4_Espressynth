import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || "playwright");
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined });
const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
const errors = [];
page.on("pageerror", (error) => errors.push(error.message));
await page.addInitScript(() => {
  window.WebSocket = class extends EventTarget {
    static OPEN = 1;
    readyState = 1;
    send() {}
    close() {}
  };
});
await page.route("http://monoflop.test/**", async (route) => {
  const url = new URL(route.request().url());
  if (url.pathname.startsWith("/api/")) {
    const data = url.pathname === "/api/preset" ? { schemaVersion: 4, widgets: [], connections: [] }
      : url.pathname === "/api/presets" ? { presets: [] } : [];
    return route.fulfill({ contentType: "application/json", body: JSON.stringify(data) });
  }
  try {
    const file = path.join(root, "data", url.pathname === "/" ? "index.html" : url.pathname);
    let body = readFileSync(file);
    if (url.pathname === "/static/js/main.js") body = Buffer.concat([body, Buffer.from(`
window.__monoflopTest = { createWidget, serializePreset, applyPresetData, deliverToWidget, audioEngine,
 switchPage, openMonoflopSettings, WIDGET_CATEGORIES, monoflopStates, updateSampleHoldVisual,
 select(id) { selectOnlyWidget(id); },
 entry(id) { const entry = audioEngine.nodes.get(id); return entry.voiced ? entry.instances[0] : entry; },
 wire(source, target) { connections.push({ id: 'test-threshold', from: { widgetId: source, portId: 'audio' }, to: { widgetId: target, portId: 'threshold' } }); updateSampleHoldVisual(target); },
 process(id, input, threshold) {
   const output = new Float32Array(input.length);
   const entry = this.entry(id);
   entry.processor.onaudioprocess({ inputBuffer: { getChannelData: (i) => Float32Array.from(i === 0 ? input : threshold ?? input.map(() => entry.controlSources[1].offset.value)) },
     outputBuffer: { getChannelData: () => output } });
   return [...output];
 } };
`)]);
    return route.fulfill({ contentType: file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html", body });
  } catch {
    return route.fulfill({ status: 404 });
  }
});

try {
  await page.goto("http://monoflop.test/");
  await page.waitForFunction(() => window.__monoflopTest && !document.getElementById("audioStartBtn")?.disabled);
  const id = await page.evaluate(() => window.__monoflopTest.createWidget("monoflop", "Monoflop", 40, 40).id);
  assert.deepEqual(await page.locator(`#${id} .port-row-in`).evaluateAll((rows) => rows.map((row) => row.dataset.portId)), ["in", "threshold"]);
  assert.equal(await page.locator(`#${id} .port-row-out .port-label`).textContent(), "Trigger");
  await page.locator(`#${id} .port-row-in[data-port-id="in"] .port-label`).dblclick();
  await page.locator("#monoflopPulseMsInput").fill("25,5");
  await page.locator("#monoflopSettingsSave").click();
  assert.equal(await page.evaluate((id) => window.__monoflopTest.entry(id).pulseMs, id), 25.5);
  await page.evaluate((id) => window.__monoflopTest.openMonoflopSettings(id), id);
  await page.locator("#monoflopPulseMsInput").fill("1000");
  await page.locator("#monoflopSettingsCancel").click();
  assert.equal(await page.evaluate((id) => window.__monoflopTest.entry(id).pulseMs, id), 25.5);

  const dsp = await page.evaluate((id) => {
    const t = window.__monoflopTest;
    const entry = t.entry(id);
    const rate = t.audioEngine.ctx.sampleRate;
    const durations = [];
    for (const ms of [1, 10, 25.5, 1000]) {
      entry.countWasHigh = false;
      entry.samplesLeft = 0;
      t.audioEngine.setParam(id, "pulseMs", ms);
      const length = Math.ceil(ms * (rate / 1000));
      const first = t.process(id, Array(17).fill(10));
      const tail = t.process(id, Array(length + 257 - 17).fill(10));
      const output = first.concat(tail);
      if (output.some((value, i) => value !== (i < length ? 10 : 0))) throw new Error('Wrong pulse length or held-high repeat');
      durations.push(output.filter((value) => value === 10).length);
    }
    entry.countWasHigh = false;
    entry.samplesLeft = 0;
    t.audioEngine.setParam(id, "pulseMs", 1);
    const length = Math.ceil(rate / 1000);
    const input = Array(length * 3).fill(0);
    input[0] = 5;
    input[1] = 5.01;
    input[10] = 10;
    input[length + 12] = 10;
    const output = t.process(id, input);
    for (let i = 0; i < output.length; i++) {
      const high = (i >= 1 && i < 1 + length) || (i >= length + 12 && i < length * 2 + 12);
      if (output[i] !== (high ? 10 : 0)) throw new Error('Early low, equality, busy edge or rearming failed');
    }
    entry.countWasHigh = false;
    entry.samplesLeft = 0;
    const cv = t.process(id, [5, 5, 5, 5], [10, 5, 4, -1]);
    t.audioEngine.setParam(id, "pulseMs", NaN);
    const defaultDuration = entry.pulseMs;
    t.audioEngine.setParam(id, "pulseMs", 0);
    const minimum = entry.pulseMs;
    t.audioEngine.setParam(id, "pulseMs", 100000);
    const maximum = entry.pulseMs;
    return { durations, expected: [1, 10, 25.5, 1000].map((ms) => Math.ceil(ms * (rate / 1000))), cv, defaultDuration, minimum, maximum };
  }, id);
  assert.deepEqual(dsp.durations, dsp.expected);
  assert.deepEqual(dsp.cv, [0, 0, 10, 10]);
  assert.deepEqual([dsp.defaultDuration, dsp.minimum, dsp.maximum], [10, 1, 60000]);
  const wiring = await page.evaluate((id) => {
    const t = window.__monoflopTest;
    t.deliverToWidget(id, "in", 10);
    t.deliverToWidget(id, "threshold", 3);
    const scalar = t.entry(id).controlSources.map((source) => source.offset.value);
    t.audioEngine.createNode('mono-in', 'const', { value: 10 });
    t.audioEngine.createNode('mono-threshold', 'const', { value: 3 });
    t.audioEngine.connect('mono-in', 'audio', id, 'in');
    t.audioEngine.connect('mono-threshold', 'audio', id, 'threshold');
    const connected = [...t.entry(id).controlConnectionCounts];
    t.audioEngine.disconnect('mono-in', 'audio', id, 'in');
    t.audioEngine.disconnect('mono-threshold', 'audio', id, 'threshold');
    const restored = t.entry(id).controlSources.map((source) => source.offset.value);
    return { scalar, connected, restored };
  }, id);
  assert.deepEqual(wiring, { scalar: [10, 3], connected: [1, 1], restored: [0, 5] });
  await page.locator(`#${id} input.port-slider`).evaluate((slider) => { slider.value = '7'; slider.dispatchEvent(new Event('input', { bubbles: true })); });
  await page.evaluate((id) => window.__monoflopTest.openMonoflopSettings(id), id);
  await page.locator('#monoflopPulseMsInput').fill('25.5');
  await page.locator('#monoflopSettingsSave').click();
  await page.evaluate((id) => window.__monoflopTest.select(id), id);
  await page.keyboard.press('Control+c');
  await page.keyboard.press('Control+v');
  await page.mouse.click(700, 400);
  const saved = await page.evaluate(() => window.__monoflopTest.serializePreset());
  const widgets = saved.pages.generator.widgets.filter((widget) => widget.type === 'monoflop');
  assert.equal(widgets.length, 2);
  for (const widget of widgets) assert.deepEqual(widget.params, { threshold: 7, pulseMs: 25.5 });
  await page.evaluate((saved) => window.__monoflopTest.applyPresetData(saved), saved);
  assert.equal(await page.evaluate((id) => window.__monoflopTest.entry(id).samplesLeft, id), 0);
  assert.equal(await page.locator(`#${id} input.port-slider`).inputValue(), '7');
  await page.evaluate((id) => window.__monoflopTest.wire('mono-threshold', id), id);
  assert.equal(await page.locator(`#${id} input.port-slider`).isDisabled(), true);
  const fx = await page.evaluate(() => {
    const t = window.__monoflopTest;
    t.switchPage('fx');
    return t.createWidget('monoflop', 'Monoflop', 40, 40).id;
  });
  assert.equal(await page.locator(`#${fx}`).count(), 1);
  const cleanup = await page.evaluate((id) => {
    const t = window.__monoflopTest, entry = t.entry(id);
    t.audioEngine.removeNode(id);
    return !t.audioEngine.nodes.has(id) && entry.processor.onaudioprocess === null;
  }, fx);
  assert.equal(cleanup, true);
  assert.deepEqual(errors, []);
  console.log('PASS Monoflop: ports, settings, fixed duration, held high, block continuity, threshold CV, strict crossing, busy edges, rearming, scalar/audio wiring, slider lock, copy/paste, preset load, FX page, cleanup');
} finally {
  await browser.close();
}
