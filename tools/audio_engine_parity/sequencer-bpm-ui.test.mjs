import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || "playwright");
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined });
const page = await browser.newPage({ viewport: { width: 1600, height: 1200 } });
const errors = [];
page.on("pageerror", (error) => errors.push(error.message));
await page.addInitScript(() => {
  window.WebSocket = class extends EventTarget { static OPEN = 1; readyState = 1; send(value) { (window.__wsMessages ||= []).push(JSON.parse(value)); } close() {} };
});
await page.route("http://bpm.test/**", async (route) => {
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
window.__bpmTest = { createWidget, serializePreset, applyPresetData, deliverToWidget, setMasterClockBpm,
 stepArpStates, drumSequencerStates, clockStates, restartAllClockTimers, refreshControlGraph, drawConnections,
 stepArpRateBpm, drumSequencerBpm, stepArpRestartInternalClock, drumSequencerRestartInternalClock,
 openStepArpSettings, setStepArpValue, stepArpTriggerStep, audioEngine,
 select(id) { selectOnlyWidget(id); },
 wire(source, fromPort, target, toPort) {
   connections.push({ id: 'test-' + connections.length, from: { widgetId: source, portId: fromPort }, to: { widgetId: target, portId: toPort } });
   refreshControlGraph(); drawConnections();
 },
 unwire(target, port) { connections = connections.filter((c) => c.to.widgetId !== target || c.to.portId !== port); refreshControlGraph(); drawConnections(); }
};
`)]);
    return route.fulfill({ contentType: file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html", body });
  } catch { return route.fulfill({ status: 404 }); }
});

try {
  await page.goto("http://bpm.test/");
  await page.waitForFunction(() => window.__bpmTest && !document.getElementById("audioStartBtn")?.disabled);
  const ids = await page.evaluate(() => {
    const t = window.__bpmTest;
    return { clock: t.createWidget('clock', 'Clock', 40, 40).id,
      arp: t.createWidget('step-arpeggiator', 'Arp', 40, 300).id,
      drum: t.createWidget('drum-sequencer', 'Drums', 800, 40).id,
      value: t.createWidget('const', 'BPM Value', 800, 700).id };
  });
  const labels = (id, selector) => page.locator(`#${id} ${selector}`).allTextContents();
  assert.deepEqual(await labels(ids.clock, '.port-row-out .port-label'), ['Trigger', 'BPM']);
  assert.deepEqual(await labels(ids.arp, '.step-arp-io-line-in .port-label'), ['Trigger', 'BPM', 'Gate', 'Pitch', 'Direction', 'Gate-Out-Time']);
  assert.deepEqual(await labels(ids.drum, '.drum-sequencer-port-in .port-label'), ['Trigger', 'BPM', 'Gate', 'Pattern', 'Switch at End']);
  assert.equal(await page.locator(`#${ids.arp} [data-port-id="rate"] input`).count(), 0);
  assert.equal(await page.locator(`#${ids.drum} [data-port-id="rate"] input`).count(), 0);
  assert.equal(await page.locator('#stepArpClockMaxBpmInput, #drumSequencerClockMaxBpmInput').count(), 0);
  const tempos = await page.evaluate((ids) => {
    const t = window.__bpmTest;
    const arp = t.stepArpStates.get(ids.arp), drum = t.drumSequencerStates.get(ids.drum);
    arp.clockRoundBpm = drum.clockRoundBpm = true; // Ignore obsolete per-widget tempo mapping/rounding.
    const values = () => [t.stepArpRateBpm(ids.arp), t.drumSequencerBpm(drum, ids.drum)];
    t.setMasterClockBpm(120);
    const fallback = values();
    t.wire(ids.clock, 'frequency', ids.arp, 'rate');
    t.wire(ids.clock, 'frequency', ids.drum, 'rate');
    const connected = values();
    t.setMasterClockBpm(73.5);
    const changed = values();
    t.unwire(ids.arp, 'rate');
    t.unwire(ids.drum, 'rate');
    t.wire(ids.value, 'value', ids.arp, 'rate');
    t.wire(ids.value, 'value', ids.drum, 'rate');
    t.deliverToWidget(ids.arp, 'rate', 300);
    t.deliverToWidget(ids.drum, 'rate', 300);
    const external = values();
    t.setMasterClockBpm(60);
    const independent = values();
    const realInterval = window.setInterval, realClear = window.clearInterval;
    window.setInterval = (callback, ms) => ({ callback, ms });
    window.clearInterval = () => {};
    let periods, stopped, triggerPriority;
    try {
      t.stepArpRestartInternalClock(ids.arp);
      t.drumSequencerRestartInternalClock(ids.drum);
      periods = [arp.clockTickTimer.ms, drum.clockTickTimer.ms];
      t.deliverToWidget(ids.arp, 'rate', 0);
      t.deliverToWidget(ids.drum, 'rate', 0);
      stopped = [arp.clockTickTimer, drum.clockTickTimer];
      t.deliverToWidget(ids.arp, 'rate', 120);
      t.deliverToWidget(ids.drum, 'rate', 120);
      t.wire(ids.clock, 'pulse', ids.arp, 'clock');
      t.wire(ids.clock, 'pulse', ids.drum, 'clock');
      triggerPriority = [arp.clockTickTimer, drum.clockTickTimer];
      t.unwire(ids.arp, 'clock');
      t.unwire(ids.drum, 'clock');
      t.unwire(ids.arp, 'rate');
      t.unwire(ids.drum, 'rate');
      t.wire(ids.clock, 'frequency', ids.arp, 'rate');
      t.wire(ids.clock, 'frequency', ids.drum, 'rate');
    } finally { window.setInterval = realInterval; window.clearInterval = realClear; }
    t.restartAllClockTimers();
    return { fallback, connected, changed, external, independent, periods, stopped, triggerPriority, restored: values() };
  }, ids);
  assert.deepEqual(tempos, { fallback: [120, 120], connected: [120, 120], changed: [73.5, 73.5], external: [300, 300], independent: [300, 300],
    periods: [50, 50], stopped: [null, null], triggerPriority: [null, null], restored: [60, 60] });
  assert.equal(await page.locator(`#${ids.arp} [data-role="step-arp-rate-value"]`).textContent(), '60.00 BPM');
  assert.equal(await page.locator(`#${ids.drum} [data-role="drum-sequencer-rate-value"]`).textContent(), '60.00 BPM');
  const saved = await page.evaluate(() => window.__bpmTest.serializePreset());
  await page.evaluate((saved) => window.__bpmTest.applyPresetData(saved), saved);
  assert.deepEqual(await page.evaluate((ids) => {
    const t = window.__bpmTest;
    return [t.stepArpRateBpm(ids.arp), t.drumSequencerBpm(t.drumSequencerStates.get(ids.drum), ids.drum)];
  }, ids), [60, 60]);
  assert.equal(saved.pages.generator.connections.filter((c) => c.from.portId === 'frequency' && c.to.portId === 'rate').length, 2);
  // Settings, editor playback and appearance must match the native rest behavior.
  assert.equal(await page.evaluate((id) => window.__bpmTest.stepArpStates.get(id).skipOnC0, ids.arp), false);
  await page.evaluate((id) => window.__bpmTest.openStepArpSettings(id), ids.arp);
  await page.locator('#stepArpSkipOnC0Input').check();
  await page.locator('#stepArpSettingsCancel').click();
  assert.equal(await page.evaluate((id) => window.__bpmTest.stepArpStates.get(id).skipOnC0, ids.arp), false);
  await page.evaluate((id) => window.__bpmTest.openStepArpSettings(id), ids.arp);
  await page.locator('#stepArpSkipOnC0Input').check();
  await page.locator('#stepArpSettingsSave').click();
  await page.waitForFunction((id) => window.__wsMessages?.some((message) => message.widgetId === id && message.params?.skipOnC0 === 1), ids.arp);
  const rests = await page.evaluate((id) => {
    const t = window.__bpmTest, state = t.stepArpStates.get(id), widget = document.getElementById(id);
    const target = t.createWidget('counter', 'Outputs', 800, 850).id;
    t.wire(id, 'cv', target, 'reset');
    t.wire(id, 'gateOut', target, 'count');
    const entry = () => { const node = t.audioEngine.nodes.get(target); return node.voiced ? node.instances[0] : node; };
    const output = () => ({ cv: state.heldCv, gate: entry().controlSources[0].offset.value, receivedCv: entry().controlSources[1].offset.value, next: state.currentStep });
    state.pulseMs = 1000;
    t.setStepArpValue(id, 0, 6);
    t.setStepArpValue(id, 1, 0);
    t.setStepArpValue(id, 2, 7);
    state.currentStep = 0;
    t.stepArpTriggerStep(id, state, widget);
    const first = output();
    t.stepArpTriggerStep(id, state, widget);
    const skipped = output();
    const dotOff = !widget.querySelector('.port-dot[data-port-id="gateOut"]').classList.contains('port-active');
    const noTimer = state.gateOutTimer === null;
    t.stepArpTriggerStep(id, state, widget);
    const resumed = output();
    for (let index = 0; index < 16; index++) t.setStepArpValue(id, index, 0);
    for (let step = 0; step < 16; step++) t.stepArpTriggerStep(id, state, widget);
    const allZero = output();
    return { first, skipped, resumed, allZero, noTimer, dotOff };
  }, ids.arp);
  assert.deepEqual(rests, {
    first: { cv: 6, gate: 10, receivedCv: 6, next: 1 },
    skipped: { cv: 6, gate: 0, receivedCv: 6, next: 2 },
    resumed: { cv: 7, gate: 10, receivedCv: 7, next: 3 },
    allZero: { cv: 7, gate: 0, receivedCv: 7, next: 3 }, noTimer: true, dotOff: true,
  });
  const knob = page.locator(`#${ids.arp} [data-role="step-arp-knob-1"]`);
  assert.equal(await knob.evaluate((knob) => Number(getComputedStyle(knob).opacity)), 0.45);
  assert.notEqual(await knob.evaluate((knob) => getComputedStyle(knob).pointerEvents), 'none');
  const box = await knob.boundingBox();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2 - 25, { steps: 5 });
  await page.mouse.up();
  assert.equal(await knob.evaluate((knob) => Number(getComputedStyle(knob).opacity)), 1);
  await page.evaluate((id) => { const t = window.__bpmTest; t.setStepArpValue(id, 0, 0); t.select(id); }, ids.arp);
  await page.keyboard.press('Control+c');
  await page.keyboard.press('Control+v');
  await page.mouse.click(1000, 1050);
  const skipPreset = await page.evaluate(() => window.__bpmTest.serializePreset());
  const arps = skipPreset.pages.generator.widgets.filter((widget) => widget.type === 'step-arpeggiator');
  assert.equal(arps.length, 2);
  for (const widget of arps) assert.equal(widget.params.skipOnC0, true);
  await page.evaluate((preset) => window.__bpmTest.applyPresetData(preset), skipPreset);
  assert.equal(await page.locator(`#${ids.arp} .step-arp-cell-skipped`).count(), 16);
  await page.evaluate((id) => window.__bpmTest.openStepArpSettings(id), ids.arp);
  assert.equal(await page.locator('#stepArpSkipOnC0Input').isChecked(), true);
  await page.locator('#stepArpSkipOnC0Input').uncheck();
  await page.locator('#stepArpSettingsSave').click();
  assert.equal(await page.locator(`#${ids.arp} .step-arp-cell-skipped`).count(), 0);
  for (const widget of arps) delete widget.params.skipOnC0;
  await page.evaluate((preset) => window.__bpmTest.applyPresetData(preset), skipPreset);
  assert.equal(await page.evaluate((id) => window.__bpmTest.stepArpStates.get(id).skipOnC0, ids.arp), false);
  // Test semitone snapping with real mouse drags in absolute and Pitch-offset modes.
  assert.equal(await page.evaluate((id) => window.__bpmTest.stepArpStates.get(id).snapToSemitones, ids.arp), false);
  await page.evaluate((id) => window.__bpmTest.openStepArpSettings(id), ids.arp);
  await page.locator('#stepArpSnapToSemitonesInput').check();
  await page.locator('#stepArpSettingsCancel').click();
  assert.equal(await page.evaluate((id) => window.__bpmTest.stepArpStates.get(id).snapToSemitones, ids.arp), false);
  await page.evaluate((id) => window.__bpmTest.openStepArpSettings(id), ids.arp);
  await page.locator('#stepArpSnapToSemitonesInput').check();
  await page.locator('#stepArpSettingsSave').click();
  await page.waitForFunction((id) => window.__wsMessages?.some((message) => message.widgetId === id && message.params?.snapToSemitones === 1), ids.arp);
  const dragStep = async (dy) => {
    const rect = await page.locator(`#${ids.arp} [data-role="step-arp-knob-1"]`).boundingBox();
    await page.mouse.move(rect.x + rect.width / 2, rect.y + rect.height / 2);
    await page.mouse.down();
    await page.mouse.move(rect.x + rect.width / 2, rect.y + rect.height / 2 - dy, { steps: 4 });
    await page.mouse.up();
    return page.evaluate((id) => window.__bpmTest.stepArpStates.get(id).steps[0], ids.arp);
  };
  await page.evaluate((id) => window.__bpmTest.setStepArpValue(id, 0, 5), ids.arp);
  assert.equal(await dragStep(7), 5.25);
  await page.waitForFunction((id) => window.__wsMessages?.some((message) => message.widgetId === id && message.params?.step1 === 5.25), ids.arp);
  await page.evaluate((id) => window.__bpmTest.setStepArpValue(id, 0, 0), ids.arp);
  assert.equal(await dragStep(-7), 0);
  await page.evaluate((id) => window.__bpmTest.setStepArpValue(id, 0, 10), ids.arp);
  assert.equal(await dragStep(7), 10);
  await page.evaluate((id) => window.__bpmTest.openStepArpSettings(id), ids.arp);
  await page.locator('#stepArpSnapToSemitonesInput').uncheck();
  await page.locator('#stepArpSettingsSave').click();
  await page.evaluate((id) => window.__bpmTest.setStepArpValue(id, 0, 5), ids.arp);
  assert.ok(Math.abs(await dragStep(7) - (5 + 70 / 254)) < 1e-8);
  await page.evaluate((id) => window.__bpmTest.openStepArpSettings(id), ids.arp);
  await page.locator('#stepArpSnapToSemitonesInput').check();
  await page.locator('#stepArpMaxSemitoneOffsetInput').fill('12');
  await page.locator('#stepArpSettingsSave').click();
  await page.evaluate((ids) => {
    const t = window.__bpmTest;
    t.wire(ids.value, 'value', ids.arp, 'pitch');
    t.deliverToWidget(ids.arp, 'pitch', 6);
    t.setStepArpValue(ids.arp, 0, 5);
  }, ids);
  assert.ok(Math.abs(await dragStep(7) - (5 + 5 / 12)) < 1e-8);
  const snappedCv = await page.evaluate((id) => {
    const t = window.__bpmTest, state = t.stepArpStates.get(id);
    state.currentStep = 0;
    t.stepArpTriggerStep(id, state, document.getElementById(id));
    return state.heldCv;
  }, ids.arp);
  assert.ok(Math.abs(snappedCv - (6 + 1 / 12)) < 1e-8);
  await page.evaluate((id) => window.__bpmTest.deliverToWidget(id, 'step1', 5.123), ids.arp);
  assert.equal(await page.evaluate((id) => window.__bpmTest.stepArpStates.get(id).steps[0], ids.arp), 5, 'MIDI/external step CV uses the same semitone grid');
  await page.evaluate((id) => window.__bpmTest.select(id), ids.arp);
  await page.keyboard.press('Control+c');
  await page.keyboard.press('Control+v');
  await page.mouse.click(1000, 1000);
  const snapPreset = await page.evaluate(() => window.__bpmTest.serializePreset());
  assert.equal(snapPreset.pages.generator.widgets.filter((widget) => widget.type === 'step-arpeggiator' && widget.params.snapToSemitones).length, 2);
  await page.evaluate((preset) => window.__bpmTest.applyPresetData(preset), snapPreset);
  await page.evaluate((id) => window.__bpmTest.openStepArpSettings(id), ids.arp);
  assert.equal(await page.locator('#stepArpSnapToSemitonesInput').isChecked(), true);
  await page.locator('#stepArpSettingsCancel').click();
  for (const widget of snapPreset.pages.generator.widgets) if (widget.type === 'step-arpeggiator') delete widget.params.snapToSemitones;
  await page.evaluate((preset) => window.__bpmTest.applyPresetData(preset), snapPreset);
  assert.equal(await page.evaluate((id) => window.__bpmTest.stepArpStates.get(id).snapToSemitones, ids.arp), false);
  assert.deepEqual(errors, []);
  console.log('PASS BPM UI: labels/order, removed sliders, direct Clock tempo, live changes, Master fallback, arbitrary external BPM, zero stop, trigger priority, legacy IDs, preset reload');
  console.log('PASS Skip on C0: checkbox/save/cancel/live update, CV hold, silent gate, overlap cancellation, all-zero pattern, dimmed editable knobs, copy/paste, preset reload, default off');
  console.log('PASS Snap to Semitones: settings/save/cancel, absolute 1/12 V steps, bounds, free mode, Pitch-offset grid, live flag/value, MIDI/CV quantization, copy/paste, preset reload, default off');
} finally { await browser.close(); }
