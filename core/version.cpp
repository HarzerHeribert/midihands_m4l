#include "version.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace mh {

namespace {
std::vector<int> parts(const std::string& v) {
  std::vector<int> out;
  size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
  while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
    int n = 0;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) n = n * 10 + (v[i++] - '0');
    out.push_back(n);
    if (i < v.size() && v[i] == '.') ++i;
    else break;
  }
  return out;
}


// "beta.1" in "0.3.0-beta.1"; empty for a release.
std::string prerelease(const std::string& v) {
  const size_t dash = v.find('-');
  return dash == std::string::npos ? "" : v.substr(dash + 1);
}

bool numeric(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

// Semantic versioning: a prerelease ranks below its release; prereleases compare by their
// dot-separated parts, numbers numerically ("beta.2" < "beta.10").
int comparePrerelease(const std::string& a, const std::string& b) {
  if (a == b) return 0;
  if (a.empty()) return 1;
  if (b.empty()) return -1;
  size_t i = 0, j = 0;
  while (i <= a.size() && j <= b.size()) {
    const size_t ei = std::min(a.find('.', i), a.size()), ej = std::min(b.find('.', j), b.size());
    const std::string x = a.substr(i, ei - i), y = b.substr(j, ej - j);
    if (x != y) {
      if (numeric(x) && numeric(y)) return std::stoll(x) < std::stoll(y) ? -1 : 1;
      if (numeric(x) != numeric(y)) return numeric(x) ? -1 : 1;
      return x < y ? -1 : 1;
    }
    i = ei + 1;
    j = ej + 1;
    if (i > a.size() || j > b.size()) return i > a.size() && j > b.size() ? 0 : (i > a.size() ? -1 : 1);
  }
  return 0;
}
}  // namespace

int compareVersions(const std::string& a, const std::string& b) {
  const std::vector<int> pa = parts(a), pb = parts(b);
  for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
    const int x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  return comparePrerelease(prerelease(a), prerelease(b));
}

}  // namespace mh
