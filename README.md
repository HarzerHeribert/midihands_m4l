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

## Build and install

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

| Control | What it does |
|---|---|
| Camera / device menu | Turns tracking on and picks the camera |
| Layout | **Split**: left hand plays chords I, IV, V, vi (pinky → index); right hand plays melody notes. **Keys**: eight fingers are eight scale notes, left pinky to right pinky. **Chords**: same as Keys, but every finger plays a triad |
| Live Scale | Follows the Set's root and scale (Live 12). Turn it off to pick root and scale yourself |
| Sens | How far a finger must straighten to play. Higher means it triggers sooner |
| Velocity / Vel mode | Fixed velocity, hand height, or finger speed |
| Oct | Octave shift |
| CC Out | Also sends the ten hand movements as CC 20–29 (for hardware or plugins with MIDI learn) |
| Map slots | Pick a movement (height, x, pinch, fist, tilt of either hand), click Map, click any parameter in Live. Min/max set the range |

A finger plays while it is straightened and stops when it bends. Thumbs never play notes.
Notes last at least 180 ms and survive tracking dropouts up to 300 ms, so flicker does not
retrigger.

## How it is built

```
core/     portable C++17: filters, finger features, hand sides, scales, note bookkeeping, engine
mac/      macOS only: AVFoundation camera + Apple Vision hand pose
max/      the mh.hands Max external (wraps mac/ + core/)
device/   build_device.py generates MidiHands.amxd (never edit the .amxd by hand)
package/  Max package: externals/ (built) and javascript/mh-view.js (hand view)
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
./build/mh live "My Camera" --seconds 10
```

## Shipping a build

Open the device in Live's Max editor and click **Freeze**: this embeds the external and the
view script so the single `.amxd` works without `make install`. For other people's Macs the
external also has to be signed with a Developer ID and notarized.
