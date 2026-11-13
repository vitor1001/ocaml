#pragma once
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace absl {
inline std::string StrReplaceAll(
    std::string_view s,
    std::initializer_list<std::pair<std::string_view, std::string_view>> replacements) {
  std::string out(s);
  for (const auto& [old_sub, new_sub] : replacements) {
    if (old_sub.empty()) continue;
    std::string next;
    size_t pos = 0;
    while (true) {
      size_t found = out.find(old_sub, pos);
      if (found == std::string::npos) {
        next.append(out, pos, std::string::npos);
        break;
      }
      next.append(out, pos, found - pos);
      next.append(new_sub);
      pos = found + old_sub.size();
    }
    out = std::move(next);
  }
  return out;
}
}  // namespace absl
