import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || "playwright");
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined });
const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
const errors = [];
const saves = [];
page.on("pageerror", (error) => errors.push(error.message));
await page.addInitScript(() => {
  window.WebSocket = class extends EventTarget {
    static OPEN = 1;
    readyState = 1;
    send() {}
    close() {}
  };
});
await page.route("http://tabs.test/**", async (route) => {
  const url = new URL(route.request().url());
  if (url.pathname.startsWith("/api/")) {
    if (url.pathname === "/api/preset" && route.request().method() === "POST") {
      saves.push(route.request().postDataJSON());
      return route.fulfill({ contentType: "application/json", body: '{"saved":true}' });
    }
    const data = url.pathname === "/api/preset" ? { schemaVersion: 4, widgets: [], connections: [] }
      : url.pathname === "/api/presets" ? { presets: [] } : [];
    return route.fulfill({ contentType: "application/json", body: JSON.stringify(data) });
  }
  try {
    const file = path.join(root, "data", url.pathname === "/" ? "index.html" : url.pathname);
    let body = readFileSync(file);
    if (url.pathname === "/static/js/main.js") body = Buffer.concat([body, Buffer.from(`
window.__tabsTest = { serializePreset, applyPresetData, saveToServer, undoLastAction, redoLastAction,
 startPageRename, reorderPageTab,
 currentPage() { return currentPageName; },
 historyLength() { return undoStack.length; } };
`)]);
    return route.fulfill({ contentType: file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html", body });
  } catch {
    return route.fulfill({ status: 404 });
  }
});

const fixture = {
  schemaVersion: 4,
  uiPages: [{ id: "generator", title: "Drum" }, { id: "page-a", title: "Sound A" }, { id: "page-b", title: "Sound B" }],
  pages: {
    generator: {
      widgets: [{ id: "widget-101", type: "const", label: "Test CV", ui: { x: 40, y: 40, pageId: "page-a" }, params: {} }],
      connections: [],
    },
    fx: { widgets: [], connections: [] },
  },
};
const order = () => page.locator("#pageTabList .page-tab").evaluateAll((tabs) => tabs.map((tab) => tab.dataset.page));
const tab = (name) => page.locator(`#pageTabList .page-tab[data-page="${name}"]`);

try {
  await page.goto("http://tabs.test/");
  await page.waitForFunction(() => window.__tabsTest && !document.getElementById("audioStartBtn")?.disabled);
  await page.evaluate((data) => window.__tabsTest.applyPresetData(data), fixture);
  assert.deepEqual(await order(), ["generator", "page-a", "page-b", "fx"]);
  const active = await page.evaluate(() => window.__tabsTest.currentPage());
  const saveCount = saves.length;

  await tab("fx").dragTo(tab("generator"), { targetPosition: { x: 4, y: 15 } });
  assert.deepEqual(await order(), ["fx", "generator", "page-a", "page-b"]);
  const targetWidth = (await tab("page-b").boundingBox()).width;
  await tab("generator").dragTo(tab("page-b"), { targetPosition: { x: targetWidth - 4, y: 15 } });
  assert.deepEqual(await order(), ["fx", "page-a", "page-b", "generator"]);
  assert.equal(await page.evaluate(() => window.__tabsTest.currentPage()), active);
  assert.equal(saves.length, saveCount, "Reordering must wait for a real preset save");
  assert.equal(await page.locator(".page-tab.dragging, .page-tab.drop-before, .page-tab.drop-after").count(), 0);

  await tab("fx").focus();
  await page.keyboard.press("Alt+ArrowRight");
  assert.deepEqual(await order(), ["page-a", "fx", "page-b", "generator"]);
  assert.equal(await tab("fx").evaluate((el) => el === document.activeElement), true);
  await page.evaluate(() => window.__tabsTest.undoLastAction());
  assert.deepEqual(await order(), ["fx", "page-a", "page-b", "generator"]);
  await page.evaluate(() => window.__tabsTest.redoLastAction());
  assert.deepEqual(await order(), ["page-a", "fx", "page-b", "generator"]);

  const snapshot = await page.evaluate(() => window.__tabsTest.serializePreset());
  assert.deepEqual(snapshot.uiTabOrder, await order());
  assert.equal(await page.evaluate(() => window.__tabsTest.saveToServer("Tab Order")), true);
  const saved = saves.at(-1);
  assert.deepEqual(saved.uiTabOrder, snapshot.uiTabOrder);
  await page.evaluate((data) => window.__tabsTest.applyPresetData(data), saved);
  assert.deepEqual(await order(), snapshot.uiTabOrder);
  assert.equal(await page.locator("#widget-101").count(), 1);
  assert.equal((await page.evaluate(() => window.__tabsTest.serializePreset())).pages.generator.widgets
    .find((widget) => widget.id === "widget-101").ui.pageId, "page-a");
  await tab("page-a").click();
  assert.equal(await page.evaluate(() => window.__tabsTest.currentPage()), "page-a");

  await page.evaluate(() => window.__tabsTest.startPageRename("page-a"));
  const input = tab("page-a").locator("input");
  await input.fill("Renamed");
  await input.press("Enter");
  assert.equal(await tab("page-a").textContent(), "Renamed");

  const history = await page.evaluate(() => window.__tabsTest.historyLength());
  await page.evaluate(() => {
    window.__tabsTest.reorderPageTab("fx", "fx");
    window.__tabsTest.reorderPageTab("unknown", "fx");
  });
  assert.equal(await page.evaluate(() => window.__tabsTest.historyLength()), history);
  await page.evaluate((data) => window.__tabsTest.applyPresetData(data), fixture);
  assert.deepEqual(await order(), ["generator", "page-a", "page-b", "fx"], "Legacy presets use the default order");
  await page.evaluate((data) => window.__tabsTest.applyPresetData({ ...data, uiTabOrder: ["fx", "fx", "unknown", "page-b"] }), fixture);
  assert.deepEqual(await order(), ["fx", "page-b", "generator", "page-a"]);
  assert.deepEqual(errors, []);
  console.log("PASS page tabs: native drag/drop, keyboard, undo/redo, save/load, rename, legacy and invalid order");
} finally {
  await browser.close();
}
