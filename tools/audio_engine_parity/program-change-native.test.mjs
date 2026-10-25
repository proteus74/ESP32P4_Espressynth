import assert from "node:assert/strict";
import { copyFileSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const read = file => readFileSync(path.join(root, "main/audio/engine", file), "utf8");
function declaration(source, signature) {
  const start = source.indexOf(signature);
  assert.ok(start >= 0, signature);
  let end = source.indexOf("{", start), depth = 1;
  while (depth && ++end < source.length) {
    if (source[end] === "{") ++depth;
    if (source[end] === "}") --depth;
  }
  assert.equal(depth, 0);
  return source.slice(start, end + 1);
}
const graph = read("Graph.h"), builder = read("GraphBuilder.h"), button = read("nodes/ButtonNode.h");
const midiIn = read("nodes/MidiInNode.h"), parser = read("PresetParser.h"), state = read("NodeState.h");
const start = graph.indexOf("    uint8_t buttonMidiType", graph.indexOf("inline void Graph::handleMidiEvent"));
const end = graph.indexOf("    // Control Change: knob CC-learn/mapping", start);
assert.ok(start > 0 && end > start);
const directory = mkdtempSync(path.join(os.tmpdir(), "espressynth-program-change-"));
for (const file of ["cJSON.h", "cJSON.c"]) copyFileSync(path.join(root, "managed_components/espressif__cjson/cJSON", file), path.join(directory, file));
const seedStart = builder.lastIndexOf("    for (uint16_t r = 0; r < g->controlRouteCount; ++r)", builder.indexOf("float initial = 0.0f;"));
const parserFns = { getFloat: "inline float getFloat(", getInt: "inline int getInt(", getString: "inline const char *getString(",
  getFloatArray: "inline uint8_t getFloatArray(", getBool: "inline bool getBool(" };
writeFileSync(path.join(directory, "test.cpp"), `
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <initializer_list>
#include "cJSON.h"
#include "${path.join(root, "main/audio/engine/NodeTypes.h").replaceAll("\\", "/")}"
namespace synth {constexpr int kValueListMax=16,kButtonControlMax=16,kMaxConnections=16;inline unsigned portBit(int p){return 1u<<p;}
struct Settings{float volume=0;};Settings settings;Settings &deviceSettings(){return settings;}float clampDeviceVolume(float v){return v;}}
namespace cv {constexpr float kMaxVolts=10;}
namespace presetparser {${Object.values(parserFns).map(signature => declaration(parser, signature)).join("\n")}}
namespace nodes::button {
constexpr uint8_t kButtonMidiCc=0,kButtonMidiNote=1,kButtonMidiProgram=2;
${declaration(button, "struct ButtonControl")};
${declaration(button, "struct UpDownButtonState")};
struct ButtonState {uint8_t count=1,activeLayer=0;ButtonControl controls[32];static uint8_t controlIndex(uint8_t b,uint8_t l){return l*16+b;}};
}
using namespace nodes::button;
${declaration(state, "inline bool parseKeyMaskHex(")}
${declaration(state, "struct MidiInState")};
struct WidgetParams {int8_t channel=-1;uint8_t voices=1;OverflowMode overflowMode=OverflowMode::StealOldest;bool glide=false;
float glideTime=0,midiInPitchBendRangeCents=200;bool midiInPitchBendQuantizeCents=false;float midiInProgram=0;
uint32_t midiInKeyMask[4]={0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu};
int16_t upDownCcUp=-1,upDownCcDown=-1;int8_t upDownChannelUp=-1,upDownChannelDown=-1;
uint8_t upDownMidiTypeUp=0,upDownMidiTypeDown=0;float upDownMin=0,upDownMax=10,upDownStep=1,upDownCurrentValue=0;
float valueList[16]={};uint8_t valueListCount=0,upDownListIndex=0;};
struct NodeInstance {NodeType type=NodeType::Unknown;ButtonState *button=nullptr;UpDownButtonState *upDownButton=nullptr;
MidiInState midiIn;struct Controls{struct Control{float currentValue=0,raw=0;}controls[32];}*knob=nullptr;
struct{float value=0;}constant;struct{float maxRateHz=50;}lfo;float controlValue[24]={};unsigned controlConnectedPortMask=0;};
namespace nodes::button {${declaration(button,"inline void parsePresetUpDownButton(")}${declaration(button,"inline void buildUpDownButton(")}}
namespace nodes::midiin {${declaration(midiIn, "inline void build(")}${declaration(midiIn, "inline void parsePreset(")}}
inline bool setFloatFieldByName(NodeInstance &inst,NodeType,const char*,float value){inst.controlValue[1]=value;return true;}
${declaration(graph,"struct ControlRoute")};
struct WidgetSlot {NodeType type=NodeType::Unknown;uint16_t instanceBase=0,instanceCount=1,routeStart=0,routeCount=0;};
struct UsbMidiEvent {uint8_t codeIndex,status,data1,data2;};
struct Graph {uint16_t widgetCount=5,controlRouteCount=0;WidgetSlot widgets[5];NodeInstance instances[6];ControlRoute controlRoutes[16];
void recordControlDebug(uint16_t,uint8_t,bool,uint8_t,uint8_t,uint8_t){};void handleMidiEvent(const UsbMidiEvent &event);};
${declaration(graph,"inline void pushControlRouteValue(")}
void Graph::handleMidiEvent(const UsbMidiEvent &event){${graph.slice(start,end)}}
void connect(Graph *g){int fromIdx=2,toIdx=3,toPort=0;auto fromType=NodeType::MidiIn,toType=NodeType::Gain;struct{const char *fromPort,*toPort;}cd{"program","gain"};
for(int once=0;once<1;++once){${declaration(builder,"if ((fromType == NodeType::MidiIn || fromType == NodeType::MidiFilter) &&")}}
g->widgets[fromIdx].routeCount=g->controlRouteCount;}
void seed(Graph *g){${declaration(builder.slice(seedStart),"    for (uint16_t r = 0; r < g->controlRouteCount; ++r)")}}
float parsedProgram(const char *text){cJSON *json=cJSON_Parse(text);WidgetParams p;nodes::midiin::parsePreset(json,p);cJSON_Delete(json);return p.midiInProgram;}
int main(){
assert(nodeTypeFromString("midi-program")==NodeType::Unknown);
Graph g;ButtonState b;UpDownButtonState ud;for(int i=0;i<5;++i)g.widgets[i].instanceBase=i;
g.widgets[0].type=NodeType::Button;g.instances[0].button=&b;
b.controls[0].midiType=kButtonMidiProgram;b.controls[0].cc=5;b.controls[0].channel=2;
g.widgets[1].type=NodeType::UpDownButton;g.instances[1].upDownButton=&ud;
cJSON *json=cJSON_Parse(R"({"ccUp":10,"channelUp":2,"midiTypeUp":"program","ccDown":11,"channelDown":2,"midiTypeDown":"program"})");
WidgetParams p;nodes::button::parsePresetUpDownButton(json,p);nodes::button::buildUpDownButton(g.instances[1],p);cJSON_Delete(json);
assert(ud.midiTypeUp==kButtonMidiProgram && ud.midiTypeDown==kButtonMidiProgram);
g.widgets[2].type=NodeType::MidiIn;g.widgets[3].instanceCount=2;
json=cJSON_Parse(R"({"channel":2,"program":73})");WidgetParams m;nodes::midiin::parsePreset(json,m);nodes::midiin::build(g.instances[2],m);cJSON_Delete(json);
assert(g.instances[2].midiIn.program==73);
assert(parsedProgram("{}")==0 && parsedProgram(R"({"program":300})")==127 && parsedProgram(R"({"program":-4})")==0 && parsedProgram(R"({"program":12.6})")==13);
connect(&g);seed(&g);assert(g.controlRouteCount==1 && g.controlRoutes[0].kind==ControlRoute::Kind::MidiProgram);
for(int v:{3,4})assert(g.instances[v].controlValue[0]==73);
auto send=[&](uint8_t number,uint8_t ch=2,uint8_t type=0xC,uint8_t value=127){g.handleMidiEvent({type,static_cast<uint8_t>((type<<4)|ch),number,value});};
send(5);assert(b.controls[0].wasAbove);send(6);send(5,1);send(5,2,0xB);assert(b.controls[0].wasAbove);send(5);assert(!b.controls[0].wasAbove);
b.controls[0].valueListCount=3;b.controls[0].valueList[1]=4;b.controls[0].valueList[2]=7;
send(5);send(5);assert(b.controls[0].currentValue==7);send(5);assert(b.controls[0].currentValue==0);
send(10);send(10);send(11,1);send(10,2,0xB);send(12);assert(ud.currentValue==2);send(11);send(11);send(11);assert(ud.currentValue==0);
ud.valueListCount=3;ud.valueList[1]=4;ud.valueList[2]=7;send(10);send(10);send(10);assert(ud.currentOutput()==7);send(11);assert(ud.currentOutput()==4);
json=cJSON_Parse(R"({"ccUp":10,"channelUp":2})");WidgetParams legacy;nodes::button::parsePresetUpDownButton(json,legacy);nodes::button::buildUpDownButton(g.instances[1],legacy);cJSON_Delete(json);
send(10);assert(ud.currentValue==0);send(10,2,0xB,0);assert(ud.currentValue==1);
for(int number:{0,73,127}){send(number);for(int v:{3,4})assert(g.instances[v].controlValue[0]==number);}
send(3,1);send(4,2,0xB);assert(g.instances[2].midiIn.program==127);
g.instances[2].midiIn.channel=15;send(73,2);assert(g.instances[2].midiIn.program==127);send(73,15);assert(g.instances[2].midiIn.program==73);
g.instances[2].midiIn.channel=-1;send(9,4);assert(g.instances[2].midiIn.program==9);for(int v:{3,4})assert(g.instances[v].controlValue[0]==9);
puts("PASS native Program Change: Midi-In program output (channel/omni filter, preset parse/clamp/build, route construction/seeding, all target voices, raw 0-127), button/up-down program bindings and CC compatibility");
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
