#pragma once
#include <cctype>
#include <string>
#include <string_view>

namespace absl {
inline bool ascii_isspace(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
inline bool ascii_isdigit(unsigned char c) { return c >= '0' && c <= '9'; }
inline bool ascii_isxdigit(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline bool ascii_isalpha(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
inline bool ascii_isalnum(unsigned char c) {
  return ascii_isalpha(c) || ascii_isdigit(c);
}
inline bool ascii_islower(unsigned char c) { return c >= 'a' && c <= 'z'; }
inline bool ascii_isupper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
inline char ascii_tolower(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
}
inline char ascii_toupper(unsigned char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : static_cast<char>(c);
}
inline std::string_view StripLeadingAsciiWhitespace(std::string_view s) {
  size_t i = 0;
  while (i < s.size() && ascii_isspace(static_cast<unsigned char>(s[i]))) ++i;
  return s.substr(i);
}
inline std::string_view StripTrailingAsciiWhitespace(std::string_view s) {
  size_t n = s.size();
  while (n > 0 && ascii_isspace(static_cast<unsigned char>(s[n - 1]))) --n;
  return s.substr(0, n);
}
inline std::string_view StripAsciiWhitespace(std::string_view s) {
  return StripTrailingAsciiWhitespace(StripLeadingAsciiWhitespace(s));
}
}  // namespace absl
