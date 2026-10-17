// FM8 widget helpers: the patch model, DX7 SysEx import and the Dexed-style editor dialog.
//
// The patch is exactly one DX7 voice in the "unpacked" 155-byte layout that the msfa engine
// (components/msfa, as used by Dexed/MicroDexed) runs on the device. Keys are flat numbers so that
// preset save/load and live updates share one representation:
//   d0..d154  voice bytes   ops  operator on/off mask (bit n = operator n+1)   tune / gain
//   d156 cutoff (99 = open)   d157 resonance   (post-voice low-pass, not part of the DX7 voice)
//   cc_dN / ch_dN  MIDI learn: CC number / channel (0-15) bound to voice byte N, -1 = unbound
// Operator blocks run 6 .. 1: operator n starts at byte (6 - n) * 21.
//   +0..3 R1-4  +4..7 L1-4  +8 BP  +9 LD  +10 RD  +11 LC  +12 RC  +13 RS  +14 AMS  +15 KVS
//   +16 OL  +17 mode(0 ratio/1 fixed)  +18 FC  +19 FF  +20 DET
// 126..133 PR1-4 PL1-4, 134 ALG(0-31), 135 FB, 136 OKS, 137 LFS, 138 LFD, 139 LPMD, 140 LAMD,
// 141 LKS, 142 LFW, 143 LPMS, 144 TRNSP(24 = 0), 145..154 name.

export const FM8_OPS = 6;
export const FM8_VOICE_BYTES = 155;

const LFO_WAVES = ["Triangle", "Saw down", "Saw up", "Square", "Sine", "S&H"];
const CURVES = ["-LIN", "-EXP", "+EXP", "+LIN"];

export function opByteIndex(opNumber, offset) {
  return (6 - opNumber) * 21 + offset;
}

export function defaultFm8Keys() {
  const keys = { ops: 63, tune: 5, gain: 10 };
  for (let n = 1; n <= 6; n += 1) {
    const o = (6 - n) * 21;
    [99, 99, 99, 99, 99, 99, 99, 0, 39, 0, 0, 0, 0, 0, 0, 0, n === 1 ? 99 : 0, 0, 1, 0, 7].forEach((v, i) => {
      keys[`d${o + i}`] = v;
    });
  }
  [99, 99, 99, 99, 50, 50, 50, 50, 0, 0, 1, 35, 0, 0, 0, 1, 0, 3, 24].forEach((v, i) => {
    keys[`d${126 + i}`] = v;
  });
  const name = "INIT VOICE";
  for (let i = 0; i < 10; i += 1) keys[`d${145 + i}`] = name.charCodeAt(i);
  keys.d156 = 99; // cutoff (99 = open)
  keys.d157 = 0; // resonance
  return keys;
}

export function fm8PatchName(keys) {
  let s = "";
  for (let i = 0; i < 10; i += 1) {
    const c = Number(keys[`d${145 + i}`]) || 32;
    s += c >= 32 && c < 127 ? String.fromCharCode(c) : " ";
  }
  return s.trim() || "Init";
}

// ---- DX7 algorithms (Yamaha DX7 voice data, flags per operator 6..1) ----------------------
// bit0-1: output bus (0 = audio out, 1/2 = modulation bus), bit2: add to bus instead of
// replacing it, bit4-5: input bus (0 = none), bit6: feedback in, bit7: feedback out.
const DX7_ALGORITHMS = [
  [0xc1, 0x11, 0x11, 0x14, 0x01, 0x14], [0x01, 0x11, 0x11, 0x14, 0xc1, 0x14],
  [0xc1, 0x11, 0x14, 0x01, 0x11, 0x14], [0xc1, 0x11, 0x94, 0x01, 0x11, 0x14],
  [0xc1, 0x14, 0x01, 0x14, 0x01, 0x14], [0xc1, 0x94, 0x01, 0x14, 0x01, 0x14],
  [0xc1, 0x11, 0x05, 0x14, 0x01, 0x14], [0x01, 0x11, 0xc5, 0x14, 0x01, 0x14],
  [0x01, 0x11, 0x05, 0x14, 0xc1, 0x14], [0x01, 0x05, 0x14, 0xc1, 0x11, 0x14],
  [0xc1, 0x05, 0x14, 0x01, 0x11, 0x14], [0x01, 0x05, 0x05, 0x14, 0xc1, 0x14],
  [0xc1, 0x05, 0x05, 0x14, 0x01, 0x14], [0xc1, 0x05, 0x11, 0x14, 0x01, 0x14],
  [0x01, 0x05, 0x11, 0x14, 0xc1, 0x14], [0xc1, 0x11, 0x02, 0x25, 0x05, 0x14],
  [0x01, 0x11, 0x02, 0x25, 0xc5, 0x14], [0x01, 0x11, 0x11, 0xc5, 0x05, 0x14],
  [0xc1, 0x14, 0x14, 0x01, 0x11, 0x14], [0x01, 0x05, 0x14, 0xc1, 0x14, 0x14],
  [0x01, 0x14, 0x14, 0xc1, 0x14, 0x14], [0xc1, 0x14, 0x14, 0x14, 0x01, 0x14],
  [0xc1, 0x14, 0x14, 0x01, 0x14, 0x04], [0xc1, 0x14, 0x14, 0x14, 0x04, 0x04],
  [0xc1, 0x14, 0x14, 0x04, 0x04, 0x04], [0xc1, 0x05, 0x14, 0x01, 0x14, 0x04],
  [0x01, 0x05, 0x14, 0xc1, 0x14, 0x04], [0x04, 0xc1, 0x11, 0x14, 0x01, 0x14],
  [0xc1, 0x14, 0x01, 0x14, 0x04, 0x04], [0x04, 0xc1, 0x11, 0x14, 0x04, 0x04],
  [0xc1, 0x14, 0x04, 0x04, 0x04, 0x04], [0xc4, 0x04, 0x04, 0x04, 0x04, 0x04],
];

// { carriers: [op numbers], mods: [[src, dst]...], feedback: [src, dst] | null } for algorithm 1..32
export function dx7AlgorithmRouting(algorithm) {
  const flags = DX7_ALGORITHMS[Math.max(1, Math.min(32, algorithm)) - 1];
  const carriers = [];
  const mods = [];
  const bus = [[], [], []];
  let fbIn = 0;
  let fbOut = 0;
  for (let k = 0; k < 6; k += 1) {
    const op = 6 - k; // operator number
    const f = flags[k];
    const inBus = (f >> 4) & 3;
    const outBus = f & 3;
    if (inBus) bus[inBus].forEach((src) => mods.push([src, op]));
    if (f & 0x40) fbIn = op;
    if (f & 0x80) fbOut = op;
    if (outBus === 0) carriers.push(op);
    else if (f & 4) bus[outBus].push(op);
    else bus[outBus] = [op];
  }
  return { carriers: carriers.sort(), mods, feedback: fbIn && fbOut ? [fbOut, fbIn] : null };
}

function algorithmText(algorithm) {
  const r = dx7AlgorithmRouting(algorithm);
  const parts = r.mods.map(([s, d]) => `${s}→${d}`).join("  ");
  const fb = r.feedback ? `  FB ${r.feedback[0]}${r.feedback[0] === r.feedback[1] ? "" : "→" + r.feedback[1]}` : "";
  return `Carriers ${r.carriers.join(",")}   ${parts}${fb}`;
}

// ---- SysEx --------------------------------------------------------------------------------

// 128-byte packed bulk voice -> 155 unpacked bytes.
function unpackBulkVoice(data, offset) {
  const out = new Array(FM8_VOICE_BYTES).fill(0);
  for (let b = 0; b < 6; b += 1) {
    const i = offset + b * 17;
    const o = b * 21;
    for (let k = 0; k < 11; k += 1) out[o + k] = data[i + k] & 0x7f; // R1-4 L1-4 BP LD RD
    out[o + 11] = data[i + 11] & 3;
    out[o + 12] = (data[i + 11] >> 2) & 3;
    out[o + 13] = data[i + 12] & 7;
    out[o + 20] = (data[i + 12] >> 3) & 15;
    out[o + 14] = data[i + 13] & 3;
    out[o + 15] = (data[i + 13] >> 2) & 7;
    out[o + 16] = data[i + 14] & 0x7f;
    out[o + 17] = data[i + 15] & 1;
    out[o + 18] = (data[i + 15] >> 1) & 31;
    out[o + 19] = data[i + 16] & 0x7f;
  }
  const g = offset + 102;
  for (let k = 0; k < 8; k += 1) out[126 + k] = data[g + k] & 0x7f; // PR1-4 PL1-4
  out[134] = data[g + 8] & 31;
  out[135] = data[g + 9] & 7;
  out[136] = (data[g + 9] >> 3) & 1;
  for (let k = 0; k < 4; k += 1) out[137 + k] = data[g + 10 + k] & 0x7f; // LFS LFD LPMD LAMD
  out[141] = data[g + 14] & 1;
  out[142] = (data[g + 14] >> 1) & 7;
  out[143] = (data[g + 14] >> 4) & 7;
  out[144] = data[g + 15] & 0x7f;
  for (let k = 0; k < 10; k += 1) out[145 + k] = data[g + 16 + k] & 0x7f;
  return out;
}

// Single-voice sysex data is already in the unpacked layout.
function unpackSingleVoice(data, offset) {
  const out = new Array(FM8_VOICE_BYTES).fill(0);
  for (let k = 0; k < FM8_VOICE_BYTES; k += 1) out[k] = data[offset + k] & 0x7f;
  return out;
}

// Accepts a whole .syx file (Uint8Array). Returns [{bytes[155], name}] (1 or 32 voices).
export function parseDx7Sysex(bytes) {
  const voices = [];
  const nameOf = (v) => fm8PatchName(Object.fromEntries(v.map((b, i) => [`d${i}`, b])));
  for (let i = 0; i < bytes.length - 6; i += 1) {
    if (bytes[i] !== 0xf0 || bytes[i + 1] !== 0x43) continue;
    const format = bytes[i + 3];
    const size = (bytes[i + 4] << 7) | bytes[i + 5];
    if (format === 0x09 && size === 4096 && bytes.length >= i + 6 + 4096) {
      for (let n = 0; n < 32; n += 1) {
        const v = unpackBulkVoice(bytes, i + 6 + n * 128);
        voices.push({ bytes: v, name: nameOf(v) });
      }
      i += 6 + 4096;
    } else if (format === 0x00 && size === 155 && bytes.length >= i + 6 + 155) {
      const v = unpackSingleVoice(bytes, i + 6);
      voices.push({ bytes: v, name: nameOf(v) });
      i += 6 + 155;
    }
  }
  if (voices.length === 0) throw new Error("No DX7 voice data found (expected 32-voice bulk or single voice SysEx).");
  return voices;
}

// Inverse of unpackBulkVoice: 155 unpacked bytes -> 128 packed bytes at out[offset..].
function packBulkVoice(v, out, offset) {
  for (let b = 0; b < 6; b += 1) {
    const i = offset + b * 17;
    const o = b * 21;
    for (let k = 0; k < 11; k += 1) out[i + k] = v[o + k] & 0x7f;
    out[i + 11] = (v[o + 11] & 3) | ((v[o + 12] & 3) << 2);
    out[i + 12] = (v[o + 13] & 7) | ((v[o + 20] & 15) << 3);
    out[i + 13] = (v[o + 14] & 3) | ((v[o + 15] & 7) << 2);
    out[i + 14] = v[o + 16] & 0x7f;
    out[i + 15] = (v[o + 17] & 1) | ((v[o + 18] & 31) << 1);
    out[i + 16] = v[o + 19] & 0x7f;
  }
  const g = offset + 102;
  for (let k = 0; k < 8; k += 1) out[g + k] = v[126 + k] & 0x7f;
  out[g + 8] = v[134] & 31;
  out[g + 9] = (v[135] & 7) | ((v[136] & 1) << 3);
  for (let k = 0; k < 4; k += 1) out[g + 10 + k] = v[137 + k] & 0x7f;
  out[g + 14] = (v[141] & 1) | ((v[142] & 7) << 1) | ((v[143] & 7) << 4);
  out[g + 15] = v[144] & 0x7f;
  for (let k = 0; k < 10; k += 1) out[g + 16 + k] = v[145 + k] & 0x7f;
}

// 32 unpacked voices (155 bytes each) -> one 4104-byte DX7 32-voice bulk dump.
function buildDx7Bank(voices) {
  const out = new Uint8Array(4104);
  out.set([0xf0, 0x43, 0x00, 0x09, 0x20, 0x00], 0);
  voices.forEach((v, n) => packBulkVoice(v, out, 6 + n * 128));
  let sum = 0;
  for (let i = 6; i < 6 + 4096; i += 1) sum += out[i];
  out[6 + 4096] = -sum & 0x7f;
  out[6 + 4097] = 0xf7;
  return out;
}

// Own presets live in this bank (32 slots), next to the factory banks in /FM8.
const FM8_USER_BANK = "FM8/user.syx";

export function voiceToKeys(voice) {
  const keys = defaultFm8Keys();
  voice.bytes.forEach((b, i) => {
    keys[`d${i}`] = b;
  });
  keys.ops = 63;
  return keys;
}

// ---- UI ---------------------------------------------------------------------------------

// Compact read-out inside the widget: patch name, algorithm and a level bar per operator.
export function renderFm8Summary(container, state) {
  container.textContent = "";
  const title = document.createElement("div");
  title.className = "fm8-summary-title";
  title.textContent = fm8PatchName(state.keys) + (state.modified ? " *" : "");
  const alg = (Number(state.keys.d134) || 0) + 1;
  const sub = document.createElement("div");
  sub.className = "fm8-summary-sub";
  sub.textContent = `DX7 algorithm ${alg}`;
  container.append(title, sub);
}

// ---- Dexed-style editor ------------------------------------------------------------------
// Knobs: drag up/down (Shift = fine), wheel, double-click resets. Right-click a learnable knob
// for MIDI Learn / Unlearn. A learned CC is stored as "cc_dN" / "ch_dN" next to the patch, so the
// firmware applies it natively (Fm8Patch.h applyMidiCc) and the browser only mirrors it.

const SVG_NS = "http://www.w3.org/2000/svg";
const LEARN_PREFIX = /^(cc|ch)_d\d+$/;

// Largest value of a patch byte (its range starts at 0), -1 if it is not learnable.
// Mirrors maxValueOf() in Fm8Patch.h - keep both in sync.
export function fm8ParamMax(index) {
  if (index >= 0 && index < 126) {
    switch (index % 21) {
      case 11: case 12: case 14: return 3; // LC RC AMS
      case 13: case 15: return 7; // RS KVel
      case 17: return 1; // mode
      case 18: return 31; // coarse
      case 20: return 14; // det
      default: return 99;
    }
  }
  if (index >= 126 && index <= 133) return 99; // pitch EG
  switch (index) {
    case 134: return 31; // algorithm
    case 135: return 7; // feedback
    case 136: case 141: return 1; // osc / LFO key sync
    case 137: case 138: case 139: case 140: return 99; // LFO speed, delay, PMD, AMD
    case 142: return 5; // LFO wave
    case 143: return 7; // LFO pitch mod sens
    case 144: return 48; // transpose
    case 156: case 157: return 99; // cutoff, resonance
    default: return -1;
  }
}

export function isFm8BindingKey(key) {
  return LEARN_PREFIX.test(key);
}

// Same rounding as Patch::applyMidiCc (0..127 -> 0..max).
const scaleCc = (value, max) => Math.floor((value * max + 63) / 127);

const el = (tag, cls, text) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
};

// Only one learn at a time, shared by all FM8 editors.
let learning = null; // { state, key, label, onKeys }
let learnPanel = null;
let learnMenu = null;
let learnMenuTarget = null;
let learnMenuUnlearn = null;

function stopLearn() {
  learning = null;
  if (learnPanel) learnPanel.classList.add("hidden");
}

function closeLearnMenu() {
  if (learnMenu) learnMenu.classList.add("hidden");
  learnMenuTarget = null;
}

function ensureLearnUi() {
  if (learnPanel) return;
  learnPanel = el("div", "fm8-learn hidden");
  const msg = el("span", "fm8-learn-msg");
  msg.dataset.role = "msg";
  const cancel = el("button", "btn-secondary", "Cancel");
  cancel.type = "button";
  cancel.addEventListener("click", stopLearn);
  learnPanel.append(el("strong", "", "MIDI Learn"), msg, cancel);
  document.body.appendChild(learnPanel);

  learnMenu = el("div", "page-tab-context-menu fm8-ctx hidden");
  const learnBtn = el("button", "", "MIDI Learn");
  learnBtn.type = "button";
  learnBtn.dataset.action = "learn";
  learnMenuUnlearn = el("button", "", "Unlearn");
  learnMenuUnlearn.type = "button";
  learnMenuUnlearn.dataset.action = "unlearn";
  learnMenu.append(learnBtn, learnMenuUnlearn);
  learnMenu.addEventListener("click", (e) => {
    const action = e.target.closest("button")?.dataset.action;
    const target = learnMenuTarget;
    closeLearnMenu();
    if (!target) return;
    if (action === "learn") startLearn(target);
    else if (action === "unlearn") unlearn(target);
  });
  document.body.appendChild(learnMenu);

  document.addEventListener("mousedown", (e) => {
    if (!e.target.closest(".fm8-ctx")) closeLearnMenu();
  });
  // Capture phase: Escape cancels the learn without also closing the editor dialog.
  window.addEventListener(
    "keydown",
    (e) => {
      if (e.key !== "Escape" || !learning) return;
      e.preventDefault();
      e.stopImmediatePropagation();
      stopLearn();
    },
    true
  );
}

function startLearn(target) {
  ensureLearnUi();
  learning = { state: target.state, key: target.key, label: target.label, onKeys: target.host.onKeys };
  learnPanel.querySelector('[data-role="msg"]').textContent = `Turn a controller for "${target.label}" (waiting for CC...)`;
  learnPanel.classList.remove("hidden");
}

function unlearn(target) {
  const { state, key, host } = target;
  const keys = { [`cc_${key}`]: -1, [`ch_${key}`]: -1 };
  delete state.keys[`cc_${key}`];
  delete state.keys[`ch_${key}`];
  host.onKeys(keys);
  state.ui?.refresh();
}

function openLearnMenu(event, target) {
  ensureLearnUi();
  event.preventDefault();
  event.stopPropagation();
  learnMenuTarget = target;
  learnMenuUnlearn.disabled = target.state.keys[`cc_${target.key}`] === undefined;
  learnMenu.classList.remove("hidden");
  const rect = learnMenu.getBoundingClientRect();
  learnMenu.style.left = `${Math.min(event.clientX, window.innerWidth - rect.width - 8)}px`;
  learnMenu.style.top = `${Math.min(event.clientY, window.innerHeight - rect.height - 8)}px`;
}

// Called for every incoming MIDI CC. `states`: Map widgetId -> FM8 state. Returns true when the
// CC was consumed by a learn (the caller then skips the other routes for it).
export function handleFm8Midi(data, states, onChanged) {
  if (data.type !== "midi" || data.midiType !== "control_change") return false;
  if (learning) {
    const { state, key, onKeys } = learning;
    stopLearn();
    const keys = { [`ch_${key}`]: data.channel, [`cc_${key}`]: data.control };
    Object.assign(state.keys, keys);
    onKeys(keys);
    state.ui?.refresh();
    return true;
  }
  // Following a CC only updates the display: the firmware already applied it.
  states.forEach((state, id) => {
    let changed = false;
    Object.keys(state.keys).forEach((k) => {
      const m = /^cc_(d\d+)$/.exec(k);
      if (!m || Number(state.keys[k]) !== data.control) return;
      if (Number(state.keys[`ch_${m[1]}`]) !== data.channel) return;
      state.keys[m[1]] = scaleCc(data.value, fm8ParamMax(Number(m[1].slice(1))));
      changed = true;
    });
    if (changed) {
      state.modified = true;
      state.ui?.refresh();
      onChanged(id);
    }
  });
  return false;
}

// EG shape of one operator: rates set the segment widths (faster = narrower), levels the heights.
function egPoints(keys, n) {
  const rate = [0, 1, 2, 3].map((i) => Number(keys[`d${opByteIndex(n, i)}`]) || 0);
  const level = [4, 5, 6, 7].map((i) => Number(keys[`d${opByteIndex(n, i)}`]) || 0);
  const w = rate.map((r) => 0.12 + ((99 - r) / 99) * 0.88);
  const pts = [[0, 0]];
  let x = 0;
  x += w[0];
  pts.push([x, level[0]]);
  x += w[1];
  pts.push([x, level[1]]);
  x += w[2];
  pts.push([x, level[2]]);
  x += 0.3; // sustain at L3
  pts.push([x, level[2]]);
  x += w[3];
  pts.push([x, level[3]]);
  return pts.map(([px, py]) => [(px / x) * 100, 50 - (py / 99) * 46]);
}

// Dexed-style editor + SysEx loader. `host`: { state, onKeys(changedKeys), onClose() }.
// Curve shapes for the L/R curve boxes, in the order of CURVES (-LIN, -EXP, +EXP, +LIN).
const CURVE_POINTS = ["4,4 36,20", "4,4 12,7 24,13 36,20", "4,20 12,17 24,13 36,4", "4,20 36,4"];

// LFO wave icons (40x24 box), in the order of LFO_WAVES.
const WAVE_POINTS = [
  "2,12 9,4 20,20 31,4 38,12",
  "2,4 20,20 20,4 38,20",
  "2,20 20,4 20,20 38,4",
  "2,20 2,4 20,4 20,20 38,20 38,4",
  Array.from({ length: 20 }, (_, i) => `${2 + i * 2},${(12 - 8 * Math.sin((i / 19) * Math.PI * 3)).toFixed(1)}`).join(" "),
  "2,14 8,14 8,6 14,6 14,18 20,18 20,9 26,9 26,16 32,16 32,5 38,5",
];

const svgEl = (tag) => document.createElementNS(SVG_NS, tag);

// Algorithm diagram: operators in a grid (carriers at the bottom), modulation lines between them.
function drawAlgorithm(svg, algorithm) {
  const r = dx7AlgorithmRouting(algorithm);
  const level = {};
  for (let op = 1; op <= 6; op += 1) level[op] = 0;
  for (let pass = 0; pass < 6; pass += 1) {
    r.mods.forEach(([s, d]) => {
      level[s] = Math.max(level[s], level[d] + 1);
    });
  }
  const dest = new Map();
  r.mods.forEach(([s, d]) => {
    if (!dest.has(d)) dest.set(d, []);
    dest.get(d).push(s);
  });
  const col = {};
  let next = 0;
  const place = (op) => {
    if (col[op] !== undefined) return col[op];
    const srcs = dest.get(op) || [];
    if (!srcs.length) {
      col[op] = next;
      next += 1;
    } else {
      col[op] = srcs.map(place)[0];
    }
    return col[op];
  };
  [...r.carriers].sort().forEach(place);
  // Two operators can land in one cell (e.g. a modulator feeding two parents): move the later one
  // to the nearest free column of its row.
  const used = new Set();
  [1, 2, 3, 4, 5, 6]
    .sort((a, b) => level[a] - level[b] || col[a] - col[b] || a - b)
    .forEach((op) => {
      const free = (c) => c >= 0 && c < 6 && !used.has(`${c},${level[op]}`);
      let c = col[op];
      for (let d = 1; !free(c) && d < 6; d += 1) c = free(col[op] + d) ? col[op] + d : free(col[op] - d) ? col[op] - d : c;
      col[op] = c;
      used.add(`${c},${level[op]}`);
    });
  const rows = Math.max(4, Math.max(...Object.values(level)) + 1);
  const cell = 20;
  svg.setAttribute("viewBox", `0 0 ${6 * cell + 6} ${rows * cell}`); // +6: room for the feedback loop
  svg.textContent = "";
  const top = (op) => (rows - 1 - level[op]) * cell + 3;
  const left = (op) => col[op] * cell + 3;
  r.mods.forEach(([s, d]) => {
    const x1 = left(s) + 7;
    const y1 = top(s) + 14;
    const x2 = left(d) + 7;
    const y2 = top(d);
    const ym = (y1 + y2) / 2;
    const line = svgEl("polyline");
    line.setAttribute("points", `${x1},${y1} ${x1},${ym} ${x2},${ym} ${x2},${y2}`);
    line.setAttribute("class", "fm8-alg-line");
    svg.appendChild(line);
  });
  if (r.feedback) {
    const op = r.feedback[1];
    const x = left(op);
    const y = top(op);
    const fb = svgEl("polyline");
    fb.setAttribute("points", `${x + 14},${y + 7} ${x + 18},${y + 7} ${x + 18},${y - 2} ${x + 7},${y - 2}`);
    fb.setAttribute("class", "fm8-alg-line");
    svg.appendChild(fb);
  }
  for (let op = 1; op <= 6; op += 1) {
    const g = svgEl("g");
    const rect = svgEl("rect");
    rect.setAttribute("x", String(left(op)));
    rect.setAttribute("y", String(top(op)));
    rect.setAttribute("width", "14");
    rect.setAttribute("height", "14");
    rect.setAttribute("class", `fm8-alg-box${r.carriers.includes(op) ? " carrier" : ""}`);
    const text = svgEl("text");
    text.setAttribute("x", String(left(op) + 7));
    text.setAttribute("y", String(top(op) + 11));
    text.setAttribute("text-anchor", "middle");
    text.setAttribute("class", "fm8-alg-num");
    text.textContent = String(op);
    g.append(rect, text);
    svg.appendChild(g);
  }
}

// Pitch EG: starts at L4, rates set the segment widths, levels (50 = centre) the heights.
function pitchEgPoints(keys) {
  const rate = [126, 127, 128, 129].map((i) => Number(keys[`d${i}`]) || 0);
  const level = [130, 131, 132, 133].map((i) => Number(keys[`d${i}`]) || 0);
  const w = rate.map((r) => 0.12 + ((99 - r) / 99) * 0.88);
  const pts = [[0, level[3]]];
  let x = 0;
  [0, 1, 2].forEach((i) => {
    x += w[i];
    pts.push([x, level[i]]);
  });
  x += 0.3;
  pts.push([x, level[2]]);
  x += w[3];
  pts.push([x, level[3]]);
  return pts.map(([px, py]) => [(px / x) * 100, 50 - (py / 99) * 46]);
}

// All .syx banks below /FM8 on the device (sub-folders included), as { path, label }.
async function listFm8Banks(dir = "FM8", depth = 0) {
  try {
    const res = await fetch(`/api/files/list?dir=${encodeURIComponent(dir)}`);
    if (!res.ok) return [];
    const { entries = [] } = await res.json();
    const out = [];
    for (const e of entries) {
      const path = `${dir}/${e.name}`;
      if (e.isDir) {
        if (depth < 2) out.push(...(await listFm8Banks(path, depth + 1)));
      } else if (/\.(syx|sysex)$/i.test(e.name)) {
        out.push({ path, label: path.slice(4).replace(/\.(syx|sysex)$/i, "") });
      }
    }
    return out.sort((a, b) => a.label.localeCompare(b.label, undefined, { numeric: true }));
  } catch (err) {
    return [];
  }
}

// Dexed-style editor, laid out like the Dexed window: six operator panels, then brand/status,
// algorithm, LFO and pitch EG. `host`: { state, onKeys(changedKeys), onClose() }.
export function openFm8Dialog(host) {
  const { state } = host;
  const overlay = el("div", "modal-overlay fm8-dialog-overlay");
  const box = el("div", "modal fm8-dialog");
  overlay.appendChild(box);

  const head = el("div", "fm8-dialog-head");
  const h = el("h3");
  const closeBtn = el("button", "btn-secondary", "Close");
  closeBtn.type = "button";
  head.append(h, closeBtn);

  // Black status display (bottom left): patch name, or the parameter under the pointer.
  const statusEl = el("div", "fm8-status");
  let hoverText = null;
  const idleStatus = () => `${fm8PatchName(state.keys).toUpperCase()}${state.modified ? " *" : ""}`;
  const refreshTitle = () => {
    h.textContent = `FM8 - ${fm8PatchName(state.keys)}${state.modified ? " *" : ""}`;
    statusEl.textContent = hoverText ?? idleStatus();
  };

  // Every paint callback of this editor; run after any change, including live CC.
  const repaint = [];
  const refreshAll = () => {
    repaint.forEach((f) => f());
    refreshTitle();
  };
  state.ui = { refresh: refreshAll };

  let pending = {};
  let flushTimer = null;
  const flush = () => {
    flushTimer = null;
    if (!Object.keys(pending).length) return;
    const keys = pending;
    pending = {};
    host.onKeys(keys);
  };
  // Throttled: at most one send per 120 ms while a control is dragged.
  const queue = (key, value) => {
    state.keys[key] = value;
    state.modified = true;
    pending[key] = value;
    if (flushTimer === null) flushTimer = setTimeout(flush, 120);
    refreshAll();
  };

  const paramMax = (key) => fm8ParamMax(Number(key.slice(1)));

  // Shared by knobs and the break point slider: value text goes to the status display only.
  const attachControl = (wrap, hit, key, name, display, { min, max, learn, label, vertical }) => {
    const setValue = (v) => queue(key, Math.min(max, Math.max(min, v)));
    let hovered = false;
    const showStatus = () => {
      hoverText = hovered ? `${name} = ${display(Number(state.keys[key]) || 0)}` : null;
      statusEl.textContent = hoverText ?? idleStatus();
    };
    wrap.addEventListener("pointerenter", () => {
      hovered = true;
      showStatus();
    });
    wrap.addEventListener("pointerleave", () => {
      hovered = false;
      showStatus();
    });
    repaint.push(() => {
      if (hovered) showStatus();
      const bound = learn && state.keys[`cc_${key}`] !== undefined && Number(state.keys[`cc_${key}`]) >= 0;
      wrap.classList.toggle("learned", bound);
      wrap.title = bound ? `${name} (CC ${state.keys[`cc_${key}`]}, Ch ${Number(state.keys[`ch_${key}`]) + 1})` : "";
    });
    hit.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      hit.setPointerCapture(e.pointerId);
      const start = vertical ? e.clientY : e.clientX;
      const startV = Number(state.keys[key]) || 0;
      const onMove = (ev) => {
        const fine = ev.shiftKey ? 0.25 : 1;
        const d = vertical ? start - ev.clientY : ev.clientX - start;
        setValue(Math.round(startV + (d * fine * (max - min)) / 150));
      };
      const onUp = () => {
        hit.removeEventListener("pointermove", onMove);
        hit.removeEventListener("pointerup", onUp);
        hit.removeEventListener("pointercancel", onUp);
      };
      hit.addEventListener("pointermove", onMove);
      hit.addEventListener("pointerup", onUp);
      hit.addEventListener("pointercancel", onUp);
    });
    hit.addEventListener(
      "wheel",
      (e) => {
        e.preventDefault();
        setValue((Number(state.keys[key]) || 0) + (e.deltaY < 0 ? 1 : -1));
      },
      { passive: false }
    );
    hit.addEventListener("dblclick", () => {
      const def = defaultFm8Keys()[key];
      if (def !== undefined) setValue(def);
    });
    if (learn) {
      wrap.addEventListener("contextmenu", (e) => openLearnMenu(e, { host, state, key, label: name }));
    }
  };

  // Rotary knob. `label` is the caption under it (may be empty), `name` goes to the status display.
  const knob = (key, label, { min = 0, max = paramMax(key), learn = true, name = label, display = (v) => v } = {}) => {
    const wrap = el("div", "fm8-knob");
    const dial = el("div", "fm8-knob-dial");
    const ind = el("i", "fm8-knob-ind");
    dial.appendChild(ind);
    wrap.appendChild(dial);
    if (label) wrap.appendChild(el("span", "fm8-knob-label", label));
    repaint.push(() => {
      const t = ((Number(state.keys[key]) || 0) - min) / (max - min || 1);
      ind.style.transform = `rotate(${-135 + t * 270}deg)`;
    });
    attachControl(wrap, dial, key, name.toUpperCase(), display, { min, max, learn, label: name, vertical: true });
    return wrap;
  };

  // Horizontal slider (break point).
  const hslider = (key, label, name) => {
    const wrap = el("div", "fm8-hslider");
    const track = el("div", "fm8-hslider-track");
    const thumb = el("i", "fm8-hslider-thumb");
    track.appendChild(thumb);
    wrap.append(el("span", "fm8-knob-label", label), track);
    repaint.push(() => {
      // keep the whole thumb inside the track (its width is 6px, see .fm8-hslider-thumb)
      thumb.style.left = `calc((100% - 6px) * ${(Number(state.keys[key]) || 0) / 99})`;
    });
    attachControl(wrap, track, key, name.toUpperCase(), (v) => v, { min: 0, max: 99, learn: true, label: name, vertical: false });
    return wrap;
  };

  // Ratio/fixed switch with its two LEDs.
  const modeSwitch = (key) => {
    const wrap = el("div", "fm8-mode");
    const ratio = el("span", "fm8-mode-opt", "ratio");
    const fixed = el("span", "fm8-mode-opt", "fixed");
    const ledA = el("i", "fm8-led");
    const ledB = el("i", "fm8-led");
    const cap = el("button", "fm8-sw");
    cap.type = "button";
    cap.title = "ratio / fixed";
    cap.addEventListener("click", () => queue(key, Number(state.keys[key]) ? 0 : 1));
    wrap.append(ratio, fixed, ledA, cap, ledB);
    repaint.push(() => {
      const isFixed = Number(state.keys[key]) === 1;
      cap.classList.toggle("on", isFixed);
      ledA.classList.toggle("lit", !isFixed);
      ledB.classList.toggle("lit", isFixed);
    });
    return wrap;
  };

  // Plain on/off switch with a caption (LFO key sync, osc key sync).
  const toggle = (key, label) => {
    const wrap = el("div", "fm8-toggle");
    const sw = el("button", "fm8-sw");
    sw.type = "button";
    const led = el("i", "fm8-led");
    sw.addEventListener("click", () => queue(key, Number(state.keys[key]) ? 0 : 1));
    wrap.append(el("span", "fm8-knob-label", label), el("div", "fm8-toggle-row"));
    wrap.lastChild.append(sw, led);
    repaint.push(() => {
      const on = Number(state.keys[key]) === 1;
      sw.classList.toggle("on", on);
      led.classList.toggle("lit", on);
    });
    return wrap;
  };

  // L/R curve: a small black box with the curve drawn in it; click cycles the four shapes.
  const curveBox = (key, label) => {
    const wrap = el("div", "fm8-curve-cell");
    const box = el("button", "fm8-curve");
    box.type = "button";
    const svg = svgEl("svg");
    svg.setAttribute("viewBox", "0 0 40 24");
    const line = svgEl("polyline");
    svg.appendChild(line);
    box.appendChild(svg);
    box.addEventListener("click", () => queue(key, (Number(state.keys[key]) + 1) % CURVES.length));
    repaint.push(() => {
      line.setAttribute("points", CURVE_POINTS[Number(state.keys[key]) || 0]);
      box.title = `curve ${CURVES[Number(state.keys[key]) || 0]}`;
    });
    wrap.append(box, el("span", "fm8-knob-label", label));
    return wrap;
  };

  const row = (cls, ...items) => {
    const r = el("div", cls);
    r.append(...items);
    return r;
  };

  // Black graph box with a teal filled envelope; `pointsOf` returns the polyline in a 100x50 box.
  const egGraph = (pointsOf) => {
    const wrap = el("div", "fm8-eg-wrap");
    const svg = svgEl("svg");
    svg.setAttribute("viewBox", "0 0 100 50");
    svg.setAttribute("preserveAspectRatio", "none");
    svg.setAttribute("class", "fm8-eg");
    const fill = svgEl("polygon");
    const line = svgEl("polyline");
    svg.append(fill, line);
    wrap.appendChild(svg);
    repaint.push(() => {
      const pts = pointsOf();
      line.setAttribute("points", pts.map((p) => p.join(",")).join(" "));
      fill.setAttribute("points", [...pts, [100, 50], [0, 50]].map((p) => p.join(",")).join(" "));
    });
    return wrap;
  };

  // ---- Six operator panels ------------------------------------------------------------------
  const opsWrap = el("div", "fm8-ops");
  for (let n = 1; n <= 6; n += 1) {
    const k = (off) => `d${opByteIndex(n, off)}`;
    const op = `OP${n} `;
    const card = el("section", "fm8-op");

    // Top left: frequency display with detune / coarse / fine.
    const freq = el("div", "fm8-freq");
    repaint.push(() => {
      const c = Number(state.keys[k(18)]) || 0;
      const f = Number(state.keys[k(19)]) || 0;
      freq.textContent =
        Number(state.keys[k(17)]) === 1
          ? `f = ${(10 ** (c & 3) * (1 + f / 100)).toFixed(2)} Hz`
          : `f = ${((c === 0 ? 0.5 : c) * (1 + f / 100)).toFixed(2)}`;
    });
    const freqBlock = row(
      "fm8-op-freq",
      freq,
      row(
        "fm8-knob-row",
        knob(k(20), "det", { name: `${op}DETUNE`, display: (v) => v - 7 }),
        knob(k(18), "coarse", { max: 31, name: `${op}FREQUENCY COARSE` }),
        knob(k(19), "fine", { name: `${op}FREQUENCY FINE` })
      )
    );

    // Top right: ratio/fixed switch, operator on/off switch, number.
    const onSw = el("button", "fm8-sw vertical");
    onSw.type = "button";
    onSw.title = "operator on/off";
    onSw.addEventListener("click", () => {
      const bit = 1 << (n - 1);
      queue("ops", Number(state.keys.ops) ^ bit);
    });
    repaint.push(() => {
      onSw.classList.toggle("on", (Number(state.keys.ops) & (1 << (n - 1))) !== 0);
    });
    const modeBlock = row("fm8-op-mode", modeSwitch(k(17)), onSw, el("strong", "fm8-op-num", String(n)));

    // Left: EG graph, level knobs, rate knobs.
    const eg = row(
      "fm8-eg-block",
      egGraph(() => egPoints(state.keys, n)),
      el("span", "fm8-eg-caption", "EG level"),
      row("fm8-knob-row", ...[4, 5, 6, 7].map((off, i) => knob(k(off), `${i + 1}`, { name: `${op}EG LEVEL ${i + 1}` }))),
      row("fm8-knob-row", ...[0, 1, 2, 3].map((off, i) => knob(k(off), "", { name: `${op}EG RATE ${i + 1}` }))),
      el("span", "fm8-eg-caption", "EG rate")
    );

    // Right: A mod sens / key vel / level; scaling; curves.
    const upper = row(
      "fm8-op-panel",
      row(
        "fm8-knob-row",
        knob(k(14), "A mod sens", { name: `${op}A MOD SENS` }),
        knob(k(15), "key vel", { name: `${op}KEY VELOCITY` }),
        knob(k(16), "level", { name: `${op}OUTPUT LEVEL` })
      )
    );
    const lower = row(
      "fm8-op-panel",
      row(
        "fm8-scaling",
        knob(k(9), "L depth", { name: `${op}LEFT DEPTH` }),
        hslider(k(8), "break point", `${op}BREAK POINT`),
        knob(k(10), "R depth", { name: `${op}RIGHT DEPTH` })
      ),
      row(
        "fm8-curve-row",
        curveBox(k(11), "L curve"),
        knob(k(13), "rate scaling", { name: `${op}RATE SCALING` }),
        curveBox(k(12), "R curve")
      )
    );
    card.append(freqBlock, modeBlock, eg, row("fm8-op-right", upper, lower));
    opsWrap.appendChild(card);
  }

  // ---- Bottom row: brand/status, algorithm, LFO, pitch EG -----------------------------------
  // Bank = a .syx file in /FM8 on the device (put there with the file explorer), voice = an
  // instrument in it.
  const bankSelect = el("select", "fm8-bank-select");
  bankSelect.add(new Option("Bank", ""));
  const voiceSelect = el("select", "fm8-voice-select");
  voiceSelect.disabled = true;
  voiceSelect.add(new Option("Voice", ""));
  const saveBtn = el("button", "fm8-btn", "SAVE");
  saveBtn.type = "button";
  saveBtn.title = "Save this sound as a preset in /FM8/user.syx";
  saveBtn.addEventListener("click", () => openSavePanel());
  const initBtn = el("button", "fm8-btn", "INIT");
  initBtn.type = "button";
  initBtn.addEventListener("click", () => {
    const keys = defaultFm8Keys();
    Object.assign(state.keys, keys);
    state.modified = false;
    state.origin = "";
    bankSelect.value = "";
    refreshAll();
    host.onKeys(keys);
  });

  const brand = row(
    "fm8-brand",
    el("div", "fm8-logo", "FM8"),
    row(
      "fm8-brand-knobs",
      row(
        "fm8-knob-row fm8-brand-inset",
        knob("tune", "tune", { min: 0, max: 10, learn: false, name: "TUNE" }),
        knob("d156", "cutoff", { name: "CUTOFF" }),
        knob("d157", "reso", { name: "RESONANCE" })
      ),
      row(
        "fm8-knob-row fm8-brand-inset",
        knob("gain", "level", { min: 0, max: 10, learn: false, name: "OUTPUT LEVEL" }),
        knob("d144", "middle C", { name: "TRANSPOSE", display: (v) => v - 24 })
      )
    ),
    statusEl,
    bankSelect,
    row("fm8-btn-row", saveBtn, initBtn),
    voiceSelect
  );

  const algSvg = svgEl("svg");
  algSvg.setAttribute("class", "fm8-alg-svg");
  repaint.push(() => drawAlgorithm(algSvg, (Number(state.keys.d134) || 0) + 1));
  const algorithm = row(
    "fm8-algorithm",
    el("div", "fm8-alg-grid"),
    row(
      "fm8-alg-knobs",
      knob("d134", "algorithm", { name: "ALGORITHM", display: (v) => v + 1 }),
      knob("d135", "feedback", { name: "FEEDBACK" })
    )
  );
  algorithm.firstChild.appendChild(algSvg);

  const waveBox = el("button", "fm8-curve fm8-wave");
  waveBox.type = "button";
  const waveSvg = svgEl("svg");
  waveSvg.setAttribute("viewBox", "0 0 40 24");
  const wavePoly = svgEl("polyline");
  waveSvg.appendChild(wavePoly);
  waveBox.appendChild(waveSvg);
  waveBox.addEventListener("click", () => queue("d142", (Number(state.keys.d142) + 1) % LFO_WAVES.length));
  repaint.push(() => {
    wavePoly.setAttribute("points", WAVE_POINTS[Number(state.keys.d142) || 0]);
    waveBox.title = `LFO wave: ${LFO_WAVES[Number(state.keys.d142) || 0]}`;
  });
  const lfo = row(
    "fm8-lfo",
    row("fm8-lfo-top", row("fm8-curve-cell", waveBox, el("span", "fm8-knob-label", "wave")), knob("d143", "P mod sens", { name: "PITCH MOD SENS" })),
    row(
      "fm8-knob-row",
      knob("d137", "speed", { name: "LFO SPEED" }),
      knob("d138", "delay", { name: "LFO DELAY" }),
      knob("d139", "PMD", { name: "LFO PITCH MOD DEPTH" }),
      knob("d140", "AMD", { name: "LFO AMP MOD DEPTH" })
    ),
    row("fm8-lfo-sync", toggle("d141", "LFO key sync"), toggle("d136", "OSC key sync"))
  );

  const pitchEg = row(
    "fm8-pitch-eg",
    egGraph(() => pitchEgPoints(state.keys)),
    el("span", "fm8-eg-caption", "P EG level"),
    row("fm8-knob-row", ...[130, 131, 132, 133].map((i) => knob(`d${i}`, `${i - 129}`, { name: `PITCH EG LEVEL ${i - 129}` }))),
    row("fm8-knob-row", ...[126, 127, 128, 129].map((i) => knob(`d${i}`, "", { name: `PITCH EG RATE ${i - 125}` }))),
    el("span", "fm8-eg-caption", "P EG rate")
  );
  const bottom = row("fm8-bottom", brand, algorithm, lfo, pitchEg);

  const info = el("span", "fm8-info", "Pick a bank, then a voice. A * marks edits that are not saved yet (SAVE).");
  box.append(head, opsWrap, bottom, info);
  document.body.appendChild(overlay);
  refreshAll();

  // The editor has a fixed design size; scale it to the window (also follows browser zoom, which
  // changes the CSS viewport size).
  const fit = () => {
    box.style.zoom = "1";
    const scale = Math.min((window.innerWidth - 16) / box.offsetWidth, (window.innerHeight - 16) / box.offsetHeight);
    box.style.zoom = String(Math.max(0.4, Math.min(2, scale)));
  };
  fit();
  window.addEventListener("resize", fit);

  const close = () => {
    window.removeEventListener("resize", fit);
    clearTimeout(flushTimer);
    flushTimer = null;
    if (Object.keys(pending).length) host.onKeys(pending);
    pending = {};
    if (learning?.state === state) stopLearn();
    closeLearnMenu();
    savePanel.remove();
    state.ui = null;
    overlay.remove();
    host.onClose();
  };
  closeBtn.addEventListener("click", close);
  overlay.addEventListener("mousedown", (e) => {
    if (e.target === overlay) close();
  });

  let voices = [];
  let shownBank = ""; // bank id the voice list shows
  let lastBank = "";
  let lastVoice = "";
  const bankName = (id) => id.replace(/^FM8\//, "").replace(/\.(syx|sysex)$/i, "");
  const confirmDiscard = () => !state.modified || window.confirm(`Discard the changes to "${fm8PatchName(state.keys)}"?`);

  // Replace the edited patch by one voice of the shown bank.
  const applyVoice = (index) => {
    const voice = voices[index];
    if (!voice) return;
    const keys = voiceToKeys(voice);
    Object.assign(state.keys, keys);
    state.origin = `${shownBank}#${index}`;
    state.modified = false;
    refreshAll();
    info.textContent = `${bankName(shownBank)} #${index + 1} - algorithm ${keys.d134 + 1}, feedback ${keys.d135}`;
    host.onKeys(keys);
  };
  // List the voices of a bank. `select` = voice to apply, or null to only list (keeps the edit).
  const showBank = (bytes, bankId, select) => {
    try {
      voices = parseDx7Sysex(bytes);
      shownBank = bankId;
      voiceSelect.textContent = "";
      voices.forEach((v, i) => voiceSelect.add(new Option(`${String(i + 1).padStart(2, "0")}  ${v.name || "(unnamed)"}`, String(i))));
      voiceSelect.disabled = voices.length < 2;
      lastBank = bankId;
      if (select !== null) {
        voiceSelect.value = String(select);
        lastVoice = voiceSelect.value;
        applyVoice(select);
      }
    } catch (err) {
      info.textContent = `SysEx error: ${err.message}`;
    }
  };
  const fetchBank = async (id) => {
    const res = await fetch(`/api/files/download?path=${encodeURIComponent(id)}`);
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return new Uint8Array(await res.arrayBuffer());
  };
  bankSelect.addEventListener("change", async () => {
    const id = bankSelect.value;
    if (!id) return;
    if (!confirmDiscard()) {
      bankSelect.value = lastBank;
      return;
    }
    try {
      showBank(await fetchBank(id), id, 0);
    } catch (err) {
      info.textContent = `Bank load failed: ${err.message}`;
      bankSelect.value = lastBank;
    }
  });
  voiceSelect.addEventListener("change", () => {
    if (!confirmDiscard()) {
      voiceSelect.value = lastVoice;
      return;
    }
    lastVoice = voiceSelect.value;
    applyVoice(Number(voiceSelect.value));
  });

  const fillBankSelect = async (banks) => {
    const keep = bankSelect.value;
    bankSelect.length = 1;
    banks.forEach((b) => bankSelect.add(new Option(b.label, b.path)));
    bankSelect.value = [...bankSelect.options].some((o) => o.value === keep) ? keep : "";
  };
  // Show where the current patch came from (bank + voice) without touching the edit.
  const restoreOrigin = async (banks) => {
    const m = /^(.+)#(\d+)$/.exec(state.origin || "");
    if (!m || !banks.some((b) => b.path === m[1])) return;
    try {
      showBank(await fetchBank(m[1]), m[1], null);
      bankSelect.value = m[1];
      voiceSelect.value = m[2];
      lastVoice = m[2];
    } catch (err) {
      // bank is gone: just no preselection
    }
  };
  listFm8Banks().then(async (banks) => {
    if (!overlay.isConnected) return;
    await fillBankSelect(banks);
    if (banks.length) info.textContent = `${banks.length} bank${banks.length > 1 ? "s" : ""} in /FM8 - pick a bank, then a voice. A * marks unsaved edits.`;
    await restoreOrigin(banks);
  });

  // ---- SAVE: store the patch as a voice in /FM8/user.syx ----------------------------------
  const savePanel = el("div", "fm8-save hidden");
  const saveName = el("input", "fm8-save-name");
  saveName.maxLength = 10;
  saveName.placeholder = "name";
  const saveSlot = el("select", "fm8-save-slot");
  const saveOk = el("button", "fm8-btn", "Save");
  saveOk.type = "button";
  const saveCancel = el("button", "btn-secondary", "Cancel");
  saveCancel.type = "button";
  savePanel.append(el("strong", "", "Save preset to user bank"), saveName, saveSlot, saveOk, saveCancel);
  document.body.appendChild(savePanel);
  const initVoiceBytes = () => {
    const d = defaultFm8Keys();
    return Array.from({ length: 155 }, (_, i) => Number(d[`d${i}`]) || 0);
  };
  let userVoices = null; // 32 x 155 bytes of the user bank, read when the panel opens
  const openSavePanel = async () => {
    userVoices = Array.from({ length: 32 }, initVoiceBytes);
    let names = Array.from({ length: 32 }, () => "INIT VOICE");
    try {
      // Only fetch the user bank if it exists (avoids a 404 on the very first save).
      if ((await listFm8Banks()).some((b) => b.path === FM8_USER_BANK)) {
        const parsed = parseDx7Sysex(await fetchBank(FM8_USER_BANK));
        if (parsed.length === 32) {
          userVoices = parsed.map((v) => v.bytes);
          names = parsed.map((v) => v.name);
        }
      }
    } catch (err) {
      // unreadable user bank: it is rewritten by the save
    }
    saveSlot.textContent = "";
    names.forEach((n, i) => saveSlot.add(new Option(`${String(i + 1).padStart(2, "0")}  ${n}`, String(i))));
    const m = new RegExp(`^${FM8_USER_BANK.replace(".", "\\.")}#(\\d+)$`).exec(state.origin || "");
    const free = names.findIndex((n) => n === "INIT VOICE");
    saveSlot.value = String(m ? Number(m[1]) : Math.max(0, free));
    saveName.value = fm8PatchName(state.keys).slice(0, 10);
    savePanel.classList.remove("hidden");
    saveName.focus();
    saveName.select();
  };
  saveCancel.addEventListener("click", () => savePanel.classList.add("hidden"));
  saveOk.addEventListener("click", async () => {
    const slot = Number(saveSlot.value);
    const name = saveName.value.replace(/[^\x20-\x7e]/g, "").slice(0, 10).padEnd(10, " ");
    const voiceBytes = Array.from({ length: 155 }, (_, i) => Number(state.keys[`d${i}`]) || 0);
    const nameKeys = {};
    for (let i = 0; i < 10; i += 1) {
      voiceBytes[145 + i] = name.charCodeAt(i);
      nameKeys[`d${145 + i}`] = name.charCodeAt(i);
    }
    const bank = userVoices.map((v) => v.slice());
    bank[slot] = voiceBytes;
    const bytes = buildDx7Bank(bank);
    try {
      const res = await fetch("/api/files/upload?volume=flash&dir=FM8&name=user.syx", { method: "POST", body: bytes });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
    } catch (err) {
      info.textContent = `Save failed: ${err.message}`;
      return;
    }
    Object.assign(state.keys, nameKeys);
    state.origin = `${FM8_USER_BANK}#${slot}`;
    state.modified = false;
    savePanel.classList.add("hidden");
    refreshAll();
    host.onKeys(nameKeys);
    info.textContent = `Saved as ${String(slot + 1).padStart(2, "0")} ${name.trim()} in user bank.`;
    fillBankSelect(await listFm8Banks());
    showBank(bytes, FM8_USER_BANK, null);
    bankSelect.value = FM8_USER_BANK;
    voiceSelect.value = String(slot);
    lastVoice = String(slot);
  });
}
