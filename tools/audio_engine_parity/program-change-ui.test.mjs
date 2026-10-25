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
    send(value) { (window.__wsMessages ||= []).push(JSON.parse(value)); }
    close() {}
  };
});
await page.route("http://program-change.test/**", async route => {
  const url = new URL(route.request().url());
  if (url.pathname.startsWith("/api/")) return route.fulfill({ contentType: "application/json", body: JSON.stringify(
    url.pathname === "/api/preset" ? { schemaVersion: 4, widgets: [], connections: [] } :
    url.pathname === "/api/presets" ? { presets: [] } : []) });
  try {
    const file = path.join(root, "data", url.pathname === "/" ? "index.html" : url.pathname);
    let body = readFileSync(file);
    if (url.pathname === "/static/js/main.js") body = Buffer.concat([body, Buffer.from(`
window.__programTest = { createWidget, serializePreset, applyPresetData, removeWidget,
 routeMidiToWidgets, routeMidiToButtons, routeMidiToUpDownButtons, openButtonSettings,
 startButtonMidiLearn, openUpDownButtonSettings, closeButtonSettings, closeUpDownButtonSettings,
 buttonStates, upDownButtonStates, midiInStates, WIDGET_CATEGORIES, DUAL_PAGE_TYPES,
 getParams(id) { return getWidgetParams(document.getElementById(id).dataset.widgetType, id); },
 applyParams(id, params) { applyWidgetParams(document.getElementById(id).dataset.widgetType, id, params); },
 connect(fromId, fromPort, toId, toPort) { connections.push({ id: 'test-' + connections.length,
 from: { widgetId: fromId, portId: fromPort }, to: { widgetId: toId, portId: toPort } }); refreshControlGraph(); } };
`)]);
    return route.fulfill({ contentType: file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html", body });
  } catch { return route.fulfill({ status: 404 }); }
});
try {
  await page.goto("http://program-change.test/");
  await page.waitForFunction(() => window.__programTest && !document.getElementById("audioStartBtn")?.disabled);
  const result = await page.evaluate(async () => {
    const t = window.__programTest;
    const program = t.createWidget("midi-in", "Midi-In", 10, 10).id;
    const debug = t.createWidget("debug", "Number", 400, 10).id;
    const button = t.createWidget("button", "Button", 10, 240).id;
    const upDown = t.createWidget("up-down-button", "Up/Down", 400, 240).id;
    t.connect(program, "program", debug, "in");
    const send = (number, channel = 2, midiType = "program_change") => {
      const event = { type: "midi", midiType, program: number, channel, control: number, value: 127 };
      t.routeMidiToWidgets(event); t.routeMidiToButtons(event); t.routeMidiToUpDownButtons(event);
    };
    t.applyParams(program, { channel: 2 });
    const values = [];
    for (const [number, channel, type] of [[127, 1], [127, 2, "control_change"], [0, 2], [73, 2], [127, 2], [128, 2]]) {
      send(number, channel, type);
      values.push(Number(document.getElementById(debug).querySelector('[data-role="debug-value"]').textContent));
    }
    t.openButtonSettings(button); t.startButtonMidiLearn(button, 0); send(5);
    const binding = t.getParams(button).bindings[0]; t.closeButtonSettings();
    const buttonValues = [];
    for (const [number, channel, type] of [[5, 2], [6, 2], [5, 1], [5, 2, "control_change"], [5, 2], [5, 2]]) {
      send(number, channel, type);
      buttonValues.push(t.buttonStates.get(button).layers[0].controls[0].wasAbove);
    }
    t.openUpDownButtonSettings(upDown);
    document.getElementById("upDownButtonLearnUpBtn").click(); send(10);
    document.getElementById("upDownButtonLearnDownBtn").click(); send(11);
    t.closeUpDownButtonSettings();
    const upDownValues = [];
    for (const [number, channel, type] of [[10, 2], [10, 2], [11, 1], [10, 2, "control_change"], [12, 2], [11, 2], [11, 2], [11, 2]]) {
      send(number, channel, type); upDownValues.push(t.upDownButtonStates.get(upDown).currentValue);
    }
    const preset = t.serializePreset(); await t.applyPresetData(preset);
    const restored = [t.getParams(program), t.getParams(button).bindings[0], t.getParams(upDown)];
    const ports = [...document.getElementById(program).querySelectorAll('.port-dot[data-port-dir="out"]')].map(dot => dot.dataset.portId);
    t.applyParams(program, { channel: 15 });
    send(42, 2); send(42, 15);
    const channelEdited = (({ channel, program }) => ({ channel, program }))(t.getParams(program));
    t.applyParams(program, { channel: null });
    send(9, 4);
    const omniProgram = t.getParams(program).program;
    t.applyParams(upDown, { midiTypeUp: "cc", ccUp: 10, channelUp: 2 });
    send(10, 2, "control_change"); const legacyCc = t.upDownButtonStates.get(upDown).currentValue;
    t.removeWidget(program);
    return { values, binding, buttonValues, upDownValues, restored, ports, channelEdited, omniProgram, legacyCc,
      removed: !t.midiInStates.has(program), messages: window.__wsMessages,
      unlisted: !t.WIDGET_CATEGORIES.some(c => c.types.includes("midi-program")) && !t.DUAL_PAGE_TYPES.includes("midi-program") };
  });
  assert.deepEqual(result.values, [0, 0, 0, 73, 127, 127]);
  assert.deepEqual(result.binding, { cc: 5, channel: 2, midiType: "program", switchMode: false });
  assert.deepEqual(result.buttonValues, [true, true, true, true, false, true]);
  assert.deepEqual(result.upDownValues, [1, 2, 2, 2, 2, 1, 0, 0]);
  assert.equal(result.restored[0].channel, 2);
  assert.equal(result.restored[0].program, 127);
  assert.deepEqual(result.restored[1], result.binding);
  assert.equal(result.restored[2].midiTypeUp, "program");
  assert.equal(result.restored[2].midiTypeDown, "program");
  assert.deepEqual(result.ports, ["pitchCv", "gate", "velocity", "pitchBend", "aftertouch", "midi", "note", "program"]);
  assert.deepEqual(result.channelEdited, { channel: 15, program: 42 });
  assert.equal(result.omniProgram, 9);
  assert.equal(result.legacyCc, 1);
  assert.ok(result.removed && result.unlisted);
  assert.ok(result.messages.some(m => m.params?.midiTypeUp === "program"));
  assert.deepEqual(errors, []);
  console.log("PASS Program Change: Midi-In program output (channel/omni matching, raw 0-127, port order), button/up-down learn, repeated events, CC compatibility, preset round-trip, old widget gone");
} finally { await browser.close(); }
