"""Guard for CI and for use before a commit: no Encarta content may be tracked by git.

  python3 tools/check_no_content.py

Fails if a tracked file looks like extracted game data (pictures, sounds, music, question files, tables,
program files, disc images) or like art generated from it. The only pictures allowed are listed below.
"""
import subprocess
import sys

ALLOWED_PICTURES = {"docs/screenshot.png", "docs/icon.png", "src/platform/win32/mindmaze.ico"}
# the Android app's launcher icons (tools/art/make_android_icons.py, our own drawing) and Capacitor's splash images
ALLOWED_PREFIXES = ("app/android/app/src/main/res/",)
PICTURE_EXT = {"png", "jpg", "jpeg", "gif", "webp", "bmp", "ico", "dib"}
DATA_EXT = {"dib", "wav", "mid", "ogg", "mp3", "db", "idx", "eit", "iso", "exe", "dll", "lst", "apk"}
DATA_NAMES = {"questions.json", "tables.json", "mm_tables_generated.h", "triviaapi.json"}

files = subprocess.run(["git", "ls-files"], stdout=subprocess.PIPE, check=True).stdout.decode().splitlines()
bad = []
for f in files:
    name = f.rsplit("/", 1)[-1].lower()
    ext = name.rsplit(".", 1)[-1] if "." in name else ""
    if f.startswith(("data/", "assets/", "original/", "engine/")):
        bad.append((f, "extracted data folder"))
    elif ext in DATA_EXT or name in DATA_NAMES:
        bad.append((f, "game data or program file"))
    elif ext in PICTURE_EXT and f not in ALLOWED_PICTURES and not f.startswith(ALLOWED_PREFIXES):
        bad.append((f, "picture (only the README screenshot and the project's own icon are allowed)"))
for f, why in bad:
    print("NOT ALLOWED: %s - %s" % (f, why))
print("content check: %d tracked files, %s" % (len(files), "%d problems" % len(bad) if bad else "ok"))
sys.exit(1 if bad else 0)
