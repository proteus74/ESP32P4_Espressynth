import { copyFileSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const temporary = mkdtempSync(path.join(os.tmpdir(), "espressynth-control-memory-"));
const knob = readFileSync(path.join(root, "main/audio/engine/nodes/KnobNode.h"), "utf8");
const button = readFileSync(path.join(root, "main/audio/engine/nodes/ButtonNode.h"), "utf8");
const engine = readFileSync(path.join(root, "main/audio/engine/SynthEngine.h"), "utf8");
const usb = readFileSync(path.join(root, "main/features/usb/host/usb_midi_host.hpp"), "utf8");
const masterClock = readFileSync(path.join(root, "main/audio/engine/MasterClock.h"), "utf8");
const presetFill = readFileSync(path.join(root, "main/audio/engine/PresetFill.h"), "utf8");
function method(source, signature) {
 const start=source.indexOf(signature);if(start<0)throw new Error(signature);
 let end=source.indexOf("{",start),depth=1;
 while(depth && ++end<source.length){if(source[end]==="{")++depth;if(source[end]==="}")--depth;}
 return source.slice(start,end+1);
}
const states = (source, name, render) => source.slice(source.indexOf(`namespace nodes::${name}`), source.indexOf(render)) + "}\n";
const methods = (source, name, render) => `namespace nodes::${name} {\n` + source.slice(source.indexOf(render), source.indexOf("inline void parsePreset")) + "}\n";
copyFileSync(path.join(root, "main/audio/engine/LayerControls.h"), path.join(temporary, "LayerControls.h"));
for (const file of ["cJSON.h", "cJSON.c"])
 copyFileSync(path.join(root, "managed_components/espressif__cjson/cJSON", file), path.join(temporary, file));
writeFileSync(path.join(temporary, "esp_heap_caps.h"), `#pragma once
#include <cassert>
#include <cstdlib>
#include <map>
constexpr int MALLOC_CAP_INTERNAL=1, MALLOC_CAP_SPIRAM=2, MALLOC_CAP_8BIT=4;
inline std::map<void *,size_t> allocations;
inline int failures=0, lastCaps=0;
inline void *heap_caps_malloc(size_t bytes,int caps) {
 lastCaps=caps;if(failures){--failures;return nullptr;}
 void *p=std::malloc(bytes);assert(p);allocations[p]=bytes;return p;
}
inline void heap_caps_free(void *p) {if(p){assert(allocations.erase(p)==1);std::free(p);}}
inline size_t used(){size_t bytes=0;for(auto a:allocations)bytes+=a.second;return bytes;}
`);
writeFileSync(path.join(temporary, "test.cpp"), `
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <array>
#include <deque>
#include <atomic>
#include <limits>
#include "cJSON.h"
#include "LayerControls.h"
namespace synth {
 constexpr int kValueListMax=16,kKnobControlMax=16,kKnobLayerMax=8,kKnobOutputMax=128;
 constexpr int kButtonControlMax=16,kButtonLayerMax=8,kButtonOutputMax=128;
 constexpr size_t kMaxPresetBytes=1024*1024;
 inline uint32_t portBit(int p){return 1u<<p;}
 ${method(masterClock,"struct MasterClockState")};
 ${method(masterClock,"inline MasterClockState &masterClock(")}
}
${states(knob, "knob", "inline void render(")}
${states(button, "button", "inline void renderButton(")}
using nodes::knob::KnobState;using nodes::knob::KnobControl;
using nodes::button::ButtonState;using nodes::button::ButtonControl;
enum class NodeType {Knob,Button,Unknown};
struct NodeInstance {
 NodeType type=NodeType::Unknown;
 KnobState *knob=nullptr;ButtonState *button=nullptr;
 nodes::button::UpDownButtonState *upDownButton=nullptr;
 uint32_t audioConnectedPortMask=0;float input=0;float *outL=nullptr;
};
namespace noderender {inline float portSample(const NodeInstance &i,int,size_t){return i.input;}}
struct WidgetParams {
 struct KnobControlParams : KnobControl {bool hasConstraintMin=false,hasConstraintMax=false;float constraintMin=0,constraintMax=0;};
 using ButtonControlParams=ButtonControl;
 uint8_t knobCount=1,knobLayerCount=1,knobActiveLayer=0;
 uint8_t buttonCount=1,buttonLayerCount=1,buttonActiveLayer=0;
 KnobControlParams knobs[128];ButtonControlParams buttons[128];
 int16_t upDownCcUp=-1,upDownCcDown=-1;int8_t upDownChannelUp=-1,upDownChannelDown=-1;
 float upDownMin=0,upDownMax=10,upDownStep=1,upDownCurrentValue=0,valueList[16]={};
 uint8_t valueListCount=0,upDownListIndex=0;
};
${methods(knob, "knob", "inline void render(")}
${methods(button, "button", "inline void renderButton(")}
struct PresetDoc {float bpm=-1;uint16_t widgetCount=0,connectionCount=0;};
void parseWidgetsArray(cJSON *,PresetDoc &,bool){}
void parseConnectionsArray(cJSON *,PresetDoc &){}
${method(presetFill,"inline bool parsePreset(")}
struct UsbMidiEvent {uint8_t codeIndex,status,data1,data2;};
${method(engine,"inline void handleMasterClockMidi(")}
struct WidgetSlot {int instanceBase=0;};
struct Graph {float presetBpm=-1;uint16_t widgetCount=0;WidgetSlot widgets[2];NodeInstance instances[2];};
struct FeedbackEngine {
 Graph *liveGraph_=nullptr;
 bool (*midiFeedbackOut_)(uint8_t,uint8_t,uint8_t)=nullptr;
 uint32_t (*midiConnectionGeneration_)()=nullptr;
 uint32_t midiFeedbackGeneration_=0;uint16_t midiFeedbackWidget_=0;
 ${method(engine,"void resetMidiControlFeedback(")}
 ${method(engine,"void synchronizeMidiControls(")}
};
std::vector<std::array<uint8_t,3>> feedback;
bool failSend=false;
uint32_t generation=0;
bool sendFeedback(uint8_t ch,uint8_t cc,uint8_t raw){if(failSend)return false;feedback.push_back({ch,cc,raw});return true;}
uint32_t connectionGeneration(){return generation;}
struct tx_message_t {uint8_t cable,length,data[3];};
std::deque<tx_message_t> transmit;
auto *tx_queue=&transmit;
bool connected=true;
uint8_t connected_idx=0,tx_offset=0;
std::atomic<bool> tx_drain_pending{false};
constexpr int pdTRUE=1;
int xQueuePeek(std::deque<tx_message_t> *q,tx_message_t *p,int){if(q->empty())return 0;*p=q->front();return 1;}
int xQueueReceive(std::deque<tx_message_t> *q,tx_message_t *p,int){if(q->empty())return 0;*p=q->front();q->pop_front();return 1;}
void xQueueReset(std::deque<tx_message_t> *q){q->clear();}
size_t uxQueueMessagesWaiting(std::deque<tx_message_t> *q){return q->size();}
bool tuh_midi_mounted(uint8_t){return connected;}
size_t capacity=0;
std::vector<uint8_t> written;
uint32_t tuh_midi_stream_write(uint8_t,uint8_t,const uint8_t *data,uint32_t count){uint32_t n=static_cast<uint32_t>(capacity<count?capacity:count);written.insert(written.end(),data,data+n);capacity-=n;return n;}
void tuh_midi_write_flush(uint8_t){}
int deferred=0;
void usbh_defer_func(void(*)(void *),void *,bool){++deferred;}
${method(usb,"inline void tx_drain(")}
${method(usb,'extern "C" __attribute__((used)) void tuh_midi_tx_cb(').replace('__attribute__((used)) ', '').replace('using namespace Feature::MIDI;', '')}
int main(){
 assert(allocations.empty());
 {
  KnobState k;ButtonState b;
  assert(used()==sizeof(KnobControl)+sizeof(ButtonControl));
  printf("Single control payload: knob %zu bytes, button %zu bytes\\n",sizeof(KnobControl),sizeof(ButtonControl));
  WidgetParams p;p.knobCount=2;p.knobLayerCount=3;p.knobActiveLayer=2;
  p.knobs[33].raw=127;p.knobs[33].point2Out=8;strcpy(p.knobs[33].label,"Layer 3 knob 2");
  p.knobs[32].valueListCount=2;p.knobs[32].valueList[0]=3;p.knobs[32].valueList[1]=7;
  p.buttonCount=2;p.buttonLayerCount=3;p.buttonActiveLayer=2;
  p.buttons[32].valueListCount=2;p.buttons[32].valueList[0]=2;p.buttons[32].valueList[1]=9;p.buttons[32].listIndex=1;
  p.buttons[33].wasAbove=true;p.buttons[33].value127=6;
  NodeInstance i;i.knob=&k;i.button=&b;float out[4];i.outL=out;
  nodes::knob::build(i,p);nodes::button::buildButton(i,p);
  assert(used()==6*(sizeof(KnobControl)+sizeof(ButtonControl)));
  assert(k.controls[33].currentValue==8 && strcmp(k.controls[33].label,"Layer 3 knob 2")==0);
  assert(!k.controls.contains(34) && !k.controls.contains(48));
  assert(nodes::knob::setParam(i,"raw",127,32));
  nodes::knob::render(i,4);assert(out[3]==7);
  assert(!nodes::knob::setParam(i,"raw",127,34));
  nodes::button::renderButton(i,4);assert(out[3]==9);
  assert(nodes::button::setParamButton(i,"listIndex",0,32));
  nodes::button::renderButton(i,4);assert(out[3]==2);
  assert(!nodes::button::setParamButton(i,"wasAbove",1,34));
  // Live growth preserves layer identity; new slots use unlearned defaults.
  assert(k.resizeControls(4,4));assert(k.controls[33].currentValue==8);
  assert(k.controls[35].cc==-1 && k.controls[48].cc==-1);
  assert(b.resizeControls(4,4));assert(b.controls[33].currentValue==6);
  // Failed growth retains the previous allocation and all values.
  size_t previous=used();failures=2;
  assert(!k.resizeControls(16,8));assert(k.count==4 && k.layerCount==4 && used()==previous);
  assert(k.controls[33].currentValue==8);
  failures=1;assert(k.resizeControls(5,4));assert(lastCaps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  assert(k.controls[33].currentValue==8);
  assert(k.resizeControls(1,1));assert(k.activeLayer==0 && !k.controls.contains(32));
  assert(b.resizeControls(1,1));assert(b.activeLayer==0);
  assert(used()==sizeof(KnobControl)+sizeof(ButtonControl));
  assert(k.resizeControls(16,8));assert(b.resizeControls(16,8));
  k.controls[127].raw=42;b.controls[127].listIndex=3;
  assert(k.controls[127].raw==42 && b.controls[127].listIndex==3);
 }
 assert(allocations.empty());
 // Actual node destruction frees both the compact state and owned controls.
 NodeInstance i;
 i.knob=new(heap_caps_malloc(sizeof(KnobState),5))KnobState;
 i.button=new(heap_caps_malloc(sizeof(ButtonState),5))ButtonState;
 nodes::knob::destroy(i);nodes::button::destroyButton(i);
 assert(!i.knob && !i.button && allocations.empty());
 {
  KnobState k;ButtonState b;assert(k.resizeControls(1,2));assert(b.resizeControls(1,2));
  k.activeLayer=1;b.activeLayer=1;
  for(int index:{0,16}){k.controls[index].cc=74;k.controls[index].channel=2;b.controls[index].cc=75;b.controls[index].channel=2;}
  k.controls[0].raw=12;k.controls[16].raw=96;b.controls[16].wasAbove=true;
  Graph g;g.widgetCount=2;g.widgets[1].instanceBase=1;
  g.instances[0].type=NodeType::Knob;g.instances[0].knob=&k;
  g.instances[1].type=NodeType::Button;g.instances[1].button=&b;
  FeedbackEngine e;e.liveGraph_=&g;e.midiFeedbackOut_=sendFeedback;e.midiConnectionGeneration_=connectionGeneration;
  e.synchronizeMidiControls();assert(feedback.size()==2);
  assert(feedback[0][0]==2 && feedback[0][1]==74 && feedback[0][2]==96 && feedback[1][2]==127);
  feedback.clear();e.synchronizeMidiControls();assert(feedback.empty());
  NodeInstance n;n.knob=&k;n.button=&b;
  assert(nodes::knob::setParam(n,"raw",44,16));assert(nodes::button::setParamButton(n,"wasAbove",0,16));
  e.synchronizeMidiControls();assert(feedback.size()==2 && feedback[0][2]==44 && feedback[1][2]==0);
  feedback.clear();k.activeLayer=0;b.activeLayer=0;e.synchronizeMidiControls();
  assert(feedback.size()==2 && feedback[0][2]==12);feedback.clear();
  ++generation;e.synchronizeMidiControls();assert(feedback.size()==2);feedback.clear();
  // Full transport queues leave a dirty value for the next block.
  assert(nodes::knob::setParam(n,"raw",55,0));failSend=true;e.synchronizeMidiControls();assert(feedback.empty());
  failSend=false;e.synchronizeMidiControls();assert(feedback.size()==1 && feedback[0][2]==55);feedback.clear();
  // Sparse value lists use their selected position, not an arbitrary voltage.
  b.controls[0].valueListCount=3;b.controls[0].listIndex=1;e.synchronizeMidiControls();assert(feedback.size()==1 && feedback[0][2]==63);feedback.clear();
  // Note/program button bindings are not synthesized into unrelated CC messages.
  b.controls[0].midiType=nodes::button::kButtonMidiNote;++generation;e.synchronizeMidiControls();assert(feedback.size()==1);feedback.clear();
  assert(k.resizeControls(16,2));
  for(int j=0;j<16;++j){k.controls[j].cc=j;k.controls[j].channel=0;k.controls[j].raw=j;}
  ++generation;
  for(int block=0;block<5;++block){size_t before=feedback.size();e.synchronizeMidiControls();assert(feedback.size()-before<=4);}
  assert(feedback.size()==16);feedback.clear();
  // A fresh preset starts with invalid stamps even when bindings are identical.
  KnobState replacement;replacement.controls[0].cc=74;replacement.controls[0].channel=2;replacement.controls[0].raw=33;
  g.instances[0].knob=&replacement;e.resetMidiControlFeedback();e.synchronizeMidiControls();
  assert(feedback.size()==1 && feedback[0][2]==33);
 }
 assert(allocations.empty());
 puts("PASS MIDI feedback: active layers, touch values, buttons, lists, reconnect, bounded batches and queue retry");
 {
  auto &mc=synth::masterClock();mc.midiCc=20;mc.midiChannel=3;mc.maxBpm=240;
  Graph g;g.presetBpm=90;FeedbackEngine e;e.liveGraph_=&g;e.midiFeedbackOut_=sendFeedback;e.midiConnectionGeneration_=connectionGeneration;
  e.resetMidiControlFeedback();assert(mc.bpm==90);feedback.clear();
  e.synchronizeMidiControls();assert(feedback.size()==1 && feedback[0][0]==3 && feedback[0][1]==20 && feedback[0][2]==48);
  feedback.clear();e.synchronizeMidiControls();assert(feedback.empty());
  mc.bpm=180;failSend=true;e.synchronizeMidiControls();assert(feedback.empty());
  failSend=false;e.synchronizeMidiControls();assert(feedback.size()==1 && feedback[0][2]==95);
  feedback.clear();++generation;e.synchronizeMidiControls();assert(feedback.size()==1);
  g.presetBpm=-1;e.resetMidiControlFeedback();assert(mc.bpm==180); // Legacy/internal rebuild.
  g.presetBpm=0;e.resetMidiControlFeedback();assert(mc.bpm==0 && mc.feedbackValue()==0);
  feedback.clear();handleMasterClockMidi({0xb,0xb3,20,64});e.synchronizeMidiControls();assert(feedback.empty());
  assert(std::abs(mc.bpm-240.0f*64/127)<0.001f);
  mc.bpm=1000;assert(mc.feedbackValue()==127);
  mc.bpm=NAN;assert(mc.feedbackValue()==-1);mc.bpm=120;mc.maxBpm=0;assert(mc.feedbackValue()==-1);
  mc.maxBpm=240;mc.midiCc=128;assert(mc.feedbackValue()==-1);mc.midiCc=20;mc.midiChannel=16;assert(mc.feedbackValue()==-1);
  puts("PASS preset tempo: restore, legacy/voice rebuild preservation, BPM CC, reconnect, retry and bounds");
 }
 {
  PresetDoc doc;
  for(const char *json:{"{}","{\\"masterClock\\":null}","{\\"masterClock\\":{\\"bpm\\":-1}}","{\\"masterClock\\":{\\"bpm\\":\\"120\\"}}","{\\"masterClock\\":{\\"bpm\\":1e999}}"}){
   doc.bpm=99;assert(parsePreset(json,strlen(json),doc) && doc.bpm==-1);
  }
  for(const auto tempo:std::array<float,3>{0,91.5f,240}){
   char json[100];snprintf(json,sizeof(json),"{\\"masterClock\\":{\\"bpm\\":%f}}",tempo);
   assert(parsePreset(json,strlen(json),doc) && doc.bpm==tempo);
  }
  assert(!parsePreset("{",1,doc));
  puts("PASS preset BPM JSON: fractional/zero tempo, missing field, invalid types, negative and overflow");
 }
 transmit.push_back({0,3,{0xb2,74,55}});capacity=2;tx_drain(nullptr);
 assert(transmit.size()==1 && tx_offset==2 && written.size()==2);
 tuh_midi_tx_cb(0,4);assert(deferred==1);tuh_midi_tx_cb(0,4);assert(deferred==1);
 capacity=1;tx_drain(nullptr);assert(transmit.empty() && tx_offset==0 && written[2]==55);
 transmit.push_back({0,3,{0xb2,75,127}});capacity=0;tx_drain(nullptr);assert(transmit.size()==1 && tx_offset==0);
 capacity=3;tx_drain(nullptr);assert(transmit.empty() && written.size()==6);
 transmit.push_back({0,3,{0xb2,75,0}});connected=false;tx_drain(nullptr);assert(transmit.empty() && tx_offset==0);
 puts("PASS USB MIDI: full endpoint, partial writes, completion retry and disconnect cleanup");
 puts("PASS compact controls: layers, mapping, lists, live resizing, allocation failure, PSRAM fallback and cleanup");
}
`);
function run(command, args) {
 const result=spawnSync(command,args,{cwd:temporary,encoding:"utf8",windowsVerbatimArguments:command==="cmd.exe",timeout:30000});
 if(result.error)throw result.error;
 if(result.status!==0)throw new Error(result.stdout+result.stderr);
 return result.stdout;
}
if(process.platform==="win32") {
 const vswhere=path.join(process.env["ProgramFiles(x86)"],"Microsoft Visual Studio/Installer/vswhere.exe");
 const installation=run(vswhere,["-latest","-products","*","-requires","Microsoft.VisualStudio.Component.VC.Tools.x86.x64","-property","installationPath"]).trim();
 const vcvars=path.join(installation,"VC/Auxiliary/Build/vcvars64.bat");
 run("cmd.exe",["/d","/c",`call "${vcvars}" >nul && cl /nologo /EHsc /std:c++17 /Fe:test.exe test.cpp cJSON.c`]);
 console.log(run(path.join(temporary,"test.exe"),[]).trim());
} else {
 run(process.env.CXX||"c++",["-std=c++17","test.cpp","cJSON.c","-o","test"]);
 console.log(run(path.join(temporary,"test"),[]).trim());
}
