import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const graph = readFileSync(path.join(root, "main/audio/engine/Graph.h"), "utf8");
const engine = readFileSync(path.join(root, "main/audio/engine/SynthEngine.h"), "utf8");
const manager = readFileSync(path.join(root, "main/features/synth/synth_manager.hpp"), "utf8");
function method(source, signature) {
  const start = source.indexOf(signature);
  if (start < 0) throw new Error(`Missing ${signature}`);
  let end = source.indexOf("{", start), depth = 1;
  while (depth && ++end < source.length) {
    if (source[end] === "{") ++depth;
    if (source[end] === "}") --depth;
  }
  if (depth) throw new Error(`Unclosed ${signature}`);
  return source.slice(start, end + 1);
}
const declaration = name => graph.match(new RegExp(`struct ${name}\\s*\\{[\\s\\S]*?\\n\\};`))[0];
const temporary = mkdtempSync(path.join(os.tmpdir(), "espressynth-control-backpressure-"));
writeFileSync(path.join(temporary, "test.cpp"), `
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <cstdarg>
constexpr size_t kMaxMidiCcDebugTargets=16;
enum class NodeType : uint8_t { Unknown };
${declaration("MidiCcDebugTarget")}
${declaration("MidiCcDebugSnapshot")}
${readFileSync(path.join(root, "main/features/synth/KnobActivity.h"), "utf8").replace("#pragma once", "")}
constexpr int pdTRUE=1, MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
struct Queue { size_t capacity,size; std::deque<std::vector<uint8_t>> data; };
using QueueHandle_t=Queue *;
int xQueueSend(Queue *q,const void *p,int) { if(q->data.size()==q->capacity)return 0; const auto *b=static_cast<const uint8_t *>(p); q->data.emplace_back(b,b+q->size); return pdTRUE; }
int xQueueReceive(Queue *q,void *p,int) { if(q->data.empty())return 0; std::memcpy(p,q->data.front().data(),q->size); q->data.pop_front(); return pdTRUE; }
int64_t nowUs=200000;
int64_t esp_timer_get_time() { return nowUs; }
void *heap_caps_calloc(size_t count,size_t size,int) { return std::calloc(count,size); }
std::vector<std::string> logs;
void logLine(const char *,const char *format,...) { char text[512]; va_list args; va_start(args,format); std::vsnprintf(text,sizeof(text),format,args); va_end(args); logs.emplace_back(text); }
#define ESP_LOGI logLine
#define ESP_LOGW logLine
struct Engine {
 Queue console{16,sizeof(MidiCcDebugSnapshot)},display{512,sizeof(KnobActivity)};
 QueueHandle_t midiCcDebugQueue_=&console,knobActivityQueue_=&display;
 std::atomic<uint32_t> controlDebugDropped_{0};
 uint32_t presetChangeSequence() { return 1; }
 ${method(engine, "bool readMidiCcDebugSnapshot(")}
 ${method(engine, "bool readKnobActivity(")}
 ${method(engine, "uint32_t takeControlDebugDropped(")}
 ${method(engine, "void publishMidiCcDebugSnapshot(")}
} instance;
Engine &engine() { return instance; }
${method(manager, "inline void log_midi_cc_debug_tick(")}
MidiCcDebugSnapshot event(int control,int raw,const char *port="pitchCv") {
 MidiCcDebugSnapshot s{}; s.isKnob=true; s.controlIndex=control; s.rawValue=raw; s.sourceValue=raw;
 std::strcpy(s.sourceId,"knob-a"); std::strcpy(s.sourceLabel,"Knob"); s.targetCount=1;
 std::strcpy(s.targets[0].targetId,"osc-a"); std::strcpy(s.targets[0].targetLabel,"Oscillator");
 std::strcpy(s.targets[0].paramName,port); s.targets[0].value=raw; return s;
}
int main() {
 // Flood both real publication queues: newest end value survives diagnostic backpressure.
 for(int i=0;i<1000;++i) instance.publishMidiCcDebugSnapshot(event(0,i%128));
 assert(instance.console.data.size()==16 && instance.display.data.size()==512);
 KnobActivity activity{}; while(instance.readKnobActivity(activity)) {}
 assert(activity.raw==103 && activity.updatedUs==nowUs && instance.controlDebugDropped_==0);
 log_midi_cc_debug_tick();
 assert(logs.size()==2 && logs[0].find("raw=103")!=std::string::npos &&
        logs[1].find("Oscillator.pitchCv = 103.000")!=std::string::npos);
 // Separate knobs/ports/layers and continuation pages must not collapse together.
 logs.clear(); nowUs+=50000;
 instance.publishMidiCcDebugSnapshot(event(0,10));
 instance.publishMidiCcDebugSnapshot(event(1,20));
 instance.publishMidiCcDebugSnapshot(event(0,30));
 auto continuation=event(0,30,"gain"); continuation.continuation=true;
 instance.publishMidiCcDebugSnapshot(continuation);
 instance.publishMidiCcDebugSnapshot(event(16,40));
 log_midi_cc_debug_tick(); assert(logs.empty());
 nowUs+=150000; log_midi_cc_debug_tick();
 assert(logs.size()==7 && logs[0].find("raw=30")!=std::string::npos &&
        logs[2].find("raw=20")!=std::string::npos && logs[4].find("Oscillator.gain")!=std::string::npos &&
        logs[5].find("raw=40")!=std::string::npos);
 // Switch transitions are discrete: preserve both console packets.
 logs.clear(); nowUs+=200000;
 auto sw=event(0,0); sw.isKnob=false; instance.publishMidiCcDebugSnapshot(sw);
 sw.sourceValue=1; instance.publishMidiCcDebugSnapshot(sw);
 log_midi_cc_debug_tick(); assert(logs.size()==4);
 std::puts("Native control backpressure: latest final value, bounded logs, distinct controls/layers/ports, fan-out and switch transitions passed");
}
`);
const run = (command, args) => {
  const result = spawnSync(command, args, { cwd: temporary, encoding: "utf8", windowsVerbatimArguments: command === "cmd.exe" });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(result.stdout + result.stderr);
  return result.stdout;
};
if (process.platform === "win32") {
  const vswhere = path.join(process.env["ProgramFiles(x86)"], "Microsoft Visual Studio/Installer/vswhere.exe");
  const installation = run(vswhere, ["-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"]).trim();
  const vcvars = path.join(installation, "VC/Auxiliary/Build/vcvars64.bat");
  run("cmd.exe", ["/d", "/c", `call "${vcvars}" >nul && cl /nologo /EHsc /std:c++17 /Fe:test.exe test.cpp`]);
  console.log(run(path.join(temporary, "test.exe"), []).trim());
} else {
  run(process.env.CXX || "c++", ["-std=c++17", "test.cpp", "-o", "test"]);
  console.log(run(path.join(temporary, "test"), []).trim());
}
