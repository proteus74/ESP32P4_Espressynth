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
await page.route("http://counter.test/**", async (route) => {
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
window.__counterTest = { createWidget, serializePreset, applyPresetData, deliverToWidget, audioEngine,
 clockStates, setMasterClockBpm, fireClockPulse, stopClockTimer,
 wireClock(source, target) { connections.push({ id: 'test-clock', from: { widgetId: source, portId: 'pulse' }, to: { widgetId: target, portId: 'count' } }); },
 select(id) { selectOnlyWidget(id); },
 entry(id) { const entry = audioEngine.nodes.get(id); return entry.voiced ? entry.instances[0] : entry; },
 process(id, count, reset) {
   const output = new Float32Array(count.length);
   this.entry(id).processor.onaudioprocess({ inputBuffer: { getChannelData: (i) => Float32Array.from(i === 0 ? count : reset) },
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
  await page.goto("http://counter.test/");
  await page.waitForFunction(() => window.__counterTest && !document.getElementById("audioStartBtn")?.disabled);
  const id = await page.evaluate(() => window.__counterTest.createWidget("counter", "Counter", 40, 40).id);
  assert.deepEqual(await page.locator(`#${id} .port-row-in`).evaluateAll((rows) => rows.map((row) => row.dataset.portId)), ["count", "reset"]);
  assert.equal(await page.locator(`#${id} .port-row-out`).getAttribute("data-port-id"), "out");
  const process = (count, reset = count.map(() => 0)) => page.evaluate(({ id, count, reset }) => window.__counterTest.process(id, count, reset), { id, count, reset });
  assert.deepEqual(await process([0, 4.99, 5, 10, 0, 5]), [0, 0, 1, 1, 1, 2]);
  assert.deepEqual(await process([10, 0, 10]), [2, 2, 3], "Count state survives audio block boundaries");
  assert.deepEqual(await process([0, 10, 0, 10, 10, 0, 10], [0, 5, 5, 5, 0, 0, 0]), [3, 0, 0, 0, 0, 0, 1]);
  const repeated = await process(Array.from({ length: 24 }, (_, i) => i % 2 ? 10 : 0));
  assert.equal(repeated.at(-1), 13, "Output is an unscaled count, not clamped to 10V");
  const second = await page.evaluate(() => window.__counterTest.createWidget("counter", "Counter", 340, 40).id);
  assert.deepEqual(await page.evaluate((id) => window.__counterTest.process(id, [0, 5], [0, 0]), second), [0, 1]);
  const inputs = await page.evaluate((id) => {
    const t = window.__counterTest;
    t.deliverToWidget(id, "count", 10);
    t.deliverToWidget(id, "reset", 5);
    return t.entry(id).controlSources.map((source) => source.offset.value);
  }, id);
  assert.deepEqual(inputs, [10, 5], "Clock/Button control values reach separate DSP inputs");
  const wiring = await page.evaluate((id) => {
    const t = window.__counterTest;
    t.audioEngine.createNode("test-count", "const", { value: 10 });
    t.audioEngine.createNode("test-reset", "const", { value: 0 });
    t.audioEngine.connect("test-count", "audio", id, "count");
    t.audioEngine.connect("test-reset", "audio", id, "reset");
    const connected = [...t.entry(id).controlConnectionCounts];
    t.audioEngine.disconnect("test-count", "audio", id, "count");
    t.audioEngine.disconnect("test-reset", "audio", id, "reset");
    return { connected, disconnected: [...t.entry(id).controlConnectionCounts], values: t.entry(id).controlSources.map((source) => source.offset.value) };
  }, id);
  assert.deepEqual(wiring, { connected: [1, 1], disconnected: [0, 0], values: [0, 0] });
  await page.evaluate((id) => { const entry = window.__counterTest.entry(id); entry.count = 0xffffffff; entry.countWasHigh = false; }, id);
  await process([10, 0, 10]);
  assert.equal(await page.evaluate((id) => window.__counterTest.entry(id).count, id), 0xffffffff, "Counter saturates instead of wrapping");

  await page.evaluate((id) => window.__counterTest.select(id), id);
  await page.keyboard.press("Control+c");
  await page.keyboard.press("Control+v");
  await page.mouse.click(700, 400);
  const saved = await page.evaluate(() => window.__counterTest.serializePreset());
  assert.equal(saved.pages.generator.widgets.filter((widget) => widget.type === "counter").length, 3);
  await page.evaluate((data) => window.__counterTest.applyPresetData(data), saved);
  assert.deepEqual(await page.evaluate((id) => window.__counterTest.process(id, [0, 5], [0, 0]), id), [0, 1], "Preset load starts a fresh counter");
  const node = await page.evaluate((id) => {
    const t = window.__counterTest;
    const entry = t.entry(id);
    t.audioEngine.removeNode(id);
    return { removed: !t.audioEngine.nodes.has(id), released: entry.processor.onaudioprocess === null };
  }, second);
  assert.deepEqual(node, { removed: true, released: true });
  const timing = await page.evaluate(() => {
    const t = window.__counterTest;
    const clock = t.createWidget("clock", "Clock", 40, 500);
    const target = t.createWidget("counter", "Counter", 350, 500);
    t.stopClockTimer(clock.id);
    const state = t.clockStates.get(clock.id);
    const dot = clock.querySelector('.port-dot[data-port-id="pulse"][data-port-dir="out"]');
    const label = clock.querySelector('.port-row-out[data-port-id="pulse"] .port-label').textContent;
    const originals = { setInterval, clearInterval, setTimeout, clearTimeout };
    let tick;
    window.setInterval = (callback, ms) => (tick = { callback, ms });
    window.setTimeout = (callback, ms) => ({ callback, ms });
    window.clearInterval = window.clearTimeout = (timer) => { timer.cancelled = true; };
    try {
      t.setMasterClockBpm(60);
      t.fireClockPulse(clock.id);
      const unconnected = dot.classList.contains("port-active");
      state.offTimer.callback();
      t.wireClock(clock.id, target.id);
      const probe = document.createElement("div");
      probe.style.backgroundColor = "var(--ok)";
      document.body.append(probe);
      const expectedGreen = getComputedStyle(probe).backgroundColor;
      probe.remove();
      const periods = [], widths = [], flashes = [], green = [], counts = [];
      for (const bpm of [60, 120, 240]) {
        t.setMasterClockBpm(bpm);
        state.pulseMs = 1000;
        periods.push(tick.ms);
        let count = 0;
        for (let step = 0; step < 4; step += 1) {
          tick.callback();
          if (t.entry(target.id).controlSources[0].offset.value === 10) count += 1;
          if (step === 0) {
            widths.push(state.offTimer.ms);
            flashes.push(state.blinkTimer.ms);
            green.push(getComputedStyle(dot).backgroundColor === expectedGreen);
          }
          state.blinkTimer.callback();
          if (dot.classList.contains("port-active")) throw new Error("Trigger blink did not end");
          state.offTimer.callback();
          if (t.entry(target.id).controlSources[0].offset.value !== 0) throw new Error("Trigger stayed high");
        }
        counts.push(count);
      }
      t.fireClockPulse(clock.id);
      const offTimer = state.offTimer, blinkTimer = state.blinkTimer;
      t.setMasterClockBpm(0);
      return { label, unconnected, periods, widths, flashes, green, counts,
        stopped: !dot.classList.contains("port-active") && t.entry(target.id).controlSources[0].offset.value === 0,
        cancelled: offTimer.cancelled && blinkTimer.cancelled,
        port: t.serializePreset().pages.generator.connections.find((connection) => connection.id === 'test-clock').from.portId };
    } finally {
      Object.assign(window, originals);
    }
  });
  assert.equal(timing.label, "Trigger");
  assert.equal(timing.unconnected, false);
  assert.deepEqual(timing.periods, [250, 125, 62.5]);
  assert.deepEqual(timing.widths, [125, 62.5, 31.25]);
  assert.deepEqual(timing.flashes, [50, 50, 31.25]);
  assert.deepEqual(timing.green, [true, true, true]);
  assert.deepEqual(timing.counts, [4, 4, 4]);
  assert.equal(timing.stopped, true);
  assert.equal(timing.cancelled, true);
  assert.equal(timing.port, "pulse", "Existing preset connections remain compatible");
  assert.deepEqual(errors, []);
  console.log("PASS Counter: ports, threshold, edges, reset priority, blocks, raw output, independent instances, control/audio wiring, saturation, copy/paste, save/load, cleanup");
  console.log("PASS Clock: Trigger label, four triggers per beat, connected green blink, separate falling edges, stopped output, legacy port");
} finally {
  await browser.close();
}
