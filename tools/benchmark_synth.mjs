const options = Object.fromEntries(
  process.argv.slice(2).map((argument) => {
    const [key, value = "true"] = argument.replace(/^--/, "").split("=", 2);
    return [key, value];
  }),
);

const host = options.host ?? "192.168.1.121";
const voices = Number(options.voices ?? 16);
const seconds = Number(options.seconds ?? 10);
const baseNote = Number(options.baseNote ?? 36);

if (!Number.isInteger(voices) || voices < 1 || voices > 32) {
  throw new Error("--voices must be an integer from 1 to 32");
}
if (!Number.isInteger(seconds) || seconds < 1) {
  throw new Error("--seconds must be a positive integer");
}

const socket = new WebSocket(`ws://${host}/ws`);
const cpuQueue = [];
const cpuWaiters = [];
let latestNodeCpu = [];

socket.addEventListener("message", (event) => {
  const message = JSON.parse(event.data);
  if (message.type === "nodeCpu") {
    latestNodeCpu = message.w;
    return;
  }
  if (message.type !== "cpuLoad") return;
  if (cpuWaiters.length > 0) cpuWaiters.shift()(message);
  else cpuQueue.push(message);
});

const opened = new Promise((resolve, reject) => {
  socket.addEventListener("open", resolve, { once: true });
  socket.addEventListener("error", () => reject(new Error("WebSocket connection failed")), { once: true });
});

function nextCpuSample() {
  if (cpuQueue.length > 0) return Promise.resolve(cpuQueue.shift());
  return new Promise((resolve) => cpuWaiters.push(resolve));
}

function sendNote(midiType, note) {
  socket.send(JSON.stringify({
    type: "midiTest",
    midiType,
    channel: 0,
    note,
    velocity: midiType === "note_on" ? 90 : 0,
  }));
}

const timeout = setTimeout(() => {
  console.error("Benchmark timed out");
  process.exit(2);
}, (seconds + 15) * 1000);

await opened;
const baseline = await nextCpuSample();
const notes = Array.from({ length: voices }, (_, index) => baseNote + index);
notes.forEach((note) => sendNote("note_on", note));

const samples = [];
for (let second = 0; second < seconds; ++second) {
  samples.push(await nextCpuSample());
}

notes.forEach((note) => sendNote("note_off", note));
await nextCpuSample();
await nextCpuSample();

const last = samples.at(-1);
const sortedCore1 = samples.map((sample) => sample.core1).sort((left, right) => left - right);
const result = {
  host,
  voices,
  seconds,
  core1Average: Number((samples.reduce((sum, sample) => sum + sample.core1, 0) / samples.length).toFixed(1)),
  core1Median: sortedCore1[Math.floor(sortedCore1.length / 2)],
  core1Maximum: Math.max(...sortedCore1),
  p95WorstUs: Math.max(...samples.map((sample) => sample.p95Us)),
  p99WorstUs: Math.max(...samples.map((sample) => sample.p99Us)),
  peakWorstUs: Math.max(...samples.map((sample) => sample.peakUs)),
  budgetUs: last.budgetUs,
  overrunDelta: last.renderOverruns - baseline.renderOverruns,
  underrunDelta: last.underruns - baseline.underruns,
  midiDropDelta: last.midiDrops - baseline.midiDrops,
  generationStart: baseline.reloadGeneration,
  generationEnd: last.reloadGeneration,
  nodeCpu: latestNodeCpu,
};

console.log(JSON.stringify(result, null, 2));
clearTimeout(timeout);
socket.close();