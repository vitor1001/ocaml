#pragma once
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

namespace absl {
namespace str_format_internal {
template <typename T>
auto FormatArg(const T& arg) {
  if constexpr (std::is_same_v<std::decay_t<T>, std::string> ||
                std::is_same_v<std::decay_t<T>, std::string_view>) {
    return std::string(arg);
  } else {
    return arg;
  }
}
inline const char* UnwrapCStr(const std::string& s) { return s.c_str(); }
template <typename T>
T UnwrapCStr(T v) { return v; }
}  // namespace str_format_internal

template <typename... Args>
std::string StrFormat(const char* fmt, const Args&... args) {
  auto tup_call = [&](const auto&... converted) {
    int n = std::snprintf(nullptr, 0, fmt, str_format_internal::UnwrapCStr(converted)...);
    if (n <= 0) return std::string();
    std::string buf(static_cast<size_t>(n), '\0');
    std::snprintf(buf.data(), buf.size() + 1, fmt, str_format_internal::UnwrapCStr(converted)...);
    return buf;
  };
  return tup_call(str_format_internal::FormatArg(args)...);
}

template <typename... Args>
void StrAppendFormat(std::string* dst, const char* fmt, const Args&... args) {
  dst->append(StrFormat(fmt, args...));
}
}  // namespace absl
