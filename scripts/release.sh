#!/bin/bash
# Prepares release X.Y.Z: version bump, changelog, local release build, commit and tag.
# Usage: scripts/release.sh 0.2.0   (then: git push origin main v0.2.0)
set -euo pipefail
cd "$(dirname "$0")/.."

v="${1:-}"; v="${v#v}"
[[ "$v" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "usage: scripts/release.sh X.Y.Z" >&2; exit 2; }
[ "$(git rev-parse --abbrev-ref HEAD)" = "main" ] || { echo "release from main" >&2; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "commit or stash your changes first" >&2; exit 1; }
git rev-parse -q --verify "refs/tags/v$v" >/dev/null && { echo "v$v already exists" >&2; exit 1; }
awk '/^## \[Unreleased\]/{f=1;next} /^## \[/{f=0} f && NF' CHANGELOG.md | grep -q . \
  || { echo "write the changes under ## [Unreleased] in CHANGELOG.md first" >&2; exit 1; }

printf '%s\n' "$v" > VERSION
sed -i '' "s/\"version\" : \"[^\"]*\"/\"version\" : \"$v\"/" package/package-info.json
perl -pi -e "s/^## \\[Unreleased\\]\$/## [Unreleased]\n\n## [$v] - $(date +%Y-%m-%d)/" CHANGELOG.md

make dist
git add VERSION package/package-info.json CHANGELOG.md device/MidiHands.amxd
git commit -m "release: v$v"
git tag -a "v$v" -m "MidiHands $v"
echo "Ready. Publish with: git push origin main v$v"
