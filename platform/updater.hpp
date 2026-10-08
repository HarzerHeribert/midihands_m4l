// Release updates from GitHub: check for a newer release, and install one by
// running the release's install script (the same one people install with).
#pragma once

#include <functional>
#include <string>

namespace mh {

constexpr const char* kRepository = "HarzerHeribert/midihands_m4l";

struct UpdateCheck {
  bool ok = false;      // GitHub answered
  bool newer = false;   // latest release is newer than this build
  std::string latest;   // e.g. "0.2.0"
  std::string error;
};

// Asks GitHub for the latest release. Answers are cached for an hour, so
// opening editors does not hit GitHub's rate limit. `done` runs on a
// background queue.
void checkForUpdate(std::function<void(const UpdateCheck&)> done);

// Downloads and runs the latest release's install.sh. `done(ok, message)`
// runs on a background queue; the new version is used after Live restarts.
void installUpdate(std::function<void(bool, const std::string&)> done);

}  // namespace mh
