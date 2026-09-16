import { createHash } from "node:crypto";

const options = Object.fromEntries(
  process.argv.slice(2).map((argument) => {
    const [key, value = "true"] = argument.replace(/^--/, "").split("=", 2);
    return [key, value];
  }),
);

const host = options.host ?? "192.168.1.121";
const preset = options.preset ?? "Benchmark";
const voicePoints = String(options.voices ?? "1,4,8,12,16")
  .split(",")
  .map(Number);
const baseNote = Number(options.baseNote ?? 48);
const velocity = Number(options.velocity ?? 90);
const phaseSeconds = Number(options.seconds ?? 3);
const settleSeconds = Number(options.settle ?? 7);
const stressSeconds = Number(options.stressSeconds ?? 0);

if (
  voicePoints.length === 0 ||
  voicePoints.some((voices) => !Number.isInteger(voices) || voices < 1 || voices > 16)
) {
  throw new Error("--voices must contain integers from 1 to 16");
}
if (!Number.isInteger(baseNote) || baseNote < 0 || baseNote + Math.max(...voicePoints) > 128) {
  throw new Error("--baseNote plus the largest voice count must stay inside MIDI 0..127");
}
if (!Number.isInteger(velocity) || velocity < 1 || velocity > 127) {
  throw new Error("--velocity must be an integer from 1 to 127");
}
if (
  !Number.isInteger(phaseSeconds) || phaseSeconds < 1 ||
  !Number.isInteger(settleSeconds) || settleSeconds < 0 ||
  !Number.isInteger(stressSeconds) || stressSeconds < 0
) {
  throw new Error("--seconds must be positive; --settle and --stressSeconds must be non-negative integers");
}

const httpBase = `http://${host}`;
const socket = new WebSocket(`ws://${host}/ws`);
const statusQueue = [];
const statusWaiters = [];
const eventWaiters = new Map();
let pendingCpu = null;

function enqueueStatus(status) {
  if (statusWaiters.length > 0) statusWaiters.shift()(status);
  else statusQueue.push(status);
}

function deliverEvent(type, message) {
  const waiters = eventWaiters.get(type);
  if (waiters?.length) waiters.shift()(message);
}

socket.addEventListener("message", (event) => {
  const message = JSON.parse(event.data);
  if (message.type === "cpuLoad") {
    pendingCpu = message;
    return;
  }
  if (message.type === "nodeCpu" && pendingCpu !== null) {
    pendingCpu.nodeCpu = message.w;
    enqueueStatus(pendingCpu);
    pendingCpu = null;
    return;
  }
  deliverEvent(message.type, message);
});

const opened = new Promise((resolve, reject) => {
  socket.addEventListener("open", resolve, { once: true });
  socket.addEventListener("error", () => reject(new Error("WebSocket connection failed")), { once: true });
});

function nextStatus() {
  if (statusQueue.length > 0) return Promise.resolve(statusQueue.shift());
  return new Promise((resolve) => statusWaiters.push(resolve));
}

function nextEvent(type) {
  return new Promise((resolve) => {
    const waiters = eventWaiters.get(type) ?? [];
    waiters.push(resolve);
    eventWaiters.set(type, waiters);
  });
}

function sendNote(midiType, note) {
  socket.send(JSON.stringify({
    type: "midiTest",
    midiType,
    channel: 0,
    note,
    velocity: midiType === "note_on" ? velocity : 0,
  }));
}

function noteSet(count) {
  return Array.from({ length: count }, (_, index) => baseNote + index);
}

function percentile(values, fraction) {
  if (values.length === 0) return 0;
  const sorted = [...values].sort((left, right) => left - right);
  return sorted[Math.min(sorted.length - 1, Math.floor(sorted.length * fraction))];
}

function average(values) {
  return values.length === 0 ? 0 : values.reduce((sum, value) => sum + value, 0) / values.length;
}

function averageNodeCpu(samples) {
  const totals = new Map();
  const counts = new Map();
  for (const sample of samples) {
    for (const [id, value] of sample.nodeCpu ?? []) {
      totals.set(id, (totals.get(id) ?? 0) + value);
      counts.set(id, (counts.get(id) ?? 0) + 1);
    }
  }
  return [...totals].map(([id, total]) => [id, Number((total / counts.get(id)).toFixed(3))]);
}

function summarizePhase(name, samples, before) {
  const last = samples.at(-1) ?? before;
  const core1 = samples.map((sample) => sample.core1);
  const phaseKeys = ["activity", "render", "output"];
  const phases = Object.fromEntries(
    phaseKeys.map((key) => [key, Number(average(samples.map((sample) => sample.phases?.[key] ?? 0)).toFixed(2))]),
  );
  const counterSnapshot = (key) => {
    const values = samples.map((sample) => sample[key]).filter(Boolean);
    if (values.length === 0) return undefined;
    const keys = Object.keys(values[0]);
    return Object.fromEntries(
      keys.map((field) => [field, Math.round(average(values.map((value) => value[field])))]),
    );
  };
  return {
    name,
    samples: samples.length,
    core1Average: Number(average(core1).toFixed(1)),
    core1Median: percentile(core1, 0.5),
    core1Maximum: core1.length ? Math.max(...core1) : 0,
    p95WorstUs: samples.length ? Math.max(...samples.map((sample) => sample.p95Us)) : 0,
    p99WorstUs: samples.length ? Math.max(...samples.map((sample) => sample.p99Us)) : 0,
    peakWorstUs: samples.length ? Math.max(...samples.map((sample) => sample.peakUs)) : 0,
    overrunDelta: last.renderOverruns - before.renderOverruns,
    underrunDelta: last.underruns - before.underruns,
    midiDropDelta: last.midiDrops - before.midiDrops,
    voiceActiveMinimum: samples.length ? Math.min(...samples.map((sample) => sample.voiceActive ?? 0)) : 0,
    voiceActiveMaximum: samples.length ? Math.max(...samples.map((sample) => sample.voiceActive ?? 0)) : 0,
    voiceMaxMinimum: samples.length ? Math.min(...samples.map((sample) => sample.voiceMax ?? 0)) : 0,
    voiceMaxMaximum: samples.length ? Math.max(...samples.map((sample) => sample.voiceMax ?? 0)) : 0,
    reloadGenerationMinimum: samples.length
      ? Math.min(...samples.map((sample) => sample.reloadGeneration ?? 0))
      : 0,
    reloadGenerationMaximum: samples.length
      ? Math.max(...samples.map((sample) => sample.reloadGeneration ?? 0))
      : 0,
    phases,
    fm: counterSnapshot("fm"),
    dx: counterSnapshot("dx"),
    instances: counterSnapshot("instances"),
    nodeCpu: averageNodeCpu(samples),
  };
}

async function measurePhase(name, seconds, action) {
  statusQueue.length = 0;
  const before = await nextStatus();
  if (action) await action();
  // First interval can straddle the transition.
  await nextStatus();
  const samples = [];
  for (let second = 0; second < seconds; ++second) samples.push(await nextStatus());
  return summarizePhase(name, samples, before);
}

async function discardStatuses(seconds) {
  for (let second = 0; second < seconds; ++second) await nextStatus();
}

async function fetchJson(path, init) {
  const response = await fetch(`${httpBase}${path}`, init);
  if (!response.ok) throw new Error(`${path}: HTTP ${response.status}`);
  return response.json();
}

async function selectPreset() {
  const presetChanged = nextEvent("presetChanged");
  await fetchJson(`/api/preset/select?name=${encodeURIComponent(preset)}`, { method: "POST" });
  const selected = await presetChanged;
  if (selected.name !== preset) throw new Error(`Device selected unexpected preset ${selected.name}`);
}

const timeout = setTimeout(() => {
  console.error("Benchmark timed out");
  process.exit(2);
}, (45 + stressSeconds + voicePoints.length * (phaseSeconds * 5 + settleSeconds + 8)) * 1000);

const allNotes = noteSet(Math.max(...voicePoints));
try {
  await opened;

  const presetResponse = await fetch(`${httpBase}/api/preset?name=${encodeURIComponent(preset)}`);
  if (!presetResponse.ok) throw new Error(`Preset ${preset}: HTTP ${presetResponse.status}`);
  const presetText = await presetResponse.text();
  const presetJson = JSON.parse(presetText);
  const presetHash = createHash("sha256").update(presetText).digest("hex");
  await selectPreset();

  await discardStatuses(3);
  const version = await fetchJson("/api/version");
  const memory = await fetchJson("/api/diagnostics/memory");
  const latest = await nextStatus();
  const effectiveVoices = latest.voiceMax;
  const requestedVoices = presetJson.pages?.generator?.widgets
    ?.filter((widget) => widget.type === "midi-in")
    .reduce((maximum, widget) => Math.max(maximum, Number(widget.params?.voices ?? 1)), 1) ?? 1;

  const result = {
    host,
    preset,
    presetSha256: presetHash,
    firmware: version.gitVersion ?? version.version,
    requestedVoices,
    effectiveVoices,
    voiceCapRequested: latest.voiceCapRequested,
    voiceCapEffective: latest.voiceCapEffective,
    graph: memory.graph,
    memory: memory.current,
    budgetUs: latest.budgetUs,
    runs: [],
  };

  for (const [runIndex, voices] of voicePoints.entries()) {
    const notes = noteSet(voices);
    allNotes.forEach((note) => sendNote("note_off", note));
    // A reload gives every point fresh allocator/envelope state. Without it,
    // long DX releases and repeated notes leak active voices into the next
    // point and make a nominal 4-note run measure more than four voices.
    if (runIndex > 0) {
      await selectPreset();
      statusQueue.length = 0;
      await discardStatuses(3);
    }
    const idle = await measurePhase("idle", phaseSeconds);
    const attackDecay = await measurePhase("attackDecay", phaseSeconds, () => {
      notes.forEach((note) => sendNote("note_on", note));
    });
    await discardStatuses(settleSeconds);
    const sustain = await measurePhase("sustain", phaseSeconds);
    const release = await measurePhase("release", phaseSeconds, () => {
      notes.forEach((note) => sendNote("note_off", note));
    });
    const retrigger = await measurePhase("retrigger", phaseSeconds, async () => {
      notes.forEach((note) => sendNote("note_on", note));
      await new Promise((resolve) => setTimeout(resolve, 200));
      notes.forEach((note) => sendNote("note_off", note));
      await new Promise((resolve) => setTimeout(resolve, 200));
      notes.forEach((note) => sendNote("note_on", note));
    });
    notes.forEach((note) => sendNote("note_off", note));

    const phases = [idle, attackDecay, sustain, release, retrigger];
    const generationMinimum = Math.min(...phases.map((phase) => phase.reloadGenerationMinimum));
    const generationMaximum = Math.max(...phases.map((phase) => phase.reloadGenerationMaximum));
    const effectiveVoiceMinimum = Math.min(...phases.map((phase) => phase.voiceMaxMinimum));
    result.runs.push({
      requestedNotes: voices,
      effectiveVoiceMinimum,
      graphStable: generationMinimum === generationMaximum,
      validVoiceCount: voices <= effectiveVoiceMinimum && generationMinimum === generationMaximum,
      phases,
    });
  }

  if (stressSeconds > 0) {
    await selectPreset();
    statusQueue.length = 0;
    await discardStatuses(3);
    const notes = noteSet(Math.max(...voicePoints));
    result.stress = await measurePhase("stress", stressSeconds, () => {
      notes.forEach((note) => sendNote("note_on", note));
    });
    notes.forEach((note) => sendNote("note_off", note));
  }

  console.log(JSON.stringify(result, null, 2));
} finally {
  if (socket.readyState === WebSocket.OPEN) {
    allNotes.forEach((note) => sendNote("note_off", note));
    socket.close();
  }
  clearTimeout(timeout);
}
