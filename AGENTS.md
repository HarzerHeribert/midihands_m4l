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
- Live keeps a native external loaded until it quits: restart Live after `make external`.
- Work on `main`; this is a solo project.
