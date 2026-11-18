// The app's first start: no data -> import screen -> the player's disc image -> extraction in a worker -> game.
// After a reload the data comes from IndexedDB (no import). Developer check, needs the real disc image.
//   node tests/web/import.mjs <url without data> <path of the .iso> <out dir>
import { chromium } from "playwright";

const [url, iso, out] = process.argv.slice(2);
const browser = await chromium.launch();
const ctx = await browser.newContext({ viewport: { width: 844, height: 390 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
const page = await ctx.newPage();
const logs = [];
page.on("pageerror", (e) => logs.push("pageerror: " + e.message));
page.on("console", (m) => { if (m.type() === "error") logs.push("console: " + m.text()); });
let fails = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) fails++; };

await page.goto(url);
await page.waitForSelector("#import", { state: "visible", timeout: 60000 });
await page.screenshot({ path: `${out}/7-import.png` });
check(true, "no data: the import screen is shown");
const t0 = Date.now();
await page.setInputFiles("#import-iso", iso);
await page.waitForTimeout(1500);
await page.screenshot({ path: `${out}/8-importing.png` });
try { await page.waitForFunction(() => window.mmReady === true, null, { timeout: 300000 }); }
catch (e) { console.log(await page.evaluate(() => document.getElementById("import-error").textContent)); }
check(await page.evaluate(() => window.mmReady === true), `the disc image was imported and the game started (${((Date.now() - t0) / 1000).toFixed(1)} s)`);
const n = await page.evaluate(() => FS.readdir("/data").length);
check(n > 120, `data folder in the app's storage: ${n} entries`);
await page.screenshot({ path: `${out}/9-after-import.png` });

await page.reload();
await page.waitForFunction(() => window.mmReady === true, null, { timeout: 60000 }).catch(() => {});
check(await page.evaluate(() => window.mmReady === true && document.getElementById("import").style.display !== "flex"),
      "after a reload the game starts from the stored data, no import");
console.log(logs.join("\n") || "no errors in the console");
console.log(fails ? fails + " FAILED" : "import: all ok");
await browser.close();
process.exit(fails ? 1 : 0);
