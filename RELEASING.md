# Releasing

Releases are built and published by GitHub Actions when a `v*` tag is pushed.

1. Make sure `main` is green (CI) and the device works in Live.
2. Write the changes under `## [Unreleased]` in `CHANGELOG.md`.
3. Run `scripts/release.sh X.Y.Z` (the Windows half needs `brew install mingw-w64`). It sets the version in `VERSION` and
   `package/package-info.json`, turns `[Unreleased]` into `[X.Y.Z] - <date>`, builds and
   checks the release zip locally (`make dist`), commits and tags `vX.Y.Z`.
4. Push: `git push origin main vX.Y.Z`.
5. The Release workflow builds on a clean macOS runner (Windows cross-compiled), checks that
   the tag matches `VERSION`, and publishes `MidiHands-macOS.zip`, `install.sh`,
   `MidiHands-Windows.zip`, `install.ps1` and `SHA256SUMS` with the changelog section as
   release notes.

Devices with the editor open see the new version within an hour and offer **Update**.

To try the release build without publishing, run the Release workflow by hand
(Actions > Release > Run workflow): it uploads the zip as a build artifact instead.

Versions: patch (0.1.1) for fixes, minor (0.2.0) for new features, major once settings or
saved Sets would break.

Publish normal releases only. A suffix (`scripts/release.sh 0.3.0-beta.1`) still makes a
GitHub pre-release, but GitHub's "latest" skips pre-releases, so the Update button never
offers one and its testers never get offered the next beta either (tried with
0.3.0-beta.1/2, and it was a mess). Something new and less tested, like Windows in 0.3.0,
ships in a normal release and is called "beta" in the README and the release notes.
