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
const temporary = mkdtempSync(path.join(os.tmpdir(), "espressynth-dma-flush-"));
writeFileSync(path.join(temporary, "test.cpp"), `
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#define CONFIG_APP_ENABLE_LITTLEFS 1
using BaseType_t=int;
using esp_err_t=int;
using esp_lcd_panel_handle_t=void *;
struct esp_lcd_dpi_panel_event_data_t {};
constexpr int pdFALSE=0,pdTRUE=1,ESP_OK=0,ESP_ERR_TIMEOUT=1;
#define pdMS_TO_TICKS(x) (x)
#define ESP_ERROR_CHECK(x) do { if((x)!=ESP_OK) throw std::runtime_error("timeout"); } while(0)
#define ESP_LOGE(...) ((void)0)
struct Semaphore { bool done=false; } semaphore;
Semaphore *s_dpi_copy_done=nullptr;
int xSemaphoreTake(Semaphore *s,int) { if(!s->done)return pdFALSE; s->done=false; return pdTRUE; }
void xSemaphoreGiveFromISR(Semaphore *s,BaseType_t *wake) { s->done=true; *wake=pdTRUE; }
struct lv_display_t { bool last=true; int readyCalls=0; };
struct lv_area_t { int x1,y1,x2,y2; };
bool lv_display_flush_is_last(lv_display_t *d) { return d->last; }
void lv_display_flush_ready(lv_display_t *d) { ++d->readyCalls; }
esp_lcd_panel_handle_t s_panel=nullptr;
int result=ESP_OK,drawCalls=0;
int esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t,int x1,int y1,int x2,int y2,const void *pixels) {
 assert(x1==10 && y1==20 && x2==30 && y2==40 && pixels!=nullptr); ++drawCalls; return result;
}
const char *esp_err_to_name(int) { return "error"; }
std::atomic<bool> s_splash_frame_pending{true};
std::atomic<uint32_t> s_splash_visible_since{0};
std::atomic<bool> s_dma_splash_pending{false};
uint32_t tick=42;
uint32_t xTaskGetTickCount() { return tick; }
uint32_t xTaskGetTickCountFromISR() { return tick; }
${method("bool on_dpi_copy_done(")}
${method("void flush_wait_cb(")}
${method("void flush_cb(")}
int main() {
 lv_display_t display; lv_area_t area{10,20,29,39}; uint8_t pixels[800]{};
 // Synchronous fallback releases the buffer immediately after the copy.
 flush_cb(&display,&area,pixels);
 assert(display.readyCalls==1 && s_splash_visible_since==42 && !s_splash_frame_pending);
 // DMA submission alone must not release the buffer or start the splash interval.
 s_dpi_copy_done=&semaphore; s_splash_frame_pending=true; s_splash_visible_since=0;
 flush_cb(&display,&area,pixels);
 assert(display.readyCalls==1 && s_splash_frame_pending && s_splash_visible_since==0);
 // Waiting without completion fails rather than reusing memory still read by DMA.
 bool timedOut=false; try { flush_wait_cb(&display); } catch(const std::runtime_error &) { timedOut=true; }
 assert(timedOut && s_splash_frame_pending);
 tick=60; assert(on_dpi_copy_done(nullptr,nullptr,nullptr));
 assert(!s_splash_frame_pending && s_splash_visible_since==60);
 flush_wait_cb(&display);
 assert(!semaphore.done && !s_splash_frame_pending && s_splash_visible_since==60);
 // Intermediate strips cannot mark the entire splash frame visible.
 display.last=false; s_splash_frame_pending=true;
 flush_cb(&display,&area,pixels); on_dpi_copy_done(nullptr,nullptr,nullptr); flush_wait_cb(&display);
 assert(s_splash_frame_pending && s_splash_visible_since==60);
 // Failed submissions release the LVGL buffer, so no completion wait is scheduled.
 result=2; flush_cb(&display,&area,pixels);
 assert(display.readyCalls==2 && drawCalls==4 && !semaphore.done);
 std::puts("Native LVGL DMA: completion before buffer reuse, yielding wait, splash timing, error and CPU fallback passed");
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
