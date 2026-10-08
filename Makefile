# midihands for Max for Live
#
#   make            build everything (external, CLI, tests, device)
#   make test       run the core unit tests
#   make install    link the Max package into ~/Documents/Max 9/Packages (development)
#   make windows    cross-compile the Windows package into build/win (MinGW-w64)
#   make dist       build the release zips in dist/ (see RELEASING.md)
#   make replay CLIPS="a.mp4 b.mp4" [PHASES=corpus.yaml]
#   make live       run the live camera for 10 s and print timing

VERSION   := $(shell cat VERSION)
CXX       ?= clang++
SDK       := third_party/max-sdk-base/c74support/max-includes
MACOS_MIN := 14.0
ARCHS     := -arch arm64 -arch x86_64
CXXFLAGS  := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -DMH_VERSION='"$(VERSION)"'
OBJCFLAGS := -fobjc-arc -mmacosx-version-min=$(MACOS_MIN)
FRAMEWORKS := -framework Foundation -framework AVFoundation -framework CoreMedia \
              -framework CoreVideo -framework Vision -framework Network -framework ImageIO \
              -framework CoreGraphics -framework UniformTypeIdentifiers -framework CoreImage \
              -framework AppKit -framework ScreenCaptureKit

CORE_SRC := $(wildcard core/*.cpp)
CORE_HDR := $(wildcard core/*.hpp)
# Platform layer: shared interfaces and the camera hub in platform/, implementations in
# mac/ (here) and win/ (Makefile.win).
PLATFORM_SRC := platform/camera_hub.cpp
PLATFORM_HDR := $(wildcard platform/*.hpp)
MAC_SRC  := $(PLATFORM_SRC) mac/tracker.mm mac/preview_server.mm mac/updater.mm mac/recorder.mm mac/shell.mm
MAC_HDR  := $(PLATFORM_HDR)

PACKAGE  := package
EXTERNAL := $(PACKAGE)/externals/mh.hands.mxo
AUDIO_EXTERNAL := $(PACKAGE)/externals/mh.audio~.mxo
# MSP functions mh.audio~ uses, resolved by Max at load time like the Max API itself.
MSP_LINK := -Wl,-U,_z_dsp_setup -Wl,-U,_z_dsp_free -Wl,-U,_class_dspinit
MAX_PACKAGES := $(HOME)/Documents/Max 9/Packages
USER_LIBRARY ?= $(HOME)/Music/Ableton/User Library
DEVICE_DIR   := $(USER_LIBRARY)/Presets/MIDI Effects/Max MIDI Effect
AUDIO_DEVICE_DIR := $(USER_LIBRARY)/Presets/Audio Effects/Max Audio Effect

DIST     := dist/MidiHands-$(VERSION)

.PHONY: all test install uninstall device external cli replay live clean dist check-version windows dist-windows test-mediapipe

all: external cli test device

external: $(EXTERNAL)/Contents/MacOS/mh.hands $(AUDIO_EXTERNAL)/Contents/MacOS/mh.audio~
cli: build/mh
device: device/MidiHands.amxd

$(EXTERNAL)/Contents/MacOS/mh.hands: $(CORE_SRC) $(CORE_HDR) $(MAC_SRC) $(MAC_HDR) max/mh.hands.cpp max/Info.plist VERSION
	@mkdir -p $(EXTERNAL)/Contents/MacOS
	$(CXX) $(CXXFLAGS) $(OBJCFLAGS) $(ARCHS) -Wno-cast-function-type-mismatch -I$(SDK) -DMAC_VERSION \
		-bundle -o $@ $(CORE_SRC) $(MAC_SRC) max/mh.hands.cpp $(FRAMEWORKS) @$(SDK)/c74_linker_flags.txt

	sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@NAME@/mh.hands/g' -e 's/@ID@/mh.hands/g' max/Info.plist > $(EXTERNAL)/Contents/Info.plist
	printf 'iLaX????' > $(EXTERNAL)/Contents/PkgInfo
	codesign --force --sign - $(EXTERNAL)

$(AUDIO_EXTERNAL)/Contents/MacOS/mh.audio~: core/audio.cpp core/audio.hpp max/mh.audio~.cpp max/Info.plist VERSION
	@mkdir -p $(AUDIO_EXTERNAL)/Contents/MacOS
	$(CXX) $(CXXFLAGS) -mmacosx-version-min=$(MACOS_MIN) $(ARCHS) -Wno-cast-function-type-mismatch -I$(SDK) -I$(SDK)/../msp-includes -DMAC_VERSION \
		-bundle -o $@ core/audio.cpp max/mh.audio~.cpp @$(SDK)/c74_linker_flags.txt $(MSP_LINK)
	sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@NAME@/mh.audio~/g' -e 's/@ID@/mh.audio/g' max/Info.plist > $(AUDIO_EXTERNAL)/Contents/Info.plist
	printf 'iLaX????' > $(AUDIO_EXTERNAL)/Contents/PkgInfo
	codesign --force --sign - $(AUDIO_EXTERNAL)

build/mh: $(CORE_SRC) $(CORE_HDR) $(MAC_SRC) $(MAC_HDR) tools/mh.mm VERSION
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(OBJCFLAGS) -o $@ $(CORE_SRC) $(MAC_SRC) tools/mh.mm $(FRAMEWORKS)

build/test_core: $(CORE_SRC) $(CORE_HDR) tests/test_core.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(CORE_SRC) tests/test_core.cpp

test: build/test_core
	./build/test_core

device/MidiHands.amxd: device/build_device.py VERSION
	python3 device/build_device.py

# Development install: Max uses this checkout's package, Live's browser these device files.
# (build_device.py writes both devices; the rule names the first.)
install: external device
	@mkdir -p "$(MAX_PACKAGES)" "$(DEVICE_DIR)"
	@if [ -d "$(MAX_PACKAGES)/midihands" ] && [ ! -L "$(MAX_PACKAGES)/midihands" ]; then \
		rm -rf "$(MAX_PACKAGES)/midihands"; echo "Replaced the installed release with this checkout"; fi
	ln -sfn "$(CURDIR)/$(PACKAGE)" "$(MAX_PACKAGES)/midihands"
	ln -f device/MidiHands.amxd "$(DEVICE_DIR)/MidiHands.amxd"
	@mkdir -p "$(AUDIO_DEVICE_DIR)"
	ln -f "device/MidiHands Audio.amxd" "$(AUDIO_DEVICE_DIR)/MidiHands Audio.amxd"
	@echo "Restart Live to load the external from this checkout."

uninstall:
	rm -f "$(MAX_PACKAGES)/midihands" "$(DEVICE_DIR)/MidiHands.amxd" "$(AUDIO_DEVICE_DIR)/MidiHands Audio.amxd"

replay: cli
	./build/mh replay $(CLIPS) $(if $(PHASES),--phases $(PHASES))

live: cli
	./build/mh live --seconds 10

# The version lives in VERSION; package-info.json and the changelog must agree.
check-version:
	@grep -q '"version" : "$(VERSION)"' $(PACKAGE)/package-info.json || { echo "package-info.json is not at $(VERSION)"; exit 1; }
	@grep -q '^## \[$(VERSION)\]' CHANGELOG.md || { echo "CHANGELOG.md has no section for $(VERSION)"; exit 1; }
	@echo "version $(VERSION) ok"

# Release zip: a versioned folder with the Max package, the device and the install
# scripts, under a fixed asset name so releases/latest/download/... always works.
dist: check-version all
	rm -rf dist
	mkdir -p "$(DIST)"
	ditto --norsrc $(PACKAGE) "$(DIST)/midihands"
	find "$(DIST)" -name .DS_Store -delete
	cp device/MidiHands.amxd "device/MidiHands Audio.amxd" scripts/install.sh scripts/uninstall.sh "$(DIST)/"
	cp scripts/INSTALL.txt "$(DIST)/INSTALL.txt"
	cp LICENSE "$(DIST)/LICENSE.txt"
	codesign --verify --strict "$(DIST)/midihands/externals/mh.hands.mxo"
	codesign --verify --strict "$(DIST)/midihands/externals/mh.audio~.mxo"
	cd dist && ditto -c -k --norsrc --keepParent MidiHands-$(VERSION) MidiHands-macOS.zip
	cp scripts/install.sh dist/install.sh
	$(MAKE) dist-windows
	cd dist && shasum -a 256 MidiHands-macOS.zip install.sh MidiHands-Windows.zip install.ps1 > SHA256SUMS
	@echo "dist/MidiHands-macOS.zip, dist/MidiHands-Windows.zip ($(VERSION))"

# --- Windows: cross-compiled with MinGW-w64 (brew install mingw-w64) -----------------------
# The externals are .mxe64 DLLs linked statically (only Windows and Max DLLs needed); hand
# tracking runs the MediaPipe models (models/) on ONNX Runtime, which the package carries in
# support\ (scripts/fetch-onnxruntime.sh). Same devices and pages as on the Mac.
WIN_CXX   ?= x86_64-w64-mingw32-g++
ORT_DIR   := build/onnxruntime/win-x64
WIN_FLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -Wno-unknown-pragmas \
             -Wno-unused-function -DWIN_VERSION -DWIN64 -D_USE_MATH_DEFINES -D_WIN32_WINNT=0x0A00 \
             -DMH_VERSION='"$(VERSION)"' -I$(SDK) -I$(SDK)/../msp-includes -I$(ORT_DIR)/include
WIN_LINK  := -shared -static -s
WIN_PKG   := build/win/midihands
WIN_HANDS_SRC := $(CORE_SRC) $(PLATFORM_SRC) $(wildcard track/*.cpp) $(wildcard win/*.cpp) max/mh.hands.cpp
WIN_HANDS_LIBS := $(SDK)/x64/MaxAPI.lib -lmfplat -lmf -lmfreadwrite -lmfuuid -lole32 -lws2_32 -lwinhttp -lshlwapi \
                  -lshell32 -luuid
WIN_DIST  := dist/MidiHands-$(VERSION)-Windows

$(ORT_DIR)/.ok:
	scripts/fetch-onnxruntime.sh

$(WIN_PKG)/externals/mh.hands.mxe64: $(WIN_HANDS_SRC) $(CORE_HDR) $(PLATFORM_HDR) $(wildcard track/*.hpp) $(wildcard win/*.hpp) $(ORT_DIR)/.ok VERSION
	@mkdir -p $(dir $@)
	$(WIN_CXX) $(WIN_FLAGS) -o $@ $(WIN_HANDS_SRC) $(WIN_LINK) $(WIN_HANDS_LIBS)

$(WIN_PKG)/externals/mh.audio~.mxe64: core/audio.cpp core/audio.hpp max/mh.audio~.cpp VERSION
	@mkdir -p $(dir $@)
	$(WIN_CXX) $(WIN_FLAGS) -o $@ core/audio.cpp max/mh.audio~.cpp $(WIN_LINK) $(SDK)/x64/MaxAPI.lib $(SDK)/../msp-includes/x64/MaxAudio.lib

# The Windows package: the shared files of package/ (not the Mac externals), the Windows
# externals, the models and the runtime.
windows: $(WIN_PKG)/externals/mh.hands.mxe64 $(WIN_PKG)/externals/mh.audio~.mxe64 device
	rsync -a --delete --exclude externals --exclude .DS_Store $(PACKAGE)/ $(WIN_PKG)/.package/
	rsync -a $(WIN_PKG)/.package/ $(WIN_PKG)/ && rm -rf $(WIN_PKG)/.package
	@mkdir -p $(WIN_PKG)/support $(WIN_PKG)/models
	cp $(ORT_DIR)/lib/onnxruntime.dll $(WIN_PKG)/support/
	cp models/hand_detector.onnx models/hand_landmarks.onnx $(WIN_PKG)/models/
	@echo "$(WIN_PKG) (Windows package)"

# The hand pipeline on still images (tests/data), as a console program: run here with the
# Mac runtime (`make test-mediapipe`), and on Windows by CI (build/win/mh-track.exe).
build/win/mh-track.exe: tools/mh-track.cpp $(wildcard track/*.cpp) $(wildcard track/*.hpp) $(ORT_DIR)/.ok
	@mkdir -p build/win
	$(WIN_CXX) -std=c++17 -O2 -I$(ORT_DIR)/include -o $@ tools/mh-track.cpp $(wildcard track/*.cpp) -static -s

build/mh-track: tools/mh-track.cpp $(wildcard track/*.cpp) $(wildcard track/*.hpp) $(ORT_DIR)/.ok
	$(CXX) -std=c++17 -O2 -I$(ORT_DIR)/include -o $@ tools/mh-track.cpp $(wildcard track/*.cpp)

test-mediapipe: build/mh-track
	./build/mh-track build/onnxruntime/osx/lib/libonnxruntime.dylib models tests/data/thumb_up.jpg | grep -q '"side": "right"'
	@echo "MediaPipe pipeline ok"

dist-windows: windows
	rm -rf "$(WIN_DIST)" dist/MidiHands-Windows.zip
	mkdir -p "$(WIN_DIST)"
	rsync -a --exclude .DS_Store $(WIN_PKG)/ "$(WIN_DIST)/midihands/"
	cp device/MidiHands.amxd "device/MidiHands Audio.amxd" scripts/uninstall.ps1 "$(WIN_DIST)/"
	sed 's/@RELEASE_VERSION@/$(VERSION)/' scripts/install.ps1 > "$(WIN_DIST)/install.ps1"
	cp scripts/INSTALL-Windows.txt "$(WIN_DIST)/INSTALL.txt"
	cp LICENSE "$(WIN_DIST)/LICENSE.txt"
	cp THIRD_PARTY.md "$(WIN_DIST)/THIRD_PARTY.txt"
	cd dist && ditto -c -k --norsrc --keepParent MidiHands-$(VERSION)-Windows MidiHands-Windows.zip
	sed 's/@RELEASE_VERSION@/$(VERSION)/' scripts/install.ps1 > dist/install.ps1
	@echo "dist/MidiHands-Windows.zip ($(VERSION))"

clean:
	rm -rf build dist $(PACKAGE)/externals
