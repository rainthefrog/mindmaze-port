// The app's import of a MindMaze data file (.zip) - the Windows export (stored entries) or a hand-made zip of a
// data folder (deflated, inside a "data/" folder). Developer check, needs an extracted data folder.
//   node tests/web/import_zip.mjs <url without data> <zip> <out dir> <name>
import { chromium } from "playwright";

const [url, zip, out, name] = process.argv.slice(2);
const browser = await chromium.launch();
const ctx = await browser.newContext({ viewport: { width: 844, height: 390 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
const page = await ctx.newPage();
const logs = [];
page.on("pageerror", (e) => logs.push("pageerror: " + e.message));
let fails = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) fails++; };

await page.goto(url);
await page.waitForSelector("#import", { state: "visible", timeout: 60000 });
await page.screenshot({ path: `${out}/7-import-screen.png` });
const t0 = Date.now();
await page.setInputFiles("#import-zip", zip);
try { await page.waitForFunction(() => window.mmReady === true, null, { timeout: 180000 }); }
catch (e) { console.log(await page.evaluate(() => document.getElementById("import-error").textContent)); }
check(await page.evaluate(() => window.mmReady === true), `${name}: imported and started (${((Date.now() - t0) / 1000).toFixed(1)} s)`);
const n = await page.evaluate(() => { try { return FS.readdir("/data").length; } catch (e) { return 0; } });
check(n > 120, `${name}: ${n} entries in the app's data folder`);
console.log(logs.join("\n") || "no errors in the console");
await browser.close();
process.exit(fails ? 1 : 0);
