#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace absl {
template <typename T>
struct Flag {
  const char* name;
  T value;
  const char* help;
};

struct FlagRegistryEntry {
  std::string_view name;
  void (*parse_fn)(void* flag_ptr, std::string_view val);
  void* flag_ptr;
};

inline std::vector<FlagRegistryEntry>& GetFlagRegistry() {
  static std::vector<FlagRegistryEntry> reg;
  return reg;
}

template <typename T>
void ParseFlagValue(void* ptr, std::string_view val);

template <>
inline void ParseFlagValue<std::string>(void* ptr, std::string_view val) {
  * static_cast<std::string*>(ptr) = std::string(val);
}

template <>
inline void ParseFlagValue<bool>(void* ptr, std::string_view val) {
  bool b = (val.empty() || val == "true" || val == "1" || val == "yes");
  if (val == "false" || val == "0" || val == "no") b = false;
  *static_cast<bool*>(ptr) = b;
}

template <typename T>
struct FlagRegistrar {
  FlagRegistrar(Flag<T>* f) {
    GetFlagRegistry().push_back({f->name, &ParseFlagValue<T>, &f->value});
  }
};

template <typename T>
const T& GetFlag(const Flag<T>& flag) {
  return flag.value;
}

template <typename T, typename U>
void SetFlag(Flag<T>* flag, const U& v) {
  flag->value = v;
}
}  // namespace absl

#define ABSL_FLAG(Type, name, default_val, help) \
  ::absl::Flag<Type> FLAGS_##name{#name, default_val, help}; \
  static ::absl::FlagRegistrar<Type> FLAGS_reg_##name(&FLAGS_##name)
