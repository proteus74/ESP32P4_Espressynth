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


const directory = mkdtempSync(path.join(os.tmpdir(), "espressynth-master-volume-"));
for (const file of ["cJSON.h", "cJSON.c"]) copyFileSync(path.join(root, "managed_components/espressif__cjson/cJSON", file), path.join(directory, file));
const graph = engine("Graph.h"), builder = engine("GraphBuilder.h");
const nativeRoot = path.join(root, "main/audio/engine").replaceAll("\\", "/");
const seedStart = builder.lastIndexOf("    for (uint16_t r = 0; r < g->controlRouteCount; ++r)", builder.indexOf("float initial = 0.0f;"));
const midi = read("main/features/synth/synth_manager.hpp");
const api = read("main/features/synth/synth_web_api.hpp");
const volumeApply = api.slice(api.indexOf('                    cJSON *volume = cJSON_GetObjectItemCaseSensitive(params, "volume");'), api.indexOf('                    synth::saveDeviceSettingsToFs();', api.indexOf('                    cJSON *volume = cJSON_GetObjectItemCaseSensitive(params, "volume");')));
writeFileSync(path.join(directory, "test.cpp"), `
#include <cassert>
#include <cmath>
#include <limits>
#include <initializer_list>
#include <atomic>
#include "${nativeRoot}/NodeTypes.h"
#include "${nativeRoot}/DeviceSettings.h"
namespace cv {constexpr float kMaxVolts=10;}
namespace synth {constexpr int kMaxConnections=8;inline unsigned portBit(int p){return 1u<<p;}}
struct NodeInstance {
 NodeType type=NodeType::Unknown; float controlValue[24]={};unsigned controlConnectedPortMask=0;
 struct {float maxRateHz=50;} lfo;
 struct Control {float currentValue=0,raw=0;};struct Controls {Control controls[16];};
 Controls *knob=nullptr,*button=nullptr;
 struct UpDown {float currentOutput(){return 0;}} *upDownButton=nullptr;
 struct {float value=0;} constant;
 struct {float value=0;} midiProgram;
 float gain=0;
};
inline bool setFloatFieldByName(NodeInstance &inst,NodeType,const char *name,float value){if(strcmp(name,"gain")==0)inst.gain=value;return true;}
${declaration(graph,"struct ControlRoute")};
struct WidgetSlot {NodeType type=NodeType::Unknown;uint16_t instanceBase=0,instanceCount=1;};
struct Graph {uint16_t widgetCount=2,controlRouteCount=0;WidgetSlot widgets[2];NodeInstance instances[3];ControlRoute controlRoutes[8];};
${declaration(graph,"inline void pushControlRouteValue(")}
${declaration(graph,"inline void updateMasterVolumeControlRoutes(")}
void connect(Graph *g){int fromIdx=0,toIdx=1,toPort=0;auto fromType=NodeType::MasterVolume;
 struct {const char *fromPort,*toPort;} cd{"value","gain"};
 for(int once=0;once<1;++once){${declaration(builder,"if ((fromType == NodeType::Const || fromType == NodeType::MasterVolume ||")}}
}
void seed(Graph *g){${declaration(builder,"    for (uint16_t r = 0; r < g->controlRouteCount; ++r)",seedStart)}}
int64_t esp_timer_get_time(){return 1000000;}
std::atomic<int64_t> s_masterVolumeDirtyUs{0};
void *s_presetNavQueue=nullptr;
void xQueueOverwrite(void *,const int *){}
${declaration(midi,"inline void handleDeviceSettingsMidi(")}
void manual(cJSON *params){auto &ds=synth::deviceSettings();${volumeApply}}
void near(float a,float b){assert(std::fabs(a-b)<0.00001f);}
int main(){
 assert(nodeTypeFromString("master-volume")==NodeType::MasterVolume);
 Graph g;g.widgets[0].type=NodeType::MasterVolume;g.widgets[1].instanceBase=1;g.widgets[1].instanceCount=2;
 auto &ds=synth::deviceSettings();ds.volume=0.25f;connect(&g);seed(&g);
 assert(g.controlRoutes[0].kind==ControlRoute::Kind::MasterVolume);
 for(int voice:{1,2})near(g.instances[voice].gain,2.5f);
 ds.volumeCc=0;ds.volumeChannel=0;assert(synth::hasLearnedMasterVolume());
 handleDeviceSettingsMidi(0xB1,0,127);near(ds.volume,0.25f);
 handleDeviceSettingsMidi(0xB0,1,127);near(ds.volume,0.25f);
 handleDeviceSettingsMidi(0x90,0,127);near(ds.volume,0.25f);
 for(int value:{0,64,127}){
  handleDeviceSettingsMidi(0xB0,0,value);updateMasterVolumeControlRoutes(g);
  for(int voice:{1,2}){near(g.instances[voice].gain,value*10.0f/127);near(g.instances[voice].controlValue[0],value*10.0f/127);}
 }
 cJSON *params=cJSON_Parse(R"({"volume":0.1})");manual(params);near(ds.volume,1);cJSON_Delete(params);
 params=cJSON_Parse(R"({"volumeCc":-1,"volumeChannel":-1,"volume":0.4})");manual(params);near(ds.volume,0.4f);assert(!synth::hasLearnedMasterVolume());cJSON_Delete(params);
 params=cJSON_Parse(R"({"volumeCc":500,"volumeChannel":50})");manual(params);assert(ds.volumeCc==-1 && ds.volumeChannel==-1);cJSON_Delete(params);
 ds.volumeChannel=16;assert(!synth::hasLearnedMasterVolume());ds.volumeCc=128;ds.volumeChannel=0;assert(!synth::hasLearnedMasterVolume());
 ds.volume=std::numeric_limits<float>::quiet_NaN();updateMasterVolumeControlRoutes(g);near(g.instances[1].gain,0);
 ds.volume=2;updateMasterVolumeControlRoutes(g);near(g.instances[1].gain,10);
 ds.volume=-1;updateMasterVolumeControlRoutes(g);near(g.instances[1].gain,0);
 ds.volume=0.7f;Graph next;next.widgets[0].type=NodeType::MasterVolume;next.widgets[1].instanceBase=1;connect(&next);seed(&next);near(next.instances[1].gain,7);
 puts("PASS firmware: actual Master Volume route construction/seed/block delivery, all voices, learned CC/channel matching, finite 0-10 V bounds, preset-independent value");
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
