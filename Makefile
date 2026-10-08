# midihands for Max for Live
#
#   make            build everything (external, CLI, tests, device)
#   make test       run the core unit tests
#   make install    link the Max package into ~/Documents/Max 9/Packages (development)
#   make dist       build the release zip in dist/ (see RELEASING.md)
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
MAC_SRC  := mac/tracker.mm mac/camera_hub.mm mac/preview_server.mm mac/updater.mm mac/recorder.mm
MAC_HDR  := mac/tracker.hpp mac/camera_hub.hpp mac/preview_server.hpp mac/updater.hpp mac/recorder.hpp

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

.PHONY: all test install uninstall device external cli replay live clean dist check-version

all: external cli test device

external: $(EXTERNAL)/Contents/MacOS/mh.hands $(AUDIO_EXTERNAL)/Contents/MacOS/mh.audio~
cli: build/mh
device: device/MidiHands.amxd

$(EXTERNAL)/Contents/MacOS/mh.hands: $(CORE_SRC) $(CORE_HDR) $(MAC_SRC) $(MAC_HDR) max/mh.hands.mm max/Info.plist VERSION
	@mkdir -p $(EXTERNAL)/Contents/MacOS
	$(CXX) $(CXXFLAGS) $(OBJCFLAGS) $(ARCHS) -Wno-cast-function-type-mismatch -I$(SDK) -DMAC_VERSION \
		-bundle -o $@ $(CORE_SRC) $(MAC_SRC) max/mh.hands.mm $(FRAMEWORKS) @$(SDK)/c74_linker_flags.txt

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
	cd dist && shasum -a 256 MidiHands-macOS.zip install.sh > SHA256SUMS
	@echo "dist/MidiHands-macOS.zip ($(VERSION))"

clean:
	rm -rf build dist $(PACKAGE)/externals
