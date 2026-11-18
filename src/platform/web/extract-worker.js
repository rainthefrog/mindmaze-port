/* Runs the built-in extractor (mmextract.wasm) on the player's disc image or files, off the page's thread.
 * (Names here start with "mm": mmextract.js shares this global scope and has its own run(), walk etc.)
 * In:  { files: [File, ...] }  - one disc image (.iso) or the five files (ENCARTA.EXE, ENCARTAR.DLL, MINDMAZE.DB,
 *                                MINDMAZE.IDX, MINDMAZE.EIT) from a disc or a copied folder
 *      { url }                  - a link to a disc image, read in pieces (HTTP Range)
 *      { files | url, add: true } - only the disc's questions (an image, or MINDMAZE.DB + .IDX): editions/<id>/
 * Out: { progress, step } while it works, then { done: true, files: [[path, ArrayBuffer], ...] (add: id, name, count) }
 *      or { error } */
"use strict";
var Module = {
  onRuntimeInitialized: function () { self.ready = true; if (self.pending) mmRunExtract(self.pending); }
};
importScripts("mmextract.js");

function mmWalk(dir, base, out) {
  FS.readdir(dir).forEach(function (n) {
    if (n === "." || n === "..") return;
    var p = dir + "/" + n, rel = base ? base + "/" + n : n;
    if (FS.isDir(FS.stat(p).mode)) mmWalk(p, rel, out);
    else out.push(rel);
  });
}

function mmRunExtract(job) {
  try {
    var source;
    FS.mkdir("/src");
    if (job.url) {
      /* a link to a disc image (CHANGE 2026-11-18): a lazy file, read in 1 MB pieces with HTTP Range
       * requests (synchronous XHR, allowed in a worker), so only the parts the extractor reads are downloaded */
      FS.createLazyFile("/src", "disc.iso", job.url, true, false);
      source = "/src/disc.iso";
    } else {
      FS.mount(WORKERFS, { files: job.files }, "/src");
      var iso = job.files.filter(function (f) { return /\.iso$/i.test(f.name); })[0];
      source = iso ? "/src/" + iso.name : "/src";
    }
    /* add: only that disc's questions for Settings > Add questions from a disc (CHANGE 2026-11-18) */
    var ok = Module.ccall(job.add ? "web_add_disc" : "web_extract", "number", ["string"], [source]);
    if (!ok) { postMessage({ error: Module.UTF8ToString(Module._web_extract_error()) }); return; }
    var info = job.add ? Module.UTF8ToString(Module._web_add_info()).split("\t") : null;
    var list = [], out = [], transfer = [];
    mmWalk("/out", "", list);
    list.forEach(function (rel) {
      var b = FS.readFile("/out/" + rel);
      out.push([rel, b.buffer]);
      transfer.push(b.buffer);
    });
    var msg = { done: true, files: out };
    if (info) { msg.id = info[0]; msg.name = info[1]; msg.count = +info[2]; }
    postMessage(msg, transfer);
  } catch (e) {
    postMessage({ error: String(e && e.message || e) });
  }
}

onmessage = function (e) {
  if (self.ready) mmRunExtract(e.data); else self.pending = e.data;
};
