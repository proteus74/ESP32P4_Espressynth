import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const graph = readFileSync(path.join(root, "main/audio/engine/Graph.h"), "utf8");
const engine = readFileSync(path.join(root, "main/audio/engine/SynthEngine.h"), "utf8");
const declaration = (name) => graph.match(new RegExp(`struct ${name}\\s*\\{[\\s\\S]*?\\n\\};`))[0];
const methods = graph.slice(graph.indexOf("inline void copyMidiCcDebugText("), graph.indexOf("// Delivers one note-on/off event's control routes")) +
  graph.slice(graph.indexOf("inline void Graph::recordControlDebug("), graph.indexOf("inline void Graph::handleMidiEvent("));
// Check entry points in addition to executing the unchanged native logging code.
assert.match(graph, /recordControlDebug\(w, controlIndex, false, ccChannel0, ccNumber, ccValue\)/);
assert.match(graph, /recordControlDebug\(static_cast<uint16_t>\(widgetIdx\), knobControlIndex, true\)/);
assert.match(graph, /recordControlDebug\(w, 0, false, ccChannel0, ccNumber, ccValue\)/);
assert.match(graph, /selectOut2[\s\S]*recordControlDebug\(static_cast<uint16_t>\(widgetIdx\), 0, true\)/);
assert.ok(engine.indexOf("liveGraph_->renderBlock(gout") < engine.indexOf("liveGraph_->flushControlDebug("));

const temporary = mkdtempSync(path.join(os.tmpdir(), "espressynth-control-log-"));
writeFileSync(path.join(temporary, "test.cpp"), `
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
namespace synth { constexpr int kBlockSize=4, kMaxWidgets=64, kMaxAudioInPorts=22, kKnobControlMax=16, kButtonControlMax=16; inline uint32_t portBit(int port) { return 1u << port; } }
enum class NodeType { Unknown, Knob, Button, Switch, OutputSwitch, Gain, Multiplier };
struct Control { float currentValue=0, raw=0; char label[48]={}; };
struct Controls { uint8_t activeLayer=0; Control controls[64]; };
struct NodeInstance {
 uint16_t widgetIndex=0;
 uint32_t audioConnectedPortMask=0;
 Controls *knob=nullptr, *button=nullptr;
 struct { bool selectIn2=false; } switchState;
 struct { bool selectOut2=false; } outputSwitch;
 float controlValue[22]={}, *inPorts[22]={}, *outL=nullptr, *outR=nullptr;
};
inline float *outputPortBuffer(NodeInstance &inst, uint8_t port) { return port ? inst.outR : inst.outL; }
namespace noderender { inline float portSample(const NodeInstance &inst, int port, size_t sample) { return inst.inPorts[port] ? inst.inPorts[port][sample] : inst.controlValue[port]; } }
${["WidgetSlot", "MidiCcDebugTarget", "MidiCcDebugSnapshot", "ControlDebugEvent", "Edge", "ControlRoute"].map(declaration).join("\n").replace("MidiCcDebugTarget targets[kMaxMidiCcDebugTargets]", "MidiCcDebugTarget targets[16]")}
constexpr size_t kMaxMidiCcDebugTargets=16;
struct Graph {
 WidgetSlot widgets[64]; NodeInstance instances[64]; ControlRoute controlRoutes[256]; Edge edges[256];
 uint16_t widgetCount=0, controlRouteCount=0, edgeCount=0;
 uint8_t monitorVoiceIndex=0;
 ControlDebugEvent controlDebugEvents[32]; uint8_t controlDebugEventCount=0; uint32_t controlDebugDropped=0;
 void recordControlDebug(uint16_t, uint8_t, bool, uint8_t=0, uint8_t=0, uint8_t=0);
 void flushControlDebug(MidiCcDebugSnapshot &, void (*)(const MidiCcDebugSnapshot &, void *), void *);
};
${methods}
std::vector<MidiCcDebugSnapshot> packets;
void capture(const MidiCcDebugSnapshot &snapshot, void *) { packets.push_back(snapshot); }
void flush(Graph &g) { MidiCcDebugSnapshot scratch; packets.clear(); g.flushControlDebug(scratch, capture, nullptr); }
void widget(Graph &g, int i, NodeType type, const char *label) {
 g.widgets[i].type=type; g.widgets[i].instanceBase=i; g.widgets[i].instanceCount=1;
 std::snprintf(g.widgets[i].label,48,"%s",label); std::snprintf(g.widgets[i].id,24,"widget-%d",i);
 g.instances[i].widgetIndex=i; g.widgetCount=i+1;
}
void route(Graph &g, int from, int side, int to, int port, const char *name, bool raw=false) {
 auto &r=g.controlRoutes[g.controlRouteCount++]; r.sourceWidget=from; r.side=side;
 r.targetWidget=to; r.targetPort=port; r.kind=raw ? ControlRoute::Kind::KnobCc : ControlRoute::Kind::Knob;
 std::snprintf(r.paramName,16,"%s",name);
}
void edge(Graph &g,int from,int to,int port,const char *name,int fromPort=0) {
 auto &e=g.edges[g.edgeCount++]; e.fromInstance=from; e.toInstance=to; e.toPort=port; e.fromPort=fromPort;
 std::snprintf(e.paramName,16,"%s",name);
}
int main() {
 Graph g; Controls knobs; widget(g,0,NodeType::Knob,"Volume"); g.instances[0].knob=&knobs;
 knobs.controls[17].currentValue=7.5f; knobs.controls[17].raw=95;
 std::snprintf(knobs.controls[17].label,48,"My Pitch");
 widget(g,1,NodeType::Multiplier,"Multiply"); widget(g,2,NodeType::Gain,"Gain");
 widget(g,3,NodeType::Gain,"Raw target");
 route(g,0,17,1,0,"in1"); route(g,0,17,3,0,"amount",true);
 route(g,0,0,2,1,"unusedLayer");
 edge(g,1,2,0,"amount"); g.instances[1].controlValue[0]=7.5f;
 g.instances[2].controlValue[0]=3.75f; g.instances[3].controlValue[0]=95;
 g.recordControlDebug(0,17,false,2,74,95); flush(g);
 assert(packets.size()==1 && packets[0].targetCount==2);
 assert(std::strstr(packets[0].sourceLabel,"Knob 2 Layer 2"));
 assert(packets[0].channel==2 && packets[0].cc==74 && packets[0].rawValue==95);
 assert(packets[0].isKnob && packets[0].controlIndex==17);
 assert(std::string(packets[0].knobName)=="My Pitch" && std::string(packets[0].sourceId)=="widget-0");
 assert(std::string(packets[0].targets[0].targetId)=="widget-1");
 assert(packets[0].targets[0].value==7.5f && packets[0].targets[1].raw);
 assert(std::string(packets[0].targets[0].targetLabel)=="Multiply");
 assert(std::string(packets[0].targets[1].targetLabel)=="Raw target");
 // The downstream Gain must not appear when its upstream knob is operated.
 // Browser operation needs no MIDI binding and uses the same native graph.
 g.recordControlDebug(0,17,true); flush(g); assert(packets[0].fromUi && packets[0].targetCount==2);
 assert(packets[0].rawValue==95 && std::string(packets[0].knobName)=="My Pitch");
 // Unconnected knobs still identify the operated control.
 g.recordControlDebug(0,1,true); flush(g); assert(packets[0].targetCount==0);
 // Capture after rendering, not the old target value from before the event.
 g.recordControlDebug(0,17,true); g.instances[1].controlValue[0]=4.25f; flush(g);
 assert(packets[0].targets[0].value==4.25f);
 g.instances[1].controlValue[0]=7.5f;
 // A stale/unused DSP buffer must not hide a direct control parameter change.
 float stale[4]={0,0,0,0}; g.instances[1].inPorts[0]=stale; g.widgets[1].needed=false;
 g.recordControlDebug(0,17,true); flush(g); assert(packets[0].targets[0].value==7.5f);
 g.widgets[1].needed=true; g.instances[1].inPorts[0]=nullptr;
 // Selector state and its actual output, including downstream gain.
 Graph sw; float chosen[4]={9,9,9,9}; widget(sw,0,NodeType::Switch,"Select");
 sw.instances[0].switchState.selectIn2=true; sw.instances[0].outL=chosen;
 widget(sw,1,NodeType::Gain,"Amplifier"); sw.instances[1].inPorts[0]=chosen; edge(sw,0,1,0,"amount");
 sw.recordControlDebug(0,0,false,0,12,127); flush(sw);
 assert(std::strstr(packets[0].sourceLabel,"Switch In 2"));
 assert(packets[0].sourceValue==9 && packets[0].targets[0].value==9);
 sw.recordControlDebug(0,0,true); flush(sw); assert(packets[0].fromUi);
 widget(sw,2,NodeType::Gain,"Downstream"); edge(sw,1,2,0,"amount");
 sw.recordControlDebug(0,0,true); flush(sw);
 assert(packets[0].targetCount==1 && std::string(packets[0].targets[0].targetLabel)=="Amplifier");
 // Knob on the inactive selector input must not claim a downstream change.
 Graph inactive; widget(inactive,0,NodeType::Knob,"Knob"); inactive.instances[0].knob=&knobs;
 widget(inactive,1,NodeType::Switch,"Select"); widget(inactive,2,NodeType::Gain,"Gain");
 route(inactive,0,0,1,1,"in2"); edge(inactive,1,2,0,"amount");
 inactive.recordControlDebug(0,0,true); flush(inactive); assert(packets[0].targetCount==1);
 inactive.instances[1].switchState.selectIn2=true;
 inactive.recordControlDebug(0,0,true); flush(inactive); assert(packets[0].targetCount==1);
 // Both Output Switch branches are reported: the inactive branch becomes zero.
 sw.widgets[0].type=NodeType::OutputSwitch; sw.instances[0].outputSwitch.selectOut2=true;
 sw.instances[0].outR=chosen; sw.instances[1].controlValue[0]=0; sw.instances[1].inPorts[0]=nullptr;
 widget(sw,2,NodeType::Gain,"Second"); sw.instances[2].inPorts[0]=chosen; edge(sw,0,2,0,"amount",1);
 sw.recordControlDebug(0,0,true); flush(sw);
 assert(std::strstr(packets[0].sourceLabel,"Output Switch Out 2"));
 assert(packets[0].targetCount==2 && packets[0].targets[0].value==0 && packets[0].targets[1].value==9);
 // Fan-out beyond the former 16-target limit uses continuation packets.
 Graph many; widget(many,0,NodeType::Knob,"Many"); many.instances[0].knob=&knobs;
 for(int i=1;i<=33;++i) { widget(many,i,NodeType::Gain,"Target"); route(many,0,0,i,0,"amount"); }
 many.recordControlDebug(0,0,true); flush(many);
 assert(packets.size()==3 && packets[0].targetCount==16 && packets[1].targetCount==16 && packets[2].targetCount==1);
 assert(!packets[0].continuation && packets[1].continuation && packets[2].continuation);
 // Downstream cycles stay excluded; duplicate direct input/voice edges appear once.
 edge(g,2,1,1,"in2"); edge(g,1,2,0,"amount"); g.recordControlDebug(0,17,true); flush(g);
 assert(packets.size()==1 && packets[0].targetCount==2);
 // Rapid positions of one control coalesce; different controls retain identity.
 g.recordControlDebug(0,17,true); g.recordControlDebug(0,17,true); g.recordControlDebug(0,1,true);
 assert(g.controlDebugEventCount==2); flush(g); assert(packets.size()==2);
 for(int i=0;i<33;++i) g.recordControlDebug(0,i,true);
 assert(g.controlDebugEventCount==32 && g.controlDebugDropped==1);
 std::puts("Native control console: MIDI/UI knobs and switches, direct targets only, layers, fan-out, coalescing passed");
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
