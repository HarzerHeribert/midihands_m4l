# midihands for Max for Live
#
#   make            build everything (external, CLI, tests, device)
#   make test       run the core unit tests
#   make install    link the Max package into ~/Documents/Max 9/Packages
#   make replay CLIPS="a.mp4 b.mp4" [PHASES=corpus.yaml]
#   make live       run the live camera for 10 s and print timing

CXX       ?= clang++
SDK       := third_party/max-sdk-base/c74support/max-includes
JIT       := third_party/max-sdk-base/c74support/jit-includes
MACOS_MIN := 14.0
ARCHS     := -arch arm64 -arch x86_64
CXXFLAGS  := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
OBJCFLAGS := -fobjc-arc -mmacosx-version-min=$(MACOS_MIN)
FRAMEWORKS := -framework Foundation -framework AVFoundation -framework CoreMedia \
              -framework CoreVideo -framework Vision

CORE_SRC := $(wildcard core/*.cpp)
CORE_HDR := $(wildcard core/*.hpp)
MAC_SRC  := mac/tracker.mm mac/camera_hub.mm
MAC_HDR  := mac/tracker.hpp mac/camera_hub.hpp

PACKAGE  := package
EXTERNAL := $(PACKAGE)/externals/mh.hands.mxo
MAX_PACKAGES := $(HOME)/Documents/Max 9/Packages

.PHONY: all test install uninstall device external cli replay live clean

all: external cli test device

external: $(EXTERNAL)/Contents/MacOS/mh.hands
cli: build/mh
device: device/MidiHands.amxd

$(EXTERNAL)/Contents/MacOS/mh.hands: $(CORE_SRC) $(CORE_HDR) $(MAC_SRC) $(MAC_HDR) max/mh.hands.mm max/Info.plist
	@mkdir -p $(EXTERNAL)/Contents/MacOS
	$(CXX) $(CXXFLAGS) $(OBJCFLAGS) $(ARCHS) -Wno-cast-function-type-mismatch -I$(SDK) -I$(JIT) -DMAC_VERSION \
		-bundle -o $@ $(CORE_SRC) $(MAC_SRC) max/mh.hands.mm $(FRAMEWORKS) @$(SDK)/c74_linker_flags.txt \
		-F$(JIT) -framework JitterAPI
	cp max/Info.plist $(EXTERNAL)/Contents/Info.plist
	printf 'iLaX????' > $(EXTERNAL)/Contents/PkgInfo
	codesign --force --sign - $(EXTERNAL)

build/mh: $(CORE_SRC) $(CORE_HDR) $(MAC_SRC) $(MAC_HDR) tools/mh.mm
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(OBJCFLAGS) -o $@ $(CORE_SRC) $(MAC_SRC) tools/mh.mm $(FRAMEWORKS) \
		-framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers

build/test_core: $(CORE_SRC) $(CORE_HDR) tests/test_core.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(CORE_SRC) tests/test_core.cpp

test: build/test_core
	./build/test_core

device/MidiHands.amxd: device/build_device.py
	python3 device/build_device.py

install: external
	@mkdir -p "$(MAX_PACKAGES)"
	ln -sfn "$(CURDIR)/$(PACKAGE)" "$(MAX_PACKAGES)/midihands"
	@echo "Linked $(MAX_PACKAGES)/midihands -> $(CURDIR)/$(PACKAGE)"

uninstall:
	rm -f "$(MAX_PACKAGES)/midihands"

replay: cli
	./build/mh replay $(CLIPS) $(if $(PHASES),--phases $(PHASES))

live: cli
	./build/mh live --seconds 10

clean:
	rm -rf build $(PACKAGE)/externals
