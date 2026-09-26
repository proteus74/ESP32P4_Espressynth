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
    send(value) { (window.__wsMessages ||= []).push(JSON.parse(value)); }
    close() {}
  };
});
await page.route("http://drum.test/**", async (route) => {
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
window.__drumTest = { createWidget, serializePreset, applyPresetData, openDrumSequencerSettings,
 drumSequencerStates, drumSequencerTriggerStep, drumSequencerStopTriggers, updateDrumSequencerVisual,
 getParams(id) { return getWidgetParams('drum-sequencer', id); },
 applyParams(id, params) { applyWidgetParams('drum-sequencer', id, params); },
 wireRate(id, source) { connections.push({ id: 'test-rate', from: { widgetId: source, portId: 'out' }, to: { widgetId: id, portId: 'rate' } }); updateDrumSequencerVisual(id); },
 clearConnections() { connections.length = 0; } };
`)]);
    return route.fulfill({ contentType: file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html", body });
  } catch {
    return route.fulfill({ status: 404 });
  }
});

try {
  await page.goto("http://drum.test/");
  await page.waitForFunction(() => window.__drumTest && !document.getElementById("audioStartBtn")?.disabled);
  const id = await page.evaluate(() => window.__drumTest.createWidget("drum-sequencer", "Drums", 40, 40).id);
  const pulses = () => page.evaluate((id) => window.__drumTest.getParams(id).trackPulseMs, id);
  assert.deepEqual(await pulses(), new Array(8).fill(50));
  await page.evaluate((id) => window.__drumTest.openDrumSequencerSettings(id), id);
  await page.locator("#drumSequencerStepCountInput").selectOption("4");
  await page.locator("#drumSequencerTrackCountInput").fill("3");
  await page.locator("#drumSequencerSettingsSave").click();
  const buttons = page.locator(`#${id} [data-drum-pulse-track]`);
  assert.deepEqual(await buttons.allTextContents(), ["50 ms", "50 ms", "50 ms"]);
  assert.equal(await page.locator("#drumSequencerPulseMsInput").count(), 0);
  await buttons.nth(0).click();
  assert.equal(await page.locator("#numericEditUnit").textContent(), "ms");
  assert.equal(await page.locator("#numericEditInput").inputValue(), "50");
  await page.locator("#numericEditInput").fill("125");
  await page.locator("#numericEditInput").press("Enter");
  assert.deepEqual((await pulses()).slice(0, 3), [125, 50, 50]);
  await buttons.nth(1).click();
  await page.locator("#numericEditInput").fill("200");
  await page.locator("#numericEditInput").press("Escape");
  assert.equal((await pulses())[1], 50);
  await buttons.nth(1).click();
  await page.locator("#numericEditInput").fill("0");
  await page.locator("#numericEditSave").click();
  assert.equal((await pulses())[1], 0.1);
  await buttons.nth(1).click();
  await page.locator("#numericEditInput").fill("70000");
  await page.locator("#numericEditSave").click();
  assert.equal((await pulses())[1], 60000);
  await buttons.nth(1).click();
  await page.locator("#numericEditInput").fill("");
  await page.locator("#numericEditSave").click();
  assert.equal((await pulses())[1], 60000);
  await buttons.nth(1).click();
  await page.locator("#numericEditInput").fill("25");
  await page.locator("#numericEditSave").click();

  const result = await page.evaluate(async (id) => {
    const t = window.__drumTest;
    const state = t.drumSequencerStates.get(id);
    state.gateVolts = 10;
    for (let track = 0; track < 3; track += 1) {
      state.patterns[0].active[track * 4] = true;
      state.patterns[0].active[track * 4 + 1] = true;
    }
    const realTimeout = window.setTimeout;
    const realClearTimeout = window.clearTimeout;
    window.setTimeout = (callback, ms) => ({ callback, ms });
    window.clearTimeout = () => {};
    let durations, retriggerGap, retriggerDurations;
    try {
      t.drumSequencerTriggerStep(id);
      durations = state.triggerTimers.slice(0, 3).map((timer) => timer.ms);
      t.drumSequencerTriggerStep(id);
      const pending = state.triggerTimers.slice(0, 3);
      retriggerGap = pending.map((timer) => timer.ms);
      pending.forEach((timer) => timer.callback());
      retriggerDurations = state.triggerTimers.slice(0, 3).map((timer) => timer.ms);
      t.drumSequencerStopTriggers(id);
    } finally {
      window.setTimeout = realTimeout;
      window.clearTimeout = realClearTimeout;
      state.gateVolts = 0;
    }
    const pulseRect = document.getElementById(id).querySelector('[data-drum-pulse-track="0"]').getBoundingClientRect();
    const cellRect = document.getElementById(id).querySelector('[data-track="0"][data-step="3"]').getBoundingClientRect();
    const source = t.createWidget("gain", "Gain", 840, 40).id;
    const style = (slider) => { const css = getComputedStyle(slider); return { width: css.width, height: css.height, appearance: css.appearance }; };
    const referenceStyle = style(document.getElementById(source).querySelector(".port-slider"));
    const sliders = Array.from(document.getElementById(id).querySelectorAll('input[type="range"]'));
    const styles = sliders.map(style);
    const classes = sliders.map((slider) => slider.classList.contains("port-slider"));
    const preset = t.serializePreset();
    await t.applyPresetData(preset);
    const restored = t.getParams(id).trackPulseMs;
    t.applyParams(id, { trackCount: 3, pulseMs: 12.5 });
    const legacy = t.getParams(id).trackPulseMs;
    t.applyParams(id, { trackCount: 3, pulseMs: 12.5, trackPulseMs: [33, null, -5, 100000] });
    const partial = t.getParams(id).trackPulseMs;
    t.applyParams(id, { stepCount: 4, trackCount: 3, trackPulseMs: restored });
    const live = window.__wsMessages.filter((message) => message.widgetId === id && message.params?.trackPulseMs);
    return { durations, retriggerGap, retriggerDurations, pulseRightOfTrack: pulseRect.x >= cellRect.right && pulseRect.x - cellRect.right < 24,
      pulseAlignedWithTrack: Math.abs((pulseRect.y + pulseRect.height / 2) - (cellRect.y + cellRect.height / 2)) < 2,
      styles, referenceStyle, classes, restored, legacy, partial, live };
  }, id);
  assert.deepEqual(result.durations, [125, 25, 50]);
  assert.deepEqual(result.retriggerGap, [1, 1, 1]);
  assert.deepEqual(result.retriggerDurations, [125, 25, 50]);
  assert.equal(result.pulseRightOfTrack, true);
  assert.equal(result.pulseAlignedWithTrack, true);
  assert.deepEqual(result.classes, [true, true]);
  for (const style of result.styles) assert.deepEqual(style, result.referenceStyle);
  assert.deepEqual(result.restored, [125, 25, 50, 50, 50, 50, 50, 50]);
  assert.deepEqual(result.legacy, new Array(8).fill(12.5));
  assert.deepEqual(result.partial, [33, 12.5, 0.1, 60000, 12.5, 12.5, 12.5, 12.5]);
  assert.deepEqual(result.live.at(-1).params.trackPulseMs.slice(0, 3), [125, 25, 50]);

  const rate = page.locator(`#${id} [data-role="drum-sequencer-rate"]`);
  await page.evaluate((id) => { document.getElementById(id).style.zIndex = "100"; }, id);
  await rate.dblclick();
  await page.locator("#numericEditInput").fill("2.5");
  await page.locator("#numericEditSave").click();
  assert.equal(await rate.inputValue(), "2.5");
  await page.evaluate((id) => {
    const t = window.__drumTest;
    t.wireRate(id, t.createWidget("const", "Rate CV", 850, 300).id);
  }, id);
  assert.equal(await rate.isDisabled(), true);
  await rate.dblclick({ force: true });
  assert.equal(await page.locator("#numericEditOverlay").isVisible(), false);
  await page.evaluate((id) => { window.__drumTest.clearConnections(); window.__drumTest.updateDrumSequencerVisual(id); }, id);
  if (process.env.SCREENSHOT_PATH) await page.locator(`#${id}`).screenshot({ path: process.env.SCREENSHOT_PATH });
  assert.deepEqual(errors, []);
  console.log("PASS drum sequencer: per-track timing/retrigger, 50ms defaults, click popup, cancel/bounds, live params, preset migration/round-trip, slider style/editing/CV override");
} finally {
  await browser.close();
}
