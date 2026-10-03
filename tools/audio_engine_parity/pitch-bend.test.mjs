import assert from "node:assert/strict";
import { copyFileSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import vm from "node:vm";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const read = file => readFileSync(path.join(root, file), "utf8");
const engine = file => read(`main/audio/engine/${file}`);
function declaration(source, signature, offset = 0) {
  const start = source.indexOf(signature, offset);
  assert.ok(start >= 0, signature);
  let end = source.indexOf("{", start), depth = 1;
  while (depth && ++end < source.length) {
    if (source[end] === "{") ++depth;
    if (source[end] === "}") --depth;
  }
  assert.equal(depth, 0);
  return source.slice(start, end + 1);
}

// Execute the browser's real MIDI handler, cable delivery and settings save.
const ui = read("data/static/js/main.js");
const outputs = [], updates = [], listeners = new Map();
const types = { midi: "midi-in", osc: "oscillator", fm: "fm-oscillator", organ: "hammond", debug: "debug", knob: "knob" };
const context = {
  CV_MAX_VOLTS: 10, midiVoiceSlotStates: new Map([["midi", {}]]),
  document: {
    getElementById(id) {
      return { dataset: { widgetType: types[id] }, addEventListener: (event, callback) => listeners.set(id, callback) };
    }
  },
  connections: ["osc", "fm", "organ", "debug"].map(id => ({ from: { widgetId: "midi", portId: "pitchBend" }, to: { widgetId: id, portId: id === "debug" ? "in" : "tune" } })),
  getPortKind: () => "control", withDeviceSyncMuted: callback => callback(),
  deliverToWidget: (id, port, value) => outputs.push({ id, port, value }),
  clamp: (value, low, high) => Math.max(low, Math.min(high, value)),
  pendingMidiInId: "midi", midiInStates: new Map([["midi", {}]]),
  midiInChannelSelect: { value: "omni" }, midiInOverflowModeSelect: { value: "steal-oldest" },
  midiInPitchBendRangeInput: { value: "1" }, midiInPitchBendQuantizeInput: { checked: false },
  parseFloatFlexible: value => Number.parseFloat(value.replace(",", ".")), isFxMidiIn: () => false,
  sendLiveParamUpdate: (id, params) => updates.push(params), savePresetNow() {}, closeMidiInSettings() {}
};
vm.createContext(context);
vm.runInContext(declaration(ui, "function propagate(") + declaration(ui, "function routeMidiWidget("), context);
const saveStart = ui.indexOf('document.getElementById("midiInSettingsSave").addEventListener');
vm.runInContext(ui.slice(saveStart, ui.indexOf("\n});", saveStart) + 4), context);
for (const volts of [0, 1, 3, 5]) {
  context.midiInPitchBendRangeInput.value = String(volts);
  listeners.get("midiInSettingsSave")();
  assert.equal(updates.at(-1).pitchBendRangeCents, volts * 1200);
  for (const [bend, expected] of [[-8192, -volts], [0, 0], [8191, volts]]) {
    outputs.length = 0;
    context.routeMidiWidget({ midiType: "pitchwheel", value: bend }, "midi", context.midiInStates.get("midi"));
    for (const output of outputs) assert.ok(Math.abs(output.value - (output.id === "debug" ? expected : 5 + expected)) < 1e-9);
    assert.equal(outputs.length, 4);
  }
}
for (const [input, cents] of [["-1", 0], ["9", 6000], ["0,5", 600], ["invalid", 200], ["Infinity", 200]]) {
  context.midiInPitchBendRangeInput.value = input;
  listeners.get("midiInSettingsSave")();
  assert.equal(updates.at(-1).pitchBendRangeCents, cents);
}
context.midiInStates.set("midi", { pitchBendRangeCents: 4096, pitchBendQuantizeCents: true });
outputs.length = 0;
context.routeMidiWidget({ midiType: "pitchwheel", value: -1 }, "midi", context.midiInStates.get("midi"));
assert.equal(outputs.find(output => output.id === "debug").value, -1 / 1200, "Negative half-cent rounds away from zero, matching firmware");
context.midiInStates.get("midi").pitchBendRangeCents = 0.5;
outputs.length = 0;
context.routeMidiWidget({ midiType: "pitchwheel", value: 8191 }, "midi", context.midiInStates.get("midi"));
assert.equal(outputs.find(output => output.id === "debug").value, 0, "Quantization never exceeds the range or emits fractional cents");
outputs.length = 0;
context.connections = [{ from: { widgetId: "knob", portId: "voltage" }, to: { widgetId: "osc", portId: "tune" } }];
context.propagate("knob", "voltage", 3);
assert.equal(outputs[0].value, 3, "Ordinary Tune cables keep their existing scale");
console.log("PASS browser: voltage settings, symmetric endpoints, zero center, Tune adaptation and ordinary CV cables");

// Run the firmware's real route construction, seeding, bend event and WaveGen
// renderer on the host. Only graph storage and ESP allocation APIs are stubbed.
const directory = mkdtempSync(path.join(os.tmpdir(), "espressynth-pitch-bend-"));
writeFileSync(path.join(directory, "sdkconfig.h"), "#define CONFIG_AUDIO_SYNTH_MAX_VOICES 4\n#define CONFIG_AUDIO_SYNTH_BLOCK_SIZE 128\n");
writeFileSync(path.join(directory, "esp_attr.h"), "#define IRAM_ATTR\n");
writeFileSync(path.join(directory, "esp_heap_caps.h"), `#include <cstdlib>
constexpr int MALLOC_CAP_SPIRAM=1, MALLOC_CAP_INTERNAL=2, MALLOC_CAP_8BIT=4;
inline void *heap_caps_malloc(size_t size,int){return std::malloc(size);}
inline void heap_caps_free(void *value){std::free(value);}
`);
for (const file of ["cJSON.h", "cJSON.c"]) copyFileSync(path.join(root, "managed_components/espressif__cjson/cJSON", file), path.join(directory, file));
const graph = engine("Graph.h"), builder = engine("GraphBuilder.h"), oscillator = engine("nodes/OscillatorNode.h");
const node = engine("NodeInstance.h");
const seedStart = builder.lastIndexOf("    for (uint16_t r = 0; r < g->controlRouteCount; ++r)", builder.indexOf("float initial = 0.0f;"));
const nativeRoot = path.join(root, "main/audio/engine").replaceAll("\\", "/");
writeFileSync(path.join(directory, "test.cpp"), `
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include "cJSON.h"
#include "${nativeRoot}/CvConventions.h"
#include "${nativeRoot}/DeviceSettings.h"
${declaration(engine("NodeState.h"), "struct OscillatorState")};
struct MidiInState {int channel=-1;float pitchBendRangeCents=200;bool pitchBendQuantizeCents=false;};
struct NodeInstance {
 NodeType type=NodeType::Oscillator;OscillatorState osc;MidiInState midiIn;
 uint32_t controlConnectedPortMask=0,audioConnectedPortMask=0,controlRampPortMask=0,rampPortMask=0,constantPortMask=0;
 float controlValue[24]={},*inPorts[24]={},*outL=nullptr;
 bool outIsConstant=false;float outConstantValue=0;
 struct RampDescriptor {uint8_t port=255;float start=0,inc=0;};
 struct ConstDescriptor {uint8_t port=255;float value=0;};
 static constexpr int kMaxConstantPorts=2;ConstDescriptor constDesc[2];
 struct {float maxRateHz=50;} lfo;
 struct Control {float currentValue=0,raw=0;};struct Controls {Control controls[16];};
 Controls *knob=nullptr,*button=nullptr;
 struct UpDown {float currentOutput(){return 0;}} *upDownButton=nullptr;
 struct {float value=0;} constant;
};
namespace noderender {
 ${["inline float portSample(", "inline bool controlPortConnected(", "inline bool portIsFed(", "inline bool audioPortConstantForBlock("].map(signature => declaration(node, signature)).join("\n")}
 inline const NodeInstance::RampDescriptor *rampForPort(const NodeInstance &,int){return nullptr;}
}
namespace enginestats {struct {int oscPitchAudioRate=0,oscPitchConstant=0,oscTuneAudioRate=0,oscTuneConstant=0,oscSilentSkipped=0,oscGainConverged=0,oscAmpConstant=0,oscAmpAudioRate=0;} current;}
${oscillator.slice(oscillator.indexOf("namespace nodes::oscillator"), oscillator.indexOf("inline void build("))}
${declaration(oscillator, "inline bool setParam(")}
}
inline bool setFloatFieldByName(NodeInstance &inst,NodeType,const char *name,float value){return nodes::oscillator::setParam(inst,name,value);}
${declaration(graph, "struct ControlRoute")};
struct WidgetSlot {NodeType type=NodeType::MidiIn;uint16_t instanceBase=0,instanceCount=1;};
struct UsbMidiEvent {uint8_t codeIndex=0xE,status=0xE0,data1=0,data2=64;};
struct Graph {
 uint16_t widgetCount=3,controlRouteCount=0;WidgetSlot widgets[3];NodeInstance instances[4];ControlRoute controlRoutes[8];
 void bend(const UsbMidiEvent &event);
};
${declaration(graph, "inline void pushControlRouteValue(")}
void Graph::bend(const UsbMidiEvent &event){${declaration(graph, "if (event.codeIndex == 0xE)")}}
void connect(Graph *g,int toIdx,const char *port){
 int fromIdx=0,toPort=strcmp(port,"tune")==0?2:0;auto fromType=NodeType::MidiIn,toType=g->widgets[toIdx].type;
 struct {const char *fromPort,*toPort;} cd{"pitchBend",port};
 for(int once=0;once<1;++once){${declaration(builder, "if ((fromType == NodeType::MidiIn || fromType == NodeType::MidiFilter)")}}
}
void seed(Graph *g){${declaration(builder, "    for (uint16_t r = 0; r < g->controlRouteCount; ++r)", seedStart)}}
void liveRange(Graph &g,cJSON *item){
 auto &instances=g.instances;auto &slot=g.widgets[0];
 ${declaration(graph,"else if (slot.type == NodeType::MidiIn && strcmp(key, \"pitchBendRangeCents\")", graph.indexOf("inline bool Graph::applyLiveParams")).replace(/^else if[^]*?\{/, "{")}
}
void near(float actual,float expected){assert(std::fabs(actual-expected)<0.00001f);}
void send(Graph &g,int raw,int channel=0){UsbMidiEvent event;event.status|=channel;event.data1=raw&127;event.data2=(raw>>7)&127;g.bend(event);}
float frequency(NodeInstance &inst){
 inst.osc.phaseGen.resetPhase();inst.osc.gainSmooth.snapTo(1);inst.controlValue[0]=10;
 float samples[128];inst.outL=samples;int crossings=0;float previous=0,peak=0;
 for(int block=0;block<345;++block){nodes::oscillator::render(inst,128);for(float sample:samples){if(previous<=0 && sample>0)++crossings;previous=sample;peak=std::fmax(peak,std::fabs(sample));}}
 assert(peak>4.9f);return crossings*static_cast<float>(synth::kSampleRate)/(345*128);
}
int main(){
 dsp::initSineLut();dsp::initPow2Lut();Graph g;
 g.widgets[0].type=NodeType::MidiIn;g.widgets[0].instanceBase=0;
 g.widgets[1].type=NodeType::Oscillator;g.widgets[1].instanceBase=1;g.widgets[1].instanceCount=2;
 g.widgets[2].type=NodeType::Unknown;g.widgets[2].instanceBase=3;
 connect(&g,1,"tune");connect(&g,2,"in");seed(&g);
 assert(strcmp(g.controlRoutes[0].paramName,"tune")==0);
 near(g.instances[1].osc.tuneVolts,5);near(g.instances[2].osc.tuneVolts,5);near(g.instances[3].controlValue[0],0);
 auto &m=g.instances[0].midiIn;
 for(float volts:{0.0f,1.0f,3.0f,5.0f}){
  m.pitchBendRangeCents=volts*1200;
  for(int raw:{0,8192,16383}){send(g,raw);float expected=raw==0?-volts:(raw==8192?0:volts);
   near(g.instances[3].controlValue[0],expected);
   for(int voice:{1,2}){near(g.instances[voice].osc.tuneVolts,5+expected);near(g.instances[voice].controlValue[2],5+expected);}
  }
 }
 m.pitchBendRangeCents=1200;send(g,8192);float center=frequency(g.instances[1]);
 send(g,16383);float up=frequency(g.instances[1]);send(g,0);float down=frequency(g.instances[1]);
 assert(std::fabs(center-261.6256f)<2 && std::fabs(up-center*2)<3 && std::fabs(down-center/2)<2);
 printf("PASS firmware audio: center %.2f Hz, +1 V %.2f Hz, -1 V %.2f Hz\\n",center,up,down);
 m.channel=1;send(g,8192,0);near(g.instances[1].osc.tuneVolts,4);send(g,8192,1);near(g.instances[1].osc.tuneVolts,5);
 m.channel=-1;m.pitchBendRangeCents=200;m.pitchBendQuantizeCents=true;send(g,12001);
 near(g.instances[3].controlValue[0]*1200,std::round(g.instances[3].controlValue[0]*1200));
 m.pitchBendRangeCents=4096;send(g,8191);near(g.instances[3].controlValue[0],-1.0f/1200);
 m.pitchBendRangeCents=0.5f;send(g,16383);near(g.instances[3].controlValue[0],0);
 cJSON *item=cJSON_CreateNumber(9000);liveRange(g,item);near(m.pitchBendRangeCents,6000);
 item->valuedouble=-1;liveRange(g,item);near(m.pitchBendRangeCents,0);
 item->valuedouble=std::numeric_limits<double>::quiet_NaN();liveRange(g,item);near(m.pitchBendRangeCents,200);cJSON_Delete(item);
 g.controlRoutes[0].kind=ControlRoute::Kind::Knob;pushControlRouteValue(g,0,ControlRoute::Kind::Knob,3);near(g.instances[1].osc.tuneVolts,3);
 puts("PASS firmware: actual MIDI routes, neutral seed, all voices, 0/1/3/5 V endpoints, channel, quantization, range validation and ordinary Tune CV");
}
`);
function run(command, args) {
  const result = spawnSync(command, args, { cwd: directory, encoding: "utf8", windowsVerbatimArguments: command === "cmd.exe", timeout: 60000 });
  if (result.error) throw result.error;
  assert.equal(result.status, 0, result.stdout + result.stderr);
  return result.stdout;
}
if (process.platform === "win32") {
  const vswhere = path.join(process.env["ProgramFiles(x86)"], "Microsoft Visual Studio/Installer/vswhere.exe");
  const installation = run(vswhere, ["-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"]).trim();
  run("cmd.exe", ["/d", "/c", `call "${path.join(installation, "VC/Auxiliary/Build/vcvars64.bat")}" >nul && cl /nologo /EHsc /std:c++17 /I. /Fe:test.exe test.cpp cJSON.c`]);
  console.log(run(path.join(directory, "test.exe"), []).trim());
} else {
  run(process.env.CXX || "c++", ["-std=c++17", "-I.", "test.cpp", "cJSON.c", "-o", "test"]);
  console.log(run(path.join(directory, "test"), []).trim());
}
