#pragma once
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <type_traits>
#include <vector>

namespace absl {
template <typename T>
class Span {
 public:
  using value_type = std::remove_cv_t<T>;
  using pointer = T*;
  using reference = T&;
  using iterator = T*;
  using const_iterator = const T*;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  constexpr Span() noexcept : ptr_(nullptr), len_(0) {}
  constexpr Span(T* ptr, size_t len) noexcept : ptr_(ptr), len_(len) {}
  template <size_t N>
  constexpr Span(T (&arr)[N]) noexcept : ptr_(arr), len_(N) {}
  template <typename Alloc>
  Span(std::vector<value_type, Alloc>& v) noexcept : ptr_(v.data()), len_(v.size()) {}
  template <typename Alloc, typename U = T, typename = std::enable_if_t<std::is_const_v<U>>>
  Span(const std::vector<value_type, Alloc>& v) noexcept : ptr_(v.data()), len_(v.size()) {}
  template <typename U = T, typename = std::enable_if_t<std::is_const_v<U>>>
  constexpr Span(std::initializer_list<value_type> il) noexcept : ptr_(il.begin()), len_(il.size()) {}
  template <typename U, typename = std::enable_if_t<std::is_convertible_v<U (*)[], T (*)[]>>>
  constexpr Span(const Span<U>& other) noexcept : ptr_(other.data()), len_(other.size()) {}

  constexpr T* data() const noexcept { return ptr_; }
  constexpr size_t size() const noexcept { return len_; }
  constexpr bool empty() const noexcept { return len_ == 0; }
  constexpr T& operator[](size_t i) const noexcept { return ptr_[i]; }
  constexpr T& front() const noexcept { return ptr_[0]; }
  constexpr T& back() const noexcept { return ptr_[len_ - 1]; }
  constexpr iterator begin() const noexcept { return ptr_; }
  constexpr iterator end() const noexcept { return ptr_ + len_; }
  constexpr reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }
  constexpr reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }
  constexpr Span subspan(size_t pos, size_t count = static_cast<size_t>(-1)) const {
    if (pos >= len_) return Span();
    size_t rem = len_ - pos;
    return Span(ptr_ + pos, count > rem ? rem : count);
  }
 private:
  T* ptr_;
  size_t len_;
};

template <typename T>
constexpr Span<T> MakeSpan(T* ptr, size_t size) noexcept { return Span<T>(ptr, size); }
template <typename T, typename Alloc>
Span<T> MakeSpan(std::vector<T, Alloc>& v) noexcept { return Span<T>(v.data(), v.size()); }
template <typename T, typename Alloc>
Span<const T> MakeSpan(const std::vector<T, Alloc>& v) noexcept { return Span<const T>(v.data(), v.size()); }
template <typename T>
constexpr Span<const T> MakeConstSpan(const T* ptr, size_t size) noexcept { return Span<const T>(ptr, size); }
template <typename T, typename Alloc>
Span<const T> MakeConstSpan(const std::vector<T, Alloc>& v) noexcept { return Span<const T>(v.data(), v.size()); }
}  // namespace absl
