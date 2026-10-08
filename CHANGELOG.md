# Changelog

All notable changes to MidiHands. Versions follow [semantic versioning](https://semver.org);
the format follows [Keep a Changelog](https://keepachangelog.com).

## [Unreleased]

### Added
- FX page: four effect slots with 22 video effects (Film Grade, Gradient Map, Thermal, Neon
  Edges, Glow, Halftone, ASCII, Mosaic, Kaleidoscope, Ripple, Liquid, RGB Split, Mirror,
  Tunnel, Echo, RGB Trails, Time Warp, VHS, Glitch, CRT, Strobe, Reaction Diffusion). Every
  knob and the mix can follow a hand movement, with an amount in either direction.
- Hands drawn with WebGL: the original app's glossy *Jelly* look, *Lines* or off, in Live
  colors or *Amber*; optional cues that show what each movement is doing.
- Video window: the picture at full resolution in 16:9, 9:16, 1:1 or 4:5, resizable, and
  fullscreen on any display.
- Recording: the video window with Live's sound as an MP4 in Movies/MidiHands.

### Changed
- The camera picture is in color and as sharp as the window showing it (up to 1920 wide);
  the Play page draws it with WebGL.

### Fixed
- `install.sh` run from an unzipped release folder reported failure (exit code 1) although
  the install succeeded.

### Changed
- README rewritten for players, with product renderings; build and release notes moved to
  docs/DEVELOPMENT.md. `make install` now replaces a release install with the checkout.

## [0.1.0] - 2026-10-08

First release.

### Added
- Max for Live MIDI effect: camera hand tracking (Apple Vision) plays notes and chords from
  eight fingers (thumbs never play) and moves Live parameters with hand movements.
- Editor window in Live's own look: camera picture with labelled fingertips, finger pads,
  one-click degree and octave, keyboard assignment, Keys / Chords / Split presets, key that
  follows the Live Set's scale.
- MOVE: ten hand movements (height, x, pinch, fist, tilt per hand); up to 16 links to Live
  parameters, any number per movement, linked to the parameter selected in Live with one
  click; per link on/off, input range, curve, invert and output range; Learn; links saved
  with the Set.
- Notes and Movement master switches on the device (automatable, MIDI-mappable).
- One shared camera pipeline for all MidiHands devices; each device can play one hand.
- Installer script and in-editor update button.
