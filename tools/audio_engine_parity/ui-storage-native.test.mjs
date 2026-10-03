import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const source = readFileSync(path.join(root, "main/features/display/eez_ui_bridge.cpp"), "utf8").replaceAll("\r\n", "\n");
const main = readFileSync(path.join(root, "main/main.cpp"), "utf8");
function method(signature, text = source) {
  const start = text.indexOf(signature);
  if (start < 0) throw new Error(`Missing ${signature}`);
  let end = text.indexOf("{", start), depth = 1;
  while (depth && ++end < text.length) {
    if (text[end] === "{") ++depth;
    if (text[end] === "}") --depth;
  }
  if (depth) throw new Error(`Unclosed ${signature}`);
  return text.slice(start, end + 1);
}
const temporary = mkdtempSync(path.join(os.tmpdir(), "espressynth-ui-storage-"));
mkdirSync(path.join(temporary, "features/synth"), { recursive: true });
copyFileSync(path.join(root, "main/features/display/eez_ui_bridge.hpp"), path.join(temporary, "eez_ui_bridge.hpp"));
copyFileSync(path.join(root, "main/features/synth/KnobActivity.h"), path.join(temporary, "features/synth/KnobActivity.h"));
const loader = source.slice(source.indexOf("enum class DataKind"), source.indexOf("void action_open_preset_list"));
writeFileSync(path.join(temporary, "test.cpp"), `
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "eez_ui_bridge.hpp"
constexpr int pdTRUE=1,portMAX_DELAY=-1,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2,LV_OBJ_FLAG_HIDDEN=1;
thread_local bool workerThread=false;
struct StopWorker {};
struct Queue {
 size_t itemSize; size_t capacity=3; bool stop=false;
 std::mutex mutex; std::condition_variable changed;
 std::deque<std::vector<unsigned char>> entries;
 explicit Queue(size_t size):itemSize(size){}
};
using QueueHandle_t=Queue *;
int xQueueSend(Queue *q,const void *item,int timeout) {
 assert(workerThread || timeout==0);
 std::unique_lock lock(q->mutex);
 if(timeout==0 && q->entries.size()==q->capacity)return 0;
 q->changed.wait(lock,[&]{return q->stop || q->entries.size()<q->capacity;});
 if(q->stop)throw StopWorker{};
 const auto *bytes=static_cast<const unsigned char *>(item);
 q->entries.emplace_back(bytes,bytes+q->itemSize);q->changed.notify_all();return pdTRUE;
}
int xQueueReceive(Queue *q,void *item,int timeout) {
 assert(workerThread || timeout==0);
 std::unique_lock lock(q->mutex);
 if(timeout==0 && q->entries.empty())return 0;
 q->changed.wait(lock,[&]{return q->stop || !q->entries.empty();});
 if(q->stop)throw StopWorker{};
 memcpy(item,q->entries.front().data(),q->itemSize);q->entries.pop_front();q->changed.notify_all();return pdTRUE;
}
void waitEntries(Queue &q,size_t n) {
 std::unique_lock lock(q.mutex);
 assert(q.changed.wait_for(lock,std::chrono::seconds(3),[&]{return q.entries.size()>=n;}));
}
std::mutex allocationMutex;
std::set<void *> allocations;
std::atomic<bool> allocationFails{false},parseFails{false};
void *heap_caps_calloc(size_t n,size_t size,int) {
 if(allocationFails.exchange(false))return nullptr;
 void *p=calloc(n,size);std::lock_guard lock(allocationMutex);allocations.insert(p);return p;
}
void heap_caps_free(void *p) {
 if(!p)return;
 std::lock_guard lock(allocationMutex);assert(allocations.erase(p)==1);free(p);
}
#define ESP_LOGW(...) ((void)0)
struct lv_obj_t {int flags=0;std::string text;};
struct lv_event_t {};
lv_obj_t window,table,header,content,popup;
struct {lv_obj_t *wndmain=&window,*tbl_presets=&table;} objects;
lv_obj_t *lv_win_get_content(lv_obj_t *){assert(!workerThread);return &content;}
lv_obj_t *lv_win_get_header(lv_obj_t *){assert(!workerThread);return &header;}
lv_obj_t *lv_msgbox_get_content(lv_obj_t *){assert(!workerThread);return &content;}
lv_obj_t *lv_label_create(lv_obj_t *){assert(!workerThread);return new lv_obj_t;}
void lv_label_set_text(lv_obj_t *o,const char *s){assert(!workerThread);o->text=s;}
void lv_obj_delete(lv_obj_t *o){assert(!workerThread);delete o;}
void lv_obj_clean(lv_obj_t *){assert(!workerThread);}
void lv_obj_add_flag(lv_obj_t *o,int f){assert(!workerThread);o->flags|=f;}
void lv_obj_remove_flag(lv_obj_t *o,int f){assert(!workerThread);o->flags&=~f;}
bool lv_obj_has_flag(lv_obj_t *o,int f){return (o->flags&f)!=0;}
void lv_table_set_cell_value(lv_obj_t *o,int,int,const char *s){assert(!workerThread);o->text=s;}
struct PageView {
 int shows=0,clears=0,prepared=0;bool hidden=false;uint8_t value=0;
 void clear(){assert(!workerThread);++clears;}
 void show(lv_obj_t *,const EezPageControls &p,const char * = ""){assert(!workerThread);++shows;value=p.controls[0].raw;}
 void prepare(lv_obj_t *,const EezPageControls &,const char *){assert(!workerThread);++prepared;}
 void setHidden(bool h){assert(!workerThread);hidden=h;}
} s_page_view;
EezPresetList s_preset_list{};
EezTabList *s_tab_list=nullptr;
lv_obj_t *s_pages_box=nullptr,*s_page_loading=nullptr;
char s_page_id[32]={};
uint32_t s_knob_preset_sequence=10;
EezPageControls *s_cached_pages[EEZ_TAB_CAPACITY]={};
bool s_page_requested[EEZ_TAB_CAPACITY]={};
bool s_page_fetching=false,s_tabs_fetching=false,s_presets_ready=false;
int64_t s_presets_loaded_us=0;
int64_t esp_timer_get_time(){return 1000000;}
${method("int cached_page_index(")}
${method("void clear_page_cache(")}
int populatedPages=0,populatedPresets=0;
void populate_pages(){assert(!workerThread);if(s_pages_box)++populatedPages;}
void populate_presets(){assert(!workerThread);++populatedPresets;}
std::mutex loadMutex;std::condition_variable loadChanged;
bool blockLoad=true,loadEntered=false;
bool eez_list_page_controls(const char *page,EezPageControls *out){
 assert(workerThread);out->count=1;out->controls[0].raw=page[0];
 std::unique_lock lock(loadMutex);loadEntered=true;loadChanged.notify_all();
 loadChanged.wait(lock,[]{return !blockLoad;});return !parseFails.exchange(false);
}
bool eez_list_tabs(EezTabList *out){assert(workerThread);out->count=1;return true;}
#define CONFIG_APP_ENABLE_SYNTH 1
std::atomic<unsigned> filesystemCalls{0};
struct DIR {int index=0;};
struct dirent {char d_name[64];};
DIR *opendir(const char *){assert(workerThread);++filesystemCalls;static DIR d;d.index=0;return &d;}
dirent *readdir(DIR *d){assert(workerThread);++filesystemCalls;static dirent e;if(d->index++)return nullptr;strcpy(e.d_name,"bass.json");return &e;}
void closedir(DIR *){assert(workerThread);++filesystemCalls;}
void read_preset_metadata(const char *,EezPresetEntry *){assert(workerThread);++filesystemCalls;}
namespace presetpaths {
 constexpr const char *kDir="/presets";constexpr size_t kMaxNameLen=64;
 const char *unnamedPath(){return "/presets/current.json";}
 bool isSafeName(const char *){return true;}
 void pathForName(const char *name,char *out,size_t n){snprintf(out,n,"/presets/%s.json",name);}
 void resolveActivePath(char *out,size_t n,char *name,size_t m){assert(workerThread);++filesystemCalls;snprintf(out,n,"/presets/bass.json");snprintf(name,m,"bass");}
}
${main.match(/std::atomic<uint32_t> s_preset_position[^;]+;/)[0]}
${method('extern "C" bool eez_list_presets(', main)}
namespace synth {struct Clock {float bpm=120;bool roundBpm=false;};Clock &masterClock(){static Clock c;return c;}}
namespace Feature::Synth {
 struct Engine {
  uint32_t sequence=10;float audioCoreLoadPercent(){return 10;}
  struct Voices {uint16_t active=0,maximum=8;};Voices midiVoiceUsage(){return {};}
  uint32_t presetChangeSequence(){return sequence;}
  void readCurrentPresetName(char *out,size_t n){snprintf(out,n,"bass");}
 };
 Engine &engine(){static Engine e;return e;}
 uint32_t dma_underrun_count(){return 0;}
}
struct SynthEngine {using MidiVoiceUsageSnapshot=Feature::Synth::Engine::Voices;};
${method('extern "C" bool eez_synth_status(', main)}
${loader}
${method("void show_page(")}
${method("void action_back_to_main(")}
${method("void apply_data_results(")}
int main(){
 Queue requests(sizeof(DataRequest)),results(sizeof(DataResult));s_data_requests=&requests;s_data_results=&results;
 std::thread worker([]{workerThread=true;try{data_worker(nullptr);}catch(const StopWorker &){};});
 table.flags=LV_OBJ_FLAG_HIDDEN;
 // File I/O may block while the UI can request a different page and run Back.
 strcpy(s_page_id,"A");show_page();dispatch_data_requests();
 {std::unique_lock lock(loadMutex);assert(loadChanged.wait_for(lock,std::chrono::seconds(3),[]{return loadEntered;}));}
 strcpy(s_page_id,"B");show_page();dispatch_data_requests();
 action_back_to_main(nullptr);apply_data_results();assert(s_page_view.shows==0);
 {std::lock_guard lock(loadMutex);blockLoad=false;loadChanged.notify_all();}
 waitEntries(results,2);apply_data_results();
 assert(s_page_view.shows==1 && s_page_view.value=='B' && s_page_loading==nullptr);
 // Results from a previous preset never rebuild controls in the new preset.
 strcpy(s_page_id,"C");show_page();dispatch_data_requests();waitEntries(results,1);
 ++s_knob_preset_sequence;apply_data_results();assert(s_page_view.shows==1);
 show_page();dispatch_data_requests();waitEntries(results,1);apply_data_results();assert(s_page_view.value=='C');
 // A closed popup cannot reopen when its result arrives; tab IDs remain owned by UI.
 request_data(DataKind::Tabs);dispatch_data_requests();waitEntries(results,1);apply_data_results();
 assert(s_tab_list && populatedPages==0);
 s_pages_box=&popup;request_data(DataKind::Tabs);dispatch_data_requests();waitEntries(results,1);apply_data_results();
 assert(populatedPages==1);
 request_data(DataKind::Presets);dispatch_data_requests();waitEntries(results,1);apply_data_results();
 assert(s_preset_list.count==2 && populatedPresets==1);
 // Status polling reads the worker's cached position, even after a preset change.
 unsigned reads=filesystemCalls;EezSynthStatus status{};
 assert(eez_synth_status(&status) && status.preset_number==2 && status.preset_count==2);
 ++Feature::Synth::engine().sequence;assert(eez_synth_status(&status));
 assert(filesystemCalls==reads);
 // Pending selections coalesce to the latest page without filling the queue.
 for(char c='D';c<='Z';++c){s_page_id[0]=c;request_data(DataKind::Page);}
 dispatch_data_requests();waitEntries(results,1);apply_data_results();assert(s_page_view.value=='Z');
 // Queue saturation never blocks UI; unsent requests retry on the next timer tick.
 {std::lock_guard lock(requests.mutex);requests.capacity=0;}
 request_data(DataKind::Page);dispatch_data_requests();assert(s_data_pending[0]);
 {std::lock_guard lock(requests.mutex);requests.capacity=3;requests.changed.notify_all();}
 dispatch_data_requests();waitEntries(results,1);apply_data_results();assert(!s_data_pending[0]);
 // Allocation and parser failures clear the loading state with a visible error.
 allocationFails=true;show_page();dispatch_data_requests();waitEntries(results,1);apply_data_results();
 assert(s_page_loading && s_page_loading->text=="Seite konnte nicht geladen werden");
 parseFails=true;show_page();dispatch_data_requests();waitEntries(results,1);apply_data_results();
 assert(s_page_loading && s_page_loading->text=="Seite konnte nicht geladen werden");
 // Hiding and reopening the main view also restores its pending loading label.
 lv_obj_add_flag(s_page_loading,LV_OBJ_FLAG_HIDDEN);action_back_to_main(nullptr);
 assert(!lv_obj_has_flag(s_page_loading,LV_OBJ_FLAG_HIDDEN));
 show_page();dispatch_data_requests();waitEntries(results,1);apply_data_results();
 assert(s_page_loading==nullptr);
 // Cached pages open immediately; background preparation preserves selection.
 s_tab_list->count=2;strcpy(s_tab_list->ids[0],"C");strcpy(s_tab_list->ids[1],"D");
 strcpy(s_page_id,"C");show_page();dispatch_data_requests();waitEntries(results,1);apply_data_results();
 assert(s_cached_pages[0] && !s_page_fetching);
 auto generation=s_data_generation[0].load();show_page();
 assert(s_data_generation[0]==generation && s_page_loading==nullptr && s_page_view.value=='C');
 int shows=s_page_view.shows;prefetch_pages();dispatch_data_requests();waitEntries(results,1);apply_data_results();
 assert(s_cached_pages[1] && s_page_view.prepared==1 && s_page_view.shows==shows && s_page_view.value=='C');
 strcpy(s_page_id,"D");show_page();assert(s_page_view.value=='D' && s_page_loading==nullptr);
 // Identical preset metadata keeps existing table rows intact.
 request_data(DataKind::Presets);dispatch_data_requests();waitEntries(results,1);apply_data_results();
 assert(s_presets_ready && populatedPresets==1);
 {std::lock_guard lock(requests.mutex);requests.stop=true;requests.changed.notify_all();}
 worker.join();clear_page_cache();assert(allocations.empty());
 puts("PASS UI storage: blocked I/O, rapid selection, stale presets, popup ownership, queue backpressure and loading failures");
}
`);
function run(command, args) {
  const result = spawnSync(command, args, { cwd: temporary, encoding: "utf8", windowsVerbatimArguments: command === "cmd.exe", timeout: 30000 });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(result.stdout + result.stderr);
  return result.stdout;
}
if (process.platform === "win32") {
  const vswhere = path.join(process.env["ProgramFiles(x86)"], "Microsoft Visual Studio/Installer/vswhere.exe");
  const installation = run(vswhere, ["-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"]).trim();
  const vcvars = path.join(installation, "VC/Auxiliary/Build/vcvars64.bat");
  run("cmd.exe", ["/d", "/c", `call "${vcvars}" >nul && cl /nologo /EHsc /std:c++17 /Fe:test.exe test.cpp`]);
  console.log(run(path.join(temporary, "test.exe"), []).trim());
} else {
  run(process.env.CXX || "c++", ["-std=c++17", "-pthread", "test.cpp", "-o", "test"]);
  console.log(run(path.join(temporary, "test"), []).trim());
}
