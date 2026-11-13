#pragma once
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace absl {
class Status {
 public:
  Status() : ok_(true) {}
  explicit Status(std::string_view msg) : ok_(false), msg_(msg) {}
  bool ok() const { return ok_; }
  std::string_view message() const { return msg_; }
  std::string ToString() const { return ok_ ? "OK" : msg_; }
  friend std::ostream& operator<<(std::ostream& os, const Status& s) {
    return os << s.ToString();
  }
 private:
  bool ok_;
  std::string msg_;
};

inline Status OkStatus() { return Status(); }
inline Status InternalError(std::string_view msg) { return Status(msg); }
inline Status InvalidArgumentError(std::string_view msg) { return Status(msg); }
inline Status NotFoundError(std::string_view msg) { return Status(msg); }
inline Status FailedPreconditionError(std::string_view msg) { return Status(msg); }
inline Status UnimplementedError(std::string_view msg) { return Status(msg); }
}  // namespace absl
