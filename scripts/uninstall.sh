#!/bin/bash
# Removes MidiHands: the Max package and the devices in your User Library.
# Live Sets that use MidiHands keep their settings but show the device as missing.
#   ./uninstall.sh            (from a release folder)
#   --force                   also remove a development install link
set -euo pipefail

force=0
[ "${1:-}" = "--force" ] && force=1

package_dest="$HOME/Documents/Max 9/Packages/midihands"
user_library="$HOME/Music/Ableton/User Library"
library_cfg="$(ls -d "$HOME"/Library/Preferences/Ableton/Live\ 12*/Library.cfg 2>/dev/null | sort -V | tail -1 || true)"
if [ -n "$library_cfg" ]; then
  block="$(sed -n '/<UserLibrary>/,/<\/UserLibrary>/p' "$library_cfg")"
  ul_path="$(printf '%s\n' "$block" | sed -n 's/.*<ProjectPath Value="\([^"]*\)".*/\1/p' | head -1)"
  ul_name="$(printf '%s\n' "$block" | sed -n 's/.*<ProjectName Value="\([^"]*\)".*/\1/p' | head -1)"
  [ -n "$ul_path" ] && [ -n "$ul_name" ] && [ -d "$ul_path/$ul_name" ] && user_library="$ul_path/$ul_name"
fi
device="$user_library/Presets/MIDI Effects/Max MIDI Effect/MidiHands.amxd"
audio_device="$user_library/Presets/Audio Effects/Max Audio Effect/MidiHands Audio.amxd"

if [ -L "$package_dest" ] && [ "$force" -eq 0 ]; then
  echo "A development install is linked at $package_dest; rerun with --force to remove it." >&2
  exit 1
fi
rm -rf "$package_dest"
rm -f "$device" "$audio_device"
echo "MidiHands removed. Restart Live if it is running."
