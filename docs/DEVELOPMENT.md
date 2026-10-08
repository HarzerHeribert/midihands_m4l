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
          presets, note bookkeeping, engine, camera-picture thumbnails, version
mac/      macOS only: AVFoundation camera + Apple Vision hand pose (tracker), the shared
          backend that runs one pipeline per camera in use (camera_hub), a loopback MJPEG
          server for the editor's camera picture (preview_server), release updates (updater)
max/      the mh.hands Max external (one per device, subscribes to the shared backend)
device/   build_device.py generates MidiHands.amxd (never edit the .amxd by hand)
package/  the Max package: externals/ (built), javascript/ (hand view, editor page, links)
scripts/  install, uninstall and release scripts
tools/    the mh command-line tool
tests/    core unit tests
docs/     README images and the script that renders them
```

Only `mac/` is platform-specific. A Windows version would need a new tracker there (Media
Foundation + MediaPipe hand models) and a Windows build of the external; `core/`, the
device and the editor stay the same. Rules for changing things are in [AGENTS.md](../AGENTS.md).

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
