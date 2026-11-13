#pragma once
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include "third_party/absl/container/flat_hash_map.h"

namespace absl {
template <typename T, typename H = std::hash<T>, typename E = std::equal_to<T>>
class flat_hash_set : public std::unordered_set<T, H, E> {
  using Base = std::unordered_set<T, H, E>;
 public:
  using Base::Base;
  using Base::find;

  bool contains(const T& k) const { return this->find(k) != this->end(); }

  template <typename U = T, typename = std::enable_if_t<std::is_same_v<U, std::string>>>
  typename Base::const_iterator find(std::string_view sv) const {
    return Base::find(std::string(sv));
  }
  template <typename U = T, typename = std::enable_if_t<std::is_same_v<U, std::string>>>
  bool contains(std::string_view sv) const {
    return Base::find(std::string(sv)) != this->end();
  }
};
}  // namespace absl
