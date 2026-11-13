#pragma once
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

namespace absl {
template <typename K, typename V, typename H = std::hash<K>, typename E = std::equal_to<K>>
class flat_hash_map : public std::unordered_map<K, V, H, E> {
  using Base = std::unordered_map<K, V, H, E>;
 public:
  using Base::Base;
  using Base::find;
  using Base::operator[];

  bool contains(const K& k) const { return this->find(k) != this->end(); }

  template <typename U = K, typename = std::enable_if_t<std::is_same_v<U, std::string>>>
  typename Base::iterator find(std::string_view sv) {
    return Base::find(std::string(sv));
  }
  template <typename U = K, typename = std::enable_if_t<std::is_same_v<U, std::string>>>
  typename Base::const_iterator find(std::string_view sv) const {
    return Base::find(std::string(sv));
  }
  template <typename U = K, typename = std::enable_if_t<std::is_same_v<U, std::string>>>
  bool contains(std::string_view sv) const {
    return Base::find(std::string(sv)) != this->end();
  }
  template <typename U = K, typename = std::enable_if_t<std::is_same_v<U, std::string>>>
  V& operator[](std::string_view sv) {
    return Base::operator[](std::string(sv));
  }
};

template <typename Map, typename Pred>
void erase_if(Map& c, Pred pred) {
  for (auto it = c.begin(); it != c.end();) {
    if (pred(*it)) {
      it = c.erase(it);
    } else {
      ++it;
    }
  }
}
}  // namespace absl
