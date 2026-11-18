// Headless browser check of the web build: loads build/web (with a data folder) and takes screenshots at phone
// landscape size. Run in the Playwright Docker image (see tests/web/run.sh). Developer check, needs real data.
//   node tests/web/screens.mjs <url> <out dir>
import { chromium } from "playwright";

const [url, out] = process.argv.slice(2);
const browser = await chromium.launch();
const ctx = await browser.newContext({ viewport: { width: 844, height: 390 }, deviceScaleFactor: 3, isMobile: true, hasTouch: true });
const page = await ctx.newPage();
const logs = [];
page.on("console", (m) => logs.push(m.type() + ": " + m.text()));
page.on("pageerror", (e) => logs.push("pageerror: " + e.message));
await page.addInitScript(() => { localStorage.setItem("mindmaze:seed", "99"); });

const shot = async (name) => { await page.waitForTimeout(400); await page.screenshot({ path: `${out}/${name}.png` }); console.log("screenshot", name); };
const tap = (x, y) => page.evaluate(([x, y]) => MM.tap(x, y), [x, y]);

await page.goto(url);
try { await page.waitForFunction(() => window.mmReady === true, null, { timeout: 60000 }); }
catch (e) { await page.screenshot({ path: `${out}/0-failed.png` }); console.log(logs.join("\n")); console.log(await page.evaluate(() => document.getElementById("msg").textContent)); process.exit(1); }
const rms = () => page.evaluate(() => {                // render ~2 s of the playing track straight from the synth
  const p = Module._malloc(4096 * 8); let s = 0;
  for (let k = 0; k < 22; k++) { Module._web_music_render(p, 4096); const f = new Float32Array(Module.HEAPU8.buffer, p, 8192); for (const v of f) s += v * v; }
  Module._free(p); return Math.sqrt(s / (22 * 8192));
});
console.log("music state", await page.evaluate(() => Module._web_music_state().toString(2)), "track", await page.evaluate(() => JSON.stringify(MM.track)));
console.log("music level (RMS):", (await rms()).toFixed(4));
await shot("1-title");
await tap(300, 200);                                   // a tap ends the title screen
await shot("2-logon");
await tap(540, 412);                                   // Settings...
await shot("3-settings");
await tap(78 + 156 + 20, 22 + 392 - 25);               // Pictures / HD art...
await shot("4-settings-pictures");
await tap(78 + 480 - 60, 22 + 392 - 20);               // Back
await tap(78 + 480 - 60, 22 + 392 - 20);               // Done
await tap(491, 305);                                   // select Tourist
await tap(310, 120);                                   // the castle door
await page.keyboard.press("Enter");
await page.keyboard.press("Enter");
await shot("5-room");
for (const [x, y] of [[315, 130], [70, 160], [560, 160]]) await tap(x, y);   // a door: a question opens
await page.evaluate(() => MM.hover(330, 301 + 40 + 9));                       // hover answer B
await shot("6-question");
console.log(logs.filter((l) => !l.startsWith("log:")).join("\n") || "no errors in the console");
await browser.close();
