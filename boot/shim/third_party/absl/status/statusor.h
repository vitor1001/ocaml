#pragma once
#include <optional>
#include <type_traits>
#include <utility>
#include "third_party/absl/log/check.h"
#include "third_party/absl/status/status.h"

namespace absl {
template <typename T>
class StatusOr {
 public:
  StatusOr(const Status& s) : status_(s) { CHECK(!s.ok()); }
  StatusOr(Status&& s) : status_(std::move(s)) { CHECK(!status_.ok()); }
  StatusOr(const T& v) : status_(OkStatus()), value_(v) {}
  StatusOr(T&& v) : status_(OkStatus()), value_(std::move(v)) {}
  template <typename U, typename = std::enable_if_t<std::is_constructible_v<T, U&&> && !std::is_same_v<std::decay_t<U>, StatusOr> && !std::is_same_v<std::decay_t<U>, Status>>>
  StatusOr(U&& v) : status_(OkStatus()), value_(std::forward<U>(v)) {}

  bool ok() const { return status_.ok(); }
  const Status& status() const { return status_; }
  T& value() & { CHECK(ok()); return *value_; }
  const T& value() const& { CHECK(ok()); return *value_; }
  T&& value() && { CHECK(ok()); return std::move(*value_); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T&& operator*() && { return std::move(*value_); }
  T* operator->() { CHECK(ok()); return &(*value_); }
  const T* operator->() const { CHECK(ok()); return &(*value_); }

 private:
  Status status_;
  std::optional<T> value_;
};
}  // namespace absl
