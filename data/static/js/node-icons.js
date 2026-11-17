// Line icons for the Add Node picker, drawn in the same style as the
// oscillator waveform buttons (WAVE_TYPE_BUTTONS in main.js): one colored
// stroke glyph on a dark tile, 100x100 viewBox. One distinct glyph per node
// type - no shared emoji between e.g. Hammond/MIDI-In or the Link variants.

const ATTRS = 'viewBox="0 0 100 100" fill="none" stroke="currentColor" stroke-width="8" stroke-linecap="round" stroke-linejoin="round"';
const svg = (body) => `<svg ${ATTRS}>${body}</svg>`;
const path = (d, extra = "") => `<path d="${d}"${extra ? ` ${extra}` : ""}/>`;
const dot = (cx, cy, r = 6) => `<circle cx="${cx}" cy="${cy}" r="${r}" fill="currentColor" stroke="none"/>`;
const ring = (cx, cy, r) => `<circle cx="${cx}" cy="${cy}" r="${r}"/>`;
const faint = 'opacity=".4"';

// Audio codec chip with a small glyph inside (ES8311 / WM8960 / NS4168 outputs).
const chip = (inner) => svg(
  `<rect x="24" y="24" width="52" height="52" rx="6"/>` +
  path("M36,24 L36,12 M50,24 L50,12 M64,24 L64,12 M36,76 L36,88 M50,76 L50,88 M64,76 L64,88 M24,40 L12,40 M24,60 L12,60 M76,40 L88,40 M76,60 L88,60", 'stroke-width="6"') +
  inner,
);

// n parallel wires into (in) or out of (out) a link box.
const linkLines = (n, dir) => {
  const box = dir === "in" ? `<rect x="58" y="12" width="32" height="76" rx="7"/>` : `<rect x="10" y="12" width="32" height="76" rx="7"/>`;
  const step = 60 / (n - 1);
  let d = "";
  for (let i = 0; i < n; i += 1) {
    const y = Math.round(20 + i * step);
    d += dir === "in" ? `M8,${y} L46,${y} ` : `M54,${y} L92,${y} `;
  }
  return svg(box + path(d.trim(), `stroke-width="${n > 4 ? 4 : 6}"`));
};

export const NODE_ICONS = {
  // ---- MIDI ----
  "midi-in": { color: "#8b5cf6", svg: svg(ring(50, 50, 38) + dot(28, 50) + dot(72, 50) + dot(34, 32) + dot(66, 32) + dot(50, 25) + path("M42,86 L58,86")) },
  "midi-split": { color: "#a855f7", svg: svg(path("M8,50 L40,50 L88,22 M40,50 L88,78 M74,16 L88,22 L82,36 M82,64 L88,78 L74,84")) },
  "midi-filter": { color: "#6366f1", svg: svg(path("M12,16 L88,16 L58,52 L58,86 L42,76 L42,52 Z")) },

  // ---- Generator ----
  "fm-oscillator": { color: "#06b6d4", svg: svg(path("M6,50 Q28,8 50,50 T94,50", 'stroke-width="9"')) },
  // FM8: three operator boxes with modulation lines
  fm8: { color: "#f59e0b", svg: svg(`<rect x="8" y="8" width="26" height="26" rx="5"/><rect x="66" y="8" width="26" height="26" rx="5"/><rect x="37" y="62" width="26" height="26" rx="5"/>` + path("M34,21 L66,21 M21,34 L21,48 L50,48 L50,62 M79,34 L79,48 L50,48")) },
  oscillator: { color: "#06b6d4", svg: svg(path("M6,50 Q28,8 50,50 T94,50", 'stroke-width="9"')) },
  hammond: { color: "#f59e0b", svg: svg(path("M10,12 L90,12") + path("M18,12 L18,58 M34,12 L34,84 M50,12 L50,44 M66,12 L66,72 M82,12 L82,52", 'stroke-width="9"')) },
  "wave-player": { color: "#22c55e", svg: svg(path("M12,42 L12,58 M24,28 L24,72 M36,16 L36,84 M48,34 L48,66 M60,22 L60,78 M72,38 L72,62 M84,46 L84,54")) },
  sampler: { color: "#22c55e", svg: svg(path("M10,14 L10,86 M90,14 L90,86") + path("M26,40 L26,60 M38,26 L38,74 M50,18 L50,82 M62,32 L62,68 M74,44 L74,56")) },
  "soundfont-player": { color: "#ec4899", svg: svg(path("M40,76 L40,18 L82,10 L82,66") + dot(30, 76, 11) + dot(72, 66, 11)) },

  // ---- Modulator ----
  lfo: { color: "#14b8a6", svg: svg(path("M8,74 C30,74 30,22 50,22 C70,22 70,74 92,74") + dot(50, 22, 9) + path("M8,88 L92,88", `stroke-width="4" ${faint}`)) },
  wavetable: { color: "#8b5cf6", svg: svg(path("M8,78 Q22,56 36,78 T64,78", `stroke-width="6" ${faint}`) + path("M22,58 Q36,36 50,58 T78,58", 'stroke-width="6" opacity=".7"') + path("M36,38 Q50,16 64,38 T92,38", 'stroke-width="7"')) },
  adsr: { color: "#f59e0b", svg: svg(path("M8,86 L26,14 L44,46 L70,46 L92,86")) },
  "dx-envelope": { color: "#f97316", svg: svg(path("M8,86 L20,30 L36,56 L52,20 L72,62 L92,86") + dot(20, 30, 5) + dot(36, 56, 5) + dot(52, 20, 5) + dot(72, 62, 5)) },
  detune: { color: "#0ea5e9", svg: svg(path("M6,36 Q26,4 46,36 T86,36", 'stroke-width="7"') + path("M14,66 Q34,34 54,66 T94,66", 'stroke-width="7" opacity=".6"')) },
  clock: { color: "#eab308", svg: svg(ring(50, 50, 38) + path("M50,50 L50,24 M50,50 L68,62")) },
  "step-arpeggiator": { color: "#22c55e", svg: svg(path("M8,84 L28,84 L28,64 L48,64 L48,44 L68,44 L68,24 L92,24")) },
  "drum-sequencer": {
    color: "#ef4444",
    svg: svg([[0, 0, 1], [1, 0, 0], [2, 0, 1], [3, 0, 0], [0, 1, 0], [1, 1, 1], [2, 1, 0], [3, 1, 1], [0, 2, 1], [1, 2, 1], [2, 2, 0], [3, 2, 1]]
      .map(([c, r, on]) => `<rect x="${10 + c * 21}" y="${16 + r * 24}" width="16" height="18" rx="3"${on ? ' fill="currentColor" stroke="none"' : ' stroke-width="4"'}/>`).join("")),
  },
  monoflop: { color: "#3b82f6", svg: svg(path("M8,82 L28,82 L28,24 L68,24 L68,82 L92,82") + path("M28,94 L28,90", 'stroke-width="6"')) },
  "sample-hold": { color: "#a855f7", svg: svg(path("M8,50 Q22,14 40,46 T70,62 T92,40", `stroke-width="5" ${faint}`) + path("M8,56 L26,56 L26,30 L46,30 L46,58 L66,58 L66,64 L80,64 L80,40 L92,40")) },
  "burst-generator": { color: "#f43f5e", svg: svg(path("M8,80 L18,80 L18,28 L26,28 L26,80 L36,80 L36,28 L44,28 L44,80 L54,80 L54,28 L62,28 L62,80 L92,80", 'stroke-width="7"')) },

  // ---- Control ----
  knob: { color: "#06b6d4", svg: svg(path("M50,10 L50,90", `stroke-width="6" ${faint}`) + `<rect x="30" y="50" width="40" height="18" rx="5" fill="currentColor" stroke="none"/>` + path("M50,10 L50,50")) },
  button: { color: "#ef4444", svg: svg(ring(50, 50, 38) + dot(50, 50, 18)) },
  "up-down-button": { color: "#3b82f6", svg: svg(path("M28,42 L50,16 L72,42 Z M28,58 L50,84 L72,58 Z", 'fill="currentColor"')) },
  switch: { color: "#22c55e", svg: svg(path("M8,28 L26,28 M8,72 L26,72 M34,30 L66,50 L92,50") + ring(30, 28, 6) + ring(30, 72, 6) + dot(66, 50, 6)) },
  multiplexer: { color: "#f59e0b", svg: svg(path("M28,12 L72,28 L72,72 L28,88 Z M6,32 L28,32 M6,50 L28,50 M6,68 L28,68 M72,50 L94,50")) },
  "output-switch": { color: "#14b8a6", svg: svg(path("M8,50 L34,50 L66,30 M74,28 L92,28 M74,72 L92,72") + dot(34, 50, 6) + ring(70, 28, 6) + ring(70, 72, 6)) },
  "output-multiplexer": { color: "#f97316", svg: svg(path("M28,28 L72,12 L72,88 L28,72 Z M6,50 L28,50 M72,32 L94,32 M72,50 L94,50 M72,68 L94,68")) },
  const: { color: "#84cc16", svg: svg(path("M8,84 L92,84 M8,84 L8,16", `stroke-width="5" ${faint}`) + path("M14,40 L90,40")) },
  "master-volume": { color: "#10b981", svg: svg(path("M12,40 L28,40 L50,20 L50,80 L28,60 L12,60 Z") + path("M64,36 Q72,50 64,64 M74,24 Q90,50 74,76")) },

  // ---- Math ----
  adder: { color: "#22c55e", svg: svg(path("M50,16 L50,84 M16,50 L84,50", 'stroke-width="10"')) },
  divide: { color: "#3b82f6", svg: svg(path("M16,50 L84,50", 'stroke-width="10"') + dot(50, 24, 8) + dot(50, 76, 8)) },
  "frequency-divider": { color: "#06b6d4", svg: svg(path("M8,40 L8,14 L24,14 L24,40 L40,40 L40,14 L56,14 L56,40 L72,40 L72,14 L88,14 L88,40", 'stroke-width="6"') + path("M8,88 L8,60 L40,60 L40,88 L72,88 L72,60 L92,60", 'stroke-width="6"')) },
  counter: { color: "#eab308", svg: svg(path("M20,18 L20,82 M36,18 L36,82 M52,18 L52,82 M68,18 L68,82 M10,72 L80,28")) },
  multiplier: { color: "#a855f7", svg: svg(path("M24,24 L76,76 M76,24 L24,76", 'stroke-width="10"')) },
  abs: { color: "#0ea5e9", svg: svg(path("M12,14 L12,86 M88,14 L88,86") + path("M28,28 L50,74 L72,28")) },
  "value-mapper": { color: "#14b8a6", svg: svg(path("M14,12 L14,86 L90,86", `stroke-width="5" ${faint}`) + path("M14,80 Q42,80 52,50 T88,18")) },
  "logic-and": { color: "#f97316", svg: svg(path("M16,18 L50,18 A32,32 0 0 1 50,82 L16,82 Z")) },
  "logic-or": { color: "#ec4899", svg: svg(path("M14,18 Q34,50 14,82 Q62,82 90,50 Q62,18 14,18 Z")) },
  "logic-xor": { color: "#8b5cf6", svg: svg(path("M24,18 Q42,50 24,82 Q66,82 92,50 Q66,18 24,18 Z M8,18 Q26,50 8,82")) },
  "logic-not": { color: "#ef4444", svg: svg(path("M14,16 L14,84 L68,50 Z") + ring(80, 50, 9)) },

  // ---- Filter ----
  filter: { color: "#3b82f6", svg: svg(path("M8,40 L46,40 Q60,40 64,24 Q70,52 92,88")) },
  "curtis-filter": { color: "#f43f5e", svg: svg(path("M8,48 L42,48 Q54,48 58,12 Q62,50 70,70 L80,90") + path("M8,90 L92,90", `stroke-width="4" ${faint}`)) },
  "comb-filter": { color: "#22c55e", svg: svg(path("M8,28 Q16,84 24,28 Q32,84 40,28 Q48,84 56,28 Q64,84 72,28 Q80,84 88,28", 'stroke-width="6"')) },
  "esp-eq": { color: "#eab308", svg: svg(path("M24,12 L24,88 M50,12 L50,88 M76,12 L76,88", `stroke-width="5" ${faint}`) + dot(24, 62, 10) + dot(50, 32, 10) + dot(76, 52, 10)) },
  "graphic-eq": { color: "#06b6d4", svg: svg(path("M12,86 L12,60 M23,86 L23,42 M34,86 L34,28 M45,86 L45,50 M56,86 L56,36 M67,86 L67,22 M78,86 L78,46 M89,86 L89,62", 'stroke-width="7"')) },

  // ---- Effects ----
  bitcrusher: { color: "#84cc16", svg: svg(path("M8,50 L8,30 L20,30 L20,16 L32,16 L32,30 L44,30 L44,50 L56,50 L56,70 L68,70 L68,84 L80,84 L80,70 L92,70")) },
  "wave-folder": { color: "#a855f7", svg: svg(path("M8,50 Q14,16 24,22 Q32,28 38,16 Q46,6 50,50 Q54,94 62,84 Q68,72 76,78 Q86,84 92,50")) },
  overdrive: { color: "#f97316", svg: svg(path("M8,50 Q14,22 22,22 L40,22 Q46,22 50,50 Q54,78 60,78 L78,78 Q86,78 92,50")) },
  "envelope-follower": { color: "#10b981", svg: svg(path("M8,50 L14,36 L20,62 L26,24 L32,74 L38,20 L44,78 L50,26 L56,70 L62,34 L68,62 L74,42 L80,56 L86,46 L92,50", `stroke-width="4" ${faint}`) + path("M8,44 Q30,4 52,18 T92,44")) },
  pan: { color: "#0ea5e9", svg: svg(path("M12,50 L88,50 M26,34 L12,50 L26,66 M74,34 L88,50 L74,66") + dot(50, 50, 8)) },
  chorus: { color: "#6366f1", svg: svg(path("M8,36 Q30,6 50,36 T92,36") + path("M8,52 Q30,22 50,52 T92,52", 'opacity=".65"') + path("M8,68 Q30,38 50,68 T92,68", `${faint}`)) },
  delay: { color: "#14b8a6", svg: svg(path("M14,88 L14,12 M36,88 L36,32 M58,88 L58,52 M80,88 L80,70", 'stroke-width="9"')) },
  "reverb-light": { color: "#8b5cf6", svg: svg(dot(16, 50, 8) + path("M32,32 Q42,50 32,68 M48,20 Q64,50 48,80", 'stroke-width="7"') + path("M64,10 Q88,50 64,90", `stroke-width="7" ${faint}`)) },

  // ---- Mixing / Output ----
  gain: { color: "#22c55e", svg: svg(path("M18,14 L18,86 L84,50 Z") + path("M50,4 L50,30", 'stroke-width="6"')) },
  mixer: { color: "#f59e0b", svg: svg(path("M6,22 L40,44 M6,50 L38,50 M6,78 L40,56 M64,50 L94,50") + ring(52, 50, 12)) },
  "linked-input": { color: "#06b6d4", svg: svg(`<rect x="50" y="18" width="40" height="64" rx="8"/>` + path("M6,50 L64,50 M48,34 L64,50 L48,66")) },
  "linked-output": { color: "#ec4899", svg: svg(`<rect x="10" y="18" width="40" height="64" rx="8"/>` + path("M36,50 L94,50 M78,34 L94,50 L78,66")) },
  // Audio link pair (chain links + direction arrow)
  "audio-input-link": { color: "#10b981", svg: svg(`<rect x="34" y="18" width="40" height="26" rx="13" transform="rotate(-35 54 31)"/><rect x="48" y="50" width="40" height="26" rx="13" transform="rotate(-35 68 63)"/>` + path("M6,70 L30,70 M20,60 L30,70 L20,80", 'stroke-width="7"')) },
  "audio-output-link": { color: "#f59e0b", svg: svg(`<rect x="10" y="22" width="40" height="26" rx="13" transform="rotate(-35 30 35)"/><rect x="24" y="54" width="40" height="26" rx="13" transform="rotate(-35 44 67)"/>` + path("M70,30 L94,30 M84,20 L94,30 L84,40", 'stroke-width="7"')) },
  "linked-input-4": { color: "#0ea5e9", svg: linkLines(4, "in") },
  "linked-output-4": { color: "#f43f5e", svg: linkLines(4, "out") },
  "linked-input-8": { color: "#3b82f6", svg: linkLines(8, "in") },
  "linked-output-8": { color: "#a855f7", svg: linkLines(8, "out") },

  // ---- External audio ----
  "audio-out": { color: "#10b981", svg: svg(ring(40, 50, 28) + dot(40, 50, 9) + path("M72,50 L94,50 M84,40 L94,50 L84,60", 'stroke-width="7"')) },
  "usb-out": { color: "#3b82f6", svg: svg(path("M50,92 L50,14 M38,26 L50,10 L62,26 M50,64 L26,48 L26,36 M50,52 L74,38 L74,30", 'stroke-width="7"') + dot(26, 32, 7) + `<rect x="67" y="18" width="14" height="14" fill="currentColor" stroke="none"/>`) },
  "es8311-out": { color: "#14b8a6", svg: chip(path("M32,50 Q41,34 50,50 T68,50", 'stroke-width="6"')) },
  "wm8960-out": { color: "#f59e0b", svg: chip(path("M36,60 L36,50 Q36,34 50,34 Q64,34 64,50 L64,60", 'stroke-width="6"')) },
  "ns4168-out": { color: "#ef4444", svg: chip(path("M34,44 L42,44 L52,36 L52,64 L42,56 L34,56 Z M60,42 Q66,50 60,58", 'stroke-width="5"')) },
  "wm8960-in": { color: "#f59e0b", svg: chip(path("M36,40 Q36,30 50,30 Q64,30 64,40 L64,52 Q64,62 50,62 Q36,62 36,52 Z M30,58 Q30,72 50,72 Q70,72 70,58", 'stroke-width="5"')) },
  "mic-in": { color: "#ec4899", svg: svg(`<rect x="36" y="8" width="28" height="50" rx="14"/>` + path("M22,46 Q22,74 50,74 Q78,74 78,46 M50,74 L50,90 M36,90 L64,90")) },

  // ---- Misc ----
  annotation: { color: "#eab308", svg: svg(path("M16,12 L66,12 L84,30 L84,88 L16,88 Z M66,12 L66,30 L84,30") + path("M30,50 L70,50 M30,68 L60,68", 'stroke-width="6"')) },
  debug: { color: "#ef4444", svg: svg(`<ellipse cx="50" cy="56" rx="20" ry="28"/>` + path("M50,30 L50,84 M30,44 L14,36 M30,60 L12,60 M30,74 L14,84 M70,44 L86,36 M70,60 L88,60 M70,74 L86,84 M42,28 L34,14 M58,28 L66,14", 'stroke-width="6"')) },
  voltmeter: { color: "#f97316", svg: svg(path("M12,72 A40,40 0 0 1 88,72") + path("M50,72 L70,34") + dot(50, 72, 8)) },
  "vu-meter": { color: "#22c55e", svg: svg(path("M34,88 L34,30 M66,88 L66,52", 'stroke-width="20" stroke-linecap="butt" stroke-dasharray="9 5"')) },
  "vu-meter-horizontal": { color: "#84cc16", svg: svg(path("M10,34 L84,34 M10,66 L58,66", 'stroke-width="20" stroke-linecap="butt" stroke-dasharray="9 5"')) },
  oscilloscope: { color: "#06b6d4", svg: svg(`<rect x="8" y="14" width="84" height="72" rx="8"/>` + path("M18,50 Q29,22 40,50 T62,50 T84,50", 'stroke-width="6"')) },
  spectrum: { color: "#a855f7", svg: svg(path("M8,88 L92,88", `stroke-width="4" ${faint}`) + path("M8,78 Q18,18 30,40 Q40,58 50,30 Q62,8 72,56 Q82,82 92,76")) },

  // ---- External hardware ----
  "cv-in": { color: "#f59e0b", svg: svg(ring(66, 50, 24) + dot(66, 50, 8) + path("M6,50 L34,50 M22,38 L34,50 L22,62")) },
  "cv-out": { color: "#f97316", svg: svg(ring(34, 50, 24) + dot(34, 50, 8) + path("M66,50 L94,50 M82,38 L94,50 L82,62")) },
  "digital-in": { color: "#22c55e", svg: svg(path("M6,70 L18,70 L18,30 L38,30 L38,70 L50,70", 'stroke-width="7"') + path("M58,50 L80,50 M70,40 L80,50 L70,60 M92,18 L92,82", 'stroke-width="7"')) },
  "digital-out": { color: "#10b981", svg: svg(path("M8,18 L8,82 M14,50 L36,50 M26,40 L36,50 L26,60", 'stroke-width="7"') + path("M48,70 L60,70 L60,30 L80,30 L80,70 L94,70", 'stroke-width="7"')) },
  "rgb-led": {
    color: "#f43f5e",
    svg: svg(path("M32,62 L32,42 A18,18 0 0 1 68,42 L68,62 Z M24,62 L76,62 M42,62 L42,92 M58,62 L58,86") +
      path("M16,26 L26,34", 'stroke="#22c55e" stroke-width="6"') + path("M50,4 L50,14", 'stroke="#3b82f6" stroke-width="6"') + path("M84,26 L74,34", 'stroke="#eab308" stroke-width="6"')),
  },
};

const speaker = "M12,40 L28,40 L50,20 L50,80 L28,60 L12,60 Z";

// Header/status-bar icons (index.html's data-ui-icon placeholders), same tile style.
export const UI_ICONS = {
  volume: NODE_ICONS["master-volume"],
  tempo: { color: "#eab308", svg: svg(path("M34,88 L66,88 L56,12 L44,12 Z") + path("M50,72 L76,24", 'stroke-width="7"') + dot(76, 24, 8)) },
  "audio-off": { color: "#94a3b8", svg: svg(path(speaker) + path("M64,38 L86,62 M86,38 L64,62")) },
  "audio-on": { color: "#22c55e", svg: svg(path(speaker) + path("M64,36 Q72,50 64,64 M74,24 Q90,50 74,76")) },
  memory: { color: "#a855f7", svg: svg(`<rect x="8" y="26" width="84" height="38" rx="5"/>` + path("M24,38 L24,52 M42,38 L42,52 M58,38 L58,52 M76,38 L76,52", 'stroke-width="7"') + path("M18,64 L18,78 M34,64 L34,78 M50,64 L50,78 M66,64 L66,78 M82,64 L82,78", 'stroke-width="5"')) },
  cpu: { color: "#0ea5e9", svg: chip(`<rect x="38" y="38" width="24" height="24" rx="3" fill="currentColor" stroke="none"/>`) },
  battery: { color: "#22c55e", svg: svg(`<rect x="8" y="28" width="74" height="44" rx="7"/>` + path("M90,42 L90,58", 'stroke-width="9"') + `<rect x="18" y="38" width="38" height="24" rx="2" fill="currentColor" stroke="none"/>`) },
};

const tile = (icon, extraClass = "") =>
  `<span class="node-icon${extraClass}" style="--wave-color:${icon.color}" aria-hidden="true">${icon.svg}</span>`;

// Tile markup for one node type, or "" when no icon is defined (caller falls
// back to the widget's emoji).
export function nodeIconHtml(type) {
  const icon = NODE_ICONS[type];
  return icon ? tile(icon) : "";
}

export function uiIconHtml(name) {
  const icon = UI_ICONS[name];
  return icon ? tile(icon, " ui-icon") : "";
}
