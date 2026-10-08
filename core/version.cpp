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
}  // namespace

int compareVersions(const std::string& a, const std::string& b) {
  const std::vector<int> pa = parts(a), pb = parts(b);
  for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
    const int x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

}  // namespace mh
