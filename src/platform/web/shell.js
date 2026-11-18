/* MindMaze web shell: loads the files into the WebAssembly file system, shows the canvas letterboxed, passes input,
 * plays sound effects (WebAudio), keeps settings (localStorage) and the player's files (IndexedDB).
 *
 * Data source (development): the page fetches <data>/files.txt (one relative path per line) and every file in it;
 * <data> is "data" next to the page, or ?data=<url>. The Android app will import the data folder into app storage
 * instead (step 3). Nothing of the game's data is ever part of this build. */
"use strict";

var MM = {
  canvas: document.getElementById("screen"),
  ctx: null,
  msg: document.getElementById("msg"),
  audio: null, buffers: {}, vol: { music: 100, sound: 100 }, track: [null, null], persistTimer: 0,
  ext: function () { return Module._web_view_ext ? Module._web_view_ext() : 0; },

  present: function (ptr, w, h) {
    var c = MM.canvas;
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; MM.fit(); }
    var px = new Uint8ClampedArray(Module.HEAPU8.buffer, ptr, w * h * 4);
    MM.ctx.putImageData(new ImageData(new Uint8ClampedArray(px), w, h), 0, 0);
  },
  fit: function () {                       /* letterbox: biggest size that fits, centred, any (non-whole) factor */
    var c = MM.canvas, W = window.innerWidth, H = window.innerHeight;
    if (!c.width) return;
    var k = Math.min(W / c.width, H / c.height);
    c.style.width = Math.round(c.width * k) + "px";
    c.style.height = Math.round(c.height * k) + "px";
    c.style.left = Math.round((W - c.width * k) / 2) + "px";
    c.style.top = Math.round((H - c.height * k) / 2) + "px";
  },
  devSize: function () {
    var d = window.devicePixelRatio || 1;
    return [Math.round(window.innerWidth * d), Math.round(window.innerHeight * d)];
  },

  /* ---- sound ---- */
  ensureAudio: function () {
    if (!MM.audio) {
      var A = window.AudioContext || window.webkitAudioContext;
      if (A) {
        MM.audio = new A();
        /* limiter on the master output: music and sound effects together never clip the speakers */
        var L = MM.audio.createDynamicsCompressor();
        L.threshold.value = -6; L.knee.value = 4; L.ratio.value = 20; L.attack.value = 0.002; L.release.value = 0.2;
        L.connect(MM.audio.destination);
        MM.out = L;
      }
    }
    if (MM.audio && MM.audio.state === "suspended") MM.audio.resume();
    if (MM.audio && !MM.musicNode && Module._web_music_render) {   /* music: rendered by web_music.c */
      var a = MM.audio, n = 4096;
      Module._web_music_rate(a.sampleRate);
      MM.musicBuf = Module._malloc(n * 2 * 4);
      MM.musicNode = a.createScriptProcessor(n, 0, 2);
      MM.musicNode.onaudioprocess = function (e) {
        var L = e.outputBuffer.getChannelData(0), R = e.outputBuffer.getChannelData(1), k = L.length;
        Module._web_music_render(MM.musicBuf, k);
        var f = new Float32Array(Module.HEAPU8.buffer, MM.musicBuf, k * 2);
        for (var i = 0; i < k; i++) { L[i] = f[2 * i]; R[i] = f[2 * i + 1]; }
      };
      MM.musicNode.connect(MM.out);
    }
    return MM.audio;
  },
  sound: function (name) {
    var a = MM.ensureAudio();
    if (!a) return;
    var play = function (buf) {
      var src = a.createBufferSource(), g = a.createGain();
      g.gain.value = MM.vol.sound / 100;
      src.buffer = buf;
      src.connect(g).connect(MM.out);
      src.start();
    };
    if (MM.buffers[name]) { play(MM.buffers[name]); return; }
    try {
      var bytes = FS.readFile("/data/" + name);
      a.decodeAudioData(bytes.slice().buffer, function (buf) { MM.buffers[name] = buf; play(buf); }, function () {});
    } catch (e) { /* missing sound file */ }
  },
  beep: function () {
    var a = MM.ensureAudio();
    if (!a) return;
    var o = a.createOscillator(), g = a.createGain();
    o.frequency.value = 880;
    g.gain.value = 0.08 * MM.vol.sound / 100;
    o.connect(g).connect(MM.out);
    o.start();
    o.stop(a.currentTime + 0.08);
  },
  music: function (ch, name) { MM.track[ch] = name; MM.paused[ch] = 0; },   /* for tests; playback: web_music.c */
  paused: [0, 0],
  musicPause: function (ch, pause) { MM.paused[ch] = pause; },
  volume: function (m, s) { MM.vol.music = m; MM.vol.sound = s; },

  /* ---- misc ---- */
  cursor: function (hand) { MM.canvas.style.cursor = hand ? "pointer" : "default"; },
  getSetting: function (k) { try { return localStorage.getItem("mindmaze:" + k); } catch (e) { return null; } },
  setSetting: function (k, v) {
    try { if (v === null) localStorage.removeItem("mindmaze:" + k); else localStorage.setItem("mindmaze:" + k, v); } catch (e) {}
  },
  persistUser: function () {
    clearTimeout(MM.persistTimer);
    MM.persistTimer = setTimeout(function () { FS.syncfs(false, function () {}); }, 300);
  },
  httpGet: function (url, tag) {
    var done = function (text) {
      if (text === null) { Module._web_http_done(tag, 0, 0); return; }
      var n = lengthBytesUTF8(text) + 1, p = Module._malloc(n);
      stringToUTF8(text, p, n);
      Module._web_http_done(tag, p, n - 1);
      Module._free(p);
    };
    fetch(url).then(function (r) { return r.ok ? r.text() : null; }).then(done, function () { done(null); });
  },
  error: function (m) { MM.msg.style.display = "flex"; MM.msg.textContent = m; },

  /* ---- input ---- */
  point: function (e) {
    var r = MM.canvas.getBoundingClientRect();
    return [Math.floor((e.clientX - r.left) * MM.canvas.width / r.width),
            Math.floor((e.clientY - r.top) * MM.canvas.height / r.height)];
  },
  mods: function (e) { return (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0); },
  KEYS: { Enter: 13, Escape: 27, Backspace: 8, Tab: 9, Delete: 127, F2: 0x71, F3: 0x72 },

  /* test hook: a tap at logical coordinates of the 636x437 view */
  tap: function (x, y) {
    var s = Module._web_scale(), e = MM.ext(), X = (x + e) * s + 1, Y = y * s + 1;
    Module._web_mouse(X, Y, 0, 2, 0);
    Module._web_mouse(X, Y, 1, 0, 0);
    Module._web_mouse(X, Y, 1, 1, 0);
  },
  hover: function (x, y) {
    var s = Module._web_scale(), e = MM.ext();
    Module._web_mouse((x + e) * s + 1, y * s + 1, 0, 2, 0);
  }
};
MM.ctx = MM.canvas.getContext("2d");

/* ---- load files before main() runs ---- */
function mkdirs(path) {
  var parts = path.split("/"), cur = "";
  for (var i = 1; i < parts.length - 1; i++) {
    cur += "/" + parts[i];
    try { FS.mkdir(cur); } catch (e) {}
  }
}
function fetchBytes(url) {
  return fetch(url).then(function (r) {
    if (!r.ok) throw new Error(url + ": " + r.status);
    return r.arrayBuffer();
  }).then(function (b) { return new Uint8Array(b); });
}
var FONTS = ["LiberationSans-Regular.ttf", "LiberationSans-Bold.ttf", "LiberationSans-Italic.ttf",
             "LiberationSerif-Regular.ttf", "LiberationSerif-Bold.ttf", "texgyrepagella-italic.otf"];
/* Data: development servers provide <data>/files.txt (?data=<url>, default "data"); the app instead keeps the data
 * folder in IndexedDB (/data), filled once by importing the player's Encarta disc (import screen below). */
function loadAll() {
  var params = new URLSearchParams(location.search), dataUrl = params.get("data") || "data";
  FS.mkdir("/data"); FS.mkdir("/fonts"); FS.mkdir("/user"); FS.mkdir("/soundfonts");
  FS.mount(IDBFS, {}, "/user");
  FS.mount(IDBFS, {}, "/data");
  var soundfonts = fetch("soundfonts/list.txt").then(function (r) { return r.ok ? r.text() : ""; }).then(function (list) {
    FS.writeFile("/soundfonts/list.txt", list);
    return Promise.all(list.split(/\r?\n/).filter(Boolean).map(function (line) {
      var f = line.split("\t")[0];
      return fetchBytes("soundfonts/" + f).then(function (b) { FS.writeFile("/soundfonts/" + f, b); }, function () {});
    }));
  }, function () {});
  var stored = new Promise(function (ok) { FS.syncfs(true, function () { ok(); }); });     /* /user and /data */
  var fonts = Promise.all(FONTS.map(function (f) {
    return fetchBytes("fonts/" + f).then(function (b) { FS.writeFile("/fonts/" + f, b); }, function () {});
  }));
  var data = stored.then(function () {
    return fetch(dataUrl + "/files.txt").then(function (r) { return r.ok ? r.text() : null; }, function () { return null; });
  }).then(function (list) {
    if (list === null) return;                               /* the app: data from the import (IndexedDB) */
    var files = list.split(/\r?\n/).filter(function (l) { return l && !/^art\/work\//.test(l); });
    var i = 0, n = files.length;
    var next = function () {
      if (i >= n) return Promise.resolve();
      var f = files[i++];
      MM.msg.textContent = "Loading MindMaze... " + Math.round(100 * i / n) + "%";
      return fetchBytes(dataUrl + "/" + f).then(function (b) { mkdirs("/data/" + f); FS.writeFile("/data/" + f, b); }).then(next);
    };
    MM.devData = true;
    return Promise.all([next(), next(), next(), next()]);
  });
  return Promise.all([fonts, data, soundfonts]);
}

function hasData() { try { FS.stat("/data/tables.json"); return true; } catch (e) { return false; } }

function startGame() {
  var d = MM.devSize();
  MM.msg.style.display = "flex";
  MM.msg.textContent = "Starting MindMaze...";
  if (!Module._web_start(d[0], d[1])) return;
  MM.msg.style.display = "none";
  document.getElementById("import").style.display = "none";
  MM.canvas.focus();
  window.mmReady = true;
  MM.takePendingLink();
}

/* ---- import: the player's Encarta disc image (2006, or 95 to 98) or the 2006 files -> /data (IndexedDB), with the built-in extractor ---- */
function showImport(errorText) {
  MM.msg.style.display = "none";
  var box = document.getElementById("import");
  box.style.display = "flex";
  document.getElementById("import-error").textContent = errorText || "";
  document.getElementById("import-progress").style.display = "none";
  document.getElementById("import-buttons").style.display = "block";
  if (!errorText) MM.takePendingLink();
}

/* job: { files: [File...] } (a disc image or the five files on this device) or { url } (a link to a disc image, read
 * in pieces: only what the extractor needs is fetched). art: a link to the HD pictures (.zip) to install after. */
function runImport(files, url, art) {
  if (!url && (!files || !files.length)) return;
  document.getElementById("import-buttons").style.display = "none";
  document.getElementById("import-error").textContent = "";
  var prog = document.getElementById("import-progress"), bar = document.getElementById("import-bar"),
      step = document.getElementById("import-step");
  prog.style.display = "block";
  step.textContent = url ? "Reading the disc image..." : "Starting...";
  var w = new Worker("extract-worker.js");
  w.onmessage = function (e) {
    var m = e.data;
    if (m.progress !== undefined) { bar.style.width = m.progress + "%"; step.textContent = m.step; return; }
    w.terminate();
    if (m.error) { showImport(m.error); return; }
    step.textContent = "Saving...";
    m.files.forEach(function (f) { mkdirs("/data/" + f[0]); FS.writeFile("/data/" + f[0], new Uint8Array(f[1])); });
    var finish = function () {
      FS.syncfs(false, function (err) {
        if (err) { showImport("The data could not be saved in the app's storage (" + err + ")."); return; }
        startGame();
      });
    };
    if (!art) { finish(); return; }
    installArt(art, function (pct, what) { bar.style.width = pct + "%"; step.textContent = what; }, function (err) {
      if (err) step.textContent = "The HD pictures were not installed (" + err + "); starting without them.";
      finish();
    });
  };
  w.onerror = function (e) { showImport("The import stopped: " + (e.message || "error")); };
  w.postMessage(url ? { url: url } : { files: Array.prototype.slice.call(files) });
}

/* ---- links (CHANGE 2026-11-18): a link to a disc image is read in pieces (the server must allow HTTP Range and
 * CORS); a link to a .zip is downloaded: a MindMaze data file at the first start, else the HD pictures. ---- */
function isZipLink(u) { return /\.zip($|[?#])/i.test(u); }
function fetchBlob(url) {
  return fetch(url).then(function (r) {
    if (!r.ok) throw new Error("HTTP " + r.status);
    return r.blob();
  });
}
function cleanLink(u) {
  u = (u || "").trim();
  return u && !/^[a-z]+:\/\//i.test(u) ? "http://" + u : u;
}

/* the import screen: a disc image link (art: HD pictures link to install after) or a data file link */
function importLink(url, art) {
  url = cleanLink(url);
  if (!url) { document.getElementById("import-error").textContent = "Paste a link to a disc image (.iso) or a MindMaze data file (.zip)."; return; }
  if (!isZipLink(url)) { runImport(null, url, cleanLink(art)); return; }
  document.getElementById("import-buttons").style.display = "none";
  document.getElementById("import-progress").style.display = "block";
  document.getElementById("import-step").textContent = "Downloading...";
  fetchBlob(url).then(runZipImport, function (e) { showImport("The link could not be read (" + (e.message || e) + ")."); });
}

/* the HD pictures MindMaze-HD-art.zip (hd/ + wide/ of a data folder; a link or a File) -> /data */
function installArt(src, progress, done) {
  progress(0, typeof src === "string" ? "Downloading the HD pictures..." : "Reading the HD pictures...");
  (typeof src === "string" ? fetchBlob(src) : Promise.resolve(src)).then(function (blob) {
    return readZip(blob, function (pct, name) { progress(pct, "Installing " + name); }, "art");
  }).then(function (files) {
    files.forEach(function (f) { mkdirs("/data/" + f[0]); FS.writeFile("/data/" + f[0], f[1]); });
    FS.syncfs(false, function (err) { done(err ? String(err) : null, files.length); });
  }).catch(function (e) { done(e.message || String(e)); });
}

document.getElementById("link-go").addEventListener("click", function () { importLink(document.getElementById("link-url").value); });

/* ---- in the game (CHANGE 2026-11-18): Settings > Add questions from a disc, and Settings > Pictures > "HD pictures
 * from a file or link...", share the overlay #remote: a file on this device, or a link. ---- */
function remoteShow(mode, url) {
  var art = mode === "art";
  document.getElementById("remote-title").textContent = art ? "HD pictures" : "Add questions from a disc";
  document.getElementById("remote-note").textContent = art
    ? "Choose MindMaze-HD-art.zip on this device, or paste a link to it."
    : "Choose a disc image (or MINDMAZE.DB and MINDMAZE.IDX) on this device, or paste a link to a disc image (.iso).";
  document.getElementById("remote-file").textContent = art ? "Choose the zip on this device" : "Choose a file on this device";
  document.getElementById("remote-go").textContent = art ? "Install" : "Add";
  document.getElementById("remote-url").value = url || "";
  var err = document.getElementById("remote-error");
  err.textContent = "";
  err.style.color = "#ff9a8a";
  var box = document.getElementById("remote");
  box.dataset.mode = mode;
  box.style.display = "flex";
}
function remoteHide() { document.getElementById("remote").style.display = "none"; MM.canvas.focus(); }
MM.addDisc = function () { remoteShow("disc"); };
MM.artImport = function () { remoteShow("art"); };
MM.discDone = function (ok, id, name, count, msg) {
  var a = [id, name, msg].map(function (s) { return stringToNewUTF8(s || ""); });
  Module._web_disc_added(ok ? 1 : 0, a[0], a[1], count || 0, a[2]);
  a.forEach(function (p) { Module._free(p); });
};
function runAddDisc(files, url) {
  if (!url && (!files || !files.length)) return;
  remoteHide();
  Module._web_disc_reading();
  var w = new Worker("extract-worker.js");
  w.onmessage = function (e) {
    var m = e.data;
    if (m.progress !== undefined) return;
    w.terminate();
    if (m.error) { MM.discDone(0, "", "", 0, m.error); return; }
    m.files.forEach(function (f) { mkdirs("/data/" + f[0]); FS.writeFile("/data/" + f[0], new Uint8Array(f[1])); });
    FS.syncfs(false, function (err) {
      if (err) MM.discDone(0, "", "", 0, "The questions could not be saved in the app's storage (" + err + ").");
      else MM.discDone(1, m.id, m.name, m.count, "");
    });
  };
  w.onerror = function (e) { w.terminate(); MM.discDone(0, "", "", 0, "Reading the disc stopped: " + (e.message || "error")); };
  w.postMessage(url ? { url: url, add: true } : { files: Array.prototype.slice.call(files), add: true });
}
function remoteArt(src) {
  var err = document.getElementById("remote-error");
  installArt(src, function (pct, what) { err.style.color = "#d9d6c5"; err.textContent = what + " (" + pct + "%)"; }, function (e, n) {
    err.style.color = e ? "#ff9a8a" : "#d9d6c5";
    err.textContent = e ? "The pictures were not installed: " + e
                        : n + " pictures installed. Switch them on in Settings > Pictures (or F2 / F3).";
    if (!e) Module._web_art_installed();
  });
}
document.getElementById("add-disc").addEventListener("change", function (e) { runAddDisc(e.target.files); });
document.getElementById("add-art").addEventListener("change", function (e) { if (e.target.files[0]) remoteArt(e.target.files[0]); });
document.getElementById("remote-file").addEventListener("click", function () {
  var input = document.getElementById(document.getElementById("remote").dataset.mode === "art" ? "add-art" : "add-disc");
  input.value = "";
  input.click();
});
document.getElementById("remote-go").addEventListener("click", function () {
  var url = cleanLink(document.getElementById("remote-url").value);
  if (!url) { document.getElementById("remote-error").textContent = "Paste a link first."; return; }
  if (isZipLink(url)) remoteArt(url);                       /* a zip: the HD pictures */
  else runAddDisc(null, url);                               /* else a disc image: its questions */
});
document.getElementById("remote-close").addEventListener("click", function () {
  var adding = document.getElementById("remote").dataset.mode !== "art";
  remoteHide();
  if (adding) MM.discDone(0, "", "", 0, "");                /* cancelled: nothing changes */
});

/* ---- opening the app with links (CHANGE 2026-11-18): mindmaze://import?url=<disc image>&art=<HD pictures .zip>
 * (Android intent filter, @capacitor/app; e.g. from a QR code); in a browser index.html?url=...&art=... At the first
 * start the disc is imported (and the pictures installed); in the game the pictures are installed and a disc link
 * is offered in the "Add questions" overlay. ---- */
function linkParam(link, key) {
  var m = new RegExp("[?&]" + key + "=([^&#]+)").exec(link || "");
  return m ? decodeURIComponent(m[1]) : "";
}
MM.openLink = function (link) {
  var url = linkParam(link, "url"), art = linkParam(link, "art");
  if (!url && !art) return;
  if (window.mmReady) {
    if (art) { remoteShow("art", art); remoteArt(art); }
    else remoteShow(isZipLink(url) ? "art" : "disc", url);
  } else if (document.getElementById("import").style.display === "flex") {
    document.getElementById("link-url").value = url;
    if (url) importLink(url, art);
  } else MM.pendingLink = link;                             /* still loading: see showImport / startGame */
};
MM.takePendingLink = function () {
  var l = MM.pendingLink;
  MM.pendingLink = null;
  if (l) setTimeout(function () { MM.openLink(l); }, 0);
};
if (/[?&](url|art)=/.test(location.search)) MM.pendingLink = location.search;
(function () {
  var App = window.Capacitor && window.Capacitor.Plugins && window.Capacitor.Plugins.App;
  if (!App) return;
  App.addListener("appUrlOpen", function (e) { MM.openLink(e.url); });
  App.getLaunchUrl().then(function (r) { if (r && r.url) MM.openLink(r.url); }, function () {});
})();


/* a MindMaze data file (.zip): made by the Windows game (Settings > Export data for the phone app) or any zip of a
 * data folder. Entries stored or deflated; the folder holding tables.json becomes /data; art/work is skipped. */
function readZip(file, progress, kind) {
  var u16 = function (b, o) { return b[o] | b[o + 1] << 8; };
  var u32 = function (b, o) { return (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0; };
  var bytes = function (from, to) {
    return file.slice(from, to).arrayBuffer().then(function (a) { return new Uint8Array(a); });
  };
  var tailLen = Math.min(file.size, 66000);
  return bytes(file.size - tailLen, file.size).then(function (t) {
    var e = -1;
    for (var i = t.length - 22; i >= 0; i--) if (u32(t, i) === 0x06054b50) { e = i; break; }
    if (e < 0) throw new Error("This is not a zip file.");
    var count = u16(t, e + 10), size = u32(t, e + 12), off = u32(t, e + 16);
    return bytes(off, off + size).then(function (cd) {
      var list = [], dec = new TextDecoder();
      for (var p = 0, k = 0; k < count && p + 46 <= cd.length; k++) {
        if (u32(cd, p) !== 0x02014b50) throw new Error("The zip file is damaged (central directory).");
        var nl = u16(cd, p + 28), xl = u16(cd, p + 30), cl = u16(cd, p + 32);
        list.push({ method: u16(cd, p + 10), csize: u32(cd, p + 20), usize: u32(cd, p + 24), local: u32(cd, p + 42),
                    name: dec.decode(cd.subarray(p + 46, p + 46 + nl)).replace(/\\/g, "/") });
        p += 46 + nl + xl + cl;
      }
      /* kind "art": an HD art pack, the folder holding hd/bounds.txt or wide/wide.txt; only hd/ and wide/ are taken */
      var mark = kind === "art" ? /(^|\/)(hd\/bounds\.txt|wide\/wide\.txt)$/i : /(^|\/)tables\.json$/i;
      var root = list.filter(function (f) { return mark.test(f.name); })
                     .map(function (f) { return f.name.slice(0, f.name.length - f.name.match(mark)[0].replace(/^\//, "").length); })
                     .sort(function (a, b) { return a.length - b.length; })[0];
      if (root === undefined)
        throw new Error(kind === "art" ? "This zip holds no HD or widescreen pictures (hd/bounds.txt, wide/wide.txt)."
                                       : "This zip has no MindMaze data folder in it (tables.json is missing).");
      var files = list.filter(function (f) {
        var rel = f.name.slice(root.length);
        return f.name.indexOf(root) === 0 && !/\/$/.test(f.name) && !/^art\/work\//.test(rel) &&
               (kind !== "art" || /^(hd|wide)\//.test(rel));
      });
      var done = 0, out = [];
      var one = function (f) {
        return bytes(f.local, f.local + 30).then(function (h) {
          if (u32(h, 0) !== 0x04034b50) throw new Error("The zip file is damaged (" + f.name + ").");
          var start = f.local + 30 + u16(h, 26) + u16(h, 28);
          return bytes(start, start + f.csize);
        }).then(function (raw) {
          if (f.method === 0) return raw;
          if (f.method !== 8 || typeof DecompressionStream === "undefined")
            throw new Error("Unsupported compression in the zip (" + f.name + ").");
          var stream = new Blob([raw]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
          return new Response(stream).arrayBuffer().then(function (a) { return new Uint8Array(a); });
        }).then(function (data) {
          if (data.length !== f.usize) throw new Error("The zip file is damaged (" + f.name + ").");
          out.push([f.name.slice(root.length), data]);
          progress(Math.round(100 * ++done / files.length), f.name.slice(root.length));
        });
      };
      var chain = Promise.resolve();
      files.forEach(function (f) { chain = chain.then(function () { return one(f); }); });
      return chain.then(function () { return out; });
    });
  });
}

function runZipImport(file) {
  if (!file) return;
  document.getElementById("import-buttons").style.display = "none";
  document.getElementById("import-error").textContent = "";
  var bar = document.getElementById("import-bar"), step = document.getElementById("import-step");
  document.getElementById("import-progress").style.display = "block";
  readZip(file, function (pct, name) { bar.style.width = pct + "%"; step.textContent = "Reading " + name; }).then(function (files) {
    step.textContent = "Saving...";
    files.forEach(function (f) { mkdirs("/data/" + f[0]); FS.writeFile("/data/" + f[0], f[1]); });
    FS.syncfs(false, function (err) {
      if (err) { showImport("The data could not be saved in the app's storage (" + err + ")."); return; }
      startGame();
    });
  }, function (e) { showImport(e.message || String(e)); });
}

document.getElementById("import-zip").addEventListener("change", function (e) { runZipImport(e.target.files[0]); });
document.getElementById("import-iso").addEventListener("change", function (e) { runImport(e.target.files); });
document.getElementById("import-files").addEventListener("change", function (e) { runImport(e.target.files); });

var Module = {
  preRun: [function () {
    addRunDependency("mindmaze-files");
    loadAll().then(function () { removeRunDependency("mindmaze-files"); },
                   function (e) { MM.error("MindMaze could not load its files:\n" + e.message); });
  }],
  onRuntimeInitialized: function () {
    if (hasData()) startGame();
    else showImport("");
  },
  print: function (t) { console.log(t); },
  printErr: function (t) { console.warn(t); }
};

/* ---- events ---- */
window.addEventListener("resize", function () {
  if (!Module._web_resize) return;
  var d = MM.devSize();
  Module._web_resize(d[0], d[1]);
  MM.fit();
});
var cv = MM.canvas;
cv.addEventListener("pointermove", function (e) { var p = MM.point(e); Module._web_mouse(p[0], p[1], 0, 2, MM.mods(e)); });
cv.addEventListener("pointerdown", function (e) {
  e.preventDefault();
  MM.ensureAudio();
  cv.focus();
  var p = MM.point(e), b = e.button === 2 ? 2 : 1;
  Module._web_mouse(p[0], p[1], 0, 2, MM.mods(e));         /* touch has no hover: move there first */
  Module._web_mouse(p[0], p[1], b, 0, MM.mods(e));
});
cv.addEventListener("pointerup", function (e) { var p = MM.point(e); Module._web_mouse(p[0], p[1], e.button === 2 ? 2 : 1, 1, MM.mods(e)); });
cv.addEventListener("dblclick", function (e) { var p = MM.point(e); Module._web_mouse(p[0], p[1], 1, 3, MM.mods(e)); });
cv.addEventListener("wheel", function (e) { e.preventDefault(); var p = MM.point(e); Module._web_mouse(p[0], p[1], e.deltaY < 0 ? 1 : -1, 4, 0); }, { passive: false });
cv.addEventListener("contextmenu", function (e) { e.preventDefault(); });
window.addEventListener("keydown", function (e) {
  if (!window.mmReady) return;
  if (e.target && e.target.tagName === "INPUT") return;    /* typing an address in the overlay */
  var k = MM.KEYS[e.key];
  if (!k && e.key.length === 1) {
    var c = e.key.toUpperCase().charCodeAt(0);
    if ((c >= 65 && c <= 90) || (c >= 48 && c <= 57)) k = c;
  }
  if (k) Module._web_key(k, MM.mods(e));
  if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) Module._web_char(e.key.codePointAt(0));
  if (k === 8 || k === 9 || k === 0x71 || k === 0x72 || e.key === " ") e.preventDefault();
});
