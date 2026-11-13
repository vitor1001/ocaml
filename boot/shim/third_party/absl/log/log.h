#pragma once
#include <cstdlib>
#include <iostream>
#include <sstream>
#include "third_party/absl/log/check.h"

namespace absl_shim_internal {
class StderrLogStream {
 public:
  explicit StderrLogStream(bool fatal) : fatal_(fatal) {}
  ~StderrLogStream() noexcept(false) {
    std::cerr << ss_.str() << std::endl;
    if (fatal_) std::abort();
  }
  template <typename T>
  StderrLogStream& operator<<(const T& v) {
    ss_ << v;
    return *this;
  }
 private:
  bool fatal_;
  std::ostringstream ss_;
};
struct FatalVoidify {
  [[noreturn]] void operator&(const StderrLogStream&) const { std::abort(); }
};
}  // namespace absl_shim_internal

#define ABSL_LOG_STREAM_INFO ::absl_shim_internal::StderrLogStream(false)
#define ABSL_LOG_STREAM_WARNING ::absl_shim_internal::StderrLogStream(false)
#define ABSL_LOG_STREAM_ERROR ::absl_shim_internal::StderrLogStream(false)
#define ABSL_LOG_STREAM_FATAL ::absl_shim_internal::FatalVoidify() & ::absl_shim_internal::StderrLogStream(true)
#define LOG(sev) ABSL_LOG_STREAM_##sev
