// Wavetable core shared by the Wavetable widget:
//  - main.js (editor popup, mini preview, control-rate LFO tick),
//  - audio-engine.js (ScriptProcessor fallback when AudioWorklet is not
//    available - e.g. the device UI served over plain http:// from a LAN IP,
//    which is not a secure context),
//  - wavetable-worklet.js (AudioWorkletProcessor).
// Plain ES module without DOM access so the AudioWorkletGlobalScope can
// import it too. Mirrors main/audio/engine/Wavetable.h 1:1 (table size,
// morph/sample interpolation, preset shapes and the base64 int16 storage
// format) so the preview and the hardware sound the same.

export const WAVETABLE_SIZE = 1024;
export const WAVETABLE_MAX_TABLES = 16;
export const WAVETABLE_MIN_TABLES = 1;
export const WAVETABLE_DEFAULT_TABLES = 4;
export const WAVETABLE_MAX_HARMONICS = 32;
export const WAVETABLE_PRESETS = ["sine", "triangle", "sawtooth", "square"];

function clamp(v, lo, hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

// Same formulas as wavetable::fillDefault() in Wavetable.h.
export function presetValue(kind, phase) {
  switch (kind) {
    case "triangle":
      return 1 - 4 * Math.abs(phase - 0.25 - Math.floor(phase + 0.25));
    case "sawtooth":
      return 2 * phase - 1;
    case "square":
      return phase < 0.5 ? 1 : -1;
    default:
      return Math.sin(2 * Math.PI * phase);
  }
}

export function createPresetTable(kind) {
  const table = new Float32Array(WAVETABLE_SIZE);
  for (let i = 0; i < WAVETABLE_SIZE; i += 1) table[i] = presetValue(kind, i / WAVETABLE_SIZE);
  return table;
}

export function createDefaultBank() {
  return WAVETABLE_PRESETS.map((kind) => createPresetTable(kind));
}

// Additive synthesis: harmonics[n-1] = amplitude (-1..1) of the n-th sine
// partial. Normalized to a +/-1 peak (a silent table stays silent).
export function createAdditiveTable(harmonics) {
  const table = new Float32Array(WAVETABLE_SIZE);
  harmonics.forEach((amp, index) => {
    if (!amp) return;
    const n = index + 1;
    for (let i = 0; i < WAVETABLE_SIZE; i += 1) {
      table[i] += amp * Math.sin((2 * Math.PI * n * i) / WAVETABLE_SIZE);
    }
  });
  return normalizeTable(table);
}

export function normalizeTable(table) {
  let peak = 0;
  for (let i = 0; i < table.length; i += 1) peak = Math.max(peak, Math.abs(table[i]));
  if (peak > 1e-9) {
    for (let i = 0; i < table.length; i += 1) table[i] /= peak;
  }
  return table;
}

// Linear-interpolated read at a fractional sample position (wraps).
function readAt(table, pos) {
  const n = table.length;
  const wrapped = ((pos % n) + n) % n;
  const i0 = Math.floor(wrapped);
  const frac = wrapped - i0;
  const a = table[i0];
  const b = table[(i0 + 1) % n];
  return a + (b - a) * frac;
}

// Every zero crossing of the looped table as { pos, up }, pos in [0, n]
// (the seam segment last -> first sample included, so a crossing sitting
// exactly on the loop point is found at pos n).
function zeroCrossings(table) {
  const n = table.length;
  const list = [];
  for (let i = 0; i < n; i += 1) {
    const a = table[i];
    const b = table[(i + 1) % n];
    if (a === 0 && i === 0) list.push({ pos: 0, up: b > 0 });
    if ((a < 0 && b >= 0) || (a > 0 && b <= 0)) list.push({ pos: i + a / (a - b), up: b > a });
  }
  return list;
}

// "Snap to Zero": searches the zero crossing nearest to the start and the
// matching crossing (same slope direction when possible) nearest to the end,
// cuts the table to the span between them and stretches that span back to
// the full length. Start and end then both sit at 0 V with a matching
// slope, so the loop seam (last -> first sample) no longer clicks. A table
// that already starts on a crossing and loops cleanly comes out unchanged;
// one without any crossing (pure DC) is left untouched (returns false).
export function snapTableToZero(table) {
  const n = table.length;
  const crossings = zeroCrossings(table);
  if (crossings.length === 0) return false;
  const start = crossings[0];
  const after = crossings.filter((c) => c.pos > start.pos + n / 4);
  const sameDir = after.filter((c) => c.up === start.up);
  const end = (sameDir.length > 0 ? sameDir : after).pop();
  const source = Float32Array.from(table);
  if (!end) {
    // Single crossing: rotate the loop so it starts there instead.
    for (let i = 0; i < n; i += 1) table[i] = readAt(source, start.pos + i);
  } else {
    const span = end.pos - start.pos;
    for (let i = 0; i < n; i += 1) table[i] = readAt(source, start.pos + (span * i) / n);
  }
  table[0] = 0;
  return true;
}

// Bank read, identical to wavetable::sample(): phase 0..1 (wraps), morph
// 0..1 across the whole bank, linear in both dimensions.
export function sampleBank(tables, phase, morph) {
  const count = tables.length;
  if (count === 0) return 0;
  const p = phase - Math.floor(phase);
  const pos = p * WAVETABLE_SIZE;
  const i0 = Math.floor(pos) & (WAVETABLE_SIZE - 1);
  const i1 = (i0 + 1) & (WAVETABLE_SIZE - 1);
  const frac = pos - Math.floor(pos);
  if (count === 1) {
    const t = tables[0];
    return t[i0] + (t[i1] - t[i0]) * frac;
  }
  const m = clamp(morph, 0, 1) * (count - 1);
  let a = Math.floor(m);
  if (a >= count - 1) a = count - 2;
  const mix = m - a;
  const ta = tables[a];
  const tb = tables[a + 1];
  const va = ta[i0] + (ta[i1] - ta[i0]) * frac;
  const vb = tb[i0] + (tb[i1] - tb[i0]) * frac;
  return va + (vb - va) * mix;
}

// --- Preset storage: base64 of little-endian int16 (full scale 32767) ---

const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

export function encodeTable(table) {
  const bytes = new Uint8Array(WAVETABLE_SIZE * 2);
  for (let i = 0; i < WAVETABLE_SIZE; i += 1) {
    const v = Math.round(clamp(Number(table[i]) || 0, -1, 1) * 32767);
    bytes[i * 2] = v & 0xff;
    bytes[i * 2 + 1] = (v >> 8) & 0xff;
  }
  let out = "";
  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i];
    const b1 = i + 1 < bytes.length ? bytes[i + 1] : 0;
    const b2 = i + 2 < bytes.length ? bytes[i + 2] : 0;
    out += B64[b0 >> 2] + B64[((b0 & 3) << 4) | (b1 >> 4)];
    out += i + 1 < bytes.length ? B64[((b1 & 15) << 2) | (b2 >> 6)] : "=";
    out += i + 2 < bytes.length ? B64[b2 & 63] : "=";
  }
  return out;
}

export function decodeTable(value) {
  if (Array.isArray(value)) {
    if (value.length !== WAVETABLE_SIZE) return null;
    return Float32Array.from(value, (v) => clamp(Number(v) || 0, -1, 1));
  }
  if (typeof value !== "string") return null;
  const table = new Float32Array(WAVETABLE_SIZE);
  let accum = 0;
  let bits = 0;
  let low = -1;
  let sample = 0;
  for (let i = 0; i < value.length && sample < WAVETABLE_SIZE; i += 1) {
    const ch = value[i];
    if (ch === "=") break;
    const v = B64.indexOf(ch === "-" ? "+" : ch === "_" ? "/" : ch);
    if (v < 0) continue;
    accum = ((accum << 6) | v) & 0xffffff;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      const byte = (accum >> bits) & 0xff;
      if (low < 0) {
        low = byte;
      } else {
        let raw = low | (byte << 8);
        if (raw >= 0x8000) raw -= 0x10000;
        table[sample] = Math.max(-1, raw / 32767);
        sample += 1;
        low = -1;
      }
    }
  }
  return sample === WAVETABLE_SIZE ? table : null;
}

export function encodeBank(tables) {
  return tables.map((t) => encodeTable(t));
}

export function decodeBank(list) {
  if (!Array.isArray(list)) return null;
  const tables = list.map((entry) => decodeTable(entry)).filter(Boolean).slice(0, WAVETABLE_MAX_TABLES);
  return tables.length > 0 ? tables : null;
}

// --- Audio-rate oscillator used by both the worklet and the fallback -----

export class WavetableLfoCore {
  constructor(sampleRate) {
    this.sampleRate = sampleRate;
    this.phase = 0;
    this.tables = createDefaultBank();
  }

  setTables(tables) {
    if (Array.isArray(tables) && tables.length > 0) this.tables = tables.map((t) => Float32Array.from(t));
  }

  reset() {
    this.phase = 0;
  }

  // out: Float32Array. rate: Hz (number or per-sample array). morphVolts:
  // 0-10 V (number or per-sample array). phaseOffset: cycles.
  process(out, rate, morphVolts, phaseOffset) {
    const invSr = 1 / this.sampleRate;
    const rateIsArray = typeof rate !== "number";
    const morphIsArray = typeof morphVolts !== "number";
    for (let i = 0; i < out.length; i += 1) {
      const hz = rateIsArray ? rate[rate.length > 1 ? i : 0] : rate;
      const mv = morphIsArray ? morphVolts[morphVolts.length > 1 ? i : 0] : morphVolts;
      out[i] = sampleBank(this.tables, this.phase + phaseOffset, mv / 10);
      this.phase += Math.max(0, hz) * invSr;
      if (this.phase >= 1) this.phase -= Math.floor(this.phase);
    }
  }
}
