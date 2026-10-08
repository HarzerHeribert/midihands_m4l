# AGENTS.md

midihands for Live: a Max for Live MIDI effect that turns camera hand tracking into MIDI.
Read README.md first.

## Commands

- `make` builds the external, CLI, tests and device; `make test` runs core unit tests.
- `make install` links `package/` into `~/Documents/Max 9/Packages/midihands`.
- `./build/mh replay <clips> --phases corpus.yaml` checks tracking against labelled clips.

## Rules

- `core/` stays portable C++17: no Apple, Max or Objective-C APIs. Platform code goes in `mac/`.
- The device is generated: edit `device/build_device.py`, then run `make device`.
  Never edit `MidiHands.amxd` by hand.
- Message names between the patch and `mh.hands` are listed at the top of `max/mh.hands.mm`;
  keep the generator, the external and README in sync when changing them.
- Expression order (`core/engine.hpp` `Expr`) and scale order (`scaleTypes()` in the external)
  are mirrored in the generator's lists.
- MIDI and expressions leave the external on Max's scheduler thread; drawing data and the
  camera picture on the main thread. Never call outlets from the capture thread.
- Cameras are owned by `mac/camera_hub` (one pipeline per camera in use, shared by every
  device instance). Instances never open cameras themselves.
- The strip and the editor window talk through `---mh_*` send/receive names (listed at the
  top of `device/build_device.py`). Every Live parameter lives in exactly one of the two views.
- The editor is `package/javascript/mh-editor.html` in a jweb. Its settings are hidden
  live.numbox parameters defined in `stored_params()` in the generator; the page and the
  patch exchange `set <key> <value>` / `param <key> <value>` (protocol at the top of the
  HTML). Adding a setting means: a row in `stored_params()`, a key in the page's `P`, and,
  if the engine needs it, an entry in `ENGINE_MESSAGES` plus handling in `mh.hands`.
- Open the page in a browser to work on the UI: without Max it runs a demo.
- Pictures are drawn by `mh-gl.js` (WebGL2: camera, hands, cues, effect chain); effects live
  in `mh-fx.js` (six 0..1 knobs each; the menu order is the `FX<n> Effect` enum, read by the
  generator). Sets store an effect by its index, so add new effects only at the end of
  `list`, and never remove or reorder one; the page groups them for the menu.
  `docs/render/gallery.html` renders every effect and reports shader errors.
  The video window is `mh-video.html` in its own subpatcher; it gets the same `param`
  messages as the editor (`---mh_param`) and sends commands back through `---mh_vcmd`.
- `mac/recorder` records the video window (ScreenCaptureKit, AVAssetWriter) into
  ~/Movies/MidiHands; Live needs the Screen & System Audio Recording permission.
- MidiHands Audio (audio effect, `build_audio()` in the generator) passes its track's sound
  through and runs `mh.audio~` (`core/audio`). It sends on the global name `mh_audio`
  (no `---`, so every device in the Set hears it); `mh-audio-hub.js` in each MidiHands
  collects the letters A..H and feeds the pages. A Max MIDI effect gets no audio, and
  `live.routing` delivered none to a Max audio effect either (tested in Live 12.4), so the
  device sits on the track it listens to.
- FX modulation sources are `MOD_SOURCES` in the generator (hand movements, then letter x
  feature): append only, Sets store the index.
- Links (MOVE) are a fixed pool of `LINKS` in the generator. A link's target id lives in a
  `live.object` saved with `_persistence 1` (as in Ableton's own mapping snippets);
  `live.remote~` only gets it while the link and Movement are on. `mh-links.js` follows Live's
  selected parameter and names targets. Nothing may touch Live API objects before
  `---mh_boot` (live.thisdevice).
- Live keeps a native external loaded until it quits: restart Live after `make external`.
- Work on `main`; this is a solo project. The owner alone decides what is merged.
- The version lives in `VERSION` (compiled in as `MH_VERSION`, see `core/version.hpp`);
  releases follow RELEASING.md (`scripts/release.sh`, tag push, Release workflow). Keep
  `CHANGELOG.md` `[Unreleased]` up to date with user-visible changes.
- `scripts/install.sh` is what users and the device's Update button run: keep it working
  both from an unzipped release folder and piped from `releases/latest/download/`.
- README images come from `docs/render/render.sh` (editor demo mode, never a real camera
  picture); re-render after visible UI changes.
- `mac/updater` talks to GitHub (release check, cached for an hour); nothing else in the
  device goes online.
