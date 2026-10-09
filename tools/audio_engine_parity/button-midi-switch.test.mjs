import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const source = readFileSync(path.join(root, "data/static/js/main.js"), "utf8");
const section = (start, end, offset = 0) => {
  const from = source.indexOf(start, offset);
  const to = source.indexOf(end, from);
  assert.ok(from >= 0 && to > from, `Missing source section: ${start}`);
  return source.slice(from, to);
};

const control = { cc: null, channel: null, switchMode: false, value0: 0, value127: 10, listText: "", listIndex: 0, wasAbove: false };
const state = { count: 1, layerCount: 1, activeLayer: 0, outputSide: "bottom", layers: [{ name: "", labels: [""], controls: [control] }] };
const readout = { textContent: "", classList: { toggle() {} }, setAttribute() {} };
const outputs = [];
const context = {
  CV_MAX_VOLTS: 10,
  BUTTON_MAX_COUNT: 16,
  BUTTON_MAX_LAYERS: 8,
  buttonStates: new Map([["button-1", state]]),
  buttonMidiLearnTargetId: null,
  document: { getElementById: () => ({ querySelector: () => readout }) },
  parseValueList: (text) => text ? text.split(",").map(Number) : null,
  clamp: (value, min, max) => Math.min(max, Math.max(min, value)),
  withDeviceSyncMuted: (callback) => callback(),
  markDirty() {},
  updateButtonSettingsCcDisplay() {},
  sendLiveParamUpdate() {},
  savePresetNow() {},
  renderButtonControls() {},
  renderButtonLayerSelector() {},
  renderButtonPorts() {},
  audioEngine: { setParam() {} },
  publishButtonValue() { outputs.push(context.buttonCurrentOutput(state.layers[state.activeLayer].controls[0])); context.updateButtonVisual("button-1"); },
};
vm.createContext(context);
vm.runInContext([
  section("function createButtonControl(", "function renderButtonPorts("),
  section("function updateButtonVisual(", "function routeMidiToButtons("),
  section("function routeMidiToButtons(", "\nconst buttonSettingsOverlay"),
  `globalThis.buttonEngine = ({${section("  button: {", '  "up-down-button": {', source.indexOf("const WIDGET_ENGINES ="))}}).button;`,
].join("\n"), context);
const cc = (value) => context.routeMidiToButtons({ type: "midi", midiType: "control_change", channel: 0, control: 116, value });

context.buttonMidiLearnTargetId = { widgetId: "button-1", index: 0 };
cc(127);
assert.equal(control.cc, 116);
assert.equal(control.channel, 0);
assert.equal(context.buttonMidiLearnTargetId, null);
assert.deepEqual(outputs, []);

cc(127); cc(127); cc(0);
assert.deepEqual(outputs.splice(0), [10, 10, 0]);
assert.equal(readout.textContent, "0");

control.switchMode = true;
cc(127); cc(127); cc(0); cc(127);
assert.deepEqual(outputs.splice(0), [10, 0, 0, 10]);

control.listText = "0, 1.234, 2";
control.listIndex = 0;
cc(127);
assert.equal(readout.textContent, "1.234");
cc(127); cc(0); cc(127);
assert.deepEqual(outputs.splice(0), [1.234, 2, 2, 0]);
assert.equal(readout.textContent, "0");

control.switchMode = false;
control.wasAbove = false;
cc(127); cc(127); cc(0); cc(127);
assert.deepEqual(outputs, [1.234, 1.234, 1.234, 2]);

// Exercise the actual preset payload sent to the ESP and restored after reload.
for (const enabled of [true, false]) {
  const current = state.layers[0].controls[0];
  Object.assign(current, { switchMode: enabled, listIndex: 0, wasAbove: false });
  const saved = JSON.parse(JSON.stringify(context.buttonEngine.getParams("button-1")));
  assert.deepEqual(saved.bindings[0], { cc: 116, channel: 0, midiType: "cc", switchMode: enabled });
  context.buttonEngine.applyParams("button-1", saved);
  assert.equal(state.layers[0].controls[0].switchMode, enabled);
  outputs.length = 0;
  cc(127); cc(127);
  assert.deepEqual(outputs, enabled ? [1.234, 2] : [1.234, 1.234]);
}
const legacy = JSON.parse(JSON.stringify(context.buttonEngine.getParams("button-1")));
delete legacy.bindings[0].switchMode;
context.buttonEngine.applyParams("button-1", legacy);
assert.equal(state.layers[0].controls[0].switchMode, false);
assert.equal(state.layers[0].controls[0].midiType, "cc");

// Note On/Off learn: the press learns, the release neither re-learns nor cancels.
const button = state.layers[0].controls[0];
Object.assign(button, { listText: "", listIndex: 0, wasAbove: false, switchMode: false });
const note = (on, number = 60, velocity = 100) =>
  context.routeMidiToButtons({ type: "midi", midiType: on ? "note_on" : "note_off", channel: 2, note: number, velocity });
context.buttonMidiLearnTargetId = { widgetId: "button-1", index: 0 };
note(false);
assert.equal(context.buttonMidiLearnTargetId?.index, 0);
note(true);
assert.deepEqual([button.cc, button.channel, button.midiType], [60, 2, "note"]);
note(false);
outputs.length = 0;
note(true); note(true, 60, 0); note(true, 61); cc(127);
assert.deepEqual(outputs, [10, 0]);

// Program Change: every matching event toggles; other programs/channels do nothing.
const program = (number) => context.routeMidiToButtons({ type: "midi", midiType: "program_change", channel: 2, program: number });
context.buttonMidiLearnTargetId = { widgetId: "button-1", index: 0 };
program(5);
assert.deepEqual([button.cc, button.channel, button.midiType], [5, 2, "program"]);
outputs.length = 0;
program(5); program(7); program(8);
context.routeMidiToButtons({ type: "midi", midiType: "program_change", channel: 1, program: 5 });
assert.deepEqual(outputs.splice(0), [10]);
program(5); program(5);
assert.deepEqual(outputs.splice(0), [0, 10]);
button.listText = "0, 4, 7";
button.listIndex = 0;
program(5); program(5); program(7); program(5);
assert.deepEqual(outputs.splice(0), [4, 7, 0]);
const pcSaved = JSON.parse(JSON.stringify(context.buttonEngine.getParams("button-1")));
assert.equal(pcSaved.bindings[0].midiType, "program");
context.buttonEngine.applyParams("button-1", pcSaved);
assert.equal(state.layers[0].controls[0].midiType, "program");
console.log("Button MIDI switch, number display and preset round-trip OK");
