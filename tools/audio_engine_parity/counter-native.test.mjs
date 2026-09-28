import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "../..");
const temporary = mkdtempSync(path.join(os.tmpdir(), "espressynth-counter-"));
mkdirSync(path.join(temporary, "nodes"));
// Compile the unchanged native render header and actual state declaration;
// shim only platform dependencies so the DSP loop can run on the host.
const state = readFileSync(path.join(root, "main/audio/engine/NodeState.h"), "utf8").match(/struct CounterState\s*\{[^}]+\};/)[0];
const clockState = readFileSync(path.join(root, "main/audio/engine/NodeState.h"), "utf8").match(/struct ClockState\s*\{[^}]+\};/)[0];
const monoflopState = readFileSync(path.join(root, "main/audio/engine/NodeState.h"), "utf8").match(/struct MonoflopState\s*\{[^}]+\};/)[0];
const drumStates = ["DrumSequencerPattern", "DrumSequencerState"].map((name) => readFileSync(path.join(root, "main/audio/engine/NodeState.h"), "utf8").match(new RegExp(`struct ${name}\\s*\\{[\\s\\S]*?\\n\\};`))[0]).join("\n");
writeFileSync(path.join(temporary, "NodeInstance.h"), `#pragma once
#include <cstddef>
#include <cstdint>
#include "SynthConfig.h"
${state}
${clockState}
${monoflopState}
${drumStates}
namespace nodes::steparp { struct StepArpState; }
namespace synth { inline uint32_t portBit(int port) { return 1u << port; } }
struct NodeInstance {
  CounterState counter;
  ClockState clock;
  MonoflopState monoflop;
  nodes::steparp::StepArpState *stepArp = nullptr;
  DrumSequencerState *drumSequencer = nullptr;
  uint32_t audioConnectedPortMask = 0, controlConnectedPortMask = 0;
  const float *inPorts[24] = {};
  float controlValue[24] = {};
  float *outL = nullptr;
  float *outR = nullptr;
};
inline float *outputPortBuffer(NodeInstance &inst, uint8_t port) { return port == 0 ? inst.outL : port == 1 ? inst.outR : nullptr; }
namespace noderender {
inline bool controlPortConnected(const NodeInstance &inst, int port) { return (inst.controlConnectedPortMask & synth::portBit(port)) != 0; }
inline float portSample(const NodeInstance &inst, int port, size_t i) {
  return inst.inPorts[port] ? inst.inPorts[port][i] : inst.controlValue[port];
}
}
`);
const cvSource = readFileSync(path.join(root, "main/audio/engine/CvConventions.h"), "utf8");
const cvToRange = cvSource.slice(cvSource.indexOf("inline float cvToRange("), cvSource.indexOf("// LUT-accelerated"));
writeFileSync(path.join(temporary, "CvConventions.h"), `#pragma once\n#include <cmath>\nnamespace cv { constexpr float kGateThresholdVolts = 5.0f, kGateOnVolts = 10.0f, kGateOffVolts = 0.0f, kMaxVolts = 10.0f, kInvMaxVolts = 0.1f; ${cvToRange} }\n`);
writeFileSync(path.join(temporary, "MasterClock.h"), "#pragma once\nnamespace synth { struct MasterClock { float bpm = 60, maxBpm = 240; bool roundBpm = false; }; inline MasterClock &masterClock() { static MasterClock value; return value; } }\n");
writeFileSync(path.join(temporary, "SynthConfig.h"), "#pragma once\nnamespace synth { constexpr int kSampleRate = 48000, kMaxAudioInPorts = 24, kStepArpMaxPatterns = 8, kDrumSequencerMaxSteps = 16, kDrumSequencerMaxTracks = 8, kDrumSequencerMaxCells = 128, kDrumSequencerMaxPatterns = 8; }\n");
const stepParams = readFileSync(path.join(root, "main/audio/engine/PresetParser.h"), "utf8").split(/\r?\n/).filter((line) => /^\s+(float|bool|uint8_t)\s+stepArp/.test(line)).join("\n");
writeFileSync(path.join(temporary, "PresetParser.h"), `#pragma once
#include <cstring>
#include "SynthConfig.h"
struct cJSON { double valuedouble = 0; bool snapToSemitones = false; };
inline bool cJSON_IsArray(cJSON *) { return false; }
inline bool cJSON_IsNumber(const cJSON *) { return false; }
inline cJSON *cJSON_GetArrayItem(cJSON *, int) { return nullptr; }
inline cJSON *cJSON_GetObjectItemCaseSensitive(cJSON *, const char *) { return nullptr; }
#define cJSON_ArrayForEach(element, array) for (element = nullptr; element != nullptr;)
struct WidgetParams { float clockPulseMs = 100, monoflopThreshold = 5, monoflopPulseMs = 10; ${stepParams} };
namespace presetparser {
inline float getFloat(cJSON *, const char *, float fallback) { return fallback; }
inline int getInt(cJSON *, const char *, int fallback) { return fallback; }
inline uint8_t getFloatArray(cJSON *, const char *, float *, uint8_t) { return 0; }
inline bool getBool(cJSON *p, const char *name, bool fallback) { return p && strcmp(name, "snapToSemitones") == 0 ? p->snapToSemitones : fallback; }
}
`);
writeFileSync(path.join(temporary, "esp_heap_caps.h"), "#pragma once\n#include <cstdlib>\ninline void heap_caps_free(void *value) { std::free(value); }\n");
// Copy the real state/render sections unchanged; ESP build validates allocation/JSON sections.
for (const [file, namespace] of [["StepArpeggiatorNode.h", "steparp"], ["DrumSequencerNode.h", "drumsequencer"]]) {
  const source = readFileSync(path.join(root, "main/audio/engine/nodes", file), "utf8");
  writeFileSync(path.join(temporary, "nodes", file), file === "StepArpeggiatorNode.h" ? source : source.slice(0, source.indexOf("inline void build(")) + `\n} // namespace nodes::${namespace}\n`);
}
copyFileSync(path.join(root, "main/audio/engine/nodes/CounterNode.h"), path.join(temporary, "nodes/CounterNode.h"));
copyFileSync(path.join(root, "main/audio/engine/nodes/ClockNode.h"), path.join(temporary, "nodes/ClockNode.h"));
copyFileSync(path.join(root, "main/audio/engine/nodes/MonoflopNode.h"), path.join(temporary, "nodes/MonoflopNode.h"));
copyFileSync(path.join(here, "counter-native.cpp"), path.join(temporary, "counter-native.cpp"));
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
  run("cmd.exe", ["/d", "/c", `call "${vcvars}" >nul && cl /nologo /EHsc /std:c++17 /Fe:counter-native.exe counter-native.cpp`]);
  console.log(run(path.join(temporary, "counter-native.exe"), []).trim());
} else {
  run(process.env.CXX || "c++", ["-std=c++17", "counter-native.cpp", "-o", "counter-native"]);
  console.log(run(path.join(temporary, "counter-native"), []).trim());
}
