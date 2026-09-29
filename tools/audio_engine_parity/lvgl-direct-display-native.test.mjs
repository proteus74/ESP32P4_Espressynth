import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const source = readFileSync(path.join(root, "main/features/display/display_lvgl.hpp"), "utf8");
function method(signature) {
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
const temporary = mkdtempSync(path.join(os.tmpdir(), "espressynth-direct-display-"));
writeFileSync(path.join(temporary, "test.cpp"), `
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#define CONFIG_APP_ENABLE_LITTLEFS 1
using esp_lcd_panel_handle_t=void *;
namespace CurrentBoard { constexpr uint32_t DISPLAY_NATIVE_WIDTH=1024,DISPLAY_NATIVE_HEIGHT=600; }
constexpr int LV_COLOR_FORMAT_RGB565=16,LV_EVENT_FLUSH_FINISH=1;
struct lv_display_t { bool last=false; } display;
struct lv_event_t { lv_display_t *target; } event{&display};
struct lvgl_port_display_cfg_t {
 esp_lcd_panel_handle_t panel_handle=nullptr;
 uint32_t hres=0,vres=0,buffer_size=0;
 bool double_buffer=false;
 int color_format=0;
 struct { bool direct_mode=false,full_refresh=false; } flags;
};
struct lvgl_port_display_dsi_cfg_t { struct { bool avoid_tearing=false; } flags; };
bool allocationFails=false;
lv_display_t *lvgl_port_add_disp_dsi(const lvgl_port_display_cfg_t *cfg,const lvgl_port_display_dsi_cfg_t *dsi) {
 assert(cfg->panel_handle==&display);
 assert(cfg->hres==1024 && cfg->vres==600 && cfg->buffer_size==1024*600);
 assert(cfg->color_format==LV_COLOR_FORMAT_RGB565 && cfg->double_buffer);
 // DIRECT keeps dirty-area rendering; avoid_tearing reuses both DPI buffers and
 // lets the ESP LVGL port wait for frame completion before reusing a buffer.
 assert(cfg->flags.direct_mode && !cfg->flags.full_refresh && dsi->flags.avoid_tearing);
 return allocationFails ? nullptr : &display;
}
using EventCallback=void (*)(lv_event_t *);
EventCallback callback=nullptr;
int registrations=0;
void lv_display_add_event_cb(lv_display_t *d,EventCallback cb,int code,void *user) {
 assert(d==&display && code==LV_EVENT_FLUSH_FINISH && user==nullptr);
 callback=cb; ++registrations;
}
void *lv_event_get_target(lv_event_t *e) { return e->target; }
bool lv_display_flush_is_last(lv_display_t *d) { return d->last; }
std::atomic<bool> s_splash_frame_pending{true};
std::atomic<uint32_t> s_splash_visible_since{0};
uint32_t tick=42;
uint32_t xTaskGetTickCount() { return tick; }
${method("void on_direct_flush_finished(")}
${method("lv_display_t* create_direct_display(")}
int main() {
 assert(create_direct_display(&display)==&display && registrations==1);
 // Intermediate dirty rectangles do not switch the frame or start the splash timer.
 callback(&event);
 assert(s_splash_frame_pending && s_splash_visible_since==0);
 display.last=true;
 callback(&event);
 assert(!s_splash_frame_pending && s_splash_visible_since==42);
 tick=80; callback(&event);
 assert(s_splash_visible_since==42);
 allocationFails=true;
 assert(create_direct_display(&display)==nullptr && registrations==1);
 std::puts("Native LVGL direct display: double-buffer configuration, dirty rendering, splash completion and allocation failure passed");
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
