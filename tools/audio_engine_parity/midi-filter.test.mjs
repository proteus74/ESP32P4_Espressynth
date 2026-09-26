import assert from "node:assert/strict";
import { test } from "node:test";
import { createMidiFilterState, acceptsMidiFilter, midiFilterCriterion } from "../../data/static/js/midi-filter-core.js";

const on = (note = 60, channel = 0, velocity = 100) => ({ type: "midi", midiType: "note_on", note, channel, velocity });
const off = (note = 60, channel = 0) => ({ ...on(note, channel), midiType: "note_off" });

test("MIDI filter: default admits all channels and note boundaries", () => {
  const state = createMidiFilterState();
  for (const channel of [0, 15]) for (const note of [0, 60, 127]) {
    assert.equal(acceptsMidiFilter(state, on(note, channel)), true);
    assert.equal(acceptsMidiFilter(state, off(note, channel)), true);
  }
});

test("MIDI filter: note and channel criteria combine with AND", () => {
  const state = { ...createMidiFilterState(), note: 60, channel: 3 };
  assert.equal(acceptsMidiFilter(state, on(60, 3)), true);
  assert.equal(acceptsMidiFilter(state, on(61, 3)), false);
  assert.equal(acceptsMidiFilter(state, on(60, 2)), false);
  assert.equal(acceptsMidiFilter(state, off(61, 3)), false);
});

test("MIDI filter: releases admitted notes after criteria change", () => {
  const state = { ...createMidiFilterState(), note: 60, channel: 3 };
  assert.equal(acceptsMidiFilter(state, on(60, 3)), true);
  state.note = 70;
  state.channel = 5;
  assert.equal(acceptsMidiFilter(state, off(60, 3)), true);
  assert.equal(acceptsMidiFilter(state, off(60, 3)), false);
});

test("MIDI filter: velocity-zero Note On follows Note Off criteria", () => {
  const state = createMidiFilterState();
  acceptsMidiFilter(state, on());
  assert.equal(acceptsMidiFilter(state, on(60, 0, 0)), true);
  assert.equal(acceptsMidiFilter(state, on(61, 0, 0)), true);
  state.note = 60;
  assert.equal(acceptsMidiFilter(state, on(61, 0, 0)), false);
});

test("MIDI filter: CC, program, bend and pressure retain channel messages", () => {
  const state = { ...createMidiFilterState(), note: 60, channel: 2 };
  for (const midiType of ["control_change", "program_change", "pitchwheel", "aftertouch"]) {
    const data = { type: "midi", midiType, channel: 2, control: 7, value: 80 };
    assert.equal(acceptsMidiFilter(state, data), true);
    assert.equal(acceptsMidiFilter(state, { ...data, channel: 1 }), false);
  }
  assert.equal(acceptsMidiFilter(state, { ...on(61, 2), midiType: "polytouch" }), false);
});

test("MIDI filter: pedal release and panic survive channel changes", () => {
  const state = createMidiFilterState();
  acceptsMidiFilter(state, on(60, 3));
  state.channel = 4;
  const cc = { type: "midi", midiType: "control_change", channel: 3, value: 0 };
  assert.equal(acceptsMidiFilter(state, { ...cc, control: 64 }), true);
  assert.equal(acceptsMidiFilter(state, { ...cc, control: 123 }), true);
  assert.equal(acceptsMidiFilter(state, off(60, 3)), false);
});

test("MIDI filter: validates criteria and incoming channel/note", () => {
  for (const invalid of [null, undefined, "", false, true, NaN, Infinity, -2, 128]) assert.equal(midiFilterCriterion(invalid, 127), -1);
  assert.equal(midiFilterCriterion("60", 127), 60);
  assert.equal(midiFilterCriterion(16, 15), -1);
  const state = createMidiFilterState();
  for (const data of [on(-1), on(128), on(60, 16), on(60, -1)]) assert.equal(acceptsMidiFilter(state, data), false);
});
