import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const engineSource = readFileSync(path.join(root, "data/static/js/audio-engine.js"), "utf8");
const start = engineSource.indexOf("const MDA_EPIANO_DEFAULT_PARAM");
const end = engineSource.indexOf("\n}\n", engineSource.indexOf("function createMdaEpianoEngine(")) + 3;
const context = {};
vm.createContext(context);
vm.runInContext(`${engineSource.slice(start, end)}\nthis.create = createMdaEpianoEngine;`, context);

// Constant, looped PCM so a voice only decays through its envelope.
const pcm = new Int16Array(4096).fill(16000);
const group = { root: 60, high: 127, pos: 0, end: 4000, loop: 2000 };
const instrument = { pcm, keygroups: [group, group, group], noteonStep: 3 };

function levelAfterRelease(sustainDown) {
  const engine = context.create(8, 48000);
  engine.loadInstrument(instrument);
  const outL = new Float32Array(4800);
  const outR = new Float32Array(4800);
  if (sustainDown) engine.controlChange(64, 127);
  engine.noteOn(60, 100);
  engine.process(outL, outR, 480);
  engine.noteOff(60);
  engine.process(outL, outR, 4800); // 100 ms after key release
  const held = Math.abs(outL[4799]);
  engine.controlChange(64, 0);
  engine.process(outL, outR, 4800); // 100 ms after pedal release
  return { held, afterPedalUp: Math.abs(outL[4799]) };
}

const dry = levelAfterRelease(false);
const sustained = levelAfterRelease(true);
assert.ok(sustained.held > 0.01, "sustain pedal keeps a released key sounding");
assert.ok(dry.held < sustained.held / 10, "without pedal, note-off releases immediately");
assert.ok(sustained.afterPedalUp < sustained.held / 10, "pedal up releases sustained notes");

console.log("mda-piano sustain: ok");
