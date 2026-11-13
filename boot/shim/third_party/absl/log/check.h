#pragma once
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace absl_shim_internal {
class FatalLogStream {
 public:
  FatalLogStream(const char* file, int line, const char* expr) {
    ss_ << file << ":" << line << ": CHECK failed: " << expr << " ";
  }
  ~FatalLogStream() noexcept(false) {
    std::cerr << ss_.str() << std::endl;
    std::abort();
  }
  template <typename T>
  FatalLogStream& operator<<(const T& v) {
    ss_ << v;
    return *this;
  }
 private:
  std::ostringstream ss_;
};

class NullLogStream {
 public:
  template <typename T>
  NullLogStream& operator<<(const T&) { return *this; }
};
}  // namespace absl_shim_internal

#define CHECK(cond) \
  while (!(cond)) ::absl_shim_internal::FatalLogStream(__FILE__, __LINE__, #cond)
#define DCHECK(cond) CHECK(cond)
#define CHECK_EQ(a, b) CHECK((a) == (b))
#define DCHECK_EQ(a, b) CHECK_EQ(a, b)
#define CHECK_NE(a, b) CHECK((a) != (b))
#define DCHECK_NE(a, b) CHECK_NE(a, b)
#define CHECK_LE(a, b) CHECK((a) <= (b))
#define DCHECK_LE(a, b) CHECK_LE(a, b)
#define CHECK_LT(a, b) CHECK((a) < (b))
#define DCHECK_LT(a, b) CHECK_LT(a, b)
#define CHECK_GE(a, b) CHECK((a) >= (b))
#define DCHECK_GE(a, b) CHECK_GE(a, b)
#define CHECK_GT(a, b) CHECK((a) > (b))
#define DCHECK_GT(a, b) CHECK_GT(a, b)
#define CHECK_OK(status_expr) \
  do { \
    auto _st = (status_expr); \
    if (!_st.ok()) { \
      ::absl_shim_internal::FatalLogStream(__FILE__, __LINE__, #status_expr) << _st.message(); \
    } \
  } while (0)
