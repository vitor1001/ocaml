#pragma once
#include <sstream>
#include <string>
#include <string_view>

namespace absl {
template <typename Range>
std::string StrJoin(const Range& r, std::string_view sep) {
  std::ostringstream oss;
  bool first = true;
  for (const auto& elem : r) {
    if (!first) oss << sep;
    first = false;
    oss << elem;
  }
  return oss.str();
}
}  // namespace absl
