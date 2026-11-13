#pragma once
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace absl {
namespace str_cat_internal {
inline void AppendOne(std::string* out, std::string_view s) {
  out->append(s.data(), s.size());
}
inline void AppendOne(std::string* out, const std::string& s) {
  out->append(s);
}
inline void AppendOne(std::string* out, const char* s) {
  if (s) out->append(s);
}
inline void AppendOne(std::string* out, char c) {
  out->push_back(c);
}
template <typename T, typename = std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, char> && !std::is_same_v<T, bool>>>
inline void AppendOne(std::string* out, T v) {
  out->append(std::to_string(v));
}
inline void AppendOne(std::string* out, bool b) {
  out->append(b ? "1" : "0");
}
}  // namespace str_cat_internal

template <typename... Args>
inline void StrAppend(std::string* dest, const Args&... args) {
  (str_cat_internal::AppendOne(dest, args), ...);
}

template <typename... Args>
inline std::string StrCat(const Args&... args) {
  std::string out;
  (str_cat_internal::AppendOne(&out, args), ...);
  return out;
}
}  // namespace absl
