#!/bin/bash
# Installs MidiHands for Ableton Live (Max for Live).
#
#   Latest release, straight from GitHub:
#     curl -fsSL https://github.com/HarzerHeribert/midihands_m4l/releases/latest/download/install.sh | bash
#   From an unzipped release folder:
#     ./install.sh
#
# Options:
#   --version X.Y.Z   install that release instead of the latest (download mode)
#   --force           replace a development install (`make install` symlink)
#   --yes             don't ask anything (used by the device's Update button)
#
# What it does: copies the Max package to ~/Documents/Max 9/Packages/midihands and the
# device to your User Library (Presets > MIDI Effects > Max MIDI Effect). Nothing else.
set -euo pipefail

REPO="HarzerHeribert/midihands_m4l"
ASSET="MidiHands-macOS.zip"
MIN_MACOS=14

version="" force=0 yes=0
while [ $# -gt 0 ]; do
  case "$1" in
    --version) version="${2#v}"; shift 2 ;;
    --force) force=1; shift ;;
    --yes) yes=1; shift ;;
    -h|--help) sed -n '2,17p' "$0" 2>/dev/null || true; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

say() { printf '%s\n' "$*"; }
fail() { printf 'MidiHands install failed: %s\n' "$*" >&2; exit 1; }

# --- prerequisites ---------------------------------------------------------------
[ "$(uname -s)" = "Darwin" ] || fail "MidiHands needs macOS."
macos_major="$(sw_vers -productVersion | cut -d. -f1)"
[ "$macos_major" -ge "$MIN_MACOS" ] || fail "MidiHands needs macOS $MIN_MACOS or newer (this Mac has $(sw_vers -productVersion))."

live_app="$(ls -d /Applications/Ableton\ Live\ 12*.app "$HOME"/Applications/Ableton\ Live\ 12*.app 2>/dev/null | tail -1 || true)"
if [ -z "$live_app" ]; then
  say "Warning: Ableton Live 12 was not found in /Applications. MidiHands needs Live 12 Suite"
  say "(or Live 12 Standard with Max for Live). Installing anyway."
fi

# --- where things go ---------------------------------------------------------------
package_dest="$HOME/Documents/Max 9/Packages/midihands"
user_library="$HOME/Music/Ableton/User Library"
library_cfg="$(ls -d "$HOME"/Library/Preferences/Ableton/Live\ 12*/Library.cfg 2>/dev/null | sort -V | tail -1 || true)"
if [ -n "$library_cfg" ]; then
  # <UserLibrary> ... <ProjectName Value="User Library" /> <ProjectPath Value="/Users/x/Music/Ableton" />
  block="$(sed -n '/<UserLibrary>/,/<\/UserLibrary>/p' "$library_cfg")"
  ul_path="$(printf '%s\n' "$block" | sed -n 's/.*<ProjectPath Value="\([^"]*\)".*/\1/p' | head -1)"
  ul_name="$(printf '%s\n' "$block" | sed -n 's/.*<ProjectName Value="\([^"]*\)".*/\1/p' | head -1)"
  [ -n "$ul_path" ] && [ -n "$ul_name" ] && [ -d "$ul_path/$ul_name" ] && user_library="$ul_path/$ul_name"
fi
device_dir="$user_library/Presets/MIDI Effects/Max MIDI Effect"

if [ -L "$package_dest" ] && [ "$force" -eq 0 ]; then
  fail "a development install is linked at $package_dest (from 'make install'). Update it with git pull && make, or rerun with --force to replace it with the release."
fi

# --- get the files ----------------------------------------------------------------------
here=""
if [ -n "${BASH_SOURCE[0]:-}" ] && [ -f "${BASH_SOURCE[0]}" ]; then
  here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi
work=""
cleanup() { [ -n "$work" ] && rm -rf "$work"; }
trap cleanup EXIT

if [ -n "$here" ] && [ -d "$here/midihands/externals/mh.hands.mxo" ] && [ -f "$here/MidiHands.amxd" ]; then
  src="$here"
else
  if [ -n "$version" ]; then base="https://github.com/$REPO/releases/download/v$version"
  else base="https://github.com/$REPO/releases/latest/download"; fi
  base="${MIDIHANDS_RELEASE_URL:-$base}"  # tests point this at a local folder (file://...)
  work="$(mktemp -d)"
  say "Downloading MidiHands ${version:-(latest)}..."
  curl -fsSL "$base/$ASSET" -o "$work/$ASSET" || fail "could not download $base/$ASSET"
  curl -fsSL "$base/SHA256SUMS" -o "$work/SHA256SUMS" || fail "could not download the checksum file"
  expected="$(awk -v f="$ASSET" '$2 == f || $2 == "*"f {print $1}' "$work/SHA256SUMS")"
  actual="$(shasum -a 256 "$work/$ASSET" | awk '{print $1}')"
  [ -n "$expected" ] && [ "$expected" = "$actual" ] || fail "the download does not match its checksum."
  ditto -x -k "$work/$ASSET" "$work/unzipped"
  src="$(ls -d "$work"/unzipped/MidiHands-* | head -1)"
  [ -d "$src/midihands" ] || fail "the download does not contain the MidiHands package."
fi

new_version="$(sed -n 's/.*"version"[^"]*"\([^"]*\)".*/\1/p' "$src/midihands/package-info.json" | head -1)"
old_version=""
[ -f "$package_dest/package-info.json" ] && old_version="$(sed -n 's/.*"version"[^"]*"\([^"]*\)".*/\1/p' "$package_dest/package-info.json" | head -1)"

if [ "$yes" -eq 0 ] && [ -t 0 ]; then
  say "Install MidiHands $new_version${old_version:+ (replacing $old_version)}:"
  say "  Max package: $package_dest"
  say "  Device:      $device_dir/MidiHands.amxd"
  printf 'Continue? [Y/n] '
  read -r answer
  case "$answer" in [nN]*) say "Nothing changed."; exit 0 ;; esac
fi

# --- install ------------------------------------------------------------------------------
mkdir -p "$(dirname "$package_dest")" "$device_dir"
rm -rf "$package_dest"
ditto "$src/midihands" "$package_dest"
# Files from a browser download carry a quarantine flag that makes macOS refuse the
# (ad-hoc signed) camera external; this install path never needs it.
xattr -dr com.apple.quarantine "$package_dest" 2>/dev/null || true

# Remove first so a hard link left by a development setup is replaced, not written through.
rm -f "$device_dir/MidiHands.amxd"
cp "$src/MidiHands.amxd" "$device_dir/MidiHands.amxd"
xattr -d com.apple.quarantine "$device_dir/MidiHands.amxd" 2>/dev/null || true

say "MidiHands $new_version installed."
if pgrep -xq Live; then
  say "Restart Ableton Live to use the new version."
fi
say "In Live's browser: User Library > Presets > MIDI Effects > Max MIDI Effect > MidiHands"
say "(or search for MidiHands), drop it on a MIDI track before an instrument, and switch the camera on."
