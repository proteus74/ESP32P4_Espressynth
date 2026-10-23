// Sampler editor popup (double-click on a Sampler widget): pick the WAV and root note, see the
// waveform, zoom into it and drag the Start / End / Loop Start / Loop End markers. While a voice
// plays, a cursor follows its position.
//
// Markers are fractions 0..1 of the sample length (nodes/SamplerNode.h). The editor works on a
// private copy and reports back through callbacks, like wavetable-editor.js:
//   onPreview(params) - after every edit, throttled while dragging (live audition),
//   onApply(params)   - "Apply",
//   onCancel()        - "Cancel"/Escape/backdrop.
// loadBuffer(file) resolves to a decoded AudioBuffer (or null) for the waveform display;
// getPlayheads() returns the playing voices' positions (fractions 0..1) for the cursor.

const GRAB_PX = 10; // CSS pixels around a marker that still grab it
const MIN_GAP = 0.0005; // smallest marker distance (fraction of the sample)
const MIN_VIEW_FRAMES = 64; // deepest zoom: this many frames across the whole view
const ZOOM_STEP = 2;
const PREVIEW_MS = 60;
const LOOP_COLOR = "#f59e0b";
const CURSOR_COLOR = "#22c55e";

const MARKERS = [
  { key: "start", label: "S", loop: false },
  { key: "end", label: "E", loop: false },
  { key: "loopStart", label: "L", loop: true },
  { key: "loopEnd", label: "L", loop: true },
];

function clamp(v, lo, hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

function cssVar(name, fallback) {
  const value = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  return value || fallback;
}

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function button(label, title, onClick) {
  const b = el("button", "btn-secondary", label);
  b.type = "button";
  b.title = title;
  b.addEventListener("click", onClick);
  return b;
}

// Keeps 0 <= start < end <= 1 and start <= loopStart < loopEnd <= end. The dragged marker is
// clamped against its fixed neighbours first, then the loop pair is pushed inside [start, end].
export function normalizeSamplerMarkers(p, dragged = null) {
  if (dragged === "start") p.start = clamp(p.start, 0, p.end - MIN_GAP);
  if (dragged === "end") p.end = clamp(p.end, p.start + MIN_GAP, 1);
  if (dragged === "loopStart") p.loopStart = clamp(p.loopStart, p.start, p.loopEnd - MIN_GAP);
  if (dragged === "loopEnd") p.loopEnd = clamp(p.loopEnd, p.loopStart + MIN_GAP, p.end);
  p.start = clamp(p.start, 0, 1 - MIN_GAP);
  p.end = clamp(p.end, p.start + MIN_GAP, 1);
  p.loopStart = clamp(p.loopStart, p.start, p.end - MIN_GAP);
  p.loopEnd = clamp(p.loopEnd, p.loopStart + MIN_GAP, p.end);
  return p;
}

// Frame of the zero crossing nearest to `frame` (left or right, whichever is closer), on the
// mix of all channels; null if the signal never crosses zero. Of the two frames around a sign
// change, the one closer to 0 wins.
export function nearestZeroCrossing(buffer, frame) {
  const channels = Array.from({ length: buffer.numberOfChannels }, (_, c) => buffer.getChannelData(c));
  const last = buffer.length - 1;
  const value = (i) => channels.reduce((sum, data) => sum + data[i], 0);
  const crossingAt = (i) => {
    if (i < 0 || i >= last) return null;
    const a = value(i);
    const b = value(i + 1);
    if (a === 0) return i;
    if (a < 0 !== b < 0) return Math.abs(a) <= Math.abs(b) ? i : i + 1;
    return null;
  };
  for (let d = 0; d <= last; d += 1) {
    const left = crossingAt(frame - d);
    if (left !== null) return left;
    const right = crossingAt(frame + d);
    if (right !== null) return right;
  }
  return null;
}

// Min/max per canvas column of the visible range [v0, v1], per channel (max. 2 lanes).
function computePeaks(buffer, v0, v1, columns) {
  const last = buffer.length - 1;
  return Array.from({ length: Math.min(2, buffer.numberOfChannels) }, (_, c) => {
    const data = buffer.getChannelData(c);
    const min = new Float32Array(columns);
    const max = new Float32Array(columns);
    for (let x = 0; x < columns; x += 1) {
      const from = Math.floor((v0 + ((v1 - v0) * x) / columns) * last);
      const to = Math.max(from + 1, Math.floor((v0 + ((v1 - v0) * (x + 1)) / columns) * last));
      let lo = 1;
      let hi = -1;
      for (let i = from; i < to && i <= last; i += 1) {
        const v = data[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
      min[x] = lo > hi ? 0 : lo;
      max[x] = lo > hi ? 0 : hi;
    }
    return { data, min, max };
  });
}

let activeEditor = null;

export function openSamplerEditor({ title, params, samples, noteName, loadBuffer, getPlayheads, onPreview, onApply, onCancel }) {
  if (activeEditor) activeEditor.close(false);

  const p = normalizeSamplerMarkers({
    sampleFile: params.sampleFile || "",
    rootNoteMidi: params.rootNoteMidi ?? 60,
    start: params.start ?? 0,
    end: params.end ?? 1,
    loopStart: params.loopStart ?? 0,
    loopEnd: params.loopEnd ?? 1,
    loop: params.loop === true,
  });
  let buffer = null;
  let loadToken = 0;
  const view = { v0: 0, v1: 1 }; // visible range, fractions of the sample
  let peaks = null; // cache for the current view and canvas width
  let peaksKey = "";

  // ---- DOM ----------------------------------------------------------------------------
  const overlay = el("div", "modal-overlay sampler-editor-overlay");
  const modal = el("div", "modal sampler-editor");
  overlay.appendChild(modal);
  const titleRow = el("div", "modal-title-row");
  titleRow.appendChild(el("h3", "", title || "Sampler"));
  modal.appendChild(titleRow);

  const toolRow = el("div", "sampler-tool-row");
  const sampleSelect = document.createElement("select");
  sampleSelect.appendChild(new Option("No sample", ""));
  samples.forEach((name) => sampleSelect.appendChild(new Option(name, name)));
  if (p.sampleFile && !samples.includes(p.sampleFile)) sampleSelect.appendChild(new Option(`${p.sampleFile} (missing)`, p.sampleFile));
  sampleSelect.value = p.sampleFile;
  const rootSelect = document.createElement("select");
  for (let n = 0; n <= 127; n += 1) rootSelect.appendChild(new Option(`${noteName(n)} (${n})`, String(n)));
  rootSelect.value = String(p.rootNoteMidi);
  const field = (label, control) => {
    const f = el("label", "modal-field");
    f.append(el("span", "", label), control);
    return f;
  };

  const loopLabel = el("label", "wavetable-snap-toggle");
  const loopSwitch = el("span", "switch");
  const loopInput = document.createElement("input");
  loopInput.type = "checkbox";
  loopInput.checked = p.loop;
  loopSwitch.append(loopInput, el("span", "switch-track"));
  loopLabel.append(loopSwitch, el("span", "", "Loop"));
  loopLabel.title = "Repeat Loop Start..Loop End until the amplitude is back at 0";

  const snapGroup = el("div", "sampler-button-group");
  const snapStartBtn = button("Loop Start → 0", "Move Loop Start to the nearest zero crossing", () => snapToZero("loopStart"));
  const snapEndBtn = button("Loop End → 0", "Move Loop End to the nearest zero crossing", () => snapToZero("loopEnd"));
  snapGroup.append(snapStartBtn, snapEndBtn);

  const zoomGroup = el("div", "sampler-button-group");
  zoomGroup.append(
    button("−", "Zoom out", () => zoomAt((view.v0 + view.v1) / 2, ZOOM_STEP)),
    button("+", "Zoom in (mouse wheel zooms at the pointer)", () => zoomAt((view.v0 + view.v1) / 2, 1 / ZOOM_STEP)),
    button("Fit", "Show the whole sample", () => setView(0, 1)),
  );
  toolRow.append(field("Sample", sampleSelect), field("Root Note", rootSelect), loopLabel, snapGroup, zoomGroup);
  modal.appendChild(toolRow);

  const viewBox = el("div", "sampler-view");
  const canvas = el("canvas", "sampler-canvas");
  const cursorCanvas = el("canvas", "sampler-cursor");
  viewBox.append(canvas, cursorCanvas);
  modal.appendChild(viewBox);
  const info = el("p", "modal-hint");
  modal.appendChild(info);
  modal.appendChild(el("p", "modal-hint",
    "Drag a marker to move it (S = Start, E = End, L = loop points with Loop on), drag elsewhere to scroll. " +
    "Mouse wheel zooms, Shift + wheel scrolls. A new gate plays from Start; the voice sounds until the amplitude is back at 0."));

  const actions = el("div", "modal-actions");
  const cancelBtn = button("Cancel", "Discard the changes", () => close(false));
  const applyBtn = button("Apply", "Keep the changes", () => close(true));
  applyBtn.className = "";
  actions.append(cancelBtn, applyBtn);
  modal.appendChild(actions);

  // ---- preview callbacks --------------------------------------------------------------
  let previewTimer = null;
  const snapshot = () => ({ ...p });
  function previewNow() {
    if (previewTimer) clearTimeout(previewTimer);
    previewTimer = null;
    if (onPreview) onPreview(snapshot());
  }
  function previewThrottled() {
    if (!previewTimer) previewTimer = setTimeout(previewNow, PREVIEW_MS);
  }

  // ---- view (zoom / scroll) -----------------------------------------------------------
  const visibleMarkers = () => MARKERS.filter((m) => p.loop || !m.loop);
  const minSpan = () => (buffer ? Math.min(1, MIN_VIEW_FRAMES / (buffer.length - 1)) : MIN_GAP * 4);
  const fracToX = (f) => ((f - view.v0) / (view.v1 - view.v0)) * canvas.width;
  const xToFrac = (x) => view.v0 + (x / canvas.width) * (view.v1 - view.v0);

  function setView(v0, v1) {
    const span = clamp(v1 - v0, minSpan(), 1);
    view.v0 = clamp(v0, 0, 1 - span);
    view.v1 = view.v0 + span;
    render();
  }
  function zoomAt(f, factor) {
    const span = clamp((view.v1 - view.v0) * factor, minSpan(), 1);
    const t = (f - view.v0) / (view.v1 - view.v0); // keep `f` under the same screen position
    setView(f - t * span, f - t * span + span);
  }

  // ---- drawing ------------------------------------------------------------------------
  function resizeCanvases() {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(viewBox.clientWidth * dpr));
    const h = Math.max(1, Math.round(viewBox.clientHeight * dpr));
    if (canvas.width === w && canvas.height === h) return;
    canvas.width = cursorCanvas.width = w;
    canvas.height = cursorCanvas.height = h;
    render();
  }

  function drawChannel(ctx, { data, min, max }, mid, halfHeight, color) {
    const w = canvas.width;
    const last = buffer.length - 1;
    const framesPerPx = ((view.v1 - view.v0) * last) / w;
    ctx.fillStyle = color;
    ctx.strokeStyle = color;
    if (framesPerPx >= 1) {
      for (let x = 0; x < w; x += 1) {
        const top = mid - max[x] * halfHeight;
        const bottom = mid - min[x] * halfHeight;
        ctx.fillRect(x, top, 1, Math.max(1, bottom - top));
      }
      return;
    }
    // Zoomed in past one frame per pixel: the samples as a line, with a dot per frame.
    const first = Math.max(0, Math.floor(view.v0 * last));
    const end = Math.min(last, Math.ceil(view.v1 * last));
    ctx.lineWidth = Math.max(1, (window.devicePixelRatio || 1) * 1.5);
    ctx.beginPath();
    for (let i = first; i <= end; i += 1) {
      const x = fracToX(i / last);
      const y = mid - data[i] * halfHeight;
      if (i === first) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.stroke();
    if (1 / framesPerPx >= 6) {
      for (let i = first; i <= end; i += 1) ctx.fillRect(fracToX(i / last) - 2, mid - data[i] * halfHeight - 2, 4, 4);
    }
  }

  function render() {
    const ctx = canvas.getContext("2d");
    const w = canvas.width;
    const h = canvas.height;
    const dpr = window.devicePixelRatio || 1;
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = "rgba(0, 0, 0, 0.25)";
    ctx.fillRect(0, 0, w, h);
    if (p.loop) {
      ctx.fillStyle = "rgba(245, 158, 11, 0.12)";
      ctx.fillRect(fracToX(p.loopStart), 0, fracToX(p.loopEnd) - fracToX(p.loopStart), h);
    }
    if (buffer) {
      const key = `${view.v0}|${view.v1}|${w}`;
      if (key !== peaksKey) {
        peaks = computePeaks(buffer, view.v0, view.v1, w);
        peaksKey = key;
      }
      const lane = h / peaks.length;
      const accent = cssVar("--accent", "#06b6d4");
      peaks.forEach((channel, c) => {
        const mid = lane * c + lane / 2;
        ctx.strokeStyle = "rgba(255, 255, 255, 0.15)";
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(0, mid);
        ctx.lineTo(w, mid);
        ctx.stroke();
        drawChannel(ctx, channel, mid, lane / 2 - 4 * dpr, accent);
      });
    } else {
      ctx.fillStyle = "rgba(255, 255, 255, 0.5)";
      ctx.font = `${20 * dpr}px sans-serif`;
      ctx.textAlign = "center";
      ctx.fillText(p.sampleFile ? "Loading…" : "No sample", w / 2, h / 2);
    }
    // Outside Start..End is not played.
    ctx.fillStyle = "rgba(0, 0, 0, 0.55)";
    ctx.fillRect(0, 0, Math.max(0, fracToX(p.start)), h);
    ctx.fillRect(fracToX(p.end), 0, w - fracToX(p.end), h);
    ctx.font = `bold ${14 * dpr}px sans-serif`;
    ctx.textAlign = "center";
    const box = 18 * dpr;
    visibleMarkers().forEach((m) => {
      const x = fracToX(p[m.key]);
      if (x < -box || x > w + box) return;
      const color = m.loop ? LOOP_COLOR : "#ffffff";
      ctx.strokeStyle = color;
      ctx.lineWidth = 2 * dpr;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, h);
      ctx.stroke();
      const y = m.loop ? h - box / 2 - 2 * dpr : box / 2 + 2 * dpr;
      ctx.fillStyle = color;
      ctx.fillRect(x - box / 2, y - box / 2, box, box);
      ctx.fillStyle = "#000000";
      ctx.fillText(m.label, x, y + 5 * dpr);
    });
    updateInfo();
    lastCursorKey = null; // cursor layer is redrawn on the next frame
  }

  function updateInfo() {
    const secs = (f) => (buffer ? `${((f * (buffer.length - 1)) / buffer.sampleRate).toFixed(3)} s` : `${(f * 100).toFixed(1)} %`);
    const zoom = 1 / (view.v1 - view.v0);
    info.textContent = `Start ${secs(p.start)} · End ${secs(p.end)}` +
      (p.loop ? ` · Loop ${secs(p.loopStart)} – ${secs(p.loopEnd)}` : "") +
      (zoom > 1.001 ? ` · Zoom ×${zoom.toFixed(zoom < 10 ? 1 : 0)} (${secs(view.v0)} – ${secs(view.v1)})` : "");
    snapStartBtn.disabled = snapEndBtn.disabled = !buffer || !p.loop;
  }

  // Cursor layer: own canvas, so following the playhead never redraws the waveform.
  let lastCursorKey = null;
  let rafId = 0;
  function drawCursor() {
    rafId = requestAnimationFrame(drawCursor);
    const positions = getPlayheads ? getPlayheads() : [];
    const key = positions.map((f) => f.toFixed(5)).join(",") + `|${view.v0}|${view.v1}|${cursorCanvas.width}`;
    if (key === lastCursorKey) return;
    lastCursorKey = key;
    const ctx = cursorCanvas.getContext("2d");
    const dpr = window.devicePixelRatio || 1;
    ctx.clearRect(0, 0, cursorCanvas.width, cursorCanvas.height);
    ctx.strokeStyle = CURSOR_COLOR;
    ctx.fillStyle = CURSOR_COLOR;
    ctx.lineWidth = 2 * dpr;
    positions.forEach((f) => {
      const x = fracToX(f);
      if (x < 0 || x > cursorCanvas.width) return;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, cursorCanvas.height);
      ctx.stroke();
      ctx.beginPath();
      ctx.moveTo(x - 6 * dpr, 0);
      ctx.lineTo(x + 6 * dpr, 0);
      ctx.lineTo(x, 9 * dpr);
      ctx.fill();
    });
  }

  // ---- editing ------------------------------------------------------------------------
  function snapToZero(key) {
    if (!buffer) return;
    const last = buffer.length - 1;
    const frame = nearestZeroCrossing(buffer, Math.round(p[key] * last));
    if (frame === null) return;
    // +0.25 frame: the engines truncate fraction * last back to a frame, so this lands exactly.
    p[key] = Math.min(1, (frame + 0.25) / last);
    normalizeSamplerMarkers(p, key);
    render();
    previewNow();
  }

  async function loadWaveform() {
    const token = (loadToken += 1);
    buffer = null;
    peaksKey = "";
    render();
    if (!p.sampleFile) return;
    const decoded = await loadBuffer(p.sampleFile);
    if (token !== loadToken) return;
    buffer = decoded;
    setView(0, 1);
    if (!decoded) info.textContent = "Sample could not be loaded.";
  }

  sampleSelect.addEventListener("change", () => {
    p.sampleFile = sampleSelect.value;
    loadWaveform();
    previewNow();
  });
  rootSelect.addEventListener("change", () => {
    p.rootNoteMidi = Number(rootSelect.value);
    previewNow();
  });
  loopInput.addEventListener("change", () => {
    p.loop = loopInput.checked;
    render();
    previewNow();
  });

  // ---- pointer: drag a marker or scroll the view; wheel zooms -------------------------
  let drag = null; // { marker } or { panFrom: clientX, v0, v1 }
  const canvasX = (event) => {
    const rect = canvas.getBoundingClientRect();
    return ((event.clientX - rect.left) / rect.width) * canvas.width;
  };
  function markerAt(x) {
    const grab = GRAB_PX * (window.devicePixelRatio || 1);
    let best = null;
    let bestDist = grab;
    // Loop markers first: they sit inside Start..End and would otherwise be hard to grab.
    [...visibleMarkers()].sort((a, b) => b.loop - a.loop).forEach((m) => {
      const dist = Math.abs(fracToX(p[m.key]) - x);
      if (dist < bestDist) {
        bestDist = dist;
        best = m.key;
      }
    });
    return best;
  }
  canvas.addEventListener("pointerdown", (event) => {
    event.preventDefault();
    const marker = markerAt(canvasX(event));
    drag = marker ? { marker } : { panFrom: event.clientX, v0: view.v0, v1: view.v1 };
    canvas.setPointerCapture(event.pointerId);
  });
  canvas.addEventListener("pointermove", (event) => {
    if (!drag) {
      canvas.style.cursor = markerAt(canvasX(event)) ? "ew-resize" : "grab";
      return;
    }
    if (drag.marker) {
      p[drag.marker] = clamp(xToFrac(canvasX(event)), 0, 1);
      normalizeSamplerMarkers(p, drag.marker);
      render();
      previewThrottled();
    } else {
      const rect = canvas.getBoundingClientRect();
      const shift = ((event.clientX - drag.panFrom) / rect.width) * (drag.v1 - drag.v0);
      setView(drag.v0 - shift, drag.v1 - shift);
    }
  });
  const endDrag = (event) => {
    if (!drag) return;
    const wasMarker = !!drag.marker;
    drag = null;
    if (canvas.hasPointerCapture(event.pointerId)) canvas.releasePointerCapture(event.pointerId);
    if (wasMarker) previewNow();
  };
  canvas.addEventListener("pointerup", endDrag);
  canvas.addEventListener("pointercancel", endDrag);
  canvas.addEventListener("wheel", (event) => {
    event.preventDefault();
    const span = view.v1 - view.v0;
    if (event.shiftKey || Math.abs(event.deltaX) > Math.abs(event.deltaY)) {
      const delta = (event.shiftKey ? event.deltaY : event.deltaX) / canvas.getBoundingClientRect().width;
      setView(view.v0 + delta * span, view.v1 + delta * span);
    } else {
      zoomAt(xToFrac(canvasX(event)), Math.exp(event.deltaY * 0.0015));
    }
  }, { passive: false });

  // ---- open / close -------------------------------------------------------------------
  const resizeObserver = new ResizeObserver(resizeCanvases);
  const onKey = (event) => {
    if (event.key === "Escape") close(false);
  };
  function close(apply) {
    if (previewTimer) clearTimeout(previewTimer);
    cancelAnimationFrame(rafId);
    resizeObserver.disconnect();
    document.removeEventListener("keydown", onKey, true);
    overlay.remove();
    activeEditor = null;
    if (apply) {
      if (onApply) onApply(snapshot());
    } else if (onCancel) {
      onCancel();
    }
  }
  overlay.addEventListener("pointerdown", (event) => {
    if (event.target === overlay) close(false);
  });
  document.addEventListener("keydown", onKey, true);

  document.body.appendChild(overlay);
  resizeObserver.observe(viewBox);
  resizeCanvases();
  activeEditor = { close };
  rafId = requestAnimationFrame(drawCursor);
  loadWaveform();
  return activeEditor;
}
