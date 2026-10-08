// Release version, from the VERSION file (passed in by the Makefile).
#pragma once

#include <string>

#ifndef MH_VERSION
#define MH_VERSION "0.0.0-dev"
#endif

namespace mh {

inline constexpr const char* kVersion = MH_VERSION;

// Compares dotted versions: -1 if a < b, 0 if equal, 1 if a > b. A leading
// "v" and anything after the numbers ("-dev", "-rc1") are ignored.
int compareVersions(const std::string& a, const std::string& b);

}  // namespace mh
