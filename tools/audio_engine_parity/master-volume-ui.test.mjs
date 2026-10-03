import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || "playwright");
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined });
const page = await browser.newPage();
const errors = [];
page.on("pageerror", error => errors.push(error.message));
await page.addInitScript(() => {
  window.WebSocket = class extends EventTarget {
    static OPEN = 1;
    readyState = 1;
    send() {}
    close() {}
  };
});
await page.route("http://master-volume.test/**", async route => {
  const url = new URL(route.request().url());
  if (url.pathname.startsWith("/api/")) {
    const data = url.pathname === "/api/preset" ? { schemaVersion: 4, widgets: [], connections: [] }
      : url.pathname === "/api/presets" ? { presets: [] } : url.pathname === "/api/settings/device" ? { volume: 0.25, volumeCc: -1, volumeChannel: -1 } : [];
    return route.fulfill({ contentType: "application/json", body: JSON.stringify(data) });
  }
  try {
    const file = path.join(root, "data", url.pathname === "/" ? "index.html" : url.pathname);
    let body = readFileSync(file);
    if (url.pathname === "/static/js/main.js") body = Buffer.concat([body, Buffer.from(`
window.__volumeTest = { createWidget, serializePreset, applyPresetData, applyGlobalMidiDeviceSettings,
 routeMidiToGlobalMidiControls, setManualMasterVolume, refreshControlGraph,
 connect(fromId, toId) { connections.push({ id: 'volume-' + connections.length,
 from: { widgetId: fromId, portId: 'value' }, to: { widgetId: toId, portId: 'in' } }); } };
`)]);
    return route.fulfill({ contentType: file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html", body });
  } catch {
    return route.fulfill({ status: 404 });
  }
});
try {
  await page.goto("http://master-volume.test/");
  await page.waitForFunction(() => window.__volumeTest && !document.getElementById("audioStartBtn")?.disabled);
  const result = await page.evaluate(() => {
    const t = window.__volumeTest;
    const first = t.createWidget("master-volume", "Master Volume", 20, 20).id;
    const second = t.createWidget("master-volume", "Master Volume", 350, 20).id;
    const debug = t.createWidget("debug", "Voltage", 600, 20).id;
    t.connect(first, debug);
    t.refreshControlGraph();
    const read = () => ({ sliders: [first, second].map(id => {
      const el = document.getElementById(id).querySelector('.master-volume-body input');
      return { value: Number(el.value), disabled: el.disabled };
    }), cv: Number(document.getElementById(debug).querySelector('[data-role="debug-value"]').textContent),
    settingsDisabled: document.getElementById('masterVolumeInput').disabled });
    const initial = read();
    const slider = document.getElementById(first).querySelector('input');
    slider.value = '3'; slider.dispatchEvent(new Event('input'));
    const manual = read();
    t.applyGlobalMidiDeviceSettings({ volume: 0.3, volumeCc: 0, volumeChannel: 0 });
    t.setManualMasterVolume(1);
    const locked = read();
    t.routeMidiToGlobalMidiControls({ type: 'midi', midiType: 'control_change', channel: 1, control: 0, value: 127 });
    const wrongChannel = read();
    const midi = [0, 127, 64].map(value => {
      t.routeMidiToGlobalMidiControls({ type: 'midi', midiType: 'control_change', channel: 0, control: 0, value });
      return read();
    });
    const preset = t.serializePreset();
    const saved = Object.values(preset.pages).flatMap(p => p.widgets).filter(w => w.type === 'master-volume').map(w => w.params);
    t.applyPresetData(preset);
    const restored = Array.from(document.querySelectorAll('[data-widget-type="master-volume"] input')).map(el => Number(el.value));
    t.applyGlobalMidiDeviceSettings({ volume: 0.5, volumeCc: -1, volumeChannel: -1 });
    t.setManualMasterVolume(0.7);
    const cleared = Array.from(document.querySelectorAll('[data-widget-type="master-volume"] input')).map(el => ({ value: Number(el.value), disabled: el.disabled }));
    return { initial, manual, locked, wrongChannel, midi, saved, restored, cleared };
  });
  const state = (v, disabled) => ({ sliders: [{ value: v, disabled }, { value: v, disabled }], cv: v, settingsDisabled: disabled });
  assert.deepEqual(result.initial, state(2.5, false));
  assert.deepEqual(result.manual, state(3, false));
  assert.deepEqual(result.locked, state(3, true));
  assert.deepEqual(result.wrongChannel, result.locked);
  assert.deepEqual(result.midi.slice(0, 2), [state(0, true), state(10, true)]);
  assert.ok(Math.abs(result.midi[2].sliders[0].value - 640 / 127) < 0.01);
  assert.deepEqual(result.saved, [{}, {}]);
  assert.ok(result.restored.every(v => Math.abs(v - 640 / 127) < 0.01));
  assert.deepEqual(result.cleared, [{ value: 7, disabled: false }, { value: 7, disabled: false }]);
  assert.deepEqual(errors, []);
  console.log('PASS Master Volume: startup value, shared sliders, 0–10 V routing, learned CC0/channel0 priority, channel filtering, clear and preset round-trip');
} finally {
  await browser.close();
}
