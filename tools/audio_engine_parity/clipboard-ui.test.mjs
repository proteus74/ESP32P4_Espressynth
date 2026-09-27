import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || "playwright");
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const browser = await chromium.launch({ executablePath: process.env.CHROMIUM_PATH || undefined });
const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
const errors = [];
page.on("pageerror", (error) => errors.push(error.message));
await page.addInitScript(() => {
  window.WebSocket = class extends EventTarget {
    static OPEN = 1;
    readyState = 1;
    send() {}
    close() {}
  };
});
await page.route("http://clipboard.test/**", async (route) => {
  const url = new URL(route.request().url());
  if (url.pathname.startsWith("/api/")) {
    const data = url.pathname === "/api/preset" ? { schemaVersion: 4, widgets: [], connections: [] }
      : url.pathname === "/api/presets" ? { presets: [] } : [];
    return route.fulfill({ contentType: "application/json", body: JSON.stringify(data) });
  }
  try {
    const file = path.join(root, "data", url.pathname === "/" ? "index.html" : url.pathname);
    let body = readFileSync(file);
    if (url.pathname === "/static/js/main.js") body = Buffer.concat([body, Buffer.from(`
window.__clipboardTest = { serializePreset, applyPresetData, switchPage, undoLastAction, redoLastAction,
 createWidget, applyWidgetParams,
 wireMaster(id) {
   connectionCounter += 1;
   connections.push({ id: 'conn-' + connectionCounter, from: { widgetId: id, portId: 'out' },
     to: { widgetId: sharedToMasterElement().id, portId: 'in1' } });
   refreshControlGraph(); drawConnections();
 },
 select(ids) { selectedWidgetIds = new Set(ids); updateSelectionClasses(); } };
`)]);
    return route.fulfill({ contentType: file.endsWith(".js") ? "text/javascript" : file.endsWith(".css") ? "text/css" : "text/html", body });
  } catch {
    return route.fulfill({ status: 404 });
  }
});

const fixture = {
  schemaVersion: 4,
  uiPages: [{ id: "generator", title: "Source" }, { id: "page-a", title: "A" }, { id: "page-b", title: "B" }],
  pages: {
    generator: {
      widgets: [
        { id: "widget-100", type: "const", label: "CV", ui: { x: 40, y: 40 }, params: { value: 6.25 } },
        { id: "widget-101", type: "gain", label: "VCA", ui: { x: 300, y: 160 }, params: { amount: 7, curve: 5, global: false } },
        { id: "widget-102", type: "const", label: "External", ui: { x: 40, y: 300 }, params: { value: 3 } },
      ],
      connections: [
        { id: "conn-1", from: { widgetId: "widget-100", portId: "value" }, to: { widgetId: "widget-101", portId: "amount" } },
        { id: "conn-2", from: { widgetId: "widget-102", portId: "value" }, to: { widgetId: "widget-101", portId: "curve" } },
      ],
    },
    fx: { widgets: [], connections: [] },
  },
};
const snapshot = () => page.evaluate(() => window.__clipboardTest.serializePreset());
let sourceGainParams;
let sourceWidgetCount;
let fxWidgetCount;
const widgetsOn = (data, name) => name === "fx" ? data.pages.fx.widgets
  : data.pages.generator.widgets.filter((widget) => (widget.ui.pageId || "generator") === name);
async function pasteOn(name) {
  await page.locator(`#pageTabList [data-page="${name}"]`).click();
  await page.mouse.move(650, 420);
  await page.keyboard.press("Control+v");
  assert.equal(await page.locator(".widget-paste-ghost").count(), 2);
  await page.mouse.click(650, 420);
  assert.equal(await page.locator(".widget-paste-ghost").count(), 0);
  const data = await snapshot();
  const copies = widgetsOn(data, name);
  const constant = copies.find((widget) => widget.type === "const");
  const gain = copies.find((widget) => widget.type === "gain");
  assert.equal(constant.params.value, 6.25);
  assert.deepEqual(gain.params, sourceGainParams);
  const wires = data.pages[name === "fx" ? "fx" : "generator"].connections
    .filter((connection) => connection.from.widgetId === constant.id);
  assert.equal(wires.length, 1);
  assert.deepEqual(wires[0].to, { widgetId: gain.id, portId: "amount" });
  assert.equal(gain.ui.x - constant.ui.x, 260);
  assert.equal(gain.ui.y - constant.ui.y, 120);
  return copies;
}

try {
  await page.goto("http://clipboard.test/");
  await page.waitForFunction(() => window.__clipboardTest && !document.getElementById("audioStartBtn")?.disabled);
  await page.evaluate((data) => window.__clipboardTest.applyPresetData(data), fixture);
  sourceGainParams = (await snapshot()).pages.generator.widgets.find((widget) => widget.id === "widget-101").params;
  sourceWidgetCount = widgetsOn(await snapshot(), "generator").length;
  fxWidgetCount = widgetsOn(await snapshot(), "fx").length;
  await page.evaluate(() => {
    window.__clipboardTest.switchPage("generator");
    window.__clipboardTest.select(["widget-100", "widget-101"]);
  });
  await page.keyboard.press("Control+c");
  const a = await pasteOn("page-a");
  const b = await pasteOn("page-b");
  assert.equal(a.length, 2);
  assert.equal(b.length, 2);
  assert.equal(widgetsOn(await snapshot(), "generator").length, sourceWidgetCount);
  assert.equal(new Set([...a, ...b].map((widget) => widget.id)).size, 4);

  await page.evaluate((ids) => window.__clipboardTest.select(ids), b.map((widget) => widget.id));
  await page.keyboard.press("Control+c");
  const fx = await pasteOn("fx");
  assert.equal(fx.length, fxWidgetCount + 2);
  await page.evaluate(() => window.__clipboardTest.undoLastAction());
  assert.equal(widgetsOn(await snapshot(), "fx").length, fxWidgetCount);
  await page.evaluate(() => window.__clipboardTest.redoLastAction());
  assert.equal(widgetsOn(await snapshot(), "fx").length, fxWidgetCount + 2);

  const saved = await snapshot();
  await page.evaluate((data) => window.__clipboardTest.applyPresetData(data), saved);
  assert.deepEqual((await snapshot()).pages, saved.pages);
  // Copy from FX back into a sound page, retaining the latest shared clipboard.
  await page.evaluate((ids) => {
    window.__clipboardTest.switchPage("fx");
    window.__clipboardTest.select(ids);
  }, fx.map((widget) => widget.id));
  await page.keyboard.press("Control+c");
  await page.locator('#pageTabList [data-page="generator"]').click();
  await page.mouse.move(800, 500);
  await page.keyboard.press("Control+v");
  await page.keyboard.press("Escape");
  assert.equal(await page.locator(".widget-paste-ghost").count(), 0);
  assert.equal(widgetsOn(await snapshot(), "generator").length, sourceWidgetCount);
  await page.keyboard.press("Control+v");
  await page.mouse.click(800, 500);
  assert.equal(widgetsOn(await snapshot(), "generator").length, sourceWidgetCount + 2);

  // Copy a whole sound page through the Edit menu, independent of widget selection.
  await page.locator('#pageTabList [data-page="page-a"]').click();
  assert.equal(await page.locator("#menuPastePage").isDisabled(), true);
  await page.evaluate((id) => window.__clipboardTest.wireMaster(id), a.find((widget) => widget.type === "gain").id);
  await page.evaluate(() => {
    const t = window.__clipboardTest;
    const input = t.createWidget("linked-input", "Link", 43, 57);
    t.applyWidgetParams("linked-input", input.id, { linkId: "page-source-link", linkName: "Page Link" });
    const output = t.createWidget("linked-output", "Linked", 363, 57);
    t.applyWidgetParams("linked-output", output.id, { sourceLinkId: "page-source-link" });
    t.select([]);
  });
  const original = widgetsOn(await snapshot(), "page-a");
  const edit = page.locator(".menu-item").filter({ has: page.locator("span", { hasText: /^Edit$/ }) });
  await edit.locator(":scope > span").click();
  await page.locator("#menuCopyPage").click();
  assert.equal(await page.locator("#menuPastePage").isDisabled(), false);
  await edit.locator(":scope > span").click();
  await page.locator("#menuPastePage").click();
  let pageCopy = await snapshot();
  const copiedPage = pageCopy.uiPages.find((entry) => entry.title === "A Copy");
  assert.ok(copiedPage);
  const copiedWidgets = widgetsOn(pageCopy, copiedPage.id);
  assert.equal(copiedWidgets.length, original.length);
  for (const source of original) {
    const clone = copiedWidgets.find((widget) => widget.type === source.type);
    assert.notEqual(clone.id, source.id);
    assert.equal(clone.ui.x, source.ui.x);
    assert.equal(clone.ui.y, source.ui.y);
    if (!source.type.startsWith("linked-")) assert.deepEqual(clone.params, source.params);
  }
  const clonedLink = copiedWidgets.find((widget) => widget.type === "linked-input");
  assert.notEqual(clonedLink.params.linkId, "page-source-link");
  assert.equal(copiedWidgets.find((widget) => widget.type === "linked-output").params.sourceLinkId, clonedLink.params.linkId);
  const clonedGain = copiedWidgets.find((widget) => widget.type === "gain");
  const master = pageCopy.pages.generator.widgets.find((widget) => widget.type === "audio-output-link");
  assert.equal(pageCopy.pages.generator.widgets.filter((widget) => widget.type === "audio-output-link").length, 1);
  assert.ok(pageCopy.pages.generator.connections.some((connection) => connection.from.widgetId === clonedGain.id && connection.to.widgetId === master.id));
  assert.deepEqual(copiedPage.toMasterUi, pageCopy.uiPages.find((entry) => entry.id === "page-a").toMasterUi);
  assert.deepEqual(widgetsOn(pageCopy, "page-a"), original);
  await page.evaluate(() => window.__clipboardTest.undoLastAction());
  assert.equal((await snapshot()).uiPages.some((entry) => entry.id === copiedPage.id), false);
  await page.evaluate(() => window.__clipboardTest.redoLastAction());
  assert.equal(widgetsOn(await snapshot(), copiedPage.id).length, original.length);
  await edit.locator(":scope > span").click();
  await page.locator("#menuPastePage").click();
  pageCopy = await snapshot();
  assert.ok(pageCopy.uiPages.some((entry) => entry.title === "A Copy 2"));
  await page.evaluate((data) => window.__clipboardTest.applyPresetData(data), pageCopy);
  const reloaded = await snapshot();
  for (const name of ["generator", "fx"]) {
    for (const field of ["widgets", "connections"]) {
      const byId = (a, b) => a.id.localeCompare(b.id);
      assert.deepEqual(reloaded.pages[name][field].toSorted(byId), pageCopy.pages[name][field].toSorted(byId));
    }
  }
  await page.locator('#pageTabList [data-page="fx"]').click();
  assert.equal(await page.locator("#menuCopyPage").isDisabled(), true);
  assert.equal(await page.locator("#menuPastePage").isDisabled(), false);
  // An empty page is still a valid page clipboard.
  await edit.locator(":scope > span").click();
  await page.locator("#menuAddPage").click();
  await edit.locator(":scope > span").click();
  await page.locator("#menuCopyPage").click();
  await edit.locator(":scope > span").click();
  await page.locator("#menuPastePage").click();
  const emptyCopy = (await snapshot()).uiPages.at(-1);
  assert.ok(emptyCopy.title.endsWith(" Copy"));
  assert.equal(widgetsOn(await snapshot(), emptyCopy.id).length, 0);
  assert.deepEqual(errors, []);
  console.log("PASS shared clipboard and Copy/Paste Page: parameters, connections, positions, master, linked I/O, repeated paste, undo/redo, save/load, empty pages, Escape");
} finally {
  await browser.close();
}
