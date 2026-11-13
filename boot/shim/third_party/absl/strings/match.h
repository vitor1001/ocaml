#pragma once
#include <string_view>

namespace absl {
inline bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}
inline bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size()) == suffix;
}
inline bool StrContains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}
inline bool StrContains(std::string_view haystack, char needle) {
  return haystack.find(needle) != std::string_view::npos;
}
}  // namespace absl
