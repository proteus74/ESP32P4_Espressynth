import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const temporary = mkdtempSync(path.join(os.tmpdir(), "espressynth-knob-overlay-"));
mkdirSync(path.join(temporary, "features/synth"), { recursive: true });
mkdirSync(path.join(temporary, "features/display"), { recursive: true });
mkdirSync(path.join(temporary, "audio/engine"), { recursive: true });
copyFileSync(path.join(root, "main/features/display/PortDisplayLabels.h"), path.join(temporary, "features/display/PortDisplayLabels.h"));
copyFileSync(path.join(root, "main/audio/engine/NodeTypes.h"), path.join(temporary, "audio/engine/NodeTypes.h"));
const source = readFileSync(path.join(root, "eezstudio/src/ui/screens.c"), "utf8");
const factory = source.slice(source.indexOf("void create_user_widget_knob("), source.indexOf("void tick_user_widget_knob(")) +
  source.slice(source.indexOf("void create_user_widget_midi_button("), source.indexOf("void tick_user_widget_midi_button("));
const overlay = readFileSync(path.join(root, "main/features/display/KnobActivityOverlay.h"), "utf8");
const styles = [...new Set([...`${factory}\n${overlay}`.matchAll(/\b(lv_obj_set_style_\w+)\(/g)].map((match) => match[1]))];
const knob = readFileSync(path.join(root, "main/audio/engine/nodes/KnobNode.h"), "utf8");
const params = readFileSync(path.join(root, "main/audio/engine/PresetParser.h"), "utf8");
const knobParams = params.slice(params.indexOf("    // knob\n") >= 0 ? params.indexOf("    // knob\n") : params.indexOf("    // knob\r\n"), params.indexOf("    // button\n") >= 0 ? params.indexOf("    // button\n") : params.indexOf("    // button\r\n"));
const knobTypes = knob.slice(knob.indexOf("struct KnobControl"), knob.indexOf("inline void render("));
const knobBuild = knob.slice(knob.indexOf("inline void build("), knob.indexOf("inline void destroy("));
const knobParse = knob.slice(knob.indexOf("inline void parsePreset("), knob.lastIndexOf("} // namespace nodes::knob"));
for (const file of ["cJSON.h", "cJSON.c"])
  copyFileSync(path.join(root, "managed_components/espressif__cjson/cJSON", file), path.join(temporary, file));
copyFileSync(path.join(root, "main/features/display/KnobActivityOverlay.h"), path.join(temporary, "KnobActivityOverlay.h"));
copyFileSync(path.join(root, "main/features/synth/KnobActivity.h"), path.join(temporary, "features/synth/KnobActivity.h"));
copyFileSync(path.join(root, "eezstudio/src/ui/screens.h"), path.join(temporary, "screens.h"));
// Execute the real overlay and EEZ factory; model only the LVGL object API.
writeFileSync(path.join(temporary, "lvgl.h"), `#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
enum lv_obj_flag_t { LV_OBJ_FLAG_CLICKABLE=1, LV_OBJ_FLAG_SCROLLABLE=2 };
constexpr int LV_PART_MAIN=0, LV_PART_INDICATOR=0, LV_PART_KNOB=0, LV_STATE_DEFAULT=0, LV_ALIGN_CENTER=0, LV_OPA_TRANSP=0, LV_FLEX_FLOW_ROW_WRAP=1,
              LV_DIR_VER=1, LV_LABEL_LONG_CLIP=0, LV_TEXT_ALIGN_CENTER=0;
inline int lv_font_montserrat_16=16, lv_font_montserrat_28=28, lv_font_montserrat_36=36;
struct lv_obj_t { lv_obj_t *parent=nullptr; std::vector<lv_obj_t *> children; std::string text; int x=0,y=0,width=0,height=0,value=0,minimum=0,maximum=100; uint32_t flags=3; };
inline lv_obj_t top;
inline int liveObjects=0;
inline int arcWrites=0;
inline lv_obj_t *lv_layer_top() { return &top; }
inline lv_obj_t *lv_obj_create(lv_obj_t *parent) { auto *obj=new lv_obj_t; ++liveObjects; obj->parent=parent; if(parent) parent->children.push_back(obj); return obj; }
inline lv_obj_t *lv_arc_create(lv_obj_t *parent) { return lv_obj_create(parent); }
inline lv_obj_t *lv_label_create(lv_obj_t *parent) { return lv_obj_create(parent); }
inline lv_obj_t *lv_button_create(lv_obj_t *parent) { return lv_obj_create(parent); }
inline lv_obj_t *lv_obj_get_parent(lv_obj_t *obj) { return obj->parent; }
inline void lv_obj_set_pos(lv_obj_t *obj,int x,int y) { obj->x=x; obj->y=y; }
inline void lv_obj_set_size(lv_obj_t *obj,int w,int h) { obj->width=w; obj->height=h; }
inline void lv_obj_clear_flag(lv_obj_t *obj,lv_obj_flag_t flags) { obj->flags &= ~flags; }
inline void lv_arc_set_value(lv_obj_t *obj,int value) { obj->value=value; ++arcWrites; }
inline void lv_arc_set_range(lv_obj_t *obj,int minimum,int maximum) { obj->minimum=minimum; obj->maximum=maximum; }
inline void lv_label_set_text(lv_obj_t *obj,const char *text) { obj->text=text; }
inline void lv_label_set_text_static(lv_obj_t *obj,const char *text) { obj->text=text; }
inline const char *lv_label_get_text(lv_obj_t *obj) { return obj->text.c_str(); }
inline void lv_label_set_long_mode(lv_obj_t *,int) {}
inline void lv_obj_set_flex_flow(lv_obj_t *,int) {}
inline void lv_obj_set_scroll_dir(lv_obj_t *,int) {}
inline void lv_obj_center(lv_obj_t *) {}
inline int lv_display_get_horizontal_resolution(void *) { return 1024; }
inline int lv_display_get_vertical_resolution(void *) { return 600; }
inline void lv_obj_delete(lv_obj_t *obj) { while(!obj->children.empty()) lv_obj_delete(obj->children.back()); if(obj->parent) { auto &c=obj->parent->children; c.erase(std::find(c.begin(),c.end(),obj)); } delete obj; --liveObjects; }
${styles.map((name) => `template<typename... T> inline void ${name}(T...) {}`).join("\n")}
`);
writeFileSync(path.join(temporary, "test.cpp"), `#include <cassert>
#include <cstring>
#include <cstdio>
#include "KnobActivityOverlay.h"
#include "cJSON.h"
namespace synth { constexpr int kKnobControlMax=16, kKnobLayerMax=4, kKnobOutputMax=64, kValueListMax=32; }
struct WidgetParams { ${knobParams} };
namespace presetparser {
 float getFloat(cJSON *p,const char *key,float fallback) { auto *v=cJSON_GetObjectItemCaseSensitive(p,key); return cJSON_IsNumber(v) ? static_cast<float>(v->valuedouble) : fallback; }
 int getInt(cJSON *p,const char *key,int fallback) { auto *v=cJSON_GetObjectItemCaseSensitive(p,key); return cJSON_IsNumber(v) ? v->valueint : fallback; }
 uint8_t getFloatArray(cJSON *p,const char *key,float *out,uint8_t maximum) { auto *a=cJSON_GetObjectItemCaseSensitive(p,key); uint8_t n=0; if(cJSON_IsArray(a)) for(int i=0;i<cJSON_GetArraySize(a) && n<maximum;++i) { auto *v=cJSON_GetArrayItem(a,i); out[n++]=cJSON_IsNumber(v) ? static_cast<float>(v->valuedouble) : 0; } return n; }
}
namespace nodes::knob { ${knobTypes} }
struct NodeInstance { nodes::knob::KnobState *knob; };
namespace nodes::knob { ${knobBuild} ${knobParse} }
objects_t objects{};
${factory}
KnobActivity event(const char *source,int control,const char *target,const char *port,int raw,int64_t time) {
 KnobActivity a; std::snprintf(a.sourceId,24,"%s",source); a.controlIndex=control; a.raw=raw; a.updatedUs=time;
 std::snprintf(a.name,48,"My Knob %d",control+1); std::snprintf(a.targetId,24,"%s",target);
 if(target[0]) std::snprintf(a.device,48,"Oscillator 1"); std::snprintf(a.port,16,"%s",port); a.targetValue=raw/127.0f*10.0f; return a;
}
lv_obj_t *card(int index) { return top.children[0]->children[index]; }
int main() {
 // Real preset parser and native build preserve controller names without JS.
 auto *json=cJSON_Parse(R"({"labels":["Legacy Name"],"knobs":[{"raw":95}]})");
 WidgetParams p; nodes::knob::parsePreset(json,p); cJSON_Delete(json);
 nodes::knob::KnobState state; NodeInstance inst{&state}; nodes::knob::build(inst,p);
 assert(std::string(state.controls[0].label)=="Legacy Name" && state.controls[0].raw==95);
 json=cJSON_Parse(R"({"layerCount":2,"activeLayer":1,"layers":[{"labels":["Pitch","Gain"],"knobs":[{"raw":32},{"raw":64}]},{"labels":["Filter","Resonance"],"knobs":[{"raw":96},{"raw":127}]}]})");
 p={}; nodes::knob::parsePreset(json,p); cJSON_Delete(json); nodes::knob::build(inst,p);
 assert(state.count==2 && state.layerCount==2 && state.activeLayer==1);
 assert(std::string(state.controls[0].label)=="Pitch" && std::string(state.controls[17].label)=="Resonance");
 assert(state.controls[17].raw==127);
 json=cJSON_Parse(R"({"knobs":[{"raw":0}]})"); p={}; nodes::knob::parsePreset(json,p); cJSON_Delete(json);
 assert(p.knobs[0].label[0]==0);
 lv_obj_t sentinel;
 auto **handles=reinterpret_cast<lv_obj_t **>(&objects);
 for(size_t i=0;i<sizeof(objects)/sizeof(lv_obj_t *);++i) handles[i]=&sentinel;
 const objects_t original=objects;
 KnobActivityOverlay overlay;
 overlay.tick(0); assert(liveObjects==0 && top.children.empty());
 overlay.update(event("knob-a",0,"osc-a","pitchCv",95,100));
 assert(top.children.size()==1 && top.children[0]->children.size()==1);
 assert(std::memcmp(&objects,&original,sizeof(objects))==0);
 auto *first=card(0);
 assert(first->height>=194 && first->children.size()==6);
 assert(first->children[0]->minimum==0 && first->children[0]->maximum==127 && first->children[0]->value==95);
 assert(first->children[1]->text=="95" && first->children[2]->text=="Oscillator 1");
 assert(first->children[3]->text=="Pitch" && first->children[4]->text=="My Knob 1");
 assert(first->children[5]->text=="7.48 V");
 assert((first->children[0]->flags & LV_OBJ_FLAG_CLICKABLE)==0);
 // More directly connected inputs create separate instances of the EEZ widget.
 overlay.update(event("knob-a",0,"osc-a","gain",95,100));
 assert(top.children[0]->children.size()==2 && card(1)->children[3]->text=="Gain");
 assert(card(0)->children[0]!=card(1)->children[0]);
 overlay.update(event("knob-a",1,"osc-a","pitchCv",64,200));
 overlay.update(event("knob-b",0,"osc-a","pitchCv",127,300));
 assert(top.children[0]->children.size()==4 && card(3)->children[1]->text=="127");
 assert(std::memcmp(&objects,&original,sizeof(objects))==0);
 // Repeated changes update existing objects and renew only that entry's timeout.
 overlay.update(event("knob-a",0,"osc-a","pitchCv",96,500000));
 assert(top.children[0]->children.size()==4 && card(0)==first && card(0)->children[1]->text=="96");
 overlay.tick(1000099); assert(top.children[0]->children.size()==4);
 overlay.tick(1000100); assert(top.children[0]->children.size()==3);
 overlay.tick(1000200); assert(top.children[0]->children.size()==2);
 overlay.tick(1000300); assert(top.children[0]->children.size()==1 && card(0)==first);
 overlay.tick(1499999); assert(liveObjects==8);
 overlay.tick(1500000); assert(liveObjects==0 && top.children.empty());
 // Unconnected controls still show their name/value without inventing a target.
 overlay.update(event("unconnected",0,"","",0,2000000));
 assert(card(0)->children[2]->text=="Nicht verbunden" && card(0)->children[3]->text=="--" && card(0)->children[5]->text.empty());
 overlay.clear(); assert(liveObjects==0);
 // A MIDI burst refreshes each card once, with its final value and latest idle deadline.
 overlay.update(event("fast",0,"osc","pitchCv",0,2100000));
 overlay.update(event("fast",0,"osc","gain",0,2100000));
 overlay.update(event("other",0,"osc","pitchCv",0,2100000));
 const int writesBefore=arcWrites;
 for(int i=0;i<500;++i) {
  overlay.update(event("fast",0,"osc","pitchCv",i%128,2200000+i),true);
  overlay.update(event("fast",0,"osc","gain",i%128,2200000+i),true);
 }
 overlay.update(event("other",0,"osc","pitchCv",99,2200499),true);
 assert(arcWrites==writesBefore);
 overlay.tick(2200500);
 assert(arcWrites==writesBefore+3 && card(0)->children[0]->value==115 &&
        card(1)->children[0]->value==115 && card(2)->children[0]->value==99);
 overlay.tick(3200498); assert(liveObjects==22);
 overlay.tick(3200499); assert(liveObjects==0);
 // A many-target patch remains inside the screen, with row wrapping/scrolling.
 for(int i=0;i<14;++i) overlay.update(event("many",i,"target","gain",i,3000000));
 assert(top.children[0]->children.size()==14 && top.children[0]->width<=992 && top.children[0]->height<=568);
 overlay.clear(); assert(liveObjects==0 && std::memcmp(&objects,&original,sizeof(objects))==0);
 // Button activity uses the MidiButton widget: output value inside the button, no arc.
 KnobActivity b=event("button-a",2,"osc-a","gain",127,4000000); b.isButton=true; b.sourceValue=5.0f; b.targetValue=2.5f;
 overlay.update(b);
 auto *pad=card(0);
 assert(pad->height==150 && pad->children.size()==5 && pad->children[0]->children.size()==1);
 assert((pad->children[0]->flags & LV_OBJ_FLAG_CLICKABLE)==0);
 assert(pad->children[0]->children[0]->text=="5" && pad->children[1]->text=="Oscillator 1");
 assert(pad->children[2]->text=="Gain" && pad->children[3]->text=="My Knob 3" && pad->children[4]->text=="2.50 V");
 assert(std::memcmp(&objects,&original,sizeof(objects))==0);
 overlay.clear(); assert(liveObjects==0);
 std::puts("Native knob overlay: real EEZ factory, raw CC, names/targets, concurrent knobs, fan-out, independent 1s deletion, layout, screen handles passed");
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
  run("cmd.exe", ["/d", "/c", `call "${vcvars}" >nul && cl /nologo /EHsc /std:c++17 /I. /Fe:test.exe test.cpp cJSON.c`]);
  console.log(run(path.join(temporary, "test.exe"), []).trim());
} else {
  run(process.env.CXX || "c++", ["-std=c++17", "-I.", "test.cpp", "cJSON.c", "-o", "test"]);
  console.log(run(path.join(temporary, "test"), []).trim());
}
