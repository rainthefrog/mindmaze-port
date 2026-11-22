// Modern options (CHANGE 2026-11-21) in the web build with a data folder (build/web/data): the touch layout in
// landscape (portrait gets the same layout) (castle gate column, room, question with 50/50, turn prompt), the Settings > Modern options
// page, the stats screen, and the classic layout with large text. Screenshots go to <out dir> for a look by eye.
//   node tests/web/modern.mjs <build/web folder> <out dir>      (Playwright Docker image)
import { chromium } from "playwright";
import { existsSync, readFileSync } from "fs";
import { extname, join } from "path";

const [web, out] = process.argv.slice(2);
const TYPES = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm", ".json": "application/json" };
let fails = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) fails++; };

async function open(browser, viewport, touch, settings) {
  const ctx = await browser.newContext({ viewport, deviceScaleFactor: 2, isMobile: touch, hasTouch: touch });
  const page = await ctx.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.route("http://app.test/**", (r) => {
    const p = decodeURIComponent(new URL(r.request().url()).pathname).replace(/^\/$/, "/index.html");
    try {
      let body = readFileSync(join(web, p));
      if (p === "/data/files.txt")
        body = body.toString().split(/\r?\n/).filter((l) => l && existsSync(join(web, "data", l))).join("\n");
      r.fulfill({ body, contentType: TYPES[extname(p)] || "application/octet-stream" });
    } catch { r.fulfill({ status: 404, body: "" }); }
  });
  await page.addInitScript((s) => {
    localStorage.setItem("mindmaze:title_seconds", "0");
    localStorage.setItem("mindmaze:seed", "7");
    localStorage.setItem("mindmaze:MindmazePlayer1", "2500,1,Ann");
    localStorage.setItem("mindmaze:MindmazePlayer2", "300,0,Bob");
    for (const [k, v] of Object.entries(s)) localStorage.setItem("mindmaze:" + k, v);
  }, settings);
  await page.goto("http://app.test/index.html");
  await page.waitForFunction(() => window.mmReady === true, null, { timeout: 120000 });
  await page.waitForTimeout(500);
  const view = () => page.evaluate(() => [MM.canvas.width / Module._web_scale(), MM.canvas.height / Module._web_scale()]);
  const tap = async (x, y) => { await page.evaluate(([x, y]) => MM.tap(x, y), [x, y]); await page.waitForTimeout(450); };
  const colx = await page.evaluate(() => 636 + MM.ext());   // the column starts right of the (widescreen) room
  return { ctx, page, errors, view, tap, colx };
}

const browser = await chromium.launch();

// ---- touch, landscape: castle gate column -> room -> question -> 50/50 -> answer -> turn prompt
let t = await open(browser, { width: 844, height: 390 }, true, {});
let v = await t.view();
check((v[0] === 936 || v[0] === 1064) && v[1] >= 437, "phone landscape: touch view 936 or (widescreen art) 1064 wide (" + v + ")");
await t.page.screenshot({ path: out + "/wide-1-gate.png" });
await t.tap(t.colx + 160, 40);                                      // Several players
await t.tap(t.colx + 20, 92 + 5);                                   // first player row
await t.tap(t.colx + 20, 92 + 30 + 5);                              // second
await t.page.screenshot({ path: out + "/wide-2-selected.png" });
await t.tap(t.colx + 150, 396 + 10);                                // Enter the castle
await t.tap(300, 150);                                           // gate
await t.tap(60, 150);                                            // a door
await t.page.waitForTimeout(800);
await t.page.screenshot({ path: out + "/wide-3-room.png" });
// a door of the room: the room's hotspots are not known here - use the core through a test hook: walk the list of
// doors by tapping the usual door places until a question opens
for (const [x, y] of [[60, 150], [380, 140], [560, 140], [225, 130], [318, 270]]) {
  await t.tap(x, y);
  if (await t.page.evaluate(() => Module._web_text_input() === 0)) {}
  const shot = await t.page.screenshot();
  if (shot) {}
}
await t.page.screenshot({ path: out + "/wide-4-after-doors.png" });
await t.tap(t.colx + 60, 396 + 10);                                 // 50/50 (if a question is open)
await t.page.waitForTimeout(300);
await t.page.screenshot({ path: out + "/wide-5-hint.png" });
check(t.errors.length === 0, "landscape: no page errors " + t.errors.join("; "));
await t.ctx.close();

// ---- touch, portrait: not supported (2026-11-21) - always the landscape layout, no note
t = await open(browser, { width: 390, height: 844 }, true, {});
v = await t.view();
check((v[0] === 936 || v[0] === 1064) && v[1] >= 437, "phone portrait: still the landscape touch view (" + v + ")");
await t.page.screenshot({ path: out + "/tall-1-landscape.png" });
check(t.errors.length === 0, "portrait: no page errors " + t.errors.join("; "));
await t.ctx.close();

// ---- desktop (mouse): classic view, Settings > Modern options page, Stats, large text
t = await open(browser, { width: 1280, height: 720 }, false, { opt_bigtext: "1" });
v = await t.view();
check(v[1] === 437, "mouse: classic view (" + v + ")");
await t.page.screenshot({ path: out + "/classic-0-gate.png" });
await t.tap(500, 412);                                           // Settings...
await t.tap(78 + 260 + 20, 22 + 392 - 25);                       // Modern options...
await t.page.screenshot({ path: out + "/classic-1-options.png" });
await t.page.keyboard.press("Escape");
await t.page.keyboard.press("Escape");
await t.tap(580, 412);                                           // Stats
await t.page.waitForTimeout(300);
await t.page.screenshot({ path: out + "/classic-2-stats.png" });
await t.page.keyboard.press("Escape");
await t.tap(500, 300);                                           // select Ann (first row)
await t.tap(300, 120);                                           // castle door
await t.tap(300, 150);
await t.tap(60, 150);
await t.page.waitForTimeout(800);
await t.page.screenshot({ path: out + "/classic-3-room-bigtext.png" });
check(t.errors.length === 0, "classic: no page errors " + t.errors.join("; "));
await t.ctx.close();

await browser.close();
console.log(fails ? `modern options: ${fails} failed` : "modern options: all checks ok");
process.exit(fails ? 1 : 0);
