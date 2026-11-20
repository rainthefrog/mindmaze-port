# mindmaze-port - build and test. Runs on Linux (or in the Docker image: see docker/Dockerfile).
#   make test                  core + extractor tests on synthetic data (no Encarta files needed)
#   make win32                 build/win32/MindMaze.exe (mingw-w64 cross build; the whole Windows download)
#   make test-data DATA=data   the core tests on a folder extracted from an Encarta 2006 disc
#   make test-extract ISO=...  the built-in C extractor against tools/extract/extract.py on a real disc
#   make linux                 build/linux/MindMaze-linux-x86_64.tar.gz (SDL3; in the image of docker/linux.Dockerfile)
#   make macos                 build/macos/MindMaze-macos.zip (SDL3; on a Mac with Xcode's tools, see release.yml)
#   make web                   build/web/ (WebAssembly engine of the Android app; needs emcc: emscripten/emsdk image)
#   make android               build/android/MindMaze.apk from build/web (mindmaze-port-android image; run make web first)
CORE = src/core/mm_util.c src/core/mm_questions.c src/core/mm_rich.c src/core/mm_sources.c src/core/mm_settings.c \
       src/core/mm_tables.c src/art/mm_art.c src/art/mm_art_img.c
EXTRACT = src/extract/mx_extract.c src/extract/mx_lzx.c src/extract/mx_util.c src/extract/mx_old.c src/extract/mx_edition.c \
          src/extract/mx_oldext.c src/extract/mx_tables.c src/extract/mx_tables_pe.c src/extract/mx_tables_ne.c \
          src/extract/mx_tables_mac.c build/embedded.c
WIN32 = src/platform/win32/win32_main.c src/platform/win32/win32_setup.c src/platform/win32/win32_export.c \
        src/platform/common/zip_write.c src/platform/common/cli.c \
        src/platform/win32/win32_update.c src/platform/common/update.c
HEADERS = src/core/mm.h src/core/mm_tables.h src/extract/mx.h src/extract/mx_tables.h src/extract/mx_oldext.h \
          src/art/mm_art.h src/art/mm_art_text.h
# the project page the game links to (Settings > Pictures); the release workflow sets it from the repository
REPO_URL ?= https://github.com/OWNER/mindmaze-port
# the version shown by --version (the release workflow sets it from the tag)
APP_VERSION_NAME ?= 1.0
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation -ffp-contract=off -DMM_REPO_URL='"$(REPO_URL)"' -DMM_VERSION='"$(APP_VERSION_NAME)"'
MINGW = i686-w64-mingw32
DATA ?= data

all: test win32

build/fixtures/tables.json: tests/make_fixtures.py tools/extract/mmdb.py
	mkdir -p build/fixtures
	python3 -I tests/make_fixtures.py build/fixtures

build/test_core: tests/test_core.c $(CORE) src/core/mm_game.c $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -Isrc/core -Isrc/art tests/test_core.c $(CORE) -lm -o $@

build/test_update: tests/test_update.c src/platform/common/update.c src/platform/common/update.h src/extract/mx_util.c
	mkdir -p build
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -Isrc/extract -Isrc/platform/common tests/test_update.c src/platform/common/update.c src/extract/mx_util.c -o $@

build/test_mx: tests/test_mx.c $(EXTRACT) $(HEADERS)
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -Isrc/extract tests/test_mx.c $(EXTRACT) -o $@

test: build/test_core build/test_mx build/fixtures/tables.json build/test_update
	build/test_core build/fixtures
	build/test_mx
	sh tests/make_update_fixtures.sh build/update-fixtures
	build/test_update build/update-fixtures
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
                          src/platform/win32/mindmaze.ico src/platform/common/cli.h src/platform/common/update.h
	mkdir -p build/win32
	cd src/platform/win32 && $(MINGW)-windres mindmaze.rc -O coff -o ../../../build/win32/mindmaze_res.o
	$(MINGW)-gcc $(CFLAGS) -msse2 -mfpmath=sse -municode -mwindows -Isrc/core -Isrc/extract -Isrc/art -Isrc/platform/common $(CORE) src/core/mm_game.c $(EXTRACT) $(WIN32) \
	  build/win32/mindmaze_res.o -o $@ -static -lwinmm -lgdi32 -lshell32 -lole32 -lwindowscodecs -luuid -lwininet -lcomdlg32
	$(MINGW)-strip $@

WEB = src/platform/web/web_main.c src/platform/common/text.c src/platform/common/music.c
WEB_EXPORTS = _main,_malloc,_free,_web_start,_web_resize,_web_mouse,_web_key,_web_char,_web_cmd,_web_music_ended,_web_http_done,_web_scale,_web_view_ext,_web_music_render,_web_music_rate,_web_music_state,_web_disc_reading,_web_disc_added,_web_art_installed
web: build/web/mindmaze.js
build/web/mindmaze.js: $(CORE) src/core/mm_game.c $(WEB) $(HEADERS) src/platform/common/text.h src/platform/common/music.h \
                       src/platform/web/index.html src/platform/web/shell.js src/platform/web/extract-worker.js \
                       tools/extract/mmextract_web.c $(EXTRACT) build/soundfonts/GeneralUser-GS.sf2
	mkdir -p build/web/fonts build/web/soundfonts
	emcc -std=c11 -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers -ffp-contract=off \
	  -DMM_REPO_URL='"$(REPO_URL)"' -Isrc/core -Isrc/art -Isrc/platform/common $(CORE) src/core/mm_game.c $(WEB) \
	  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=128MB -sFORCE_FILESYSTEM=1 -lidbfs.js -sENVIRONMENT=web \
	  -sEXPORTED_FUNCTIONS=$(WEB_EXPORTS) \
	  -sEXPORTED_RUNTIME_METHODS=FS,HEAPU8,UTF8ToString,stringToUTF8,stringToNewUTF8,lengthBytesUTF8,addRunDependency,removeRunDependency \
	  -o $@
	cp src/platform/web/index.html src/platform/web/extract-worker.js build/web/
	sed 's|@REPO_URL@|$(REPO_URL)|' src/platform/web/shell.js > build/web/shell.js
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

# Linux and macOS: one SDL3 layer (src/platform/sdl) + the shared text / music / zip code (src/platform/common).
# SDL3 is linked statically (tools/build_sdl3.sh; pkg-config finds it through PKG_CONFIG_PATH), so the download needs
# nothing installed. The fonts, the SoundFont and the icon sit next to the program (macOS: Contents/Resources).
SDL = src/platform/sdl/sdl_main.c src/platform/sdl/sdl_audio.c src/platform/sdl/sdl_files.c src/platform/sdl/sdl_setup.c \
      src/platform/common/text.c src/platform/common/music.c src/platform/common/zip_write.c src/platform/common/cli.c \
      src/platform/sdl/sdl_update.c src/platform/common/update.c
SDL_HEADERS = src/platform/sdl/sdl.h src/platform/common/text.h src/platform/common/music.h src/platform/common/zip_write.h src/platform/common/cli.h src/platform/common/update.h
SDL_SRC = $(CORE) src/core/mm_game.c $(EXTRACT) $(SDL)
SDL_INC = -Isrc/core -Isrc/extract -Isrc/art -Isrc/platform/common

linux: build/linux/MindMaze-linux-x86_64.tar.gz
build/linux/MindMaze-linux-x86_64.tar.gz: $(SDL_SRC) $(HEADERS) $(SDL_HEADERS) build/soundfonts/GeneralUser-GS.sf2 docs/icon.png \
                                          src/platform/sdl/README-linux.txt LICENSE
	rm -rf build/linux/MindMaze && mkdir -p build/linux/MindMaze/fonts build/linux/MindMaze/soundfonts
	$(CC) $(CFLAGS) $(SDL_INC) $$(pkg-config --cflags sdl3) $(SDL_SRC) -o build/linux/MindMaze/MindMaze \
	  -Wl,--as-needed $$(pkg-config --static --libs sdl3) -lm
	strip build/linux/MindMaze/MindMaze
	cp fonts/*.ttf fonts/*.otf fonts/LICENSE-*.txt build/linux/MindMaze/fonts/
	cp build/soundfonts/GeneralUser-GS.sf2 soundfonts/LICENSE-GeneralUser-GS.txt build/linux/MindMaze/soundfonts/
	cp docs/icon.png build/linux/MindMaze/mindmaze.png
	cp LICENSE build/linux/MindMaze/LICENSE.txt
	cp src/platform/sdl/README-linux.txt build/linux/MindMaze/README.txt
	tar -C build/linux -czf $@ MindMaze

# macOS: universal app (arm64 + x86_64; MACOS_ARCH="-arch arm64" for one), macOS 11 or later. Needs SDL3 built for
# the same architectures: sh tools/build_sdl3.sh build/sdl3-macos "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
# -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0, then PKG_CONFIG_PATH=build/sdl3-macos/lib/pkgconfig make macos.
# Not signed by a developer (no Apple ID): only the ad-hoc signature ("codesign -s -") that Apple Silicon needs to
# run any program at all. The linker gives the binary one, but strip and the resources added to the bundle afterwards
# invalidate it (macOS then calls the app "damaged"), so the finished bundle is signed ad-hoc once more.
MACOS_ARCH ?= -arch arm64 -arch x86_64
MACOS_APP = build/macos/MindMaze.app
macos: build/macos/MindMaze-macos.zip
build/macos/MindMaze-macos.zip: $(SDL_SRC) $(HEADERS) $(SDL_HEADERS) build/soundfonts/GeneralUser-GS.sf2 docs/icon.png \
                                src/platform/sdl/Info.plist LICENSE
	rm -rf $(MACOS_APP) build/macos/MindMaze.iconset $@
	mkdir -p $(MACOS_APP)/Contents/MacOS $(MACOS_APP)/Contents/Resources/fonts $(MACOS_APP)/Contents/Resources/soundfonts \
	         build/macos/MindMaze.iconset
	clang $(CFLAGS) -Wno-unknown-warning-option -Wno-null-pointer-subtraction $(MACOS_ARCH) -mmacosx-version-min=11.0 $(SDL_INC) $$(pkg-config --cflags sdl3) \
	  $(SDL_SRC) -o $(MACOS_APP)/Contents/MacOS/MindMaze $$(pkg-config --static --libs sdl3)
	strip -x $(MACOS_APP)/Contents/MacOS/MindMaze
	sed 's/@VERSION@/$(APP_VERSION_NAME)/g' src/platform/sdl/Info.plist > $(MACOS_APP)/Contents/Info.plist
	for s in 16 32 128 256 512; do \
	  sips -z $$s $$s docs/icon.png --out build/macos/MindMaze.iconset/icon_$${s}x$${s}.png > /dev/null && \
	  sips -z $$((s * 2)) $$((s * 2)) docs/icon.png --out build/macos/MindMaze.iconset/icon_$${s}x$${s}@2x.png > /dev/null; \
	done
	iconutil -c icns build/macos/MindMaze.iconset -o $(MACOS_APP)/Contents/Resources/MindMaze.icns
	cp fonts/*.ttf fonts/*.otf fonts/LICENSE-*.txt $(MACOS_APP)/Contents/Resources/fonts/
	cp build/soundfonts/GeneralUser-GS.sf2 soundfonts/LICENSE-GeneralUser-GS.txt $(MACOS_APP)/Contents/Resources/soundfonts/
	cp LICENSE $(MACOS_APP)/Contents/Resources/LICENSE.txt
	codesign --force --sign - $(MACOS_APP)
	cd build/macos && ditto -c -k --keepParent MindMaze.app MindMaze-macos.zip

clean:
	rm -rf build

.PHONY: all test test-data test-extract win32 web android linux macos clean art-text
