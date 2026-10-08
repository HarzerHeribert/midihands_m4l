#!/bin/bash
# Renders the README images (docs/images/*.png) from the editor page's demo
# mode, so no real camera picture is ever used. Needs Google Chrome; uses
# Ableton Sans from Live's bundle when Live 12 Suite is installed.
#   docs/render/render.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
chrome="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
fonts="file:///Applications/Ableton%20Live%2012%20Suite.app/Contents/App-Resources/Fonts"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

shot() {  # url width,height scale out
  "$chrome" --headless=new --hide-scrollbars --allow-file-access-from-files \
    --force-device-scale-factor="$3" --window-size="$2" --virtual-time-budget=3000 \
    --screenshot="$4" "$1" >/dev/null 2>&1
}

# 1. The editor itself, at 2x.
sed "s#<style>#<style>@font-face{font-family:AbletonSansSmall;src:url($fonts/AbletonSansSmall-Regular.ttf);font-weight:400}@font-face{font-family:AbletonSansSmall;src:url($fonts/AbletonSansSmall-Bold.ttf);font-weight:700}#" \
  package/javascript/mh-editor.html > "$tmp/editor.html"
cp package/javascript/mh-gl.js package/javascript/mh-fx.js "$tmp/"   # the page loads them next to itself
shot "file://$tmp/editor.html" 1200,760 2 "$tmp/play.png"
shot "file://$tmp/editor.html#move" 1200,760 2 "$tmp/move.png"
shot "file://$PWD/package/javascript/mh-video.html#fmt=1&scene&clean" 540,960 2 "$tmp/video916.png"

# 2. The compositions.
sed "s#FONTS#$fonts#g" docs/render/common.css > "$tmp/common.css"
for spec in hero:1760,980 play:1760,1080 move:1760,1080 install:1760,640 effects:1760,1000 video:1760,1060; do
  name="${spec%%:*}" size="${spec#*:}"
  [ -f "docs/render/$name.html" ] || continue
  sed -e "s#RAW_JS#file://$PWD/package/javascript#g" -e "s#RAW#file://$tmp#g" "docs/render/$name.html" > "$tmp/$name.html"
  shot "file://$tmp/$name.html" "$size" 1 "docs/images/$name.png"
  echo "docs/images/$name.png"
done
