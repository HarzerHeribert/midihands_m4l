# Development

How MidiHands is built, tested and released. For using it, see the [README](../README.md).

## Build from source

Needs Xcode command line tools and Python 3.

```bash
git clone --recurse-submodules https://github.com/HarzerHeribert/midihands_m4l.git
cd midihands_m4l
make            # external (universal, ad-hoc signed), CLI, tests, device
make install    # link this checkout into Max and Live (replaces a release install)
```

`make install` links `package/` to `~/Documents/Max 9/Packages/midihands` and hard-links
`device/MidiHands.amxd` into the User Library (Live ignores symlinks there; the hard link
follows every `make device`). Live loads a native external once per session: restart Live
after `make external`. To go back to the release, run the install command from the README.

The editor (`package/javascript/mh-editor.html`) also runs in a browser: without Max it shows
a demo with fake hands, which is the quickest way to work on the UI.

## Layout

```
core/     portable C++17: filters, finger features, hand sides, scales, finger slots and
          presets, note bookkeeping, engine, camera-picture thumbnails, version, and the
          audio analysis for MidiHands Audio (audio: bands, auto level, beats)
platform/ interfaces of the platform layer, and the camera hub (one pipeline per camera
          in use, shared by every device)
mac/      macOS: AVFoundation camera + Apple Vision hand pose (tracker), a loopback MJPEG
          server for the camera picture (preview_server), release updates (updater),
          recording the video window (recorder: ScreenCaptureKit + AVAssetWriter)
win/      Windows: Media Foundation camera (tracker), Winsock MJPEG server, WinHTTP updates;
          no recording yet
track/    MediaPipe's hand landmarker on ONNX Runtime (portable; the Windows tracker uses it)
models/   the MediaPipe hand models as ONNX (Apache 2.0, see THIRD_PARTY.md)
max/      the Max externals: mh.hands (one per MidiHands, subscribes to the camera hub)
          and mh.audio~ (one per MidiHands Audio), plain C++ for both systems
device/   build_device.py generates MidiHands.amxd and MidiHands Audio.amxd (never edit an
          .amxd by hand)
package/  the Max package: externals/ (built), javascript/ (strip hand view, editor and video
          pages, WebGL renderer mh-gl.js, effects mh-fx.js, links, audio hub)
scripts/  install, uninstall and release scripts
tools/    the mh command-line tool
tests/    core unit tests
docs/     README images, the script that renders them, and render/gallery.html (every
          effect side by side; reports shader errors)
```

Only `mac/` and `win/` are platform-specific; `core/`, `track/`, the devices and the pages
are shared. Rules for changing things are in [AGENTS.md](../AGENTS.md).

## Windows

The Windows package is cross-compiled on the Mac with MinGW-w64:

```bash
brew install mingw-w64
scripts/fetch-onnxruntime.sh      # ONNX Runtime 1.20.1, pinned checksums
make windows                      # build/win/midihands: the Windows Max package
make dist-windows                 # dist/MidiHands-Windows.zip and install.ps1
make test-mediapipe               # the hand pipeline here, on the Mac's ONNX Runtime
```

The externals are DLLs (`.mxe64`) linked statically, so they need only Windows' and Max's
own DLLs; `onnxruntime.dll` sits in the package's `support\` folder and is loaded from
there explicitly. The hand pipeline (`track/`) is a port of MediaPipe Tasks' hand landmarker
and matched the official runtime within 0.8 px on MediaPipe's test images; keep that parity
when changing it. CI runs the pipeline (`mh-track.exe`) and the installer on a Windows
runner.

## Command-line tool

```bash
./build/mh version
./build/mh checkupdate                                     # the device's update check
./build/mh cameras
./build/mh live "My Camera" --seconds 10 --instances 3     # three engines, one shared camera
./build/mh replay clips/*.mp4 --phases corpus.yaml          # tracking accuracy on recorded clips
./build/mh replay clips/*.mp4 --compare                     # note-onset delay of filter settings
./build/mh snapshot clip.mp4 480 frame.png                  # a camera picture with hands drawn
```

## Measurements

On an M4 Pro with macOS 26.5, using a 5,400-frame test corpus recorded for this project (six
lighting and background scenes at 60 fps, not part of this repository):

| | Earlier MediaPipe pipeline | MidiHands |
|---|---|---|
| Exact hand count | 90.8% | **91.6%** (94.8% on the 30 fps set) |
| Correct left/right when the count is right | 80–92% | **100%** |
| Detection time per frame (p50 / p95) | 3.3–4.9 ms | **3.3 / 4.9 ms** |
| Live camera, frame arrival → landmarks (p50 / p95) | | **4.3 / 9.1 ms** |
| Note-onset delay added by landmark smoothing (mean) | | **2 ms at 30 fps** |

Frames stay at camera resolution. Two hands covering each other remain the weak spot.
Camera timestamps mark arrival at the Mac, so sensor exposure and transport are not included;
the camera's frame rate is usually the largest part of the delay you feel.

## README images

`docs/render/render.sh` renders `docs/images/*.png` with headless Chrome from the editor's
demo mode (no real camera picture) and the compositions in `docs/render/`. Run it after UI
changes.

## Releases

See [RELEASING.md](../RELEASING.md). In short: changes under `[Unreleased]` in the changelog,
`scripts/release.sh X.Y.Z`, `git push origin main vX.Y.Z`; GitHub Actions builds and
publishes. `make dist` builds the same zip locally.
