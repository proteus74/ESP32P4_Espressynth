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
await page.route("http://pitch-bend.test/**", async route => {
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
window.__pitchTest = { createWidget, serializePreset, applyPresetData, openMidiInSettings, routeMidiToWidgets,
 midiInStates, oscillatorStates, fmOscillatorStates, hammondStates,
 connect(fromId, fromPort, toId, toPort) { connections.push({ id: 'bend-' + connections.length,
 from: { widgetId: fromId, portId: fromPort }, to: { widgetId: toId, portId: toPort } }); } };
`)]);
    return route.fulfill({ contentType: file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html", body });
  } catch {
    return route.fulfill({ status: 404 });
  }
});
try {
  await page.goto("http://pitch-bend.test/");
  await page.waitForFunction(() => window.__pitchTest && !document.getElementById("audioStartBtn")?.disabled);
  const result = await page.evaluate(() => {
    const t = window.__pitchTest;
    const source = t.createWidget("midi-in", "MIDI In", 20, 20).id;
    const debug = t.createWidget("debug", "Bend", 400, 20).id;
    t.connect(source, "pitchBend", debug, "in");
    const targets = ["oscillator", "fm-oscillator", "hammond"].map((type, index) => {
      const id = t.createWidget(type, type, 400, 200 + 200 * index).id;
      t.connect(source, "pitchBend", id, "tune");
      return id;
    });
    const states = [t.oscillatorStates, t.fmOscillatorStates, t.hammondStates];
    const send = value => {
      t.routeMidiToWidgets({ type: "midi", midiType: "pitchwheel", channel: 0, value });
      return { cv: Number(document.getElementById(debug).querySelector('[data-role="debug-value"]').textContent),
        tunes: targets.map((id, index) => states[index].get(id).tuneVolts) };
    };
    t.openMidiInSettings(source);
    const originalVolts = Number(document.getElementById("midiInPitchBendRangeInput").value);
    document.getElementById("midiInPitchBendRangeInput").value = "3";
    document.getElementById("midiInSettingsSave").click();
    const cents = t.midiInStates.get(source).pitchBendRangeCents;
    const endpoints = [-8192, 0, 8191].map(send);
    t.applyPresetData(t.serializePreset());
    t.openMidiInSettings(source);
    const restoredVolts = Number(document.getElementById("midiInPitchBendRangeInput").value);
    document.getElementById("midiInPitchBendRangeInput").value = "0";
    document.getElementById("midiInSettingsSave").click();
    t.applyPresetData(t.serializePreset());
    t.openMidiInSettings(source);
    const zeroVolts = Number(document.getElementById("midiInPitchBendRangeInput").value);
    return { originalVolts, cents, endpoints, restoredVolts, zeroVolts };
  });
  assert.ok(Math.abs(result.originalVolts - 200 / 1200) < 1e-6);
  assert.equal(result.cents, 3600);
  assert.deepEqual(result.endpoints, [-3, 0, 3].map(cv => ({ cv, tunes: [5 + cv, 5 + cv, 5 + cv] })));
  assert.equal(result.restoredVolts, 3);
  assert.equal(result.zeroVolts, 0);
  assert.deepEqual(errors, []);
  console.log("PASS full browser: MIDI-In settings in volts, bipolar output, all three Tune inputs and 3 V/0 V preset round-trips");
} finally {
  await browser.close();
}
