// Parity checks of the browser preview engine (data/static/js/audio-engine.js)
// against the firmware's node formulas (main/audio/engine/nodes/*.h).
//
// Runs tests.js inside headless Chromium with every AudioContext swapped for an
// OfflineAudioContext, so the results are deterministic and need no speakers.
//
//   npm install playwright          (once; or use a globally installed copy)
//   node tools/audio_engine_parity/run.mjs
//
// CHROMIUM_PATH=/path/to/chromium overrides Playwright's bundled browser.
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

let playwright;
try {
  playwright = await import("playwright");
} catch (err) {
  console.error("Playwright not found - run `npm install playwright` first.");
  process.exit(2);
}

const here = path.dirname(fileURLToPath(import.meta.url));
const staticRoot = path.resolve(here, "../../data/static");

const browser = await playwright.chromium.launch({
  executablePath: process.env.CHROMIUM_PATH || undefined,
});
const page = await browser.newPage();
page.on("pageerror", (err) => console.log("[pageerror]", err.message));
page.on("console", (msg) => {
  if (msg.type() === "error") console.log("[page]", msg.text());
});
await page.route("http://parity.test/**", (route) => {
  const url = new URL(route.request().url());
  if (url.pathname === "/") {
    return route.fulfill({ contentType: "text/html", body: "<!doctype html><title>parity</title>" });
  }
  const file = url.pathname.startsWith("/tests/")
    ? path.join(here, url.pathname.slice("/tests/".length))
    : path.join(staticRoot, url.pathname);
  try {
    return route.fulfill({ contentType: "text/javascript", body: readFileSync(file) });
  } catch (err) {
    return route.fulfill({ status: 404, body: String(err) });
  }
});
await page.goto("http://parity.test/");
const results = await page.evaluate(async () => (await import("/tests/tests.js")).runAll());
await browser.close();

let failed = 0;
for (const r of results) {
  if (!r.ok) failed += 1;
  console.log(`${r.ok ? "PASS" : "FAIL"}  ${r.name}${r.detail ? `  - ${r.detail}` : ""}`);
}
console.log(`\n${results.length - failed}/${results.length} passed`);
process.exit(failed ? 1 : 0);
