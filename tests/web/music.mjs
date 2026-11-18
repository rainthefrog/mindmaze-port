// Music switching in the web build, checked against the original's rules (docs/SPEC.md "Sound and music"):
// logon: INTRO.MID once; its end starts a BGLOOP; each loop's end starts a different loop; entering the castle
// does not switch; a question pauses the loop and plays QUESTION.MID; closing it resumes the loop.
// Time is advanced by rendering the audio directly (headless browsers have no sound device).
//   node tests/web/music.mjs <url>
import { chromium } from "playwright";

const url = process.argv[2];
const browser = await chromium.launch();
const page = await (await browser.newContext({ viewport: { width: 844, height: 390 } })).newPage();
await page.addInitScript(() => { localStorage.setItem("mindmaze:seed", "99"); localStorage.setItem("mindmaze:title_seconds", "0"); });
await page.goto(url);
await page.waitForFunction(() => window.mmReady === true, null, { timeout: 120000 });

let fails = 0;
const state = () => page.evaluate(() => ({ t0: MM.track[0], t1: MM.track[1], p0: MM.paused[0] }));
const check = (ok, what, s) => { console.log((ok ? "ok   " : "FAIL ") + what + "  " + JSON.stringify(s)); if (!ok) fails++; };
const play = async (seconds) => {                       // render audio for `seconds`, then let the game's frame run
  await page.evaluate((sec) => {
    const n = 4096, p = Module._malloc(n * 8);
    for (let k = 0; k < Math.ceil(sec * 44100 / n); k++) Module._web_music_render(p, n);
    Module._free(p);
  }, seconds);
  await page.waitForTimeout(150);
};
const tap = (x, y) => page.evaluate(([x, y]) => MM.tap(x, y), [x, y]);

let s = await state();
check(s.t0 === "INTRO.MID" && !s.t1, "logon screen plays INTRO.MID", s);
await play(10);
await tap(491, 305); await tap(310, 120);               // Tourist, castle door
s = await state();
check(/^BGLOOP[123].MID$/.test(s.t0), "the castle door click switches to a maze loop (CHANGE, with a fade)", s);
await page.keyboard.press("Enter"); await page.keyboard.press("Enter");
s = await state();
check(/^BGLOOP[123].MID$/.test(s.t0), "entering the castle starts a maze loop (CHANGE: the original let INTRO play on)", s);
await play(40);
s = await state();
check(/^BGLOOP[123]\.MID$/.test(s.t0), "INTRO's end starts a background loop", s);
await page.evaluate(() => { const f = MM.music; window.mmLog = [MM.track[0]]; MM.music = (ch, n) => { if (ch === 0 && n) mmLog.push(n); f(ch, n); }; });
for (let i = 0; i < 120; i++) await play(2);                                 // 4 minutes of loops, 2 s steps
const loops = await page.evaluate(() => mmLog);
check(loops.length >= 6 && loops.every((l, i) => i === 0 || l !== loops[i - 1]), "each loop's end starts a different loop: " + loops.join(" -> "), {});
for (const [x, y] of [[315, 130], [70, 160], [560, 160]]) await tap(x, y);   // a door: a question opens
s = await state();
check(s.t1 === "QUESTION.MID" && s.p0 === 1, "a question pauses the loop and plays QUESTION.MID", s);
await play(160);
s = await state();
check(s.t1 === "QUESTION.MID", "QUESTION.MID starts again at its end (2.5 min)", s);
for (let b = 0; b < 4 && (await state()).t1; b++) {     // answer until the question closes (OK on each message)
  await tap(330, 301 + 10 + 30 * b + 9); await tap(318, 243);
}
s = await state();
check(!s.t1 && s.p0 === 0 && /^BGLOOP/.test(s.t0), "closing the question resumes the loop", s);
console.log(fails ? fails + " FAILED" : "music switching: all ok");
await browser.close();
process.exit(fails ? 1 : 0);
