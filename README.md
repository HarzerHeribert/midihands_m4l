# midihands for Live

Play Ableton Live with your hands. `MidiHands` is a Max for Live MIDI effect. Put it on a
MIDI track in front of any instrument, turn the camera on, and your fingers play notes in the
Set's scale while hand movements control any parameter you map.

No separate app, no virtual MIDI ports, no Python. Tracking runs inside Live using the
camera and Apple's Vision hand-pose model on the Neural Engine/GPU.

## Requirements

- macOS 14 or later (macOS 26 recommended: it ships Apple's newer, more accurate hand model)
- Ableton Live 12 Suite, or Standard with Max for Live
- A camera. Frame rate matters more than resolution: a 60 fps camera (for example an iPhone
  via Continuity Camera) roughly halves the camera's share of the latency compared with 30 fps.
  Light matters too: webcams lower their frame rate in dim rooms (a 30 fps webcam ran at
  15 fps at night). The device shows the live frame rate under the hand view.

## Install

In Terminal:

```bash
curl -fsSL https://github.com/HarzerHeribert/midihands_m4l/releases/latest/download/install.sh | bash
```

Then restart Live and find **MidiHands** in the browser under User Library > Presets >
MIDI Effects > Max MIDI Effect (or search for it). Drop it on a MIDI track before an
instrument and switch the camera on; the first time, allow Live to use the camera.

Or download `MidiHands-macOS.zip` from [Releases](https://github.com/HarzerHeribert/midihands_m4l/releases),
unzip it and run `./install.sh` in that folder. `./uninstall.sh` removes everything again.

Why a script: MidiHands contains a native camera external that is ad-hoc signed, not
notarized by Apple. macOS blocks such files when they come from a browser download; the
script installs without that flag (and checks the download's SHA-256).

**Updates:** the editor checks GitHub for a newer release when it opens and shows an
**Update** button; restart Live afterwards. Running the install command again works too.

## Build from source

```bash
git submodule update --init   # Max SDK
make                          # external, CLI, tests, device
make install                  # links package/ into ~/Documents/Max 9/Packages/midihands
```

Then drag `device/MidiHands.amxd` onto a MIDI track. On first use, Live asks for camera access.

To find it in Live's browser, hard-link it into the User Library (Live ignores symlinks there;
a hard link stays in sync with every `make device`):

```bash
ln "$PWD/device/MidiHands.amxd" "$HOME/Music/Ableton/User Library/Presets/MIDI Effects/Max MIDI Effect/"
```

## Using the device

The device on the track is the compact view: the hand view, the camera switch and camera
choice, **Open Editor**, and two master switches:

- **Notes**: fingers play notes. Off releases sounding notes and keeps everything silent.
- **Movement**: map slots drive their Live parameters and CCs go out. Off lets go of every
  mapped parameter, so it can be turned by hand and its automation plays again.

Both are Live parameters: automate them, or MIDI-map them to a footswitch. The editor shows
them too. The editor is a separate window in Live's own look that floats above Live like a
plugin window; closing it does not stop anything. The line at its bottom explains whatever
the mouse is over.

**PLAY**
- The camera picture with both hands drawn on it. Every fingertip is labelled with what it
  plays and lights up while it plays.
- Eight pads, one per finger (thumbs never play): click a pad to edit it, then choose Off /
  Note / Chord, its scale degree (each button shows the note or chord it gives) and octave,
  or simply click a key on the keyboard. Every choice is a single click.
- Layout presets: **Keys** (eight scale notes), **Chords** (eight triads), **Split** (left
  hand chords I, IV, V, vi; right hand melody). Editing any pad switches to **Custom**.
- Sound: sensitivity, velocity (fixed, from hand height or finger speed), octave.
- Key: follows the Set's root and scale (Live 12), or your own.
- Hands that play: both, left only, or right only (see "Several devices").
- Timing & MIDI: minimum note length, dropout hold, movement smoothing, MIDI channel, and
  sending the ten movements as CCs.

**MOVE** links hand movements to Live parameters.
- The ten movements (height, x, pinch, fist, tilt of each hand) with live meters. Each
  movement lists its links underneath; one movement can drive any number of parameters
  (16 links in all).
- To link: click a control in Live (the bar on top shows what is selected), then **+** on a
  movement. The button names the parameter it will link. With nothing selected it says
  **Map…** and waits for you to click a parameter.
- **Learn** watches you for three seconds, picks the movement you made, links it to the
  selected parameter and fits the input range to how far you moved.
- Each link: **On** (automatable; off lets go of the parameter), input range, a curve (drag
  up or down), **Inv** to turn the parameter the other way, output range, and **✕** to remove it.
- Links are saved with the Set.

Every setting is a Live parameter, saved with the Set; the main ones (layout, hands, scale,
sensitivity, velocity, map min/max…) can be automated.

## Several devices, several cameras

Put MidiHands on as many tracks as you like. All of them share one tracking backend inside
Live:

- Each camera that at least one device has switched on runs **one** detection. Devices on
  the same camera get the exact same hands, so they never disagree.
- A second camera only starts when some device picks it, and stops when no device uses it.
- The camera picture in the editor is streamed over the loopback interface (127.0.0.1)
  and only encoded while an editor shows it.
- Each device keeps its own settings. For example: piano track = left hand, Chords; synth
  track = right hand, Keys.

## How it is built

```
core/     portable C++17: filters, finger features, hand sides, scales, finger slots and
          presets, note bookkeeping, engine, camera-picture thumbnails
mac/      macOS only: AVFoundation camera + Apple Vision hand pose (tracker), the shared
          backend that runs one pipeline per active camera (camera_hub), and a loopback
          MJPEG server for the editor's camera picture (preview_server)
max/      the mh.hands Max external (one per device, subscribes to the shared backend)
device/   build_device.py generates MidiHands.amxd (never edit the .amxd by hand)
package/  Max package: externals/ (built), javascript/mh-view.js (hand view on the track)
          and mh-editor.html (the editor window, shown by a jweb object)
tools/    mh command-line tool: list cameras, replay recorded clips, live timing
tests/    core unit tests
```

Only `mac/` is platform-specific. A Windows version would need a new tracker there
(Media Foundation + MediaPipe hand models) and a Windows build of the external. `core/`, the
device and the view stay the same, and one frozen `.amxd` can carry both platforms' externals.

## Measurements

Measured on an M4 Pro, macOS 26.5, with the 5,400-frame corpus from the 2026 pipeline study
(`praxisprojekt-6-midihands/07-messungen`, six lighting/background scenes at 60 fps).

| | Study's best pipeline | This device |
|---|---|---|
| Exact hand count | 90.8% (MediaPipe + CLAHE) | **91.6%** |
| Correct left/right when the count is right | 80–92% | **100%** |
| Detection time per frame (p50 / p95) | 3.3–4.9 ms | **3.3 / 4.9 ms** |
| Live camera, frame arrival → landmarks (p50 / p95) | ~27 ms capture → dispatch | **4.3 / 9.1 ms** |

The study downscaled frames before detection; here frames stay at camera resolution. Two
hands occluding each other remain the weak spot (50% exact count). Camera frame timestamps
mark arrival at the Mac, so sensor exposure and transport delay are not included in the last row.

```bash
./build/mh cameras
./build/mh replay clips/*.mp4 --phases corpus.yaml
./build/mh live "My Camera" --seconds 10 --instances 3   # three engines, one shared camera
./build/mh snapshot clip.mp4 480 frame.png                # render a camera picture with hands
```

## Releases

See [RELEASING.md](RELEASING.md): `scripts/release.sh X.Y.Z`, push the tag, and GitHub Actions
publishes the zip, the install script and checksums. `make dist` builds the same zip locally.
Changes are listed in [CHANGELOG.md](CHANGELOG.md).

The device is shipped unfrozen next to its Max package (installed together by the script):
the editor page and scripts are loaded from the package at runtime. A Developer ID
signature and notarization would allow a double-click installer later.

## License

All rights reserved; official release builds are free to use. See [LICENSE](LICENSE).
Issues and suggestions are welcome, see [CONTRIBUTING.md](CONTRIBUTING.md).
