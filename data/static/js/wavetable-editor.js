// Wavetable editor popup + mini preview for the Wavetable widget.
//
// openWavetableEditor() works on a private copy of the bank and only hands
// it back through the callbacks:
//   onPreview(tables) - after every edit and, throttled, while a stroke is
//                       still being drawn (live audition: browser engine and
//                       the device's running graph, no preset save),
//   onApply(tables)   - "Apply": the caller keeps the bank (it is written
//                       to flash with the next regular preset save),
//   onCancel()        - "Cancel"/Escape: the caller restores its own bank.
import {
  WAVETABLE_SIZE,
  WAVETABLE_MAX_TABLES,
  WAVETABLE_MIN_TABLES,
  WAVETABLE_MAX_HARMONICS,
  createPresetTable,
  createAdditiveTable,
  snapTableToZero,
  sampleBank,
} from "./wavetable-core.js?v=20260924-wavetable-1";

const PRESET_BUTTONS = [
  { id: "sine", label: "Sine", path: "M 6,50 Q 28,8 50,50 T 94,50" },
  { id: "sawtooth", label: "Saw", path: "M 10,84 L 90,16 L 90,84" },
  { id: "square", label: "Square", path: "M 10,80 L 10,20 L 50,20 L 50,80 L 90,80 L 90,20" },
  { id: "triangle", label: "Triangle", path: "M 8,50 L 29,16 L 71,84 L 92,50" },
];

function clamp(v, lo, hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

function cssVar(name, fallback) {
  const value = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  return value || fallback;
}

function drawTablePath(ctx, w, h, valueAt, steps = WAVETABLE_SIZE) {
  ctx.beginPath();
  for (let i = 0; i <= steps; i += 1) {
    const x = (i / steps) * w;
    const y = h / 2 - valueAt(i / steps) * (h / 2 - 2);
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();
}

// Mini preview used inside the Wavetable widget: the currently morphed waveform,
// plus (when cursorPhase is a number, 0..1) a playhead line with a dot on the
// curve marking where in the cycle it is right now.
export function drawWavetablePreview(canvas, tables, morphVolts, cursorPhase) {
  if (!canvas || !tables || tables.length === 0) return;
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = "rgba(255, 255, 255, 0.12)";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, h / 2);
  ctx.lineTo(w, h / 2);
  ctx.stroke();
  ctx.strokeStyle = cssVar("--accent", "#06b6d4");
  ctx.lineWidth = 1.5;
  const morph = clamp(Number(morphVolts) || 0, 0, 10) / 10;
  drawTablePath(ctx, w, h, (p) => sampleBank(tables, p, morph), Math.max(64, w));
  if (typeof cursorPhase === "number" && Number.isFinite(cursorPhase)) {
    const phase = cursorPhase - Math.floor(cursorPhase);
    const x = phase * w;
    const y = h / 2 - sampleBank(tables, phase, morph) * (h / 2 - 2);
    const cursorColor = cssVar("--cursor-color", "#f472b6");
    ctx.strokeStyle = cursorColor;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, h);
    ctx.stroke();
    ctx.fillStyle = cursorColor;
    ctx.beginPath();
    ctx.arc(x, y, 3, 0, Math.PI * 2);
    ctx.fill();
  }
}

// Sine-partial amplitudes of a table (first WAVETABLE_MAX_HARMONICS), used
// as the starting point when switching a slot to Harmonics mode.
function analyzeHarmonics(table) {
  const out = new Array(WAVETABLE_MAX_HARMONICS).fill(0);
  for (let n = 1; n <= WAVETABLE_MAX_HARMONICS; n += 1) {
    let acc = 0;
    for (let i = 0; i < WAVETABLE_SIZE; i += 1) acc += table[i] * Math.sin((2 * Math.PI * n * i) / WAVETABLE_SIZE);
    out[n - 1] = clamp((2 * acc) / WAVETABLE_SIZE, -1, 1);
  }
  return out;
}

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

let activeEditor = null;

export function openWavetableEditor({ title, tables, onPreview, onApply, onCancel }) {
  if (activeEditor) activeEditor.close(false);

  const bank = (tables && tables.length > 0 ? tables : [createPresetTable("sine")]).map((t) => Float32Array.from(t));
  const harmonics = bank.map(() => null);
  let slot = 0;
  let mode = "draw";
  let snapZero = false;

  const overlay = el("div", "modal-overlay wavetable-editor-overlay");
  const modal = el("div", "modal modal-wide wavetable-editor");
  overlay.appendChild(modal);

  const titleRow = el("div", "modal-title-row");
  titleRow.appendChild(el("h3", "", title || "Wavetable Editor"));
  modal.appendChild(titleRow);

  // Slot bar (2.4)
  const slotBar = el("div", "wavetable-slot-bar");
  modal.appendChild(slotBar);

  // Presets + mode + snap (2.1 / 2.2 / 2.3)
  const toolRow = el("div", "wavetable-tool-row");
  const presetGroup = el("div", "osc-wave-buttons wavetable-preset-buttons");
  PRESET_BUTTONS.forEach((p) => {
    const btn = el("button", "osc-wave-btn");
    btn.type = "button";
    btn.title = `${p.label}: fill this slot`;
    btn.innerHTML = `<svg viewBox="0 0 100 100" fill="none" stroke="currentColor" stroke-width="9" stroke-linecap="round" stroke-linejoin="round"><path d="${p.path}"/></svg>`;
    btn.addEventListener("click", () => {
      bank[slot] = createPresetTable(p.id);
      harmonics[slot] = null;
      commitEdit();
    });
    presetGroup.appendChild(btn);
  });
  toolRow.appendChild(presetGroup);

  const modeGroup = el("div", "wavetable-mode-toggle");
  const drawBtn = el("button", "btn-secondary", "Freehand");
  const harmBtn = el("button", "btn-secondary", "Harmonics");
  [drawBtn, harmBtn].forEach((b) => (b.type = "button"));
  drawBtn.addEventListener("click", () => setMode("draw"));
  harmBtn.addEventListener("click", () => setMode("harmonics"));
  modeGroup.append(drawBtn, harmBtn);
  toolRow.appendChild(modeGroup);

  const snapLabel = el("label", "wavetable-snap-toggle");
  const snapSwitch = el("span", "switch");
  const snapInput = document.createElement("input");
  snapInput.type = "checkbox";
  snapSwitch.append(snapInput, el("span", "switch-track"));
  snapLabel.append(snapSwitch, el("span", "", "Snap to Zero"));
  snapLabel.title = "Cut the loop at the zero crossings nearest to start and end and stretch it back to full length - removes the click at the loop point. Re-applied after every edit while active.";
  snapInput.addEventListener("change", () => {
    snapZero = snapInput.checked;
    if (snapZero) commitEdit();
  });
  toolRow.appendChild(snapLabel);
  modal.appendChild(toolRow);

  // Drawing canvas (2.2)
  const canvas = el("canvas", "wavetable-canvas");
  canvas.width = WAVETABLE_SIZE;
  canvas.height = 320;
  modal.appendChild(canvas);
  const harmCanvas = el("canvas", "wavetable-harmonics-canvas");
  harmCanvas.width = WAVETABLE_SIZE;
  harmCanvas.height = 160;
  modal.appendChild(harmCanvas);
  const hint = el("p", "modal-hint");
  modal.appendChild(hint);

  const actions = el("div", "modal-actions");
  const cancelBtn = el("button", "btn-secondary", "Cancel");
  const applyBtn = el("button", "", "Apply");
  cancelBtn.type = "button";
  applyBtn.type = "button";
  actions.append(cancelBtn, applyBtn);
  modal.appendChild(actions);

  function renderSlots() {
    slotBar.innerHTML = "";
    bank.forEach((table, index) => {
      const btn = el("button", `wavetable-slot${index === slot ? " active" : ""}`);
      btn.type = "button";
      btn.title = `Slot ${index + 1}`;
      const thumb = el("canvas");
      thumb.width = 56;
      thumb.height = 28;
      drawWavetablePreview(thumb, [table], 0);
      btn.append(thumb, el("span", "", String(index + 1)));
      btn.addEventListener("click", () => {
        slot = index;
        render();
      });
      slotBar.appendChild(btn);
    });
    const add = el("button", "wavetable-slot-action", "+");
    add.type = "button";
    add.title = "Add slot (copy of the selected one)";
    add.disabled = bank.length >= WAVETABLE_MAX_TABLES;
    add.addEventListener("click", () => {
      bank.splice(slot + 1, 0, Float32Array.from(bank[slot]));
      harmonics.splice(slot + 1, 0, harmonics[slot] ? [...harmonics[slot]] : null);
      slot += 1;
      commitEdit(false);
    });
    const remove = el("button", "wavetable-slot-action", "−");
    remove.type = "button";
    remove.title = "Remove selected slot";
    remove.disabled = bank.length <= WAVETABLE_MIN_TABLES;
    remove.addEventListener("click", () => {
      bank.splice(slot, 1);
      harmonics.splice(slot, 1);
      slot = Math.min(slot, bank.length - 1);
      commitEdit(false);
    });
    const count = el("span", "wavetable-slot-count", `${bank.length}/${WAVETABLE_MAX_TABLES}`);
    slotBar.append(add, remove, count);
  }

  function renderCanvas() {
    const ctx = canvas.getContext("2d");
    const w = canvas.width;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = "rgba(0, 0, 0, 0.25)";
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = "rgba(255, 255, 255, 0.07)";
    ctx.lineWidth = 1;
    for (let i = 1; i < 8; i += 1) {
      ctx.beginPath();
      ctx.moveTo((i / 8) * w, 0);
      ctx.lineTo((i / 8) * w, h);
      ctx.stroke();
    }
    ctx.strokeStyle = "rgba(255, 255, 255, 0.25)";
    ctx.beginPath();
    ctx.moveTo(0, h / 2);
    ctx.lineTo(w, h / 2);
    ctx.stroke();
    // Neighbour slots as faint ghosts - they are what this slot morphs into.
    ctx.lineWidth = 1.5;
    [slot - 1, slot + 1].forEach((n) => {
      if (n < 0 || n >= bank.length) return;
      ctx.strokeStyle = "rgba(255, 255, 255, 0.15)";
      const t = bank[n];
      drawTablePath(ctx, w, h, (p) => t[Math.min(WAVETABLE_SIZE - 1, Math.floor(p * WAVETABLE_SIZE))]);
    });
    ctx.strokeStyle = cssVar("--accent", "#06b6d4");
    ctx.lineWidth = 2.5;
    const t = bank[slot];
    drawTablePath(ctx, w, h, (p) => t[Math.min(WAVETABLE_SIZE - 1, Math.floor(p * WAVETABLE_SIZE))]);
  }

  function renderHarmonics() {
    const ctx = harmCanvas.getContext("2d");
    const w = harmCanvas.width;
    const h = harmCanvas.height;
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = "rgba(0, 0, 0, 0.25)";
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = "rgba(255, 255, 255, 0.25)";
    ctx.beginPath();
    ctx.moveTo(0, h / 2);
    ctx.lineTo(w, h / 2);
    ctx.stroke();
    const amps = harmonics[slot] || analyzeHarmonics(bank[slot]);
    const barW = w / WAVETABLE_MAX_HARMONICS;
    ctx.fillStyle = cssVar("--accent", "#06b6d4");
    amps.forEach((a, i) => {
      const barH = (a * (h / 2 - 4));
      ctx.fillRect(i * barW + 3, barH >= 0 ? h / 2 - barH : h / 2, barW - 6, Math.abs(barH));
    });
    ctx.fillStyle = "rgba(255, 255, 255, 0.45)";
    ctx.font = "18px sans-serif";
    ctx.textAlign = "center";
    for (let i = 0; i < WAVETABLE_MAX_HARMONICS; i += 4) ctx.fillText(String(i + 1), i * barW + barW / 2, h - 6);
  }

  function render() {
    drawBtn.classList.toggle("active", mode === "draw");
    harmBtn.classList.toggle("active", mode === "harmonics");
    harmCanvas.classList.toggle("hidden", mode !== "harmonics");
    hint.textContent = mode === "draw"
      ? "Draw directly on the waveform. Presets fill the selected slot; slots are morphed in order (Morph 0 V = slot 1, 10 V = last slot)."
      : "Drag the bars to set each harmonic's level (above the line = +, below = inverted phase). Changing a bar rebuilds the slot additively.";
    renderSlots();
    renderCanvas();
    if (mode === "harmonics") renderHarmonics();
  }

  // Applies Snap to Zero (if active), redraws and notifies the caller.
  function commitEdit(snap = true) {
    if (snap && snapZero) snapTableToZero(bank[slot]);
    render();
    livePreviewTimer = cancelLivePreview();
    if (onPreview) onPreview(bank.map((t) => Float32Array.from(t)));
  }

  // While a stroke/bar drag is still in progress the sound follows along too
  // (throttled, trailing call guaranteed) - Snap to Zero only runs once the
  // stroke ends (commitEdit), it would otherwise fight the pen.
  const LIVE_PREVIEW_MS = 60;
  let livePreviewTimer = null;
  let livePreviewLast = 0;
  function cancelLivePreview() {
    if (livePreviewTimer) clearTimeout(livePreviewTimer);
    return null;
  }
  function livePreview() {
    if (!onPreview || livePreviewTimer) return;
    const wait = Math.max(0, LIVE_PREVIEW_MS - (performance.now() - livePreviewLast));
    livePreviewTimer = setTimeout(() => {
      livePreviewTimer = null;
      livePreviewLast = performance.now();
      onPreview(bank.map((t) => Float32Array.from(t)));
    }, wait);
  }

  function setMode(next) {
    mode = next;
    if (mode === "harmonics" && !harmonics[slot]) harmonics[slot] = analyzeHarmonics(bank[slot]);
    render();
  }

  // Freehand drawing: canvas coordinates -> (sample index, value); gaps
  // between two pointer events are filled by linear interpolation so fast
  // strokes leave no holes.
  let drawing = false;
  let lastIndex = -1;
  let lastValue = 0;
  function pointFromEvent(event, target) {
    const rect = target.getBoundingClientRect();
    const x = clamp((event.clientX - rect.left) / rect.width, 0, 1);
    const y = clamp((event.clientY - rect.top) / rect.height, 0, 1);
    return { x, value: clamp(1 - 2 * y, -1, 1) };
  }
  function paint(event) {
    const { x, value } = pointFromEvent(event, canvas);
    const index = Math.min(WAVETABLE_SIZE - 1, Math.floor(x * WAVETABLE_SIZE));
    const table = bank[slot];
    if (lastIndex < 0) {
      table[index] = value;
    } else {
      const span = index - lastIndex;
      const steps = Math.abs(span);
      for (let s = 0; s <= steps; s += 1) {
        const f = steps === 0 ? 1 : s / steps;
        table[lastIndex + Math.sign(span) * s] = lastValue + (value - lastValue) * f;
      }
    }
    lastIndex = index;
    lastValue = value;
    harmonics[slot] = null;
    renderCanvas();
    livePreview();
  }
  canvas.addEventListener("pointerdown", (event) => {
    if (mode !== "draw") return;
    event.preventDefault();
    drawing = true;
    lastIndex = -1;
    canvas.setPointerCapture(event.pointerId);
    paint(event);
  });
  canvas.addEventListener("pointermove", (event) => {
    if (drawing) paint(event);
  });
  const endDraw = (event) => {
    if (!drawing) return;
    drawing = false;
    lastIndex = -1;
    if (canvas.hasPointerCapture(event.pointerId)) canvas.releasePointerCapture(event.pointerId);
    commitEdit();
  };
  canvas.addEventListener("pointerup", endDraw);
  canvas.addEventListener("pointercancel", endDraw);

  // Harmonic bars
  let harmDragging = false;
  function setHarmonic(event) {
    const { x, value } = pointFromEvent(event, harmCanvas);
    const n = Math.min(WAVETABLE_MAX_HARMONICS - 1, Math.floor(x * WAVETABLE_MAX_HARMONICS));
    const amps = harmonics[slot] || analyzeHarmonics(bank[slot]);
    amps[n] = Math.abs(value) < 0.03 ? 0 : value;
    harmonics[slot] = amps;
    bank[slot] = createAdditiveTable(amps);
    renderCanvas();
    renderHarmonics();
    livePreview();
  }
  harmCanvas.addEventListener("pointerdown", (event) => {
    event.preventDefault();
    harmDragging = true;
    harmCanvas.setPointerCapture(event.pointerId);
    setHarmonic(event);
  });
  harmCanvas.addEventListener("pointermove", (event) => {
    if (harmDragging) setHarmonic(event);
  });
  const endHarm = (event) => {
    if (!harmDragging) return;
    harmDragging = false;
    if (harmCanvas.hasPointerCapture(event.pointerId)) harmCanvas.releasePointerCapture(event.pointerId);
    commitEdit();
  };
  harmCanvas.addEventListener("pointerup", endHarm);
  harmCanvas.addEventListener("pointercancel", endHarm);

  const onKey = (event) => {
    if (event.key === "Escape") close(false);
  };

  function close(apply) {
    livePreviewTimer = cancelLivePreview();
    document.removeEventListener("keydown", onKey, true);
    overlay.remove();
    activeEditor = null;
    if (apply) {
      if (onApply) onApply(bank.map((t) => Float32Array.from(t)));
    } else if (onCancel) {
      onCancel();
    }
  }

  cancelBtn.addEventListener("click", () => close(false));
  applyBtn.addEventListener("click", () => close(true));
  overlay.addEventListener("pointerdown", (event) => {
    if (event.target === overlay) close(false);
  });
  document.addEventListener("keydown", onKey, true);

  document.body.appendChild(overlay);
  activeEditor = { close };
  render();
  return activeEditor;
}
