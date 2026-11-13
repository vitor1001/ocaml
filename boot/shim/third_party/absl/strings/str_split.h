#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "third_party/absl/strings/ascii.h"

namespace absl {
struct SkipEmpty {
  bool operator()(std::string_view s) const { return !s.empty(); }
};
struct SkipWhitespace {
  bool operator()(std::string_view s) const {
    return !StripAsciiWhitespace(s).empty();
  }
};
struct AllowEmpty {
  bool operator()(std::string_view) const { return true; }
};

template <typename Pred = AllowEmpty>
std::vector<std::string_view> StrSplit(std::string_view text, char delim, Pred pred = AllowEmpty{}) {
  std::vector<std::string_view> out;
  if (text.empty()) {
    return out;
  }
  size_t start = 0;
  while (true) {
    size_t pos = text.find(delim, start);
    std::string_view piece = (pos == std::string_view::npos)
                                 ? text.substr(start)
                                 : text.substr(start, pos - start);
    if (pred(piece)) out.push_back(piece);
    if (pos == std::string_view::npos) break;
    start = pos + 1;
  }
  return out;
}
}  // namespace absl
