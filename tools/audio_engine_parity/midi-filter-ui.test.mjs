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
await page.route("http://midi-filter.test/**", async (route) => {
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
window.__midiTest = { createWidget, serializePreset, applyPresetData,
 getWidgetParams(id) { return getWidgetParams(document.getElementById(id).dataset.widgetType, id); },
 applyWidgetParams(id, params) { applyWidgetParams(document.getElementById(id).dataset.widgetType, id, params); },
 routeMidiToWidgets, refreshVoiceGroups, midiFilterStates, midiVoiceSlotStates,
 connect(fromId, fromPort, toId, toPort) { connections.push({ id: 'test-' + connections.length,
 from: { widgetId: fromId, portId: fromPort }, to: { widgetId: toId, portId: toPort } }); },
 clearConnections() { connections.length = 0; } };
`) ]);
    return route.fulfill({ contentType: file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html", body });
  } catch {
    return route.fulfill({ status: 404 });
  }
});

try {
  await page.goto("http://midi-filter.test/");
  await page.waitForFunction(() => window.__midiTest && !document.getElementById("audioStartBtn")?.disabled);
  const result = await page.evaluate(async () => {
    const t = window.__midiTest;
    const source = t.createWidget("midi-in", "MIDI In", 20, 30).id;
    const filter = t.createWidget("midi-filter", "MIDI-Filter", 400, 30).id;
    const debug = {};
    for (const [i, port] of ["pitchCv", "note", "velocity", "gate"].entries()) {
      debug[port] = t.createWidget("debug", port, 800, 30 + i * 140).id;
      t.connect(filter, port, debug[port], "in");
    }
    const piano = t.createWidget("mda-piano", "Piano", 1100, 30).id;
    const raw = [];
    window.audioEngine.handleMdaPianoMidi = (id, data) => raw.push({ id, ...data });
    t.connect(source, "midi", filter, "midi");
    t.connect(filter, "midi", piano, "midi");
    t.applyWidgetParams(filter, { channel: 2, note: 69 });
    const send = (midiType, note = 69, channel = 2, extra = {}) => t.routeMidiToWidgets({ type: "midi", midiType, note, channel, velocity: 127, ...extra });
    const values = () => Object.fromEntries(Object.entries(debug).map(([port, id]) => [port, Number(document.getElementById(id).querySelector('[data-role="debug-value"]').textContent)]));
    const ports = Array.from(document.getElementById(filter).querySelectorAll(".port-dot")).map((dot) => ({ port: dot.dataset.portId, dir: dot.dataset.portDir, x: dot.getBoundingClientRect().x }));
    send("note_on", 68);
    send("note_on", 69, 1);
    const blocked = raw.length;
    send("note_on");
    const accepted = values();
    send("control_change", 0, 2, { control: 7, value: 88 });
    const cc = raw.at(-1);
    t.applyWidgetParams(filter, { channel: 4, note: 70 });
    send("note_off");
    const released = values().gate;
    t.applyWidgetParams(filter, { channel: 2, note: 69 });
    send("control_change", 0, 2, { control: 64, value: 127 });
    send("note_on");
    send("note_off");
    const sustained = values().gate;
    send("control_change", 0, 2, { control: 64, value: 0 });
    const pedalReleased = values().gate;
    t.applyWidgetParams(filter, { channel: 2, note: -1 });
    send("note_on", 60);
    send("note_on", 70);
    send("note_off", 70);
    const fallback = values();
    send("note_on", 60, 2, { velocity: 0 });
    const zeroRelease = values().gate;
    send("note_on", 60);
    send("control_change", 0, 2, { control: 123, value: 0 });
    const panicGate = values().gate;

    const second = t.createWidget("midi-filter", "Second", 400, 420).id;
    t.applyWidgetParams(second, { channel: 2, note: 69 });
    t.connect(filter, "midi", second, "midi");
    t.connect(second, "midi", filter, "midi");
    t.connect(second, "midi", piano, "midi");
    const beforeChain = raw.length;
    send("note_on", 69);
    const chainCount = raw.length - beforeChain;
    send("note_off", 69);

    t.clearConnections();
    const split = t.createWidget("midi-split", "Split", 20, 420).id;
    t.applyWidgetParams(split, { zoneCount: 1, zones: [{ from: 60, to: 60, octave: 1 }] });
    t.applyWidgetParams(filter, { channel: -1, note: 72 });
    t.connect(source, "midi", split, "midi");
    t.connect(split, "zone1Midi", filter, "midi");
    t.connect(filter, "midi", piano, "midi");
    send("note_on", 60);
    const splitNote = raw.at(-1).note;
    send("note_off", 60);

    t.clearConnections();
    t.connect(source, "midi", filter, "midi");
    t.applyWidgetParams(source, { voices: 4 });
    t.applyWidgetParams(filter, { channel: 2, note: -1 });
    t.refreshVoiceGroups();
    send("note_on", 60);
    send("note_on", 61, 1);
    send("note_on", 64);
    const polyNotes = t.midiVoiceSlotStates.get(filter).slots.map((slot) => slot.note);
    const preset = t.serializePreset();
    await t.applyPresetData(preset);
    const restored = t.getWidgetParams(filter);
    const savedSelectors = ["channel", "note"].map((key) => document.getElementById(filter).querySelector(`[data-role="midi-filter-${key}"]`).value);
    return { filter, ports, blocked, accepted, cc, released, sustained, pedalReleased, fallback, zeroRelease, panicGate, chainCount, splitNote, polyNotes, restored, savedSelectors };
  });
  assert.equal(result.blocked, 0);
  assert.deepEqual(result.accepted, { pitchCv: 5.75, note: 69, velocity: 10, gate: 10 });
  assert.equal(result.cc.control, 7);
  assert.equal(result.cc.value, 88);
  assert.equal(result.released, 0);
  assert.equal(result.sustained, 10);
  assert.equal(result.pedalReleased, 0);
  assert.equal(result.fallback.note, 60);
  assert.equal(result.fallback.gate, 10);
  assert.equal(result.zeroRelease, 0);
  assert.equal(result.panicGate, 0);
  assert.equal(result.chainCount, 2);
  assert.equal(result.splitNote, 72);
  assert.deepEqual(result.polyNotes, [60, 64, null, null]);
  assert.deepEqual(result.restored, { channel: 2, note: -1 });
  assert.deepEqual(result.savedSelectors, ["2", "-1"]);
  assert.deepEqual(result.ports.filter((port) => port.dir === "out").map((port) => port.port), ["midi", "pitchCv", "note", "velocity", "gate"]);
  assert.ok(result.ports.find((port) => port.dir === "in").x < result.ports.find((port) => port.dir === "out").x);
  await page.locator(`#${result.filter} [data-role="midi-filter-channel"]`).selectOption("0");
  await page.locator(`#${result.filter} [data-role="midi-filter-note"]`).selectOption("60");
  const edited = await page.evaluate((id) => ({ params: window.__midiTest.getWidgetParams(id), messages: window.__wsMessages }), result.filter);
  assert.deepEqual(edited.params, { channel: 0, note: 60 });
  assert.ok(edited.messages.some((message) => message.widgetId === result.filter && message.params?.note === 60));
  if (process.env.SCREENSHOT_PATH) await page.locator(`#${result.filter}`).screenshot({ path: process.env.SCREENSHOT_PATH });
  assert.deepEqual(errors, []);
  console.log("PASS MIDI-filter UI, MIDI/CV routing, criteria, release, sustain, mono fallback, chains/cycles, splitter, polyphony, preset round-trip");
} finally {
  await browser.close();
}
