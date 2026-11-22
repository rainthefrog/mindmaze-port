// Phone keyboard, Settings during the game and saving (CHANGE 2026-11-21) in the web build with a data folder
// (build/web/data + files.txt, as screens.mjs). The page is served by the test itself.
//   node tests/web/keyboard.mjs <build/web folder> <out dir>      (Playwright Docker image)
// Checked: "New player" focuses the hidden input #kbd (that is what opens Android's keyboard), typing through it the
// way Android keyboards do (composition, value changes, Backspace on a "blank" field) gives the right name, Enter
// commits it (saved at once in localStorage) and closes the keyboard; the gear opens Settings in the castle;
// MM.saveNow (page hidden / app paused) keeps the players.
import { chromium } from "playwright";
import { existsSync, readFileSync } from "fs";
import { extname, join } from "path";

const [web, out] = process.argv.slice(2);
const TYPES = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm", ".json": "application/json" };
let fails = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) fails++; };

const browser = await chromium.launch();
const ctx = await browser.newContext({ viewport: { width: 844, height: 390 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
const page = await ctx.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push(e.message));
await page.route("http://app.test/**", (r) => {
  const p = decodeURIComponent(new URL(r.request().url()).pathname).replace(/^\/$/, "/index.html");
  try {
    let body = readFileSync(join(web, p));
    if (p === "/data/files.txt")                            // only files that exist (an old list may name removed ones)
      body = body.toString().split(/\r?\n/).filter((l) => l && existsSync(join(web, "data", l))).join("\n");
    r.fulfill({ body, contentType: TYPES[extname(p)] || "application/octet-stream" });
  } catch { r.fulfill({ status: 404, body: "" }); }
});
await page.goto("http://app.test/index.html");
await page.waitForFunction(() => window.mmReady === true, null, { timeout: 120000 });
const tap = async (x, y) => {                                  // a real tap: pointer events on the canvas, like a finger
  const p = await page.evaluate(([x, y]) => {
    const s = Module._web_scale(), e = MM.ext(), r = MM.canvas.getBoundingClientRect();
    return [r.left + ((x + e) * s + 1) * r.width / MM.canvas.width, r.top + (y * s + 1) * r.height / MM.canvas.height];
  }, [x, y]);
  await page.mouse.click(p[0], p[1]);
  await page.waitForTimeout(150);
};
const active = () => page.evaluate(() => document.activeElement && document.activeElement.id);
const players = () => page.evaluate(() => Object.keys(localStorage).filter((k) => k.startsWith("mindmaze:MindmazePlayer"))
  .map((k) => localStorage.getItem(k)));

await page.waitForTimeout(500);
await tap(300, 200);                                           // ends the title screen
check(await active() !== "kbd", "no keyboard on the logon screen");
await tap(636 + 72, 354 + 18);                                  // New player (touch layout: in the column)
check(await active() === "kbd", "New player focuses the text input (opens the phone keyboard)");

check(await page.isVisible("#typing") && (await page.textContent("#typing-label")) === "New player's name",
      "the typing box with its label is shown at the top (above the phone keyboard)");
const top = await page.evaluate(() => document.getElementById("typing").getBoundingClientRect().bottom);
check(top < 390 / 2, `the box is in the top half (bottom at ${top.toFixed(0)} of 390 px)`);
await page.keyboard.press("Backspace");                        // Backspace in the empty box: harmless
// an Android keyboard: composition with growing values, then the keyboard corrects the word
await page.evaluate(() => {
  const k = MM.kbd, set = (v) => { k.value = v; k.dispatchEvent(new InputEvent("input", { bubbles: true })); };
  k.dispatchEvent(new CompositionEvent("compositionstart"));
  set("K"); set("Ki"); set("Kim"); set("Kimx");
  k.dispatchEvent(new CompositionEvent("compositionend"));
  set("Kim");
});
check(await page.inputValue("#kbd") === "Kim", "the box shows the game's text: " + await page.inputValue("#kbd"));
await page.keyboard.type("my");                                // a hardware keyboard into the same input
await page.keyboard.press("Backspace");                        // -> "Kimm"
await page.keyboard.press("Backspace");                        // -> "Kim"
await page.screenshot({ path: out + "/kbd-typing.png" });
await page.keyboard.press("Enter");
await page.waitForTimeout(200);
const saved = await players();
check(saved.some((v) => /,Kim$/.test(v)), "typed name committed and saved: " + JSON.stringify(saved));
check(await active() !== "kbd", "the keyboard closes after Enter");

// the game's limit (12 characters) shows in the box; OK commits like Enter
await tap(636 + 72, 354 + 18);
await page.keyboard.type("Abcdefghijklmnop");
check(await page.inputValue("#kbd") === "Abcdefghijkl", "the box keeps the game's 12-character limit: " + await page.inputValue("#kbd"));
await page.click("#typing-done");
await page.waitForTimeout(200);
check((await players()).some((v) => /,Abcdefghijkl$/.test(v)) && !(await page.isVisible("#typing")),
      "OK commits the name and closes the box");
// Settings while typing: tap New player, then Settings... -> settings open, keyboard closed
await tap(636 + 72, 354 + 18);
check(await active() === "kbd", "New player again: keyboard");
await tap(636 + 72, 396 + 18);                                  // Settings (column)
check(await active() !== "kbd", "Settings while typing closes the keyboard");
await page.screenshot({ path: out + "/settings-from-typing.png" });
await page.keyboard.press("Escape");

// into the castle with Kim (selected after the commit), gear -> Settings
await tap(310, 120);                                           // castle door
await tap(300, 150);                                           // grating
await tap(60, 150);                                            // a door of the doors screen
await page.waitForTimeout(400);
await page.screenshot({ path: out + "/room-gear.png" });
await tap(636 + 150, 396 + 19);                                 // Settings in the column (the gear is classic only)
await page.waitForTimeout(200);
await page.screenshot({ path: out + "/room-settings.png" });
await page.keyboard.press("Escape");

// the app to the background and back: the game pauses (audio output suspended), then runs again
await page.evaluate(() => MM.suspend(true));
check(await page.evaluate(() => MM.suspended && (!MM.audio || MM.audio.state === "suspended")), "suspended: audio stopped");
await page.evaluate(() => MM.suspend(false));
await page.waitForTimeout(200);
check(await page.evaluate(() => !MM.suspended && (!MM.audio || MM.audio.state === "running")), "back: audio runs again");
await page.evaluate(() => MM.saveNow());
check((await players()).some((v) => /,Kim$/.test(v)), "MM.saveNow (page hidden / app paused) keeps the players");
check(errors.length === 0, "no page errors " + errors.join("; "));
await browser.close();
console.log(fails ? `keyboard / settings / saving: ${fails} failed` : "keyboard / settings / saving: all checks ok");
process.exit(fails ? 1 : 0);
