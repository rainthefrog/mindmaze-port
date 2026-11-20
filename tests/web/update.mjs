// The Android app's update offer (CHANGE 2026-11-20; shell.js MM.updateCheck + #update, UpdaterPlugin.java): build/web
// is served by the test itself, GitHub's API answered with a fixture, and Capacitor's App / Updater plugins replaced by
// stand-ins (the real download + Android install screen can only be checked on a phone).
//   node tests/web/update.mjs <build/web folder>      (Playwright Docker image, see tests/web/run.sh)
import { chromium } from "playwright";
import { readFileSync } from "fs";
import { extname, join } from "path";

const [web] = process.argv.slice(2);
const TYPES = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm", ".json": "application/json" };
let fails = 0;
const check = (ok, what) => { console.log((ok ? "ok   " : "FAIL ") + what); if (!ok) fails++; };

const release = (tag) => JSON.stringify({
  html_url: "https://github.com/rainthefrog/mindmaze-port/releases/tag/" + tag, tag_name: tag,
  assets: [
    { name: "MindMaze.apk", browser_download_url: "https://github.com/rainthefrog/mindmaze-port/releases/download/" + tag + "/MindMaze.apk" },
    { name: "MindMaze.apk.sha256", browser_download_url: "https://github.com/rainthefrog/mindmaze-port/releases/download/" + tag + "/MindMaze.apk.sha256" },
  ],
});

async function open(browser, { tag, version, capacitor = true, installResult = "ok", stored = null }) {
  const ctx = await browser.newContext({ viewport: { width: 844, height: 390 } });
  const page = await ctx.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  await page.route("http://app.test/**", (r) => {
    const p = new URL(r.request().url()).pathname.replace(/^\/$/, "/index.html");
    try { r.fulfill({ body: readFileSync(join(web, p)), contentType: TYPES[extname(p)] || "application/octet-stream" }); }
    catch { r.fulfill({ status: 404, body: "" }); }
  });
  let apiCalls = 0;
  await page.route("https://api.github.com/**", (r) => { apiCalls++; r.fulfill({ body: release(tag), contentType: "application/json" }); });
  await page.addInitScript(([capacitor, version, installResult, stored]) => {
    if (stored !== null) localStorage.setItem("update_check", stored);
    if (!capacitor) return;
    const listeners = [];
    window.installCalls = [];
    window.Capacitor = { Plugins: {
      App: { getInfo: () => Promise.resolve({ version }), addListener() {}, getLaunchUrl: () => Promise.resolve({}) },
      Updater: {
        addListener: (ev, fn) => { listeners.push(fn); return Promise.resolve({ remove() {} }); },
        install: (o) => {
          window.installCalls.push(o);
          listeners.forEach((fn) => fn({ pct: 42 }));
          return installResult === "ok" ? Promise.resolve() : Promise.reject(new Error(installResult));
        },
      },
    } };
  }, [capacitor, version, installResult, stored]);
  await page.goto("http://app.test/index.html");
  await page.waitForFunction(() => typeof MM !== "undefined" && typeof MM.updateCheck === "function", null, { timeout: 20000 });
  await page.evaluate(() => MM.updateCheck());
  await page.waitForTimeout(800);
  return { ctx, page, errors, api: () => apiCalls };
}

const browser = await chromium.launch();

let t = await open(browser, { tag: "v1.0.4", version: "1.0.3" });
check(await t.page.isVisible("#update"), "a newer release is offered");
check((await t.page.textContent("#update-text")).includes("MindMaze 1.0.4 is available (you have 1.0.3)"), "the offer names both versions");
await t.page.click("#update-go");
await t.page.waitForTimeout(300);
const calls = await t.page.evaluate(() => window.installCalls);
check(calls.length === 1 && calls[0].url.endsWith("/v1.0.4/MindMaze.apk") && calls[0].sumUrl.endsWith("/MindMaze.apk.sha256"),
      "Update now passes the APK and its checksum to the plugin");
check((await t.page.textContent("#update-step")).includes("install screen"), "then says Android's install screen is open");
check(t.errors.length === 0, "no page errors " + t.errors.join("; "));
await t.ctx.close();

t = await open(browser, { tag: "v1.0.4", version: "1.0.3", installResult: "permission" });
await t.page.click("#update-go");
await t.page.waitForTimeout(300);
check((await t.page.textContent("#update-step")).includes("Allow MindMaze to install apps"), "missing install permission is explained");
await t.page.click("#update-never");
check(!(await t.page.isVisible("#update")) && (await t.page.evaluate(() => localStorage.getItem("update_check"))) === "0",
      "Don't ask again closes and remembers");
await t.ctx.close();

t = await open(browser, { tag: "v1.0.3", version: "1.0.3" });
check(!(await t.page.isVisible("#update")), "the same version is not offered");
await t.ctx.close();

t = await open(browser, { tag: "v1.0.4", version: "1.0.3", stored: "0" });
check(!(await t.page.isVisible("#update")) && t.api() === 0, "after Don't ask again: no check at all");
await t.ctx.close();

t = await open(browser, { tag: "v1.0.4", version: "1.0.3", capacitor: false });
check(!(await t.page.isVisible("#update")) && t.api() === 0, "in a browser (no app): no check");
await t.ctx.close();

await browser.close();
console.log(fails ? `update offer: ${fails} failed` : "update offer: all checks ok");
process.exit(fails ? 1 : 0);
