import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || "playwright");
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined });
const page = await browser.newPage();
const errors = [];
page.on("pageerror", error => errors.push(error.message));
let releaseUpload;
let uploadStarted;
const started = new Promise(resolve => { uploadStarted = resolve; });
const uploadResponse = new Promise(resolve => { releaseUpload = resolve; });
await page.addInitScript(() => {
  window.__sockets = [];
  window.WebSocket = class extends EventTarget {
    static OPEN = 1;
    readyState = 0;
    constructor() { super(); window.__sockets.push(this); }
    send() {}
    close() { this.readyState = 3; this.dispatchEvent(new Event("close")); }
  };
});
await page.route("http://file-upload.test/**", async route => {
  const url = new URL(route.request().url());
  if (url.pathname === "/api/files/upload") {
    uploadStarted();
    await uploadResponse;
    return route.fulfill({ contentType: "application/json", body: '{"saved":true}' });
  }
  if (url.pathname.startsWith("/api/")) {
    const data = url.pathname === "/api/preset" ? { schemaVersion: 4, widgets: [], connections: [] }
      : url.pathname === "/api/presets" ? { presets: [] }
      : url.pathname === "/api/files/list" ? { dir: "", entries: [] }
      : url.pathname === "/api/settings/device" ? { volume: 1 } : [];
    return route.fulfill({ contentType: "application/json", body: JSON.stringify(data) });
  }
  try {
    const file = path.join(root, "data", url.pathname === "/" ? "index.html" : url.pathname);
    let body = readFileSync(file);
    if (url.pathname === "/static/js/main.js") body = Buffer.concat([body, Buffer.from(`
window.__uploadTest = { uploadFileExplorerFiles, markWsLost,
  state() { return { lost: wsHasBeenDisconnected, sockets: window.__sockets.length,
    uploading: fileExplorerModal.classList.contains('uploading'),
    overlay: !connectionLostOverlay.classList.contains('hidden') }; } };
`)]);
    return route.fulfill({ contentType: file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html", body });
  } catch { return route.fulfill({ status: 404 }); }
});
const state = () => page.evaluate(() => window.__uploadTest.state());
try {
  await page.goto("http://file-upload.test/");
  await page.waitForFunction(() => window.__uploadTest);
  await page.clock.install();
  await page.evaluate(() => {
    const socket = window.__sockets[0];
    socket.readyState = WebSocket.OPEN;
    socket.dispatchEvent(new Event("open"));
    socket.dispatchEvent(new MessageEvent("message", { data: '{"type":"cpuLoad","core0":0,"core1":0}' }));
    window.__uploadDone = false;
    window.__uploadTest.uploadFileExplorerFiles([new File([new Uint8Array(1024)], "large.wav")])
      .finally(() => { window.__uploadDone = true; });
  });
  await started;
  await page.clock.runFor(20000);
  assert.deepEqual(await state(), { lost: false, sockets: 1, uploading: true, overlay: false });
  releaseUpload();
  await page.waitForFunction(() => window.__uploadDone);
  await page.clock.runFor(9000);
  assert.deepEqual(await state(), { lost: false, sockets: 1, uploading: false, overlay: false });
  // With no resumed status messages, a genuine outage must still be detected after grace.
  await page.clock.runFor(2000);
  assert.equal((await state()).lost, true);
  assert.equal((await state()).overlay, true);
  assert.deepEqual(errors, []);
  console.log("PASS upload spanning 20 s of WS silence; completion grace; subsequent outage detected");
} finally {
  releaseUpload();
  await browser.close();
}
