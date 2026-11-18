# mindmaze-port - build and test. Runs on Linux (or in the Docker image: see docker/Dockerfile).
#   make test                  core + extractor tests on synthetic data (no Encarta files needed)
#   make win32                 build/win32/MindMaze.exe (mingw-w64 cross build; the whole Windows download)
#   make test-data DATA=data   the core tests on a folder extracted from an Encarta 2006 disc
#   make test-extract ISO=...  the built-in C extractor against tools/extract/extract.py on a real disc
#   make web                   build/web/ (WebAssembly engine of the Android app; needs emcc: emscripten/emsdk image)
#   make android               build/android/MindMaze.apk from build/web (mindmaze-port-android image; run make web first)
CORE = src/core/mm_util.c src/core/mm_questions.c src/core/mm_rich.c src/core/mm_sources.c src/core/mm_settings.c \
       src/core/mm_tables.c src/art/mm_art.c src/art/mm_art_img.c
EXTRACT = src/extract/mx_extract.c src/extract/mx_lzx.c src/extract/mx_util.c src/extract/mx_old.c src/extract/mx_edition.c \
          src/extract/mx_oldext.c src/extract/mx_tables.c src/extract/mx_tables_pe.c src/extract/mx_tables_ne.c \
          src/extract/mx_tables_mac.c build/embedded.c
WIN32 = src/platform/win32/win32_main.c src/platform/win32/win32_setup.c src/platform/win32/win32_export.c
HEADERS = src/core/mm.h src/core/mm_tables.h src/extract/mx.h src/extract/mx_tables.h src/extract/mx_oldext.h \
          src/art/mm_art.h src/art/mm_art_text.h
# the project page the game links to (Settings > Pictures); the release workflow sets it from the repository
REPO_URL ?= https://github.com/OWNER/mindmaze-port
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation -ffp-contract=off -DMM_REPO_URL='"$(REPO_URL)"'
MINGW = i686-w64-mingw32
DATA ?= data

all: test win32

build/fixtures/tables.json: tests/make_fixtures.py tools/extract/mmdb.py
	mkdir -p build/fixtures
	python3 -I tests/make_fixtures.py build/fixtures

build/test_core: tests/test_core.c $(CORE) src/core/mm_game.c $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -Isrc/core -Isrc/art tests/test_core.c $(CORE) -lm -o $@

build/test_mx: tests/test_mx.c $(EXTRACT) $(HEADERS)
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -Isrc/extract tests/test_mx.c $(EXTRACT) -o $@

test: build/test_core build/test_mx build/fixtures/tables.json
	build/test_core build/fixtures
	build/test_mx
	python3 -I tests/test_extract.py

test-data: build/test_core
	build/test_core $(DATA) --original

build/embedded.c: tools/embed.py $(wildcard packs/*.json packs/*.md codex/*.txt codex/wide/*.txt docs/HD-ART.md)
	mkdir -p build
	python3 -I tools/embed.py $@

build/mmextract: tools/extract/mmextract.c $(EXTRACT) $(HEADERS)
	$(CC) $(CFLAGS) -Isrc/extract tools/extract/mmextract.c $(EXTRACT) -o $@

build/mmart: tools/art/mmart.c src/art/mm_art.c src/art/mm_art_img.c $(HEADERS)
	$(CC) $(CFLAGS) -Isrc/core -Isrc/art tools/art/mmart.c src/art/mm_art.c src/art/mm_art_img.c -lm -o $@

# the art text table src/art/mm_art_text.h is generated from the reference Python tools (needs Pillow; run by
# developers after changing tools/art/mm_sheets.py or mm_wide_sheets.py): make art-text
art-text:
	python3 tools/art/gen_art_text.py src/art/mm_art_text.h

test-extract: build/mmextract
	rm -rf build/x-c build/x-py
	build/mmextract "$(ISO)" build/x-c > /dev/null
	python3 -I tools/extract/extract.py "$(ISO)" -o build/x-py --no-iso-hash > /dev/null
	python3 -I tests/compare_data.py build/x-py build/x-c

win32: build/win32/MindMaze.exe
build/win32/MindMaze.exe: $(CORE) $(EXTRACT) $(WIN32) $(HEADERS) src/core/mm_game.c src/platform/win32/mindmaze.rc \
                          src/platform/win32/mindmaze.ico
	mkdir -p build/win32
	cd src/platform/win32 && $(MINGW)-windres mindmaze.rc -O coff -o ../../../build/win32/mindmaze_res.o
	$(MINGW)-gcc $(CFLAGS) -msse2 -mfpmath=sse -municode -mwindows -Isrc/core -Isrc/extract -Isrc/art $(CORE) src/core/mm_game.c $(EXTRACT) $(WIN32) \
	  build/win32/mindmaze_res.o -o $@ -static -lwinmm -lgdi32 -lshell32 -lole32 -lwindowscodecs -luuid -lwininet -lcomdlg32
	$(MINGW)-strip $@

WEB = src/platform/web/web_main.c src/platform/web/web_text.c src/platform/web/web_music.c
WEB_EXPORTS = _main,_malloc,_free,_web_start,_web_resize,_web_mouse,_web_key,_web_char,_web_cmd,_web_music_ended,_web_http_done,_web_scale,_web_view_ext,_web_music_render,_web_music_rate,_web_music_state,_web_disc_reading,_web_disc_added,_web_art_installed
web: build/web/mindmaze.js
build/web/mindmaze.js: $(CORE) src/core/mm_game.c $(WEB) $(HEADERS) src/platform/web/web_text.h \
                       src/platform/web/index.html src/platform/web/shell.js src/platform/web/extract-worker.js \
                       tools/extract/mmextract_web.c $(EXTRACT) build/soundfonts/GeneralUser-GS.sf2
	mkdir -p build/web/fonts build/web/soundfonts
	emcc -std=c11 -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers -ffp-contract=off \
	  -DMM_REPO_URL='"$(REPO_URL)"' -Isrc/core -Isrc/art -Isrc/platform/web $(CORE) src/core/mm_game.c $(WEB) \
	  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sFORCE_FILESYSTEM=1 -lidbfs.js -sENVIRONMENT=web \
	  -sEXPORTED_FUNCTIONS=$(WEB_EXPORTS) \
	  -sEXPORTED_RUNTIME_METHODS=FS,HEAPU8,UTF8ToString,stringToUTF8,stringToNewUTF8,lengthBytesUTF8,addRunDependency,removeRunDependency \
	  -o $@
	cp src/platform/web/index.html src/platform/web/shell.js src/platform/web/extract-worker.js build/web/
	emcc -std=gnu11 -O2 -ffp-contract=off -Isrc/extract tools/extract/mmextract_web.c $(EXTRACT) \
	  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sFORCE_FILESYSTEM=1 -lworkerfs.js -sENVIRONMENT=worker \
	  -sEXPORTED_FUNCTIONS=_main,_web_extract,_web_extract_error,_web_add_disc,_web_add_info \
	  -sEXPORTED_RUNTIME_METHODS=FS,ccall,UTF8ToString \
	  -o build/web/mmextract.js
	cp fonts/*.ttf fonts/*.otf fonts/LICENSE-*.txt build/web/fonts/
	cp build/soundfonts/GeneralUser-GS.sf2 soundfonts/LICENSE-GeneralUser-GS.txt build/web/soundfonts/
	printf 'GeneralUser-GS.sf2\tGeneralUser GS\n' > build/web/soundfonts/list.txt

# the SoundFont for the web / Android music (downloaded, checksum verified; not in the repository)
build/soundfonts/GeneralUser-GS.sf2: tools/fetch_soundfont.sh
	sh tools/fetch_soundfont.sh $@

# Android: the web build (without any data) in the Capacitor app -> APK. Debug-signed unless a release keystore is
# given (ANDROID_KEYSTORE, ANDROID_KEYSTORE_PASSWORD, ANDROID_KEY_ALIAS, ANDROID_KEY_PASSWORD: release workflow).
# KEYS=<folder>: sign with the keystore.properties found there (the app's fixed key, kept outside the repository)
android: build/web/mindmaze.js
	rm -rf app/www && mkdir -p app/www && cp -r build/web/. app/www/ && rm -rf app/www/data
	cd app && npm install --no-audit --no-fund --silent && npx cap sync android
	python3 -I tools/art/make_android_icons.py app/android/app/src/main/res
	cd app/android && chmod +x gradlew && $(if $(KEYS),set -a && . $(KEYS)/keystore.properties && ANDROID_KEYSTORE=$(KEYS)/$$ANDROID_KEYSTORE && set +a && ,)./gradlew --no-daemon -q assembleDebug
	mkdir -p build/android && cp app/android/app/build/outputs/apk/debug/app-debug.apk build/android/MindMaze.apk

clean:
	rm -rf build

.PHONY: all test test-data test-extract win32 web android clean art-text
