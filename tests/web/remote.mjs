// Opening the app with links (CHANGE 2026-11-18): mindmaze://import?url=<disc image>&art=<HD pictures zip> in the APK,
// index.html?url=&art= in a browser: the disc image is imported in pieces and the HD pictures installed; then
// Settings > Add questions from a disc -> "Choose a file on this device" adds another disc's questions.
// Developer check (the links need a server that allows HTTP Range and CORS):
//   node tests/web/remote.mjs <app url without data> <disc image link> <HD pictures zip link> <other .iso> <out dir>
import { chromium } from "playwright";

const [url, disc, art, otherIso, out] = process.argv.slice(2);
const browser = await chromium.launch();
const ctx = await browser.newContext({ viewport: { width: 844, height: 390 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
const page = await ctx.newPage();
const logs = [];
page.on("pageerror", (e) => logs.push("pageerror: " + e.message));
let fails = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) fails++; };

const t0 = Date.now();
await page.goto(url + "?url=" + encodeURIComponent(disc) + "&art=" + encodeURIComponent(art));
try { await page.waitForFunction(() => window.mmReady === true, null, { timeout: 600000 }); }
catch (e) { console.log(await page.evaluate(() => document.getElementById("import-error").textContent)); }
check(await page.evaluate(() => window.mmReady === true), `the links were imported (${((Date.now() - t0) / 1000).toFixed(1)} s)`);
const st = await page.evaluate(() => ({
  tables: FS.analyzePath("/data/tables.json").exists,
  hd: FS.analyzePath("/data/hd/bounds.txt").exists ? FS.readdir("/data/hd").length - 2 : 0,
  wide: FS.analyzePath("/data/wide/wide.txt").exists ? FS.readdir("/data/wide").length - 2 : 0,
}));
check(st.tables && st.hd > 100 && st.wide > 30, `data folder with HD pictures: ${JSON.stringify(st)}`);

const ltap = (x, y) => page.evaluate(([x, y]) => MM.tap(x, y), [x, y]);
await page.waitForTimeout(1000);
await ltap(300, 200);                                              // ends the title screen
await page.waitForTimeout(400);
await ltap(540, 412);                                              // Settings...
await page.waitForTimeout(400);
await ltap(372 + 60, 56);                                          // Add questions from a disc...
await page.waitForSelector("#remote", { state: "visible", timeout: 5000 }).catch(() => {});
check(await page.isVisible("#remote"), "the overlay opens");
const chooser = page.waitForEvent("filechooser", { timeout: 10000 }).catch(() => null);
await page.click("#remote-file");
const fc = await chooser;
check(fc !== null, "a file chooser opens from the overlay");
if (fc) await fc.setFiles(otherIso);
await page.waitForFunction(() => FS.analyzePath("/data/editions/index.txt").exists, null, { timeout: 300000 }).catch(() => {});
await page.waitForTimeout(1500);
const idx = await page.evaluate(() => FS.analyzePath("/data/editions/index.txt").exists ? FS.readFile("/data/editions/index.txt", { encoding: "utf8" }) : "");
check(/\t\d+/.test(idx), "questions added: " + idx.trim());
await page.screenshot({ path: `${out}/r2-added.png` });
console.log(logs.join("\n") || "no errors in the console");
console.log(fails ? fails + " FAILED" : "links: all ok");
await browser.close();
process.exit(fails ? 1 : 0);
