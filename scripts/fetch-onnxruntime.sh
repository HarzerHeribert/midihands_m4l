#!/bin/bash
# Downloads ONNX Runtime (MIT, github.com/microsoft/onnxruntime) into build/onnxruntime:
# win-x64 (shipped with the Windows release) and, on a Mac, osx (to run the MediaPipe
# tracker locally, e.g. `mh replay --tracker mediapipe`). Checksums pinned below.
set -euo pipefail
cd "$(dirname "$0")/.."
V=1.20.1
base="https://github.com/microsoft/onnxruntime/releases/download/v$V"
mkdir -p build/onnxruntime
fetch() {  # name archive sha256
  local dir="build/onnxruntime/$1" file="build/onnxruntime/$2"
  [ -f "$dir/.ok" ] && return 0
  curl -fsSL "$base/$2" -o "$file"
  echo "$3  $file" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch: $2" >&2; exit 1; }
  rm -rf "${dir:?}" && mkdir -p "$dir"
  case "$2" in *.zip) unzip -q "$file" -d "$dir.tmp" ;; *) mkdir -p "$dir.tmp" && tar -xzf "$file" -C "$dir.tmp" ;; esac
  mv "$dir.tmp"/*/* "$dir"/ && rm -rf "${dir:?}.tmp" "${file:?}"
  touch "$dir/.ok"
  echo "build/onnxruntime/$1"
}
fetch win-x64 "onnxruntime-win-x64-$V.zip" 78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581
if [ "$(uname)" = Darwin ]; then fetch osx "onnxruntime-osx-universal2-$V.tgz" da4349e01a7e997f5034563183c7183d069caadc1d95f499b560961787813efd; fi
