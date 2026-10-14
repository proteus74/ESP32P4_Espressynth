// FM8 widget helpers: the patch model, DX7 SysEx import and the Dexed-style editor dialog.
//
// The patch is exactly one DX7 voice in the "unpacked" 155-byte layout that the msfa engine
// (components/msfa, as used by Dexed/MicroDexed) runs on the device. Keys are flat numbers so that
// preset save/load and live updates share one representation:
//   d0..d154  voice bytes   ops  operator on/off mask (bit n = operator n+1)   tune / gain
// Operator blocks run 6 .. 1: operator n starts at byte (6 - n) * 21.
//   +0..3 R1-4  +4..7 L1-4  +8 BP  +9 LD  +10 RD  +11 LC  +12 RC  +13 RS  +14 AMS  +15 KVS
//   +16 OL  +17 mode(0 ratio/1 fixed)  +18 FC  +19 FF  +20 DET
// 126..133 PR1-4 PL1-4, 134 ALG(0-31), 135 FB, 136 OKS, 137 LFS, 138 LFD, 139 LPMD, 140 LAMD,
// 141 LKS, 142 LFW, 143 LPMS, 144 TRNSP(24 = 0), 145..154 name.

export const FM8_OPS = 6;
export const FM8_VOICE_BYTES = 155;

const OP_FIELDS = [
  [18, "Coarse", 0, 31],
  [19, "Fine", 0, 99],
  [20, "Det", 0, 14],
  [16, "Level", 0, 99],
  [15, "KVel", 0, 7],
  [14, "AMS", 0, 3],
  [13, "RS", 0, 7],
  [8, "BP", 0, 99],
  [9, "LD", 0, 99],
  [10, "RD", 0, 99],
  [11, "LC", 0, 3],
  [12, "RC", 0, 3],
  [0, "R1", 0, 99],
  [1, "R2", 0, 99],
  [2, "R3", 0, 99],
  [3, "R4", 0, 99],
  [4, "L1", 0, 99],
  [5, "L2", 0, 99],
  [6, "L3", 0, 99],
  [7, "L4", 0, 99],
];

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
  [0xc1, 0x11, 0x14, 0x01, 0x11, 0x14], [0x41, 0x11, 0x94, 0x01, 0x11, 0x14],
  [0xc1, 0x14, 0x01, 0x14, 0x01, 0x14], [0x41, 0x94, 0x01, 0x14, 0x01, 0x14],
  [0xc1, 0x11, 0x05, 0x14, 0x01, 0x14], [0x01, 0x11, 0xc5, 0x14, 0x01, 0x14],
  [0x01, 0x11, 0x05, 0x14, 0xc1, 0x14], [0x01, 0x05, 0x14, 0xc1, 0x11, 0x14],
  [0xc1, 0x05, 0x14, 0x01, 0x11, 0x14], [0x01, 0x05, 0x05, 0xc1, 0x14, 0x14],
  [0xc1, 0x05, 0x05, 0x01, 0x14, 0x14], [0xc1, 0x05, 0x11, 0x14, 0x01, 0x14],
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
  title.textContent = fm8PatchName(state.keys);
  const alg = (Number(state.keys.d134) || 0) + 1;
  const sub = document.createElement("div");
  sub.className = "fm8-summary-sub";
  sub.textContent = `DX7 algorithm ${alg}`;
  container.append(title, sub);
  const carriers = new Set(dx7AlgorithmRouting(alg).carriers);
  const bars = document.createElement("div");
  bars.className = "fm8-op-bars";
  for (let n = 1; n <= 6; n += 1) {
    const level = Number(state.keys[`d${opByteIndex(n, 16)}`]) || 0;
    const col = document.createElement("div");
    col.className = `fm8-op-bar${carriers.has(n) ? " carrier" : ""}`;
    col.title = `Op ${n}: level ${level}${carriers.has(n) ? " (carrier)" : ""}`;
    const fill = document.createElement("div");
    fill.style.height = `${Math.max(2, (level / 99) * 100)}%`;
    const label = document.createElement("span");
    label.textContent = String(n);
    col.append(fill, label);
    bars.appendChild(col);
  }
  container.appendChild(bars);
}

// Dexed-style editor + SysEx loader. `host`: { state, onKeys(changedKeys), onClose() }.
export function openFm8Dialog(host) {
  const { state } = host;
  const overlay = document.createElement("div");
  overlay.className = "modal-overlay fm8-dialog-overlay";
  const box = document.createElement("div");
  box.className = "modal fm8-dialog";
  overlay.appendChild(box);

  const head = document.createElement("div");
  head.className = "fm8-dialog-head";
  const h = document.createElement("h3");
  const closeBtn = document.createElement("button");
  closeBtn.type = "button";
  closeBtn.className = "btn-secondary";
  closeBtn.textContent = "Close";
  head.append(h, closeBtn);
  const refreshTitle = () => {
    h.textContent = `FM8 - ${fm8PatchName(state.keys)}`;
  };
  refreshTitle();

  // SysEx loader
  const loader = document.createElement("div");
  loader.className = "fm8-loader";
  // No accept= filter: unknown extensions like .syx plus a MIME type make some Windows
  // browsers refuse to open the native file dialog at all. Files can also be dropped on the dialog.
  const file = document.createElement("input");
  file.type = "file";
  file.className = "fm8-file";
  const voiceSelect = document.createElement("select");
  voiceSelect.disabled = true;
  const info = document.createElement("span");
  info.className = "fm8-info";
  info.textContent = "Load a DX7 .syx file (single voice or 32-voice bank) - or drop it onto this window.";
  loader.append(file, voiceSelect, info);

  const inputs = new Map(); // key -> {el, min, max}
  let pending = {};
  let flushTimer = null;
  const queue = (key, value) => {
    state.keys[key] = value;
    pending[key] = value;
    clearTimeout(flushTimer);
    flushTimer = setTimeout(() => {
      const keys = pending;
      pending = {};
      host.onKeys(keys);
      refreshTitle();
      refreshAlgorithm();
    }, 120);
  };
  const addInput = (parent, key, label, min, max, opts = {}) => {
    const wrap = document.createElement("label");
    wrap.className = "fm8-field";
    const text = document.createElement("span");
    text.textContent = label;
    let el;
    if (opts.options) {
      el = document.createElement("select");
      opts.options.forEach((o, i) => el.add(new Option(o, String(i + (opts.base || 0)))));
    } else {
      el = document.createElement("input");
      el.type = "number";
      el.min = String(min);
      el.max = String(max);
      el.step = "1";
    }
    el.value = String(state.keys[key]);
    el.addEventListener("change", () => {
      const v = Math.min(max, Math.max(min, Math.round(Number(el.value)) || 0));
      el.value = String(v);
      queue(key, v);
    });
    wrap.append(text, el);
    parent.appendChild(wrap);
    inputs.set(key, { el, min, max });
  };
  const section = (title) => {
    const s = document.createElement("section");
    s.className = "fm8-section";
    const t = document.createElement("h4");
    t.textContent = title;
    s.appendChild(t);
    return s;
  };

  // Global section: algorithm, feedback, transpose, LFO, pitch EG, tune/gain.
  const algText = document.createElement("div");
  algText.className = "fm8-alg-text";
  const refreshAlgorithm = () => {
    algText.textContent = `Algorithm ${(Number(state.keys.d134) || 0) + 1}: ${algorithmText((Number(state.keys.d134) || 0) + 1)}`;
  };
  refreshAlgorithm();

  const global = section("Voice");
  const globalGrid = document.createElement("div");
  globalGrid.className = "fm8-grid";
  addInput(globalGrid, "d134", "Algorithm (1-32)", 0, 31, { options: Array.from({ length: 32 }, (_, i) => String(i + 1)) });
  addInput(globalGrid, "d135", "Feedback", 0, 7);
  addInput(globalGrid, "d144", "Transpose (24 = 0)", 0, 48);
  addInput(globalGrid, "d136", "Osc key sync", 0, 1);
  addInput(globalGrid, "tune", "Tune (V, 5 = 0)", 0, 10);
  addInput(globalGrid, "gain", "Gain (V)", 0, 10);
  global.append(globalGrid, algText);

  const lfo = section("LFO");
  const lfoGrid = document.createElement("div");
  lfoGrid.className = "fm8-grid";
  addInput(lfoGrid, "d142", "Wave", 0, 5, { options: LFO_WAVES });
  addInput(lfoGrid, "d137", "Speed", 0, 99);
  addInput(lfoGrid, "d138", "Delay", 0, 99);
  addInput(lfoGrid, "d139", "Pitch mod depth", 0, 99);
  addInput(lfoGrid, "d140", "Amp mod depth", 0, 99);
  addInput(lfoGrid, "d143", "Pitch mod sens", 0, 7);
  addInput(lfoGrid, "d141", "Key sync", 0, 1);
  lfo.appendChild(lfoGrid);

  const peg = section("Pitch EG");
  const pegGrid = document.createElement("div");
  pegGrid.className = "fm8-grid";
  ["R1", "R2", "R3", "R4"].forEach((l, i) => addInput(pegGrid, `d${126 + i}`, l, 0, 99));
  ["L1", "L2", "L3", "L4"].forEach((l, i) => addInput(pegGrid, `d${130 + i}`, l, 0, 99));
  peg.appendChild(pegGrid);

  // Six operator cards.
  const opsWrap = document.createElement("div");
  opsWrap.className = "fm8-ops";
  for (let n = 1; n <= 6; n += 1) {
    const card = section(`Operator ${n}`);
    const grid = document.createElement("div");
    grid.className = "fm8-grid";
    addInput(grid, `d${opByteIndex(n, 17)}`, "Mode", 0, 1, { options: ["Ratio", "Fixed"] });
    OP_FIELDS.forEach(([off, label, min, max]) => {
      const key = `d${opByteIndex(n, off)}`;
      if (label === "LC" || label === "RC") addInput(grid, key, label, min, max, { options: CURVES });
      else addInput(grid, key, label, min, max);
    });
    // Operator on/off switch (bit n-1 of the "ops" byte).
    const sw = document.createElement("label");
    sw.className = "fm8-field";
    const swText = document.createElement("span");
    swText.textContent = "On";
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.checked = (Number(state.keys.ops) & (1 << (n - 1))) !== 0;
    cb.addEventListener("change", () => {
      const mask = (Number(state.keys.ops) & ~(1 << (n - 1))) | (cb.checked ? 1 << (n - 1) : 0);
      queue("ops", mask);
    });
    sw.append(swText, cb);
    grid.appendChild(sw);
    inputs.set(`__sw${n}`, { el: cb, sw: n });
    card.appendChild(grid);
    opsWrap.appendChild(card);
  }

  const hint = document.createElement("p");
  hint.className = "modal-hint";
  hint.textContent =
    "A DX7 voice, run by the msfa engine (the core of Dexed/MicroDexed). R = rates, L = levels, " +
    "BP/LD/RD/LC/RC = keyboard level scaling, RS = rate scaling, KVel = velocity sensitivity, " +
    "Det 7 = centre. Fixed mode: Coarse 0-31 selects 1/10/100/1000 Hz decades, Fine the step.";

  box.append(head, loader, global, opsWrap, lfo, peg, hint);
  document.body.appendChild(overlay);

  const refreshInputs = () => {
    inputs.forEach((entry, key) => {
      if (entry.sw) entry.el.checked = (Number(state.keys.ops) & (1 << (entry.sw - 1))) !== 0;
      else entry.el.value = String(state.keys[key]);
    });
    refreshTitle();
    refreshAlgorithm();
  };
  const close = () => {
    clearTimeout(flushTimer);
    if (Object.keys(pending).length) host.onKeys(pending);
    overlay.remove();
    host.onClose();
  };
  closeBtn.addEventListener("click", close);
  overlay.addEventListener("mousedown", (e) => {
    if (e.target === overlay) close();
  });

  let voices = [];
  const applyVoice = (index) => {
    const voice = voices[index];
    if (!voice) return;
    const keys = voiceToKeys(voice);
    Object.assign(state.keys, keys);
    refreshInputs();
    const alg = keys.d134 + 1;
    info.textContent = `${voice.name} - algorithm ${alg}, feedback ${keys.d135}`;
    host.onKeys(keys);
  };
  const loadFile = async (f) => {
    if (!f) return;
    try {
      voices = parseDx7Sysex(new Uint8Array(await f.arrayBuffer()));
      voiceSelect.textContent = "";
      voices.forEach((v, i) => voiceSelect.add(new Option(`${String(i + 1).padStart(2, "0")}  ${v.name || "(unnamed)"}`, String(i))));
      voiceSelect.disabled = voices.length < 2;
      info.textContent = `${voices.length} voice${voices.length > 1 ? "s" : ""} loaded.`;
      applyVoice(0);
    } catch (err) {
      info.textContent = `SysEx error: ${err.message}`;
    }
  };
  file.addEventListener("change", () => loadFile(file.files && file.files[0]));
  ["dragenter", "dragover"].forEach((type) =>
    overlay.addEventListener(type, (e) => {
      e.preventDefault();
      box.classList.add("fm8-drop");
    })
  );
  overlay.addEventListener("dragleave", (e) => {
    if (e.target === overlay) box.classList.remove("fm8-drop");
  });
  overlay.addEventListener("drop", (e) => {
    e.preventDefault();
    box.classList.remove("fm8-drop");
    loadFile(e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0]);
  });
  voiceSelect.addEventListener("change", () => applyVoice(Number(voiceSelect.value)));
}
