// Copyright 2026 Google LLC.
// Implementation of all external primitives (`%...` compiler built-ins and
// `caml_*` C runtime primitives) for the OCaml 5.3 bootstrap interpreter.

#include "prims.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "third_party/absl/container/flat_hash_map.h"
#include "third_party/absl/log/check.h"
#include "third_party/absl/log/log.h"
#include "third_party/absl/strings/ascii.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/absl/strings/string_view.h"
#include "third_party/absl/types/span.h"
#include "eval.h"
#include "intern.h"
#include "value.h"

extern "C" {
struct MD5Context {
  uint32_t buf[4];
  uint32_t bits[2];
  unsigned char in[64];
};
void caml_MD5Init(struct MD5Context* context);
void caml_MD5Update(struct MD5Context* context, const unsigned char* buf,
                    size_t len);
void caml_MD5Final(unsigned char digest[16], struct MD5Context* ctx);

struct BLAKE2_context {
  uint64_t h[8];
  uint64_t t[2];
  size_t buflen;
  unsigned char buf[128];
};
void caml_BLAKE2Init(struct BLAKE2_context* ctx, size_t hashlen, size_t keylen,
                     const unsigned char* key);
void caml_BLAKE2Update(struct BLAKE2_context* ctx, const unsigned char* addr,
                       size_t len);
void caml_BLAKE2Final(struct BLAKE2_context* ctx, size_t hashlen,
                      unsigned char* digest);
}

namespace ocaml_boot {
namespace {

// Sign-extends a 63-bit OCaml integer into a 64-bit signed integer so that
// 63-bit OCaml integer arithmetic (`max_int = (-1) lsr 1`, `Sys.int_size = 63`)
// matches the target 64-bit OCaml runtime (`:ocamlrun`) bit-for-bit.
inline int64_t SignExtend63(uint64_t u) {
  u &= 0x7FFFFFFFFFFFFFFFULL;
  if ((u & 0x4000000000000000ULL) != 0) {
    u |= 0x8000000000000000ULL;
  }
  return static_cast<int64_t>(u);
}

inline uint16_t Bswap16(uint16_t x) { return __builtin_bswap16(x); }
inline uint32_t Bswap32(uint32_t x) { return __builtin_bswap32(x); }
inline uint64_t Bswap64(uint64_t x) { return __builtin_bswap64(x); }

// Constructs `Some(v)`.
Value MakeSome(InternTable& intern, Value v) {
  return Value::Constructor(intern.Intern("Some"), 0, std::move(v));
}

// Constructs `None`.
Value MakeNone(InternTable& intern) {
  return Value::Constructor(intern.Intern("None"), 0, std::nullopt);
}

// Constructs an OCaml list from a span of Values.
Value MakeList(InternTable& intern, absl::Span<const Value> elems) {
  SymbolId nil_sym = intern.Intern("[]");
  SymbolId cons_sym = intern.Intern("::");
  Value acc = Value::Constructor(nil_sym);
  for (auto it = elems.rbegin(); it != elems.rend(); ++it) {
    acc = Value::Constructor(cons_sym, 0, Value::Tuple({*it, std::move(acc)}));
  }
  return acc;
}

// Extracts elements of an OCaml list into a C++ vector.
std::vector<Value> ExtractList(const Value& list) {
  std::vector<Value> out;
  const Value* cur = &list;
  while (cur->IsConstructor()) {
    const auto& c = cur->AsConstructor();
    if (!c->arg.has_value()) {
      break;
    }
    if (c->arg->IsTuple() && c->arg->AsTuple()->elements.size() >= 2) {
      out.push_back(c->arg->AsTuple()->elements[0]);
      cur = &c->arg->AsTuple()->elements[1];
    } else {
      break;
    }
  }
  return out;
}

// Extracts or mutates the single mutable field of a `ref` or `Atomic.t`.
Value GetRefField0(const Value& v) {
  if (v.IsRecord()) {
    const auto& f = v.AsRecord()->fields;
    CHECK(!f.empty());
    return f[0].second;
  }
  if (v.IsTuple()) {
    const auto& e = v.AsTuple()->elements;
    CHECK(!e.empty());
    return e[0];
  }
  if (v.IsArray()) {
    const auto& e = v.AsArray()->elements;
    CHECK(!e.empty());
    return e[0];
  }
  if (v.IsRawBlock()) {
    const auto& f = v.AsRawBlock()->fields;
    CHECK(!f.empty());
    return f[0];
  }
  if (v.IsConstructor() && v.AsConstructor()->arg.has_value()) {
    const Value& arg = *v.AsConstructor()->arg;
    if (arg.IsTuple() && !arg.AsTuple()->elements.empty()) {
      return arg.AsTuple()->elements[0];
    }
    return arg;
  }
  return Value::Unit();
}

Value GetField1(const Value& v) {
  if (v.IsRecord()) {
    const auto& f = v.AsRecord()->fields;
    CHECK_GE(f.size(), 2u);
    return f[1].second;
  }
  if (v.IsTuple()) {
    const auto& e = v.AsTuple()->elements;
    CHECK_GE(e.size(), 2u);
    return e[1];
  }
  if (v.IsArray()) {
    const auto& e = v.AsArray()->elements;
    CHECK_GE(e.size(), 2u);
    return e[1];
  }
  if (v.IsRawBlock()) {
    const auto& f = v.AsRawBlock()->fields;
    CHECK_GE(f.size(), 2u);
    return f[1];
  }
  if (v.IsDummyObject()) {
    return Value::Int(v.AsDummyObjectId());
  }
  if (v.IsConstructor()) {
    const auto& c = v.AsConstructor();
    if (c->exn_stamp != 0) {
      return Value::Int(c->exn_stamp);
    }
    if (c->arg.has_value() && c->arg->IsTuple() &&
        c->arg->AsTuple()->elements.size() >= 2) {
      return c->arg->AsTuple()->elements[1];
    }
  }
  return Value::Int(0);
}

void SetRefField0(const Value& v, Value new_val) {
  if (v.IsRecord()) {
    auto& f = v.AsRecord()->fields;
    CHECK(!f.empty());
    f[0].second = std::move(new_val);
    return;
  }
  if (v.IsTuple()) {
    auto& e = v.AsTuple()->elements;
    CHECK(!e.empty());
    e[0] = std::move(new_val);
    return;
  }
  if (v.IsArray()) {
    auto& e = v.AsArray()->elements;
    CHECK(!e.empty());
    e[0] = std::move(new_val);
    return;
  }
  if (v.IsRawBlock()) {
    auto& f = v.AsRawBlock()->fields;
    CHECK(!f.empty());
    f[0] = std::move(new_val);
    return;
  }
  LOG(FATAL) << "SetRefField0 on unsupported value kind.";
}

// Generic `Obj.size`, `Obj.field`, `Obj.set_field`, `Obj.tag` helpers.
int64_t ObjSize(const Value& v) {
  switch (v.kind()) {
    case Value::Kind::kTuple:
      return static_cast<int64_t>(v.AsTuple()->elements.size());
    case Value::Kind::kRecord:
      return static_cast<int64_t>(v.AsRecord()->fields.size());
    case Value::Kind::kArray:
      return static_cast<int64_t>(v.AsArray()->elements.size());
    case Value::Kind::kFloatArray:
      return static_cast<int64_t>(v.AsFloatArray()->elements.size());
    case Value::Kind::kRawBlock:
      return static_cast<int64_t>(v.AsRawBlock()->fields.size());
    case Value::Kind::kConstructor: {
      const auto& c = v.AsConstructor();
      int64_t base = (c->exn_stamp != 0) ? 2 : 0;
      if (!c->arg.has_value()) {
        return base;
      }
      if (c->arg->IsTuple()) {
        return base + static_cast<int64_t>(c->arg->AsTuple()->elements.size());
      }
      return base + 1;
    }
    case Value::Kind::kString:
      return static_cast<int64_t>((v.AsString()->data.size() + 8) / 8);
    case Value::Kind::kLazy:
      return 1;
    default:
      return 0;
  }
}

int ObjTag(const Value& v, const InternTable& intern) {
  if (v.IsImmediate()) {
    return 1000;  // Obj.int_tag.
  }
  switch (v.kind()) {
    case Value::Kind::kString:
      return 252;  // Obj.string_tag.
    case Value::Kind::kFloat:
      return 253;  // Obj.double_tag.
    case Value::Kind::kFloatArray:
      return 254;  // Obj.double_array_tag.
    case Value::Kind::kInt32:
    case Value::Kind::kInt64:
    case Value::Kind::kNativeint:
    case Value::Kind::kBigarray1:
    case Value::Kind::kChannel:
      return 255;  // Obj.custom_tag.
    case Value::Kind::kClosure:
    case Value::Kind::kPrimitive:
      return 247;  // Obj.closure_tag.
    case Value::Kind::kDummyObject:
      return 248;  // Obj.object_tag.
    case Value::Kind::kLazy: {
      const auto& lz = v.AsLazy();
      if (lz->forced) return 250;      // Obj.forward_tag.
      if (lz->evaluating) return 244;  // Obj.forcing_tag.
      return 246;                      // Obj.lazy_tag.
    }
    case Value::Kind::kRawBlock:
      return v.AsRawBlock()->tag;
    case Value::Kind::kRecord:
      return v.AsRecord()->tag;
    case Value::Kind::kConstructor: {
      const auto& c = v.AsConstructor();
      if (c->exn_stamp != 0) {
        return 0;
      }
      if (c->tag != 0) {
        return c->tag;
      }
      // Map well-known ocamlyacc block tokens for `lex/parser.mly`.
      absl::string_view name = intern.Resolve(c->ctor);
      if (name == "Tident") return 0;
      if (name == "Tchar") return 1;
      if (name == "Tstring") return 2;
      if (name == "Taction") return 3;
      return 0;
    }
    default:
      return 0;
  }
}

Value ObjField(const Value& v, int64_t idx, InternTable& intern) {
  switch (v.kind()) {
    case Value::Kind::kTuple: {
      const auto& e = v.AsTuple()->elements;
      CHECK_GE(idx, 0);
      CHECK_LT(static_cast<size_t>(idx), e.size());
      return e[idx];
    }
    case Value::Kind::kRecord: {
      const auto& f = v.AsRecord()->fields;
      CHECK_GE(idx, 0);
      CHECK_LT(static_cast<size_t>(idx), f.size());
      return f[idx].second;
    }
    case Value::Kind::kArray: {
      const auto& e = v.AsArray()->elements;
      CHECK_GE(idx, 0);
      CHECK_LT(static_cast<size_t>(idx), e.size());
      return e[idx];
    }
    case Value::Kind::kFloatArray: {
      const auto& e = v.AsFloatArray()->elements;
      CHECK_GE(idx, 0);
      CHECK_LT(static_cast<size_t>(idx), e.size());
      return Value::Float(e[idx]);
    }
    case Value::Kind::kRawBlock: {
      const auto& f = v.AsRawBlock()->fields;
      CHECK_GE(idx, 0);
      CHECK_LT(static_cast<size_t>(idx), f.size());
      return f[idx];
    }
    case Value::Kind::kConstructor: {
      const auto& c = v.AsConstructor();
      if (c->exn_stamp != 0) {
        if (idx == 0) {
          // Extension slot: block of [name, id].
          return Value::RawBlock(
              248, {Value::String(intern.Resolve(c->ctor)),
                    Value::Int(c->exn_stamp)});
        }
        idx -= 1;
      }
      if (!c->arg.has_value()) {
        return Value::Unit();
      }
      if (c->arg->IsTuple()) {
        const auto& e = c->arg->AsTuple()->elements;
        if (idx >= 0 && static_cast<size_t>(idx) < e.size()) {
          return e[idx];
        }
      }
      if (idx == 0) {
        return *c->arg;
      }
      return Value::Unit();
    }
    case Value::Kind::kLazy: {
      return v.AsLazy()->value;
    }
    case Value::Kind::kModule: {
      return v;
    }
    default:
      return Value::Unit();
  }
}

void ObjSetField(const Value& v, int64_t idx, Value val) {
  switch (v.kind()) {
    case Value::Kind::kTuple: {
      auto& e = v.AsTuple()->elements;
      CHECK_GE(idx, 0);
      CHECK_LT(static_cast<size_t>(idx), e.size());
      e[idx] = std::move(val);
      return;
    }
    case Value::Kind::kRecord: {
      auto& f = v.AsRecord()->fields;
      CHECK_GE(idx, 0);
      CHECK_LT(static_cast<size_t>(idx), f.size());
      f[idx].second = std::move(val);
      return;
    }
    case Value::Kind::kArray: {
      auto& e = v.AsArray()->elements;
      CHECK_GE(idx, 0);
      CHECK_LT(static_cast<size_t>(idx), e.size());
      e[idx] = std::move(val);
      return;
    }
    case Value::Kind::kFloatArray: {
      auto& e = v.AsFloatArray()->elements;
      CHECK_GE(idx, 0);
      CHECK_LT(static_cast<size_t>(idx), e.size());
      e[idx] = val.AsFloat();
      return;
    }
    case Value::Kind::kRawBlock: {
      auto& f = v.AsRawBlock()->fields;
      CHECK_GE(idx, 0);
      CHECK_LT(static_cast<size_t>(idx), f.size());
      f[idx] = std::move(val);
      return;
    }
    case Value::Kind::kConstructor: {
      auto c = v.AsConstructor();
      if (!c->arg.has_value()) {
        c->arg = std::move(val);
      } else if (c->arg->IsTuple() && idx >= 0 &&
                 static_cast<size_t>(idx) < c->arg->AsTuple()->elements.size()) {
        c->arg->AsTuple()->elements[idx] = std::move(val);
      } else if (idx == 0) {
        c->arg = std::move(val);
      }
      return;
    }
    case Value::Kind::kLazy: {
      auto lz = v.AsLazy();
      lz->value = std::move(val);
      lz->forced = true;
      return;
    }
    default:
      break;
  }
}

// Parses an OCaml integer literal string (`0x...`, `0o...`, `0b...`, `0u...`,
// with optional `_` separators and leading sign), throwing `Failure` on error.
int64_t ParseOcamlIntString(absl::string_view s, int nbits,
                            absl::string_view err_msg, InternTable& intern) {
  size_t i = 0;
  while (i < s.size() && absl::ascii_isspace(s[i])) {
    ++i;
  }
  if (i == s.size()) {
    throw OcamlException(MakeExnFailure(intern, err_msg));
  }
  int sign = 1;
  if (s[i] == '+') {
    ++i;
  } else if (s[i] == '-') {
    sign = -1;
    ++i;
  }
  int base = 10;
  bool is_unsigned = false;
  if (i + 2 <= s.size() && s[i] == '0') {
    char c = s[i + 1];
    if (c == 'x' || c == 'X') {
      base = 16;
      i += 2;
    } else if (c == 'o' || c == 'O') {
      base = 8;
      i += 2;
    } else if (c == 'b' || c == 'B') {
      base = 2;
      i += 2;
    } else if (c == 'u' || c == 'U') {
      is_unsigned = true;
      i += 2;
    }
  }
  auto digit_val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  if (i == s.size()) {
    throw OcamlException(MakeExnFailure(intern, err_msg));
  }
  int first_d = digit_val(s[i]);
  if (first_d < 0 || first_d >= base) {
    throw OcamlException(MakeExnFailure(intern, err_msg));
  }
  uint64_t res = static_cast<uint64_t>(first_d);
  uint64_t threshold = UINT64_MAX / static_cast<uint64_t>(base);
  for (++i; i < s.size(); ++i) {
    if (s[i] == '_') continue;
    int d = digit_val(s[i]);
    if (d < 0 || d >= base) {
      throw OcamlException(MakeExnFailure(intern, err_msg));
    }
    if (res > threshold) {
      throw OcamlException(MakeExnFailure(intern, err_msg));
    }
    res = res * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
    if (res < static_cast<uint64_t>(d)) {
      throw OcamlException(MakeExnFailure(intern, err_msg));
    }
  }
  if (base == 10 && !is_unsigned) {
    uint64_t max_pos = (nbits == 64) ? 0x7FFFFFFFFFFFFFFFULL
                                     : ((1ULL << (nbits - 1)) - 1ULL);
    uint64_t max_neg = max_pos + 1ULL;
    if ((sign > 0 && res > max_pos) || (sign < 0 && res > max_neg)) {
      throw OcamlException(MakeExnFailure(intern, err_msg));
    }
  } else {
    if (nbits < 64 && res >= (1ULL << nbits)) {
      throw OcamlException(MakeExnFailure(intern, err_msg));
    }
  }
  uint64_t signed_res = (sign < 0) ? (0ULL - res) : res;
  if (nbits == 32) {
    return static_cast<int32_t>(static_cast<uint32_t>(signed_res));
  }
  if (nbits == 63) {
    return SignExtend63(signed_res);
  }
  return static_cast<int64_t>(signed_res);
}

// Parses an OCaml float literal string (allowing `_` separators and hex floats).
double ParseOcamlFloatString(absl::string_view s, InternTable& intern) {
  std::string cleaned;
  cleaned.reserve(s.size());
  for (char c : s) {
    if (c != '_') {
      cleaned.push_back(c);
    }
  }
  if (cleaned.empty() || absl::ascii_isspace(cleaned[0])) {
    throw OcamlException(MakeExnFailure(intern, "float_of_string"));
  }
  if (cleaned == "nan" || cleaned == "+nan" || cleaned == "-nan" ||
      cleaned == "NaN") {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (cleaned == "inf" || cleaned == "+inf" || cleaned == "infinity" ||
      cleaned == "+infinity") {
    return std::numeric_limits<double>::infinity();
  }
  if (cleaned == "-inf" || cleaned == "-infinity") {
    return -std::numeric_limits<double>::infinity();
  }
  char* endptr = nullptr;
  errno = 0;
  double d = std::strtod(cleaned.c_str(), &endptr);
  if (endptr != cleaned.c_str() + cleaned.size()) {
    throw OcamlException(MakeExnFailure(intern, "float_of_string"));
  }
  return d;
}

// Normalizes a printf integer format string (stripping `l`/`n`/`L` length
// modifiers and replacing them with `ll`) and formats `val`.
std::string FormatIntWithPrintf(absl::string_view fmt, int64_t val,
                                int nbits) {
  if (fmt.empty()) return "";
  char conv = fmt.back();
  std::string prefix(fmt.substr(0, fmt.size() - 1));
  if (!prefix.empty() &&
      (prefix.back() == 'l' || prefix.back() == 'n' || prefix.back() == 'L')) {
    prefix.pop_back();
  }
  std::string c_fmt = absl::StrCat(prefix, "ll", std::string(1, conv));
  char buf[128];
  if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
    uint64_t u = static_cast<uint64_t>(val);
    if (nbits == 32) {
      u &= 0xFFFFFFFFULL;
    } else if (nbits == 63) {
      u &= 0x7FFFFFFFFFFFFFFFULL;
    }
    std::snprintf(buf, sizeof(buf), c_fmt.c_str(),
                  static_cast<unsigned long long>(u));
  } else {
    std::snprintf(buf, sizeof(buf), c_fmt.c_str(),
                  static_cast<long long>(val));
  }
  return std::string(buf);
}

// Formats a float according to `caml_hexstring_of_float` in `runtime/floats.c`.
std::string FormatHexFloat(double d, int64_t prec, char style) {
  uint64_t bits = 0;
  std::memcpy(&bits, &d, 8);
  int sign = (bits >> 63) != 0;
  int exp = static_cast<int>((bits >> 52) & 0x7FF);
  uint64_t m = bits & ((1ULL << 52) - 1ULL);

  std::string out;
  if (sign) {
    out.push_back('-');
  } else if (style != '-') {
    out.push_back(style);
  }
  if (exp == 0x7FF) {
    out.append(m == 0 ? "infinity" : "nan");
    return out;
  }
  int unit = (exp == 0) ? 0 : 1;
  int shift = (exp == 0 && m == 0) ? 0 : (exp - 1023 + (1 - unit));
  if (prec < 0) {
    out.append(absl::StrFormat("0x%d", unit));
    if (m != 0) {
      out.push_back('.');
      while (m != 0) {
        out.append(absl::StrFormat("%x", static_cast<int>((m >> 48) & 0xF)));
        m = (m << 4) & ((1ULL << 52) - 1ULL);
      }
    }
    out.append(absl::StrFormat("p%+d", shift));
  } else {
    if (prec < 13) {
      uint64_t d_half = 1ULL << (48 - prec * 4);
      uint64_t mask = 2 * d_half - 1;
      uint64_t frac = m & mask;
      m &= ~mask;
      if (frac > d_half || (frac == d_half && ((m & (2 * d_half)) != 0 ||
                                               (m == 0 && (unit & 1) != 0)))) {
        m += 2 * d_half;
        if ((m >> 52) != 0) {
          unit += 1;
          m = 0;
        }
      }
    }
    out.append(absl::StrFormat("0x%d", unit));
    if (prec > 0) {
      out.push_back('.');
      for (int64_t i = 0; i < prec; ++i) {
        if (i < 13) {
          out.append(absl::StrFormat("%x", static_cast<int>((m >> 48) & 0xF)));
          m = (m << 4) & ((1ULL << 52) - 1ULL);
        } else {
          out.push_back('0');
        }
      }
    }
    out.append(absl::StrFormat("p%+d", shift));
  }
  return out;
}

// Lexer & Parser engine helpers (`runtime/lexing.c` and `runtime/parsing.c`).
inline int16_t ReadLeShort(const std::string& tbl, int64_t n) {
  size_t idx = static_cast<size_t>(n) * 2;
  DCHECK_LE(idx + 2, tbl.size());
  uint8_t lo = static_cast<uint8_t>(tbl[idx]);
  int8_t hi = static_cast<int8_t>(tbl[idx + 1]);
  return static_cast<int16_t>(lo + (static_cast<int16_t>(hi) << 8));
}

inline int32_t ReadLeUShort(const std::string& tbl, int64_t n) {
  size_t idx = static_cast<size_t>(n) * 2;
  DCHECK_LE(idx + 2, tbl.size());
  uint8_t lo = static_cast<uint8_t>(tbl[idx]);
  uint8_t hi = static_cast<uint8_t>(tbl[idx + 1]);
  return static_cast<int32_t>(lo) + (static_cast<int32_t>(hi) << 8);
}

void RunLexMem(const std::string& code, size_t pc, std::vector<Value>& mem,
               int64_t curr_pos) {
  while (pc < code.size()) {
    uint8_t dst = static_cast<uint8_t>(code[pc++]);
    if (dst == 0xFF) return;
    uint8_t src = static_cast<uint8_t>(code[pc++]);
    if (src == 0xFF) {
      mem[dst] = Value::Int(curr_pos);
    } else {
      mem[dst] = mem[src];
    }
  }
}

void RunLexTag(const std::string& code, size_t pc, std::vector<Value>& mem) {
  while (pc < code.size()) {
    uint8_t dst = static_cast<uint8_t>(code[pc++]);
    if (dst == 0xFF) return;
    uint8_t src = static_cast<uint8_t>(code[pc++]);
    if (src == 0xFF) {
      mem[dst] = Value::Int(-1);
    } else {
      mem[dst] = mem[src];
    }
  }
}

// Bigarray element size helper matching `stdlib/bigarray.ml`.
size_t BigarrayElemSize(int kind) {
  switch (kind) {
    case 0:   // Float32.
    case 6:   // Int32.
      return 4;
    case 1:   // Float64.
    case 7:   // Int64.
    case 8:   // Int.
    case 9:   // Nativeint.
    case 10:  // Complex32.
      return 8;
    case 2:   // Int8_signed.
    case 3:   // Int8_unsigned.
    case 12:  // Char.
      return 1;
    case 4:   // Int16_signed.
    case 5:   // Int16_unsigned.
    case 13:  // Float16.
      return 2;
    case 11:  // Complex64.
      return 16;
    default:
      return 1;
  }
}

int BigarrayKindFromValue(const Value& v, const InternTable& intern) {
  if (v.IsIntLike()) {
    return static_cast<int>(v.AsInt());
  }
  if (v.IsConstructor()) {
    absl::string_view name = intern.Resolve(v.AsConstructor()->ctor);
    if (name == "Float32") return 0;
    if (name == "Float64") return 1;
    if (name == "Int8_signed") return 2;
    if (name == "Int8_unsigned") return 3;
    if (name == "Int16_signed") return 4;
    if (name == "Int16_unsigned") return 5;
    if (name == "Int32") return 6;
    if (name == "Int64") return 7;
    if (name == "Int") return 8;
    if (name == "Nativeint") return 9;
    if (name == "Complex32") return 10;
    if (name == "Complex64") return 11;
    if (name == "Char") return 12;
    if (name == "Float16") return 13;
  }
  return 12;
}

Value BigarrayKindToValue(int kind, InternTable& intern) {
  constexpr absl::string_view kNames[] = {
      "Float32", "Float64",     "Int8_signed",  "Int8_unsigned",
      "Int16_signed", "Int16_unsigned", "Int32", "Int64",
      "Int",     "Nativeint",   "Complex32",    "Complex64",
      "Char",    "Float16",
  };
  if (kind >= 0 && kind < 14) {
    return Value::Constructor(intern.Intern(kNames[kind]));
  }
  return Value::Constructor(intern.Intern("Char"));
}

}  // namespace

void RegisterPrimitives(Evaluator& eval) {
  InternTable& intern = eval.intern();
  SymbolId sym_contents = intern.Intern("contents");

  auto reg = [&](absl::string_view name, int arity,
                 std::function<Value(absl::Span<const Value>)> fn) {
    eval.RegisterPrimitive(name, arity, std::move(fn));
  };

  // Tracked `Sys.argv` array cell that stays synchronized with `eval.argv()`.
  Value sys_argv_val = Value::Array({});
  std::shared_ptr<ArrayVal> sys_argv_arr = sys_argv_val.AsArray();
  auto sync_sys_argv = [&eval, sys_argv_arr]() {
    const auto& argv = eval.argv();
    bool needs_update = (sys_argv_arr->elements.size() != argv.size());
    if (!needs_update) {
      for (size_t i = 0; i < argv.size(); ++i) {
        if (!sys_argv_arr->elements[i].IsString() ||
            sys_argv_arr->elements[i].AsString()->data != argv[i]) {
          needs_update = true;
          break;
        }
      }
    }
    if (needs_update) {
      sys_argv_arr->elements.clear();
      sys_argv_arr->elements.reserve(argv.size());
      for (const std::string& s : argv) {
        sys_argv_arr->elements.push_back(Value::String(s));
      }
    }
  };
  sync_sys_argv();

  auto maybe_sync_argv = [sys_argv_arr, sync_sys_argv](const Value& v) {
    if (v.IsArray() && v.AsArray() == sys_argv_arr) {
      sync_sys_argv();
    }
  };

  // =========================================================================
  // 1. Identity, Control, Exceptions, References, and Tuples
  // =========================================================================

  reg("%identity", 1, [](absl::Span<const Value> a) { return a[0]; });
  reg("%opaque", 1, [](absl::Span<const Value> a) { return a[0]; });
  reg("%ignore", 1, [](absl::Span<const Value>) { return Value::Unit(); });
  reg("%poll", 1, [](absl::Span<const Value>) { return Value::Unit(); });

  reg("%revapply", 2, [&eval](absl::Span<const Value> a) {
    return eval.ApplyValues(a[1], a.subspan(0, 1));
  });
  reg("%apply", 2, [&eval](absl::Span<const Value> a) {
    return eval.ApplyValues(a[0], a.subspan(1, 1));
  });

  auto do_raise = [](const Value& exn, bool is_reraise) -> Value {
    if (exn.IsConstructor()) {
      exn.AsConstructor()->is_raised_exn = true;
    }
    throw OcamlException(exn, is_reraise);
  };
  reg("%raise", 1, [do_raise](absl::Span<const Value> a) -> Value {
    return do_raise(a[0], /*is_reraise=*/false);
  });
  reg("%raise_notrace", 1, [do_raise](absl::Span<const Value> a) -> Value {
    return do_raise(a[0], /*is_reraise=*/false);
  });
  reg("%reraise", 1, [do_raise](absl::Span<const Value> a) -> Value {
    return do_raise(a[0], /*is_reraise=*/true);
  });
  reg("%raise_with_backtrace", 2, [do_raise](absl::Span<const Value> a) -> Value {
    return do_raise(a[0], /*is_reraise=*/false);
  });
  reg("caml_raise_exn", 1, [do_raise](absl::Span<const Value> a) -> Value {
    return do_raise(a[0], /*is_reraise=*/false);
  });

  reg("%makemutable", 1, [sym_contents](absl::Span<const Value> a) {
    return Value::Ref(a[0], sym_contents);
  });
  reg("%field0", 1, [](absl::Span<const Value> a) {
    return GetRefField0(a[0]);
  });
  reg("%field1", 1, [](absl::Span<const Value> a) {
    return GetField1(a[0]);
  });
  reg("%setfield0", 2, [](absl::Span<const Value> a) {
    SetRefField0(a[0], a[1]);
    return Value::Unit();
  });
  reg("%setfield1", 2, [](absl::Span<const Value> a) {
    ObjSetField(a[0], 1, a[1]);
    return Value::Unit();
  });
  reg("%makeblock", 1, [](absl::Span<const Value> a) {
    return Value::Tuple({a[0]});
  });
  reg("%incr", 1, [](absl::Span<const Value> a) {
    int64_t cur = GetRefField0(a[0]).AsInt();
    SetRefField0(
        a[0], Value::Int(SignExtend63(static_cast<uint64_t>(cur) + 1ULL)));
    return Value::Unit();
  });
  reg("%decr", 1, [](absl::Span<const Value> a) {
    int64_t cur = GetRefField0(a[0]).AsInt();
    SetRefField0(
        a[0], Value::Int(SignExtend63(static_cast<uint64_t>(cur) - 1ULL)));
    return Value::Unit();
  });
  reg("%offsetint", 2, [](absl::Span<const Value> a) {
    return Value::Int(SignExtend63(static_cast<uint64_t>(a[0].AsInt()) +
                                   static_cast<uint64_t>(a[1].AsInt())));
  });
  reg("%offsetref", 2, [](absl::Span<const Value> a) {
    int64_t cur = GetRefField0(a[0]).AsInt();
    int64_t delta = a[1].AsInt();
    SetRefField0(a[0], Value::Int(SignExtend63(static_cast<uint64_t>(cur) +
                                               static_cast<uint64_t>(delta))));
    return Value::Unit();
  });
  reg("%loc_LOC", 0, [](absl::Span<const Value>) {
    return Value::String("");
  });
  reg("%loc_FILE", 0, [](absl::Span<const Value>) {
    return Value::String("");
  });
  reg("%loc_LINE", 0, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("%loc_MODULE", 0, [](absl::Span<const Value>) {
    return Value::String("");
  });
  reg("%loc_POS", 0, [](absl::Span<const Value>) {
    return Value::Tuple(
        {Value::String(""), Value::Int(0), Value::Int(0), Value::Int(0)});
  });
  reg("%loc_FUNCTION", 0, [](absl::Span<const Value>) {
    return Value::String("");
  });

  // =========================================================================
  // 2. Booleans and 63-bit OCaml Integers
  // =========================================================================

  reg("%boolnot", 1, [](absl::Span<const Value> a) {
    return Value::Bool(!a[0].AsBool());
  });
  reg("%sequand", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsBool() && a[1].AsBool());
  });
  reg("%sequor", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsBool() || a[1].AsBool());
  });

  reg("%negint", 1, [](absl::Span<const Value> a) {
    return Value::Int(SignExtend63(0ULL - static_cast<uint64_t>(a[0].AsInt())));
  });
  reg("%succint", 1, [](absl::Span<const Value> a) {
    return Value::Int(SignExtend63(static_cast<uint64_t>(a[0].AsInt()) + 1ULL));
  });
  reg("%predint", 1, [](absl::Span<const Value> a) {
    return Value::Int(SignExtend63(static_cast<uint64_t>(a[0].AsInt()) - 1ULL));
  });
  reg("%addint", 2, [](absl::Span<const Value> a) {
    return Value::Int(SignExtend63(static_cast<uint64_t>(a[0].AsInt()) +
                                   static_cast<uint64_t>(a[1].AsInt())));
  });
  reg("%subint", 2, [](absl::Span<const Value> a) {
    return Value::Int(SignExtend63(static_cast<uint64_t>(a[0].AsInt()) -
                                   static_cast<uint64_t>(a[1].AsInt())));
  });
  reg("%mulint", 2, [](absl::Span<const Value> a) {
    return Value::Int(SignExtend63(static_cast<uint64_t>(a[0].AsInt()) *
                                   static_cast<uint64_t>(a[1].AsInt())));
  });
  reg("%divint", 2, [&intern](absl::Span<const Value> a) {
    int64_t x = a[0].AsInt();
    int64_t y = a[1].AsInt();
    if (y == 0) {
      throw OcamlException(MakeExnDivisionByZero(intern));
    }
    if (x == INT64_MIN && y == -1) {
      return Value::Int(SignExtend63(static_cast<uint64_t>(x)));
    }
    return Value::Int(SignExtend63(static_cast<uint64_t>(x / y)));
  });
  reg("%modint", 2, [&intern](absl::Span<const Value> a) {
    int64_t x = a[0].AsInt();
    int64_t y = a[1].AsInt();
    if (y == 0) {
      throw OcamlException(MakeExnDivisionByZero(intern));
    }
    if (x == INT64_MIN && y == -1) {
      return Value::Int(0);
    }
    return Value::Int(SignExtend63(static_cast<uint64_t>(x % y)));
  });
  reg("%andint", 2, [](absl::Span<const Value> a) {
    return Value::Int(a[0].AsInt() & a[1].AsInt());
  });
  reg("%orint", 2, [](absl::Span<const Value> a) {
    return Value::Int(a[0].AsInt() | a[1].AsInt());
  });
  reg("%xorint", 2, [](absl::Span<const Value> a) {
    return Value::Int(a[0].AsInt() ^ a[1].AsInt());
  });
  reg("%lslint", 2, [](absl::Span<const Value> a) {
    uint64_t x = static_cast<uint64_t>(a[0].AsInt());
    int shift = static_cast<int>(a[1].AsInt()) & 63;
    return Value::Int(SignExtend63(x << shift));
  });
  reg("%lsrint", 2, [](absl::Span<const Value> a) {
    uint64_t x = static_cast<uint64_t>(a[0].AsInt()) & 0x7FFFFFFFFFFFFFFFULL;
    int shift = static_cast<int>(a[1].AsInt()) & 63;
    return Value::Int(SignExtend63(x >> shift));
  });
  reg("%asrint", 2, [](absl::Span<const Value> a) {
    int64_t x = a[0].AsInt();
    int shift = static_cast<int>(a[1].AsInt()) & 63;
    return Value::Int(SignExtend63(static_cast<uint64_t>(x >> shift)));
  });

  // =========================================================================
  // 3. Comparisons (Physical, Integer, Structural)
  // =========================================================================

  auto phys_eq = [](const Value& x, const Value& y) -> bool {
    if (x.IsIntLike() && y.IsIntLike()) {
      return x.AsInt() == y.AsInt();
    }
    if (x.kind() != y.kind()) {
      return false;
    }
    switch (x.kind()) {
      case Value::Kind::kInt32:
        return x.AsInt32() == y.AsInt32();
      case Value::Kind::kInt64:
      case Value::Kind::kNativeint:
        return x.AsInt() == y.AsInt();
      case Value::Kind::kFloat: {
        uint64_t bx = 0, by = 0;
        double dx = x.AsFloat(), dy = y.AsFloat();
        std::memcpy(&bx, &dx, 8);
        std::memcpy(&by, &dy, 8);
        return bx == by;
      }
      case Value::Kind::kConstructor: {
        const auto& cx = x.AsConstructor();
        const auto& cy = y.AsConstructor();
        if (!cx->arg.has_value() && !cy->arg.has_value() &&
            cx->exn_stamp == 0 && cy->exn_stamp == 0) {
          return cx->ctor == cy->ctor;
        }
        return x.RawPtr() == y.RawPtr();
      }
      case Value::Kind::kDummyObject:
        return x.AsDummyObjectId() == y.AsDummyObjectId();
      default:
        return x.RawPtr() == y.RawPtr();
    }
  };

  reg("%eq", 2, [phys_eq](absl::Span<const Value> a) {
    return Value::Bool(phys_eq(a[0], a[1]));
  });
  reg("%noteq", 2, [phys_eq](absl::Span<const Value> a) {
    return Value::Bool(!phys_eq(a[0], a[1]));
  });
  reg("%ltint", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsInt() < a[1].AsInt());
  });
  reg("%leint", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsInt() <= a[1].AsInt());
  });
  reg("%gtint", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsInt() > a[1].AsInt());
  });
  reg("%geint", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsInt() >= a[1].AsInt());
  });
  reg("%ultint", 2, [](absl::Span<const Value> a) {
    uint64_t x = static_cast<uint64_t>(a[0].AsInt()) & 0x7FFFFFFFFFFFFFFFULL;
    uint64_t y = static_cast<uint64_t>(a[1].AsInt()) & 0x7FFFFFFFFFFFFFFFULL;
    return Value::Bool(x < y);
  });
  reg("%ugeint", 2, [](absl::Span<const Value> a) {
    uint64_t x = static_cast<uint64_t>(a[0].AsInt()) & 0x7FFFFFFFFFFFFFFFULL;
    uint64_t y = static_cast<uint64_t>(a[1].AsInt()) & 0x7FFFFFFFFFFFFFFFULL;
    return Value::Bool(x >= y);
  });

  auto cmp_total = [&intern](absl::Span<const Value> a) {
    return Value::Int(CompareValues(a[0], a[1], /*total=*/true, &intern));
  };
  reg("%compare", 2, cmp_total);
  reg("caml_compare", 2, cmp_total);
  reg("caml_int_compare", 2, [](absl::Span<const Value> a) {
    int64_t x = a[0].AsInt(), y = a[1].AsInt();
    return Value::Int((x > y) - (x < y));
  });

  auto eq_fn = [&intern](absl::Span<const Value> a) {
    return Value::Bool(EqualValues(a[0], a[1], &intern));
  };
  auto neq_fn = [&intern](absl::Span<const Value> a) {
    return Value::Bool(!EqualValues(a[0], a[1], &intern));
  };
  auto lt_fn = [&intern](absl::Span<const Value> a) {
    int64_t c = CompareValues(a[0], a[1], /*total=*/false, &intern);
    return Value::Bool(c != INT64_MIN && c < 0);
  };
  auto le_fn = [&intern](absl::Span<const Value> a) {
    int64_t c = CompareValues(a[0], a[1], /*total=*/false, &intern);
    return Value::Bool(c != INT64_MIN && c <= 0);
  };
  auto gt_fn = [&intern](absl::Span<const Value> a) {
    int64_t c = CompareValues(a[0], a[1], /*total=*/false, &intern);
    return Value::Bool(c != INT64_MIN && c > 0);
  };
  auto ge_fn = [&intern](absl::Span<const Value> a) {
    int64_t c = CompareValues(a[0], a[1], /*total=*/false, &intern);
    return Value::Bool(c != INT64_MIN && c >= 0);
  };

  reg("%equal", 2, eq_fn);
  reg("caml_equal", 2, eq_fn);
  reg("%notequal", 2, neq_fn);
  reg("caml_notequal", 2, neq_fn);
  reg("%lessthan", 2, lt_fn);
  reg("caml_lessthan", 2, lt_fn);
  reg("%lessequal", 2, le_fn);
  reg("caml_lessequal", 2, le_fn);
  reg("%greaterthan", 2, gt_fn);
  reg("caml_greaterthan", 2, gt_fn);
  reg("%greaterequal", 2, ge_fn);
  reg("caml_greaterequal", 2, ge_fn);

  // =========================================================================
  // 4. Floating-Point Built-ins and Math Runtime Primitives
  // =========================================================================

  reg("%intoffloat", 1, [](absl::Span<const Value> a) {
    return Value::Int(
        SignExtend63(static_cast<uint64_t>(static_cast<int64_t>(a[0].AsFloat()))));
  });
  reg("%int_of_float", 1, [](absl::Span<const Value> a) {
    return Value::Int(
        SignExtend63(static_cast<uint64_t>(static_cast<int64_t>(a[0].AsFloat()))));
  });
  reg("%floatofint", 1, [](absl::Span<const Value> a) {
    return Value::Float(static_cast<double>(a[0].AsInt()));
  });
  reg("%float_of_int", 1, [](absl::Span<const Value> a) {
    return Value::Float(static_cast<double>(a[0].AsInt()));
  });
  reg("%negfloat", 1, [](absl::Span<const Value> a) {
    return Value::Float(-a[0].AsFloat());
  });
  reg("%absfloat", 1, [](absl::Span<const Value> a) {
    return Value::Float(std::fabs(a[0].AsFloat()));
  });
  reg("%addfloat", 2, [](absl::Span<const Value> a) {
    return Value::Float(a[0].AsFloat() + a[1].AsFloat());
  });
  reg("%subfloat", 2, [](absl::Span<const Value> a) {
    return Value::Float(a[0].AsFloat() - a[1].AsFloat());
  });
  reg("%mulfloat", 2, [](absl::Span<const Value> a) {
    return Value::Float(a[0].AsFloat() * a[1].AsFloat());
  });
  reg("%divfloat", 2, [](absl::Span<const Value> a) {
    return Value::Float(a[0].AsFloat() / a[1].AsFloat());
  });
  reg("%eqfloat", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsFloat() == a[1].AsFloat());
  });
  reg("%noteqfloat", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsFloat() != a[1].AsFloat());
  });
  reg("%ltfloat", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsFloat() < a[1].AsFloat());
  });
  reg("%lefloat", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsFloat() <= a[1].AsFloat());
  });
  reg("%gtfloat", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsFloat() > a[1].AsFloat());
  });
  reg("%gefloat", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsFloat() >= a[1].AsFloat());
  });

  auto reg_math1 = [&](absl::string_view n1, absl::string_view n2,
                       double (*fn)(double)) {
    auto cb = [fn](absl::Span<const Value> a) {
      return Value::Float(fn(a[0].AsFloat()));
    };
    reg(n1, 1, cb);
    if (!n2.empty()) reg(n2, 1, cb);
  };
  auto reg_math2 = [&](absl::string_view n1, absl::string_view n2,
                       double (*fn)(double, double)) {
    auto cb = [fn](absl::Span<const Value> a) {
      return Value::Float(fn(a[0].AsFloat(), a[1].AsFloat()));
    };
    reg(n1, 2, cb);
    if (!n2.empty()) reg(n2, 2, cb);
  };

  reg_math1("caml_sin_float", "sin", std::sin);
  reg_math1("caml_cos_float", "cos", std::cos);
  reg_math1("caml_tan_float", "tan", std::tan);
  reg_math1("caml_asin_float", "asin", std::asin);
  reg_math1("caml_acos_float", "acos", std::acos);
  reg_math1("caml_atan_float", "atan", std::atan);
  reg_math2("caml_atan2_float", "atan2", std::atan2);
  reg_math1("caml_sinh_float", "sinh", std::sinh);
  reg_math1("caml_cosh_float", "cosh", std::cosh);
  reg_math1("caml_tanh_float", "tanh", std::tanh);
  reg_math1("caml_asinh_float", "caml_asinh", std::asinh);
  reg_math1("caml_acosh_float", "caml_acosh", std::acosh);
  reg_math1("caml_atanh_float", "caml_atanh", std::atanh);
  reg_math1("caml_sqrt_float", "sqrt", std::sqrt);
  reg_math1("caml_cbrt_float", "caml_cbrt", std::cbrt);
  reg_math1("caml_exp_float", "exp", std::exp);
  reg_math1("caml_exp2_float", "caml_exp2", std::exp2);
  reg_math1("caml_expm1_float", "caml_expm1", std::expm1);
  reg_math1("caml_log_float", "log", std::log);
  reg_math1("caml_log10_float", "log10", std::log10);
  reg_math1("caml_log2_float", "caml_log2", std::log2);
  reg_math1("caml_log1p_float", "caml_log1p", std::log1p);
  reg_math1("caml_ceil_float", "ceil", std::ceil);
  reg_math1("caml_floor_float", "floor", std::floor);
  reg_math1("caml_trunc_float", "caml_trunc", std::trunc);
  reg_math1("caml_round_float", "caml_round", std::round);
  reg_math1("caml_erf_float", "caml_erf", std::erf);
  reg_math1("caml_erfc_float", "caml_erfc", std::erfc);
  reg_math2("caml_power_float", "pow", std::pow);
  reg_math2("caml_fmod_float", "fmod", std::fmod);
  reg_math2("caml_hypot_float", "caml_hypot", std::hypot);
  reg_math2("caml_copysign_float", "caml_copysign", std::copysign);
  reg_math2("caml_nextafter_float", "caml_nextafter", std::nextafter);

  auto fma_cb = [](absl::Span<const Value> a) {
    return Value::Float(
        std::fma(a[0].AsFloat(), a[1].AsFloat(), a[2].AsFloat()));
  };
  reg("caml_fma_float", 3, fma_cb);
  reg("caml_fma", 3, fma_cb);

  reg("caml_frexp_float", 1, [](absl::Span<const Value> a) {
    int exp = 0;
    double res = std::frexp(a[0].AsFloat(), &exp);
    return Value::Tuple({Value::Float(res), Value::Int(exp)});
  });
  auto ldexp_cb = [](absl::Span<const Value> a) {
    return Value::Float(
        std::ldexp(a[0].AsFloat(), static_cast<int>(a[1].AsInt())));
  };
  reg("caml_ldexp_float", 2, ldexp_cb);
  reg("caml_ldexp_float_unboxed", 2, ldexp_cb);

  reg("caml_modf_float", 1, [](absl::Span<const Value> a) {
    double ipart = 0.0;
    double fpart = std::modf(a[0].AsFloat(), &ipart);
    return Value::Tuple({Value::Float(fpart), Value::Float(ipart)});
  });
  reg("caml_classify_float", 1, [&intern](absl::Span<const Value> a) {
    double d = a[0].AsFloat();
    switch (std::fpclassify(d)) {
      case FP_NORMAL:
        return Value::Constructor(intern.Intern("FP_normal"));
      case FP_SUBNORMAL:
        return Value::Constructor(intern.Intern("FP_subnormal"));
      case FP_ZERO:
        return Value::Constructor(intern.Intern("FP_zero"));
      case FP_INFINITE:
        return Value::Constructor(intern.Intern("FP_infinite"));
      default:
        return Value::Constructor(intern.Intern("FP_nan"));
    }
  });
  reg("caml_classify_float_unboxed", 1, [&intern](absl::Span<const Value> a) {
    double d = a[0].AsFloat();
    switch (std::fpclassify(d)) {
      case FP_NORMAL:
        return Value::Constructor(intern.Intern("FP_normal"));
      case FP_SUBNORMAL:
        return Value::Constructor(intern.Intern("FP_subnormal"));
      case FP_ZERO:
        return Value::Constructor(intern.Intern("FP_zero"));
      case FP_INFINITE:
        return Value::Constructor(intern.Intern("FP_infinite"));
      default:
        return Value::Constructor(intern.Intern("FP_nan"));
    }
  });
  auto signbit_cb = [](absl::Span<const Value> a) {
    return Value::Bool(std::signbit(a[0].AsFloat()));
  };
  reg("caml_signbit_float", 1, signbit_cb);
  reg("caml_signbit", 1, signbit_cb);

  reg("caml_float_compare", 2, [](absl::Span<const Value> a) {
    double f = a[0].AsFloat();
    double g = a[1].AsFloat();
    int64_t res =
        static_cast<int64_t>(f > g) - static_cast<int64_t>(f < g) +
        static_cast<int64_t>(f == f) - static_cast<int64_t>(g == g);
    return Value::Int(res);
  });

  // =========================================================================
  // 5. Int32, Int64, Nativeint, and Number Formatting / Parsing
  // =========================================================================

  reg("%int32_neg", 1, [](absl::Span<const Value> a) {
    return Value::Int32(static_cast<int32_t>(
        0U - static_cast<uint32_t>(a[0].AsInt32())));
  });
  reg("%int32_add", 2, [](absl::Span<const Value> a) {
    return Value::Int32(static_cast<int32_t>(
        static_cast<uint32_t>(a[0].AsInt32()) +
        static_cast<uint32_t>(a[1].AsInt32())));
  });
  reg("%int32_sub", 2, [](absl::Span<const Value> a) {
    return Value::Int32(static_cast<int32_t>(
        static_cast<uint32_t>(a[0].AsInt32()) -
        static_cast<uint32_t>(a[1].AsInt32())));
  });
  reg("%int32_mul", 2, [](absl::Span<const Value> a) {
    return Value::Int32(static_cast<int32_t>(
        static_cast<uint32_t>(a[0].AsInt32()) *
        static_cast<uint32_t>(a[1].AsInt32())));
  });
  reg("%int32_div", 2, [&intern](absl::Span<const Value> a) {
    int32_t x = a[0].AsInt32(), y = a[1].AsInt32();
    if (y == 0) throw OcamlException(MakeExnDivisionByZero(intern));
    if (x == INT32_MIN && y == -1) return Value::Int32(INT32_MIN);
    return Value::Int32(x / y);
  });
  reg("%int32_mod", 2, [&intern](absl::Span<const Value> a) {
    int32_t x = a[0].AsInt32(), y = a[1].AsInt32();
    if (y == 0) throw OcamlException(MakeExnDivisionByZero(intern));
    if (x == INT32_MIN && y == -1) return Value::Int32(0);
    return Value::Int32(x % y);
  });
  reg("%int32_and", 2, [](absl::Span<const Value> a) {
    return Value::Int32(a[0].AsInt32() & a[1].AsInt32());
  });
  reg("%int32_or", 2, [](absl::Span<const Value> a) {
    return Value::Int32(a[0].AsInt32() | a[1].AsInt32());
  });
  reg("%int32_xor", 2, [](absl::Span<const Value> a) {
    return Value::Int32(a[0].AsInt32() ^ a[1].AsInt32());
  });
  reg("%int32_lsl", 2, [](absl::Span<const Value> a) {
    uint32_t x = static_cast<uint32_t>(a[0].AsInt32());
    int s = static_cast<int>(a[1].AsInt()) & 31;
    return Value::Int32(static_cast<int32_t>(x << s));
  });
  reg("%int32_lsr", 2, [](absl::Span<const Value> a) {
    uint32_t x = static_cast<uint32_t>(a[0].AsInt32());
    int s = static_cast<int>(a[1].AsInt()) & 31;
    return Value::Int32(static_cast<int32_t>(x >> s));
  });
  reg("%int32_asr", 2, [](absl::Span<const Value> a) {
    int32_t x = a[0].AsInt32();
    int s = static_cast<int>(a[1].AsInt()) & 31;
    return Value::Int32(x >> s);
  });
  reg("%int32_of_int", 1, [](absl::Span<const Value> a) {
    return Value::Int32(static_cast<int32_t>(a[0].AsInt()));
  });
  reg("%int32_to_int", 1, [](absl::Span<const Value> a) {
    return Value::Int(static_cast<int64_t>(a[0].AsInt32()));
  });
  auto i32_of_f = [](absl::Span<const Value> a) {
    return Value::Int32(static_cast<int32_t>(a[0].AsFloat()));
  };
  auto i32_to_f = [](absl::Span<const Value> a) {
    return Value::Float(static_cast<double>(a[0].AsInt32()));
  };
  auto i32_bits_of_f = [](absl::Span<const Value> a) {
    float f = static_cast<float>(a[0].AsFloat());
    int32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    return Value::Int32(bits);
  };
  auto i32_f_of_bits = [](absl::Span<const Value> a) {
    int32_t bits = a[0].AsInt32();
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return Value::Float(static_cast<double>(f));
  };
  reg("caml_int32_of_float", 1, i32_of_f);
  reg("caml_int32_of_float_unboxed", 1, i32_of_f);
  reg("caml_int32_to_float", 1, i32_to_f);
  reg("caml_int32_to_float_unboxed", 1, i32_to_f);
  reg("caml_int32_bits_of_float", 1, i32_bits_of_f);
  reg("caml_int32_bits_of_float_unboxed", 1, i32_bits_of_f);
  reg("caml_int32_float_of_bits", 1, i32_f_of_bits);
  reg("caml_int32_float_of_bits_unboxed", 1, i32_f_of_bits);
  reg("caml_int32_compare", 2, [](absl::Span<const Value> a) {
    int32_t x = a[0].AsInt32(), y = a[1].AsInt32();
    return Value::Int((x > y) - (x < y));
  });

  reg("%int64_neg", 1, [](absl::Span<const Value> a) {
    return Value::Int64(
        static_cast<int64_t>(0ULL - static_cast<uint64_t>(a[0].AsInt64())));
  });
  reg("%int64_add", 2, [](absl::Span<const Value> a) {
    return Value::Int64(static_cast<int64_t>(
        static_cast<uint64_t>(a[0].AsInt64()) +
        static_cast<uint64_t>(a[1].AsInt64())));
  });
  reg("%int64_sub", 2, [](absl::Span<const Value> a) {
    return Value::Int64(static_cast<int64_t>(
        static_cast<uint64_t>(a[0].AsInt64()) -
        static_cast<uint64_t>(a[1].AsInt64())));
  });
  reg("%int64_mul", 2, [](absl::Span<const Value> a) {
    return Value::Int64(static_cast<int64_t>(
        static_cast<uint64_t>(a[0].AsInt64()) *
        static_cast<uint64_t>(a[1].AsInt64())));
  });
  reg("%int64_div", 2, [&intern](absl::Span<const Value> a) {
    int64_t x = a[0].AsInt64(), y = a[1].AsInt64();
    if (y == 0) throw OcamlException(MakeExnDivisionByZero(intern));
    if (x == INT64_MIN && y == -1) return Value::Int64(INT64_MIN);
    return Value::Int64(x / y);
  });
  reg("%int64_mod", 2, [&intern](absl::Span<const Value> a) {
    int64_t x = a[0].AsInt64(), y = a[1].AsInt64();
    if (y == 0) throw OcamlException(MakeExnDivisionByZero(intern));
    if (x == INT64_MIN && y == -1) return Value::Int64(0);
    return Value::Int64(x % y);
  });
  reg("%int64_and", 2, [](absl::Span<const Value> a) {
    return Value::Int64(a[0].AsInt64() & a[1].AsInt64());
  });
  reg("%int64_or", 2, [](absl::Span<const Value> a) {
    return Value::Int64(a[0].AsInt64() | a[1].AsInt64());
  });
  reg("%int64_xor", 2, [](absl::Span<const Value> a) {
    return Value::Int64(a[0].AsInt64() ^ a[1].AsInt64());
  });
  reg("%int64_lsl", 2, [](absl::Span<const Value> a) {
    uint64_t x = static_cast<uint64_t>(a[0].AsInt64());
    int s = static_cast<int>(a[1].AsInt()) & 63;
    return Value::Int64(static_cast<int64_t>(x << s));
  });
  reg("%int64_lsr", 2, [](absl::Span<const Value> a) {
    uint64_t x = static_cast<uint64_t>(a[0].AsInt64());
    int s = static_cast<int>(a[1].AsInt()) & 63;
    return Value::Int64(static_cast<int64_t>(x >> s));
  });
  reg("%int64_asr", 2, [](absl::Span<const Value> a) {
    int64_t x = a[0].AsInt64();
    int s = static_cast<int>(a[1].AsInt()) & 63;
    return Value::Int64(x >> s);
  });
  reg("%int64_of_int", 1, [](absl::Span<const Value> a) {
    return Value::Int64(a[0].AsInt());
  });
  reg("%int64_to_int", 1, [](absl::Span<const Value> a) {
    return Value::Int(SignExtend63(static_cast<uint64_t>(a[0].AsInt64())));
  });
  reg("%int64_of_int32", 1, [](absl::Span<const Value> a) {
    return Value::Int64(static_cast<int64_t>(a[0].AsInt32()));
  });
  reg("%int64_to_int32", 1, [](absl::Span<const Value> a) {
    return Value::Int32(static_cast<int32_t>(a[0].AsInt64()));
  });
  reg("%int64_of_nativeint", 1, [](absl::Span<const Value> a) {
    return Value::Int64(a[0].AsNativeint());
  });
  reg("%int64_to_nativeint", 1, [](absl::Span<const Value> a) {
    return Value::Nativeint(a[0].AsInt64());
  });

  auto i64_of_f = [](absl::Span<const Value> a) {
    return Value::Int64(static_cast<int64_t>(a[0].AsFloat()));
  };
  auto i64_to_f = [](absl::Span<const Value> a) {
    return Value::Float(static_cast<double>(a[0].AsInt64()));
  };
  auto i64_bits_of_f = [](absl::Span<const Value> a) {
    double d = a[0].AsFloat();
    int64_t bits = 0;
    std::memcpy(&bits, &d, 8);
    return Value::Int64(bits);
  };
  auto i64_f_of_bits = [](absl::Span<const Value> a) {
    int64_t bits = a[0].AsInt64();
    double d = 0.0;
    std::memcpy(&d, &bits, 8);
    return Value::Float(d);
  };
  reg("caml_int64_of_float", 1, i64_of_f);
  reg("caml_int64_of_float_unboxed", 1, i64_of_f);
  reg("caml_int64_to_float", 1, i64_to_f);
  reg("caml_int64_to_float_unboxed", 1, i64_to_f);
  reg("caml_int64_bits_of_float", 1, i64_bits_of_f);
  reg("caml_int64_bits_of_float_unboxed", 1, i64_bits_of_f);
  reg("caml_int64_float_of_bits", 1, i64_f_of_bits);
  reg("caml_int64_float_of_bits_unboxed", 1, i64_f_of_bits);
  reg("caml_int64_compare", 2, [](absl::Span<const Value> a) {
    int64_t x = a[0].AsInt64(), y = a[1].AsInt64();
    return Value::Int((x > y) - (x < y));
  });

  reg("%nativeint_neg", 1, [](absl::Span<const Value> a) {
    return Value::Nativeint(static_cast<int64_t>(
        0ULL - static_cast<uint64_t>(a[0].AsNativeint())));
  });
  reg("%nativeint_add", 2, [](absl::Span<const Value> a) {
    return Value::Nativeint(static_cast<int64_t>(
        static_cast<uint64_t>(a[0].AsNativeint()) +
        static_cast<uint64_t>(a[1].AsNativeint())));
  });
  reg("%nativeint_sub", 2, [](absl::Span<const Value> a) {
    return Value::Nativeint(static_cast<int64_t>(
        static_cast<uint64_t>(a[0].AsNativeint()) -
        static_cast<uint64_t>(a[1].AsNativeint())));
  });
  reg("%nativeint_mul", 2, [](absl::Span<const Value> a) {
    return Value::Nativeint(static_cast<int64_t>(
        static_cast<uint64_t>(a[0].AsNativeint()) *
        static_cast<uint64_t>(a[1].AsNativeint())));
  });
  reg("%nativeint_div", 2, [&intern](absl::Span<const Value> a) {
    int64_t x = a[0].AsNativeint(), y = a[1].AsNativeint();
    if (y == 0) throw OcamlException(MakeExnDivisionByZero(intern));
    if (x == INT64_MIN && y == -1) return Value::Nativeint(INT64_MIN);
    return Value::Nativeint(x / y);
  });
  reg("%nativeint_mod", 2, [&intern](absl::Span<const Value> a) {
    int64_t x = a[0].AsNativeint(), y = a[1].AsNativeint();
    if (y == 0) throw OcamlException(MakeExnDivisionByZero(intern));
    if (x == INT64_MIN && y == -1) return Value::Nativeint(0);
    return Value::Nativeint(x % y);
  });
  reg("%nativeint_and", 2, [](absl::Span<const Value> a) {
    return Value::Nativeint(a[0].AsNativeint() & a[1].AsNativeint());
  });
  reg("%nativeint_or", 2, [](absl::Span<const Value> a) {
    return Value::Nativeint(a[0].AsNativeint() | a[1].AsNativeint());
  });
  reg("%nativeint_xor", 2, [](absl::Span<const Value> a) {
    return Value::Nativeint(a[0].AsNativeint() ^ a[1].AsNativeint());
  });
  reg("%nativeint_lsl", 2, [](absl::Span<const Value> a) {
    uint64_t x = static_cast<uint64_t>(a[0].AsNativeint());
    int s = static_cast<int>(a[1].AsInt()) & 63;
    return Value::Nativeint(static_cast<int64_t>(x << s));
  });
  reg("%nativeint_lsr", 2, [](absl::Span<const Value> a) {
    uint64_t x = static_cast<uint64_t>(a[0].AsNativeint());
    int s = static_cast<int>(a[1].AsInt()) & 63;
    return Value::Nativeint(static_cast<int64_t>(x >> s));
  });
  reg("%nativeint_asr", 2, [](absl::Span<const Value> a) {
    int64_t x = a[0].AsNativeint();
    int s = static_cast<int>(a[1].AsInt()) & 63;
    return Value::Nativeint(x >> s);
  });
  reg("%nativeint_of_int", 1, [](absl::Span<const Value> a) {
    return Value::Nativeint(a[0].AsInt());
  });
  reg("%nativeint_to_int", 1, [](absl::Span<const Value> a) {
    return Value::Int(SignExtend63(static_cast<uint64_t>(a[0].AsNativeint())));
  });
  reg("%nativeint_of_int32", 1, [](absl::Span<const Value> a) {
    return Value::Nativeint(static_cast<int64_t>(a[0].AsInt32()));
  });
  reg("%nativeint_to_int32", 1, [](absl::Span<const Value> a) {
    return Value::Int32(static_cast<int32_t>(a[0].AsNativeint()));
  });
  auto ni_of_f = [](absl::Span<const Value> a) {
    return Value::Nativeint(static_cast<int64_t>(a[0].AsFloat()));
  };
  auto ni_to_f = [](absl::Span<const Value> a) {
    return Value::Float(static_cast<double>(a[0].AsNativeint()));
  };
  reg("caml_nativeint_of_float", 1, ni_of_f);
  reg("caml_nativeint_of_float_unboxed", 1, ni_of_f);
  reg("caml_nativeint_to_float", 1, ni_to_f);
  reg("caml_nativeint_to_float_unboxed", 1, ni_to_f);
  reg("caml_nativeint_compare", 2, [](absl::Span<const Value> a) {
    int64_t x = a[0].AsNativeint(), y = a[1].AsNativeint();
    return Value::Int((x > y) - (x < y));
  });

  // Byte swaps.
  reg("%bswap16", 1, [](absl::Span<const Value> a) {
    return Value::Int(
        static_cast<int64_t>(Bswap16(static_cast<uint16_t>(a[0].AsInt()))));
  });
  reg("%bswap_int32", 1, [](absl::Span<const Value> a) {
    return Value::Int32(
        static_cast<int32_t>(Bswap32(static_cast<uint32_t>(a[0].AsInt32()))));
  });
  reg("%bswap_int64", 1, [](absl::Span<const Value> a) {
    return Value::Int64(
        static_cast<int64_t>(Bswap64(static_cast<uint64_t>(a[0].AsInt64()))));
  });
  reg("%bswap_native", 1, [](absl::Span<const Value> a) {
    return Value::Nativeint(static_cast<int64_t>(
        Bswap64(static_cast<uint64_t>(a[0].AsNativeint()))));
  });

  // Number parsing & formatting.
  reg("caml_int_of_string", 1, [&intern](absl::Span<const Value> a) {
    return Value::Int(ParseOcamlIntString(a[0].AsString()->data, 63,
                                          "int_of_string", intern));
  });
  reg("caml_int32_of_string", 1, [&intern](absl::Span<const Value> a) {
    return Value::Int32(static_cast<int32_t>(ParseOcamlIntString(
        a[0].AsString()->data, 32, "Int32.of_string", intern)));
  });
  reg("caml_int64_of_string", 1, [&intern](absl::Span<const Value> a) {
    return Value::Int64(ParseOcamlIntString(a[0].AsString()->data, 64,
                                            "Int64.of_string", intern));
  });
  reg("caml_nativeint_of_string", 1, [&intern](absl::Span<const Value> a) {
    return Value::Nativeint(ParseOcamlIntString(
        a[0].AsString()->data, 64, "Nativeint.of_string", intern));
  });
  reg("caml_float_of_string", 1, [&intern](absl::Span<const Value> a) {
    return Value::Float(ParseOcamlFloatString(a[0].AsString()->data, intern));
  });

  reg("caml_format_int", 2, [](absl::Span<const Value> a) {
    return Value::String(
        FormatIntWithPrintf(a[0].AsString()->data, a[1].AsInt(), 63));
  });
  reg("caml_int32_format", 2, [](absl::Span<const Value> a) {
    return Value::String(
        FormatIntWithPrintf(a[0].AsString()->data, a[1].AsInt32(), 32));
  });
  reg("caml_int64_format", 2, [](absl::Span<const Value> a) {
    return Value::String(
        FormatIntWithPrintf(a[0].AsString()->data, a[1].AsInt64(), 64));
  });
  reg("caml_nativeint_format", 2, [](absl::Span<const Value> a) {
    return Value::String(
        FormatIntWithPrintf(a[0].AsString()->data, a[1].AsNativeint(), 64));
  });
  reg("caml_format_float", 2, [](absl::Span<const Value> a) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), a[0].AsString()->data.c_str(),
                  a[1].AsFloat());
    return Value::String(buf);
  });
  reg("caml_hexstring_of_float", 3, [](absl::Span<const Value> a) {
    return Value::String(FormatHexFloat(a[0].AsFloat(), a[1].AsInt(),
                                        static_cast<char>(a[2].AsChar())));
  });


  // =========================================================================
  // 6. Strings and Bytes
  // =========================================================================

  auto str_len = [](absl::Span<const Value> a) {
    return Value::Int(static_cast<int64_t>(a[0].AsString()->data.size()));
  };
  reg("%string_length", 1, str_len);
  reg("%bytes_length", 1, str_len);
  reg("caml_ml_string_length", 1, str_len);
  reg("caml_ml_bytes_length", 1, str_len);

  auto str_safe_get = [&intern](absl::Span<const Value> a) {
    const std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) >= s.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    return Value::Char(static_cast<uint8_t>(s[idx]));
  };
  auto str_unsafe_get = [](absl::Span<const Value> a) {
    const std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) >= s.size()) {
      return Value::Char(0);
    }
    return Value::Char(static_cast<uint8_t>(s[idx]));
  };
  reg("%string_safe_get", 2, str_safe_get);
  reg("%bytes_safe_get", 2, str_safe_get);
  reg("caml_string_get", 2, str_safe_get);
  reg("caml_bytes_get", 2, str_safe_get);
  reg("%string_unsafe_get", 2, str_unsafe_get);
  reg("%bytes_unsafe_get", 2, str_unsafe_get);
  reg("caml_string_unsafe_get", 2, str_unsafe_get);
  reg("caml_bytes_unsafe_get", 2, str_unsafe_get);

  auto bytes_safe_set = [&intern](absl::Span<const Value> a) {
    std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) >= s.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    s[idx] = static_cast<char>(a[2].AsChar());
    return Value::Unit();
  };
  auto bytes_unsafe_set = [](absl::Span<const Value> a) {
    std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    if (idx >= 0 && static_cast<size_t>(idx) < s.size()) {
      s[idx] = static_cast<char>(a[2].AsChar());
    }
    return Value::Unit();
  };
  reg("%bytes_safe_set", 3, bytes_safe_set);
  reg("%string_safe_set", 3, bytes_safe_set);
  reg("caml_bytes_set", 3, bytes_safe_set);
  reg("caml_string_set", 3, bytes_safe_set);
  reg("%bytes_unsafe_set", 3, bytes_unsafe_set);
  reg("%string_unsafe_set", 3, bytes_unsafe_set);
  reg("caml_bytes_unsafe_set", 3, bytes_unsafe_set);
  reg("caml_string_unsafe_set", 3, bytes_unsafe_set);

  reg("%bytes_to_string", 1, [](absl::Span<const Value> a) { return a[0]; });
  reg("%bytes_of_string", 1, [](absl::Span<const Value> a) { return a[0]; });

  auto create_bytes = [&intern](absl::Span<const Value> a) {
    int64_t len = a[0].AsInt();
    if (len < 0) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "Bytes.create"));
    }
    return Value::Bytes(static_cast<size_t>(len), '\0');
  };
  reg("caml_create_bytes", 1, create_bytes);
  reg("caml_create_string", 1, create_bytes);

  auto fill_bytes = [](absl::Span<const Value> a) {
    std::string& s = a[0].AsString()->data;
    int64_t ofs = a[1].AsInt();
    int64_t len = a[2].AsInt();
    char c = static_cast<char>(a[3].AsChar());
    if (len > 0 && ofs >= 0 &&
        static_cast<size_t>(ofs + len) <= s.size()) {
      std::memset(s.data() + ofs, static_cast<unsigned char>(c),
                  static_cast<size_t>(len));
    }
    return Value::Unit();
  };
  reg("caml_fill_bytes", 4, fill_bytes);
  reg("caml_fill_string", 4, fill_bytes);

  auto blit_bytes = [](absl::Span<const Value> a) {
    const std::string& src = a[0].AsString()->data;
    int64_t src_pos = a[1].AsInt();
    std::string& dst = a[2].AsString()->data;
    int64_t dst_pos = a[3].AsInt();
    int64_t len = a[4].AsInt();
    if (len > 0 && src_pos >= 0 && dst_pos >= 0 &&
        static_cast<size_t>(src_pos + len) <= src.size() &&
        static_cast<size_t>(dst_pos + len) <= dst.size()) {
      std::memmove(dst.data() + dst_pos, src.data() + src_pos,
                   static_cast<size_t>(len));
    }
    return Value::Unit();
  };
  reg("caml_blit_bytes", 5, blit_bytes);
  reg("caml_blit_string", 5, blit_bytes);

  auto str_eq = [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsString()->data == a[1].AsString()->data);
  };
  auto str_neq = [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsString()->data != a[1].AsString()->data);
  };
  auto str_cmp = [](absl::Span<const Value> a) {
    const std::string& x = a[0].AsString()->data;
    const std::string& y = a[1].AsString()->data;
    int c = x.compare(y);
    return Value::Int((c > 0) - (c < 0));
  };
  reg("caml_string_equal", 2, str_eq);
  reg("caml_bytes_equal", 2, str_eq);
  reg("caml_string_notequal", 2, str_neq);
  reg("caml_bytes_notequal", 2, str_neq);
  reg("caml_string_compare", 2, str_cmp);
  reg("caml_bytes_compare", 2, str_cmp);
  reg("caml_string_lessthan", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsString()->data < a[1].AsString()->data);
  });
  reg("caml_string_lessequal", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsString()->data <= a[1].AsString()->data);
  });
  reg("caml_string_greaterthan", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsString()->data > a[1].AsString()->data);
  });
  reg("caml_string_greaterequal", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsString()->data >= a[1].AsString()->data);
  });
  reg("caml_bytes_lessthan", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsString()->data < a[1].AsString()->data);
  });
  reg("caml_bytes_lessequal", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsString()->data <= a[1].AsString()->data);
  });
  reg("caml_bytes_greaterthan", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsString()->data > a[1].AsString()->data);
  });
  reg("caml_bytes_greaterequal", 2, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsString()->data >= a[1].AsString()->data);
  });

  // Multi-byte reads and writes on String and Bytes.
  auto get16_safe = [&intern](absl::Span<const Value> a) {
    const std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) + 2 > s.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    uint16_t v = 0;
    std::memcpy(&v, s.data() + idx, 2);
    return Value::Int(static_cast<int64_t>(v));
  };
  auto get16_unsafe = [](absl::Span<const Value> a) {
    const std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    uint16_t v = 0;
    if (idx >= 0 && static_cast<size_t>(idx) + 2 <= s.size()) {
      std::memcpy(&v, s.data() + idx, 2);
    }
    return Value::Int(static_cast<int64_t>(v));
  };
  auto get32_safe = [&intern](absl::Span<const Value> a) {
    const std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) + 4 > s.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    int32_t v = 0;
    std::memcpy(&v, s.data() + idx, 4);
    return Value::Int32(v);
  };
  auto get32_unsafe = [](absl::Span<const Value> a) {
    const std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    int32_t v = 0;
    if (idx >= 0 && static_cast<size_t>(idx) + 4 <= s.size()) {
      std::memcpy(&v, s.data() + idx, 4);
    }
    return Value::Int32(v);
  };
  auto get64_safe = [&intern](absl::Span<const Value> a) {
    const std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) + 8 > s.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    int64_t v = 0;
    std::memcpy(&v, s.data() + idx, 8);
    return Value::Int64(v);
  };
  auto get64_unsafe = [](absl::Span<const Value> a) {
    const std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    int64_t v = 0;
    if (idx >= 0 && static_cast<size_t>(idx) + 8 <= s.size()) {
      std::memcpy(&v, s.data() + idx, 8);
    }
    return Value::Int64(v);
  };

  auto set16_safe = [&intern](absl::Span<const Value> a) {
    std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) + 2 > s.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    uint16_t v = static_cast<uint16_t>(a[2].AsInt());
    std::memcpy(s.data() + idx, &v, 2);
    return Value::Unit();
  };
  auto set16_unsafe = [](absl::Span<const Value> a) {
    std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    uint16_t v = static_cast<uint16_t>(a[2].AsInt());
    if (idx >= 0 && static_cast<size_t>(idx) + 2 <= s.size()) {
      std::memcpy(s.data() + idx, &v, 2);
    }
    return Value::Unit();
  };
  auto set32_safe = [&intern](absl::Span<const Value> a) {
    std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) + 4 > s.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    int32_t v = a[2].AsInt32();
    std::memcpy(s.data() + idx, &v, 4);
    return Value::Unit();
  };
  auto set32_unsafe = [](absl::Span<const Value> a) {
    std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    int32_t v = a[2].AsInt32();
    if (idx >= 0 && static_cast<size_t>(idx) + 4 <= s.size()) {
      std::memcpy(s.data() + idx, &v, 4);
    }
    return Value::Unit();
  };
  auto set64_safe = [&intern](absl::Span<const Value> a) {
    std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) + 8 > s.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    int64_t v = a[2].AsInt64();
    std::memcpy(s.data() + idx, &v, 8);
    return Value::Unit();
  };
  auto set64_unsafe = [](absl::Span<const Value> a) {
    std::string& s = a[0].AsString()->data;
    int64_t idx = a[1].AsInt();
    int64_t v = a[2].AsInt64();
    if (idx >= 0 && static_cast<size_t>(idx) + 8 <= s.size()) {
      std::memcpy(s.data() + idx, &v, 8);
    }
    return Value::Unit();
  };

  reg("%caml_string_get16", 2, get16_safe);
  reg("%caml_string_get16u", 2, get16_unsafe);
  reg("%caml_string_get32", 2, get32_safe);
  reg("%caml_string_get32u", 2, get32_unsafe);
  reg("%caml_string_get64", 2, get64_safe);
  reg("%caml_string_get64u", 2, get64_unsafe);
  reg("%caml_bytes_get16", 2, get16_safe);
  reg("%caml_bytes_get16u", 2, get16_unsafe);
  reg("%caml_bytes_get32", 2, get32_safe);
  reg("%caml_bytes_get32u", 2, get32_unsafe);
  reg("%caml_bytes_get64", 2, get64_safe);
  reg("%caml_bytes_get64u", 2, get64_unsafe);
  reg("%caml_bytes_set16", 3, set16_safe);
  reg("%caml_bytes_set16u", 3, set16_unsafe);
  reg("%caml_bytes_set32", 3, set32_safe);
  reg("%caml_bytes_set32u", 3, set32_unsafe);
  reg("%caml_bytes_set64", 3, set64_safe);
  reg("%caml_bytes_set64u", 3, set64_unsafe);

  // =========================================================================
  // 7. Arrays, Floatarrays, and Bigarrays (including LXM PRNG)
  // =========================================================================

  reg("%array_length", 1, [maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    if (a[0].IsFloatArray()) {
      return Value::Int(static_cast<int64_t>(a[0].AsFloatArray()->elements.size()));
    }
    return Value::Int(static_cast<int64_t>(a[0].AsArray()->elements.size()));
  });

  auto array_safe_get = [&intern, maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    int64_t idx = a[1].AsInt();
    if (a[0].IsFloatArray()) {
      const auto& elems = a[0].AsFloatArray()->elements;
      if (idx < 0 || static_cast<size_t>(idx) >= elems.size()) {
        throw OcamlException(
            MakeExnInvalidArgument(intern, "index out of bounds"));
      }
      return Value::Float(elems[idx]);
    }
    const auto& elems = a[0].AsArray()->elements;
    if (idx < 0 || static_cast<size_t>(idx) >= elems.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    return elems[idx];
  };
  auto array_unsafe_get = [maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    int64_t idx = a[1].AsInt();
    if (a[0].IsFloatArray()) {
      const auto& elems = a[0].AsFloatArray()->elements;
      if (idx < 0 || static_cast<size_t>(idx) >= elems.size()) {
        return Value::Float(0.0);
      }
      return Value::Float(elems[idx]);
    }
    const auto& elems = a[0].AsArray()->elements;
    if (idx < 0 || static_cast<size_t>(idx) >= elems.size()) {
      return Value::Unit();
    }
    return elems[idx];
  };
  auto array_safe_set = [&intern, maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    int64_t idx = a[1].AsInt();
    if (a[0].IsFloatArray()) {
      auto& elems = a[0].AsFloatArray()->elements;
      if (idx < 0 || static_cast<size_t>(idx) >= elems.size()) {
        throw OcamlException(
            MakeExnInvalidArgument(intern, "index out of bounds"));
      }
      elems[idx] = a[2].AsFloat();
      return Value::Unit();
    }
    auto& elems = a[0].AsArray()->elements;
    if (idx < 0 || static_cast<size_t>(idx) >= elems.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    elems[idx] = a[2];
    return Value::Unit();
  };
  auto array_unsafe_set = [maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    int64_t idx = a[1].AsInt();
    if (a[0].IsFloatArray()) {
      auto& elems = a[0].AsFloatArray()->elements;
      if (idx >= 0 && static_cast<size_t>(idx) < elems.size()) {
        elems[idx] = a[2].AsFloat();
      }
      return Value::Unit();
    }
    auto& elems = a[0].AsArray()->elements;
    if (idx >= 0 && static_cast<size_t>(idx) < elems.size()) {
      elems[idx] = a[2];
    }
    return Value::Unit();
  };

  reg("%array_safe_get", 2, array_safe_get);
  reg("caml_array_get", 2, array_safe_get);
  reg("caml_array_get_addr", 2, array_safe_get);
  reg("%array_unsafe_get", 2, array_unsafe_get);
  reg("caml_array_unsafe_get", 2, array_unsafe_get);
  reg("%array_safe_set", 3, array_safe_set);
  reg("caml_array_set", 3, array_safe_set);
  reg("caml_array_set_addr", 3, array_safe_set);
  reg("%array_unsafe_set", 3, array_unsafe_set);
  reg("caml_array_unsafe_set", 3, array_unsafe_set);
  reg("caml_array_unsafe_set_addr", 3, array_unsafe_set);

  auto make_vect = [&intern](absl::Span<const Value> a) {
    int64_t len = a[0].AsInt();
    if (len < 0) {
      throw OcamlException(MakeExnInvalidArgument(intern, "Array.make"));
    }
    std::vector<Value> elems(static_cast<size_t>(len), a[1]);
    return Value::Array(std::move(elems));
  };
  reg("caml_make_vect", 2, make_vect);
  reg("caml_array_make", 2, make_vect);
  reg("caml_make_array", 1, [](absl::Span<const Value> a) { return a[0]; });

  reg("caml_array_sub", 3, [&intern, maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    int64_t ofs = a[1].AsInt();
    int64_t len = a[2].AsInt();
    if (a[0].IsFloatArray()) {
      const auto& elems = a[0].AsFloatArray()->elements;
      if (ofs < 0 || len < 0 ||
          static_cast<size_t>(ofs + len) > elems.size()) {
        throw OcamlException(MakeExnInvalidArgument(intern, "Array.sub"));
      }
      std::vector<double> sub(elems.begin() + ofs, elems.begin() + ofs + len);
      return Value::FloatArray(std::move(sub));
    }
    const auto& elems = a[0].AsArray()->elements;
    if (ofs < 0 || len < 0 ||
        static_cast<size_t>(ofs + len) > elems.size()) {
      throw OcamlException(MakeExnInvalidArgument(intern, "Array.sub"));
    }
    std::vector<Value> sub(elems.begin() + ofs, elems.begin() + ofs + len);
    return Value::Array(std::move(sub));
  });

  reg("caml_array_append", 2, [maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    maybe_sync_argv(a[1]);
    if (a[0].IsFloatArray() && a[1].IsFloatArray()) {
      std::vector<double> res = a[0].AsFloatArray()->elements;
      const auto& e2 = a[1].AsFloatArray()->elements;
      res.insert(res.end(), e2.begin(), e2.end());
      return Value::FloatArray(std::move(res));
    }
    std::vector<Value> res;
    auto append_one = [&](const Value& v) {
      if (v.IsFloatArray()) {
        for (double d : v.AsFloatArray()->elements) {
          res.push_back(Value::Float(d));
        }
      } else {
        const auto& e = v.AsArray()->elements;
        res.insert(res.end(), e.begin(), e.end());
      }
    };
    append_one(a[0]);
    append_one(a[1]);
    return Value::Array(std::move(res));
  });

  reg("caml_array_concat", 1, [maybe_sync_argv](absl::Span<const Value> a) {
    std::vector<Value> list = ExtractList(a[0]);
    std::vector<Value> res;
    for (const Value& arr : list) {
      maybe_sync_argv(arr);
      if (arr.IsFloatArray()) {
        for (double d : arr.AsFloatArray()->elements) {
          res.push_back(Value::Float(d));
        }
      } else if (arr.IsArray()) {
        const auto& elems = arr.AsArray()->elements;
        res.insert(res.end(), elems.begin(), elems.end());
      }
    }
    return Value::Array(std::move(res));
  });

  reg("caml_array_blit", 5, [maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    maybe_sync_argv(a[2]);
    int64_t ofs1 = a[1].AsInt();
    int64_t ofs2 = a[3].AsInt();
    int64_t len = a[4].AsInt();
    if (len <= 0) return Value::Unit();
    if (a[0].IsFloatArray() && a[2].IsFloatArray()) {
      const auto& src = a[0].AsFloatArray()->elements;
      auto& dst = a[2].AsFloatArray()->elements;
      std::vector<double> tmp(src.begin() + ofs1, src.begin() + ofs1 + len);
      std::copy(tmp.begin(), tmp.end(), dst.begin() + ofs2);
      return Value::Unit();
    }
    std::vector<Value> tmp;
    tmp.reserve(static_cast<size_t>(len));
    if (a[0].IsFloatArray()) {
      const auto& src = a[0].AsFloatArray()->elements;
      for (int64_t i = 0; i < len; ++i) {
        tmp.push_back(Value::Float(src[ofs1 + i]));
      }
    } else {
      const auto& src = a[0].AsArray()->elements;
      for (int64_t i = 0; i < len; ++i) {
        tmp.push_back(src[ofs1 + i]);
      }
    }
    if (a[2].IsFloatArray()) {
      auto& dst = a[2].AsFloatArray()->elements;
      for (int64_t i = 0; i < len; ++i) {
        dst[ofs2 + i] = tmp[i].AsFloat();
      }
    } else {
      auto& dst = a[2].AsArray()->elements;
      for (int64_t i = 0; i < len; ++i) {
        dst[ofs2 + i] = tmp[i];
      }
    }
    return Value::Unit();
  });

  reg("caml_array_fill", 4, [maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    int64_t ofs = a[1].AsInt();
    int64_t len = a[2].AsInt();
    if (len <= 0) return Value::Unit();
    if (a[0].IsFloatArray()) {
      auto& dst = a[0].AsFloatArray()->elements;
      double v = a[3].AsFloat();
      for (int64_t i = 0; i < len; ++i) {
        if (ofs + i >= 0 && static_cast<size_t>(ofs + i) < dst.size()) {
          dst[ofs + i] = v;
        }
      }
      return Value::Unit();
    }
    auto& dst = a[0].AsArray()->elements;
    for (int64_t i = 0; i < len; ++i) {
      if (ofs + i >= 0 && static_cast<size_t>(ofs + i) < dst.size()) {
        dst[ofs + i] = a[3];
      }
    }
    return Value::Unit();
  });

  reg("caml_array_gather", 4, [maybe_sync_argv](absl::Span<const Value> a) {
    int64_t num = a[0].AsInt();
    const auto& arrays = a[1].AsArray()->elements;
    const auto& offsets = a[2].AsArray()->elements;
    const auto& lengths = a[3].AsArray()->elements;
    std::vector<Value> res;
    for (int64_t k = 0; k < num; ++k) {
      maybe_sync_argv(arrays[k]);
      int64_t ofs = offsets[k].AsInt();
      int64_t len = lengths[k].AsInt();
      if (arrays[k].IsFloatArray()) {
        const auto& src = arrays[k].AsFloatArray()->elements;
        for (int64_t i = 0; i < len; ++i) {
          res.push_back(Value::Float(src[ofs + i]));
        }
      } else {
        const auto& src = arrays[k].AsArray()->elements;
        for (int64_t i = 0; i < len; ++i) {
          res.push_back(src[ofs + i]);
        }
      }
    }
    return Value::Array(std::move(res));
  });

  // Floatarray primitives.
  auto fa_len = [](absl::Span<const Value> a) {
    if (a[0].IsFloatArray()) {
      return Value::Int(
          static_cast<int64_t>(a[0].AsFloatArray()->elements.size()));
    }
    return Value::Int(static_cast<int64_t>(a[0].AsArray()->elements.size()));
  };
  auto fa_create = [&intern](absl::Span<const Value> a) {
    int64_t len = a[0].AsInt();
    if (len < 0) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "Float.Array.create"));
    }
    return Value::FloatArray(std::vector<double>(static_cast<size_t>(len), 0.0));
  };
  reg("%floatarray_length", 1, fa_len);
  reg("caml_floatarray_length", 1, fa_len);
  reg("caml_floatarray_create", 1, fa_create);
  reg("caml_array_create_float", 1, fa_create);
  reg("caml_make_float_vect", 1, fa_create);
  reg("caml_floatarray_make", 2, [&intern](absl::Span<const Value> a) {
    int64_t len = a[0].AsInt();
    if (len < 0) {
      throw OcamlException(MakeExnInvalidArgument(intern, "Float.Array.make"));
    }
    return Value::FloatArray(
        std::vector<double>(static_cast<size_t>(len), a[1].AsFloat()));
  });
  reg("caml_floatarray_fill", 4, [&intern](absl::Span<const Value> a) {
    int64_t ofs = a[1].AsInt();
    int64_t len = a[2].AsInt();
    double v = a[3].AsFloat();
    auto& dst = a[0].AsFloatArray()->elements;
    if (ofs < 0 || len < 0 || static_cast<size_t>(ofs + len) > dst.size()) {
      throw OcamlException(MakeExnInvalidArgument(intern, "Float.Array.fill"));
    }
    for (int64_t i = 0; i < len; ++i) {
      dst[ofs + i] = v;
    }
    return Value::Unit();
  });
  reg("caml_floatarray_sub", 3, [&intern](absl::Span<const Value> a) {
    const auto& elems = a[0].AsFloatArray()->elements;
    int64_t ofs = a[1].AsInt();
    int64_t len = a[2].AsInt();
    if (ofs < 0 || len < 0 || static_cast<size_t>(ofs + len) > elems.size()) {
      throw OcamlException(MakeExnInvalidArgument(intern, "Float.Array.sub"));
    }
    std::vector<double> sub(elems.begin() + ofs, elems.begin() + ofs + len);
    return Value::FloatArray(std::move(sub));
  });
  reg("caml_floatarray_append", 2, [](absl::Span<const Value> a) {
    std::vector<double> res = a[0].AsFloatArray()->elements;
    const auto& e2 = a[1].AsFloatArray()->elements;
    res.insert(res.end(), e2.begin(), e2.end());
    return Value::FloatArray(std::move(res));
  });
  reg("caml_floatarray_concat", 1, [](absl::Span<const Value> a) {
    std::vector<Value> list = ExtractList(a[0]);
    std::vector<double> res;
    for (const Value& arr : list) {
      if (arr.IsFloatArray()) {
        const auto& elems = arr.AsFloatArray()->elements;
        res.insert(res.end(), elems.begin(), elems.end());
      }
    }
    return Value::FloatArray(std::move(res));
  });
  reg("%floatarray_safe_get", 2, array_safe_get);
  reg("caml_floatarray_get", 2, array_safe_get);
  reg("%floatarray_unsafe_get", 2, array_unsafe_get);
  reg("caml_floatarray_unsafe_get", 2, array_unsafe_get);
  reg("%floatarray_safe_set", 3, array_safe_set);
  reg("caml_floatarray_set", 3, array_safe_set);
  reg("%floatarray_unsafe_set", 3, array_unsafe_set);
  reg("caml_floatarray_unsafe_set", 3, array_unsafe_set);
  reg("caml_floatarray_blit", 5, [](absl::Span<const Value> a) {
    int64_t ofs1 = a[1].AsInt();
    int64_t ofs2 = a[3].AsInt();
    int64_t len = a[4].AsInt();
    if (len <= 0) return Value::Unit();
    const auto& src = a[0].AsFloatArray()->elements;
    auto& dst = a[2].AsFloatArray()->elements;
    std::vector<double> tmp(src.begin() + ofs1, src.begin() + ofs1 + len);
    std::copy(tmp.begin(), tmp.end(), dst.begin() + ofs2);
    return Value::Unit();
  });

  // Bigarray primitives.
  struct BaSubView {
    std::shared_ptr<Bigarray1Val> root;
    size_t byte_ofs = 0;
  };
  auto ba_sub_views =
      std::make_shared<absl::flat_hash_map<const Bigarray1Val*, BaSubView>>();
  auto ba_bytes_mut = [ba_sub_views](Bigarray1Val& ba) -> uint8_t* {
    auto it = ba_sub_views->find(&ba);
    if (it != ba_sub_views->end() &&
        it->second.byte_ofs + ba.data.size() <=
            it->second.root->data.size()) {
      return it->second.root->data.data() + it->second.byte_ofs;
    }
    return ba.data.data();
  };
  auto ba_bytes_const =
      [ba_sub_views](const Bigarray1Val& ba) -> const uint8_t* {
    auto it = ba_sub_views->find(&ba);
    if (it != ba_sub_views->end() &&
        it->second.byte_ofs + ba.data.size() <=
            it->second.root->data.size()) {
      return it->second.root->data.data() + it->second.byte_ofs;
    }
    return ba.data.data();
  };

  reg("caml_ba_create", 3, [&intern](absl::Span<const Value> a) {
    int kind = BigarrayKindFromValue(a[0], intern);
    size_t elem_size = BigarrayElemSize(kind);
    size_t total_elems = 1;
    if (a[2].IsArray()) {
      for (const Value& d : a[2].AsArray()->elements) {
        int64_t dim = d.AsInt();
        if (dim < 0) {
          throw OcamlException(MakeExnInvalidArgument(
              intern, "Bigarray.create: negative dimension"));
        }
        total_elems *= static_cast<size_t>(dim);
      }
    } else if (a[2].IsIntLike()) {
      int64_t dim = a[2].AsInt();
      if (dim < 0) {
        throw OcamlException(MakeExnInvalidArgument(
            intern, "Bigarray.create: negative dimension"));
      }
      total_elems = static_cast<size_t>(dim);
    }
    return Value::Bigarray1(std::vector<uint8_t>(total_elems * elem_size, 0),
                            kind, 0);
  });

  reg("caml_ba_num_dims", 1, [](absl::Span<const Value>) {
    return Value::Int(1);
  });
  reg("caml_ba_dim", 2, [](absl::Span<const Value> a) {
    const auto& ba = a[0].AsBigarray1();
    size_t elem_size = BigarrayElemSize(ba->kind);
    return Value::Int(static_cast<int64_t>(ba->data.size() / elem_size));
  });
  auto ba_dim1 = [](absl::Span<const Value> a) {
    const auto& ba = a[0].AsBigarray1();
    size_t elem_size = BigarrayElemSize(ba->kind);
    return Value::Int(static_cast<int64_t>(ba->data.size() / elem_size));
  };
  reg("%caml_ba_dim_1", 1, ba_dim1);
  reg("caml_ba_dim_1", 1, ba_dim1);
  reg("%caml_ba_dim_2", 1, [](absl::Span<const Value>) { return Value::Int(1); });
  reg("caml_ba_dim_2", 1, [](absl::Span<const Value>) { return Value::Int(1); });
  reg("%caml_ba_dim_3", 1, [](absl::Span<const Value>) { return Value::Int(1); });
  reg("caml_ba_dim_3", 1, [](absl::Span<const Value>) { return Value::Int(1); });

  reg("caml_ba_kind", 1, [&intern](absl::Span<const Value> a) {
    return BigarrayKindToValue(a[0].AsBigarray1()->kind, intern);
  });
  reg("caml_ba_layout", 1, [&intern](absl::Span<const Value>) {
    return Value::Constructor(intern.Intern("C_layout"));
  });
  reg("caml_ba_change_layout", 2, [](absl::Span<const Value> a) {
    return a[0];
  });

  auto ba_get_elem = [ba_bytes_const](const Bigarray1Val& ba,
                                      int64_t idx) -> Value {
    size_t elem_size = BigarrayElemSize(ba.kind);
    size_t offset = static_cast<size_t>(idx) * elem_size;
    if (idx < 0 || offset + elem_size > ba.data.size()) {
      return Value::Int(0);
    }
    const uint8_t* p = ba_bytes_const(ba) + offset;
    switch (ba.kind) {
      case 0: {  // Float32.
        float f = 0.0f;
        std::memcpy(&f, p, 4);
        return Value::Float(static_cast<double>(f));
      }
      case 1: {  // Float64.
        double d = 0.0;
        std::memcpy(&d, p, 8);
        return Value::Float(d);
      }
      case 2:  // Int8_signed.
        return Value::Int(static_cast<int64_t>(static_cast<int8_t>(*p)));
      case 3:  // Int8_unsigned.
        return Value::Int(static_cast<int64_t>(*p));
      case 4: {  // Int16_signed.
        int16_t v = 0;
        std::memcpy(&v, p, 2);
        return Value::Int(static_cast<int64_t>(v));
      }
      case 5: {  // Int16_unsigned.
        uint16_t v = 0;
        std::memcpy(&v, p, 2);
        return Value::Int(static_cast<int64_t>(v));
      }
      case 6: {  // Int32.
        int32_t v = 0;
        std::memcpy(&v, p, 4);
        return Value::Int32(v);
      }
      case 7: {  // Int64.
        int64_t v = 0;
        std::memcpy(&v, p, 8);
        return Value::Int64(v);
      }
      case 8: {  // Int.
        int64_t v = 0;
        std::memcpy(&v, p, 8);
        return Value::Int(v);
      }
      case 9: {  // Nativeint.
        int64_t v = 0;
        std::memcpy(&v, p, 8);
        return Value::Nativeint(v);
      }
      case 12:  // Char.
        return Value::Char(*p);
      default:
        return Value::Int(static_cast<int64_t>(*p));
    }
  };

  auto ba_set_elem = [ba_bytes_mut](Bigarray1Val& ba, int64_t idx,
                                    const Value& val) {
    size_t elem_size = BigarrayElemSize(ba.kind);
    size_t offset = static_cast<size_t>(idx) * elem_size;
    if (idx < 0 || offset + elem_size > ba.data.size()) {
      return;
    }
    uint8_t* p = ba_bytes_mut(ba) + offset;
    switch (ba.kind) {
      case 0: {  // Float32.
        float f = static_cast<float>(val.AsFloat());
        std::memcpy(p, &f, 4);
        break;
      }
      case 1: {  // Float64.
        double d = val.AsFloat();
        std::memcpy(p, &d, 8);
        break;
      }
      case 2:   // Int8_signed.
      case 3:   // Int8_unsigned.
      case 12:  // Char.
        *p = static_cast<uint8_t>(val.AsInt());
        break;
      case 4:    // Int16_signed.
      case 5: {  // Int16_unsigned.
        uint16_t v = static_cast<uint16_t>(val.AsInt());
        std::memcpy(p, &v, 2);
        break;
      }
      case 6: {  // Int32.
        int32_t v = val.AsInt32();
        std::memcpy(p, &v, 4);
        break;
      }
      case 7: {  // Int64.
        int64_t v = val.AsInt64();
        std::memcpy(p, &v, 8);
        break;
      }
      case 8:   // Int.
      case 9: { // Nativeint.
        int64_t v = val.AsInt();
        std::memcpy(p, &v, 8);
        break;
      }
      default:
        *p = static_cast<uint8_t>(val.AsInt());
        break;
    }
  };

  auto ba_get1 = [ba_get_elem](absl::Span<const Value> a) {
    return ba_get_elem(*a[0].AsBigarray1(), a[1].AsInt());
  };
  auto ba_set1 = [ba_set_elem](absl::Span<const Value> a) {
    ba_set_elem(*a[0].AsBigarray1(), a[1].AsInt(), a[2]);
    return Value::Unit();
  };

  reg("%caml_ba_ref_1", 2, ba_get1);
  reg("%caml_ba_unsafe_ref_1", 2, ba_get1);
  reg("caml_ba_get_1", 2, ba_get1);
  auto ba_get2 = [ba_get_elem](absl::Span<const Value> a) {
    return ba_get_elem(*a[0].AsBigarray1(), a[1].AsInt());
  };
  auto ba_get3 = [ba_get_elem](absl::Span<const Value> a) {
    return ba_get_elem(*a[0].AsBigarray1(), a[1].AsInt());
  };
  reg("%caml_ba_ref_2", 3, ba_get2);
  reg("%caml_ba_unsafe_ref_2", 3, ba_get2);
  reg("caml_ba_get_2", 3, ba_get2);
  reg("%caml_ba_ref_3", 4, ba_get3);
  reg("%caml_ba_unsafe_ref_3", 4, ba_get3);
  reg("caml_ba_get_3", 4, ba_get3);
  reg("caml_ba_get_generic", 2, [ba_get_elem](absl::Span<const Value> a) {
    int64_t idx = a[1].IsArray() && !a[1].AsArray()->elements.empty()
                      ? a[1].AsArray()->elements[0].AsInt()
                      : 0;
    return ba_get_elem(*a[0].AsBigarray1(), idx);
  });

  reg("%caml_ba_set_1", 3, ba_set1);
  reg("%caml_ba_unsafe_set_1", 3, ba_set1);
  reg("caml_ba_set_1", 3, ba_set1);
  auto ba_set2 = [ba_set_elem](absl::Span<const Value> a) {
    ba_set_elem(*a[0].AsBigarray1(), a[1].AsInt(), a[3]);
    return Value::Unit();
  };
  auto ba_set3 = [ba_set_elem](absl::Span<const Value> a) {
    ba_set_elem(*a[0].AsBigarray1(), a[1].AsInt(), a[4]);
    return Value::Unit();
  };
  reg("%caml_ba_set_2", 4, ba_set2);
  reg("%caml_ba_unsafe_set_2", 4, ba_set2);
  reg("caml_ba_set_2", 4, ba_set2);
  reg("%caml_ba_set_3", 5, ba_set3);
  reg("%caml_ba_unsafe_set_3", 5, ba_set3);
  reg("caml_ba_set_3", 5, ba_set3);
  reg("caml_ba_set_generic", 3, [ba_set_elem](absl::Span<const Value> a) {
    int64_t idx = a[1].IsArray() && !a[1].AsArray()->elements.empty()
                      ? a[1].AsArray()->elements[0].AsInt()
                      : 0;
    ba_set_elem(*a[0].AsBigarray1(), idx, a[2]);
    return Value::Unit();
  });

  // Bigarray multi-byte unaligned uint8 accessors used by `bytecomp/emitcode.ml`.
  auto ba_u8_get16 = [&intern, ba_bytes_const](absl::Span<const Value> a) {
    const auto& ba = *a[0].AsBigarray1();
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) + 2 > ba.data.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    uint16_t v = 0;
    std::memcpy(&v, ba_bytes_const(ba) + idx, 2);
    return Value::Int(static_cast<int64_t>(v));
  };
  auto ba_u8_get32 = [&intern, ba_bytes_const](absl::Span<const Value> a) {
    const auto& ba = *a[0].AsBigarray1();
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) + 4 > ba.data.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    int32_t v = 0;
    std::memcpy(&v, ba_bytes_const(ba) + idx, 4);
    return Value::Int32(v);
  };
  auto ba_u8_get64 = [&intern, ba_bytes_const](absl::Span<const Value> a) {
    const auto& ba = *a[0].AsBigarray1();
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) + 8 > ba.data.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    int64_t v = 0;
    std::memcpy(&v, ba_bytes_const(ba) + idx, 8);
    return Value::Int64(v);
  };
  auto ba_u8_set16 = [&intern, ba_bytes_mut](absl::Span<const Value> a) {
    auto& ba = *a[0].AsBigarray1();
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) + 2 > ba.data.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    uint16_t v = static_cast<uint16_t>(a[2].AsInt());
    std::memcpy(ba_bytes_mut(ba) + idx, &v, 2);
    return Value::Unit();
  };
  auto ba_u8_set32 = [&intern, ba_bytes_mut](absl::Span<const Value> a) {
    auto& ba = *a[0].AsBigarray1();
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) + 4 > ba.data.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    int32_t v = a[2].AsInt32();
    std::memcpy(ba_bytes_mut(ba) + idx, &v, 4);
    return Value::Unit();
  };
  auto ba_u8_set64 = [&intern, ba_bytes_mut](absl::Span<const Value> a) {
    auto& ba = *a[0].AsBigarray1();
    int64_t idx = a[1].AsInt();
    if (idx < 0 || static_cast<size_t>(idx) + 8 > ba.data.size()) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    int64_t v = a[2].AsInt64();
    std::memcpy(ba_bytes_mut(ba) + idx, &v, 8);
    return Value::Unit();
  };

  reg("%caml_ba_uint8_get16", 2, ba_u8_get16);
  reg("%caml_ba_uint8_get16u", 2, ba_u8_get16);
  reg("caml_ba_uint8_get16", 2, ba_u8_get16);
  reg("%caml_ba_uint8_get32", 2, ba_u8_get32);
  reg("%caml_ba_uint8_get32u", 2, ba_u8_get32);
  reg("caml_ba_uint8_get32", 2, ba_u8_get32);
  reg("%caml_ba_uint8_get64", 2, ba_u8_get64);
  reg("%caml_ba_uint8_get64u", 2, ba_u8_get64);
  reg("caml_ba_uint8_get64", 2, ba_u8_get64);
  reg("%caml_ba_uint8_set16", 3, ba_u8_set16);
  reg("%caml_ba_uint8_set16u", 3, ba_u8_set16);
  reg("caml_ba_uint8_set16", 3, ba_u8_set16);
  reg("%caml_ba_uint8_set32", 3, ba_u8_set32);
  reg("%caml_ba_uint8_set32u", 3, ba_u8_set32);
  reg("caml_ba_uint8_set32", 3, ba_u8_set32);
  reg("%caml_ba_uint8_set64", 3, ba_u8_set64);
  reg("%caml_ba_uint8_set64u", 3, ba_u8_set64);
  reg("caml_ba_uint8_set64", 3, ba_u8_set64);

  reg("caml_ba_sub", 3, [ba_sub_views](absl::Span<const Value> a) {
    auto src_ptr = a[0].AsBigarray1();
    int64_t ofs = a[1].AsInt();
    int64_t len = a[2].AsInt();
    size_t elem_size = BigarrayElemSize(src_ptr->kind);
    size_t byte_ofs = static_cast<size_t>(ofs) * elem_size;
    size_t byte_len = static_cast<size_t>(len) * elem_size;
    std::vector<uint8_t> sub_data(byte_len, 0);
    Value res =
        Value::Bigarray1(std::move(sub_data), src_ptr->kind, src_ptr->layout);
    std::shared_ptr<Bigarray1Val> root = src_ptr;
    size_t root_ofs = byte_ofs;
    if (auto it = ba_sub_views->find(src_ptr.get());
        it != ba_sub_views->end()) {
      root = it->second.root;
      root_ofs += it->second.byte_ofs;
    }
    (*ba_sub_views)[res.AsBigarray1().get()] = BaSubView{root, root_ofs};
    return res;
  });
  reg("caml_ba_slice", 2, [](absl::Span<const Value> a) { return a[0]; });
  reg("caml_ba_reshape", 2, [](absl::Span<const Value> a) { return a[0]; });
  reg("caml_ba_blit", 2,
      [ba_bytes_const, ba_bytes_mut](absl::Span<const Value> a) {
        const auto& src = *a[0].AsBigarray1();
        auto& dst = *a[1].AsBigarray1();
        size_t n = std::min(src.data.size(), dst.data.size());
        std::memmove(ba_bytes_mut(dst), ba_bytes_const(src), n);
        return Value::Unit();
      });
  reg("caml_ba_fill", 2, [ba_set_elem](absl::Span<const Value> a) {
    auto& ba = *a[0].AsBigarray1();
    size_t elem_size = BigarrayElemSize(ba.kind);
    size_t count = ba.data.size() / elem_size;
    for (size_t i = 0; i < count; ++i) {
      ba_set_elem(ba, static_cast<int64_t>(i), a[1]);
    }
    return Value::Unit();
  });

  // LXM pseudo-random number generator (`stdlib/random.ml`).
  auto lxm_next = [](absl::Span<const Value> a) {
    auto& ba = *a[0].AsBigarray1();
    if (ba.data.size() < 32) {
      ba.data.resize(32, 0);
    }
    uint64_t st[4];
    std::memcpy(st, ba.data.data(), 32);
    constexpr uint64_t kM = 0xd1342543de82ef95ULL;
    uint64_t a_param = st[0];
    uint64_t s = st[1];
    uint64_t x0 = st[2];
    uint64_t x1 = st[3];
    uint64_t z = s + x0;
    z = (z ^ (z >> 32)) * 0xdaba0b6eb09322e3ULL;
    z = (z ^ (z >> 32)) * 0xdaba0b6eb09322e3ULL;
    z = z ^ (z >> 32);
    s = s * kM + a_param;
    uint64_t q0 = x0;
    uint64_t q1 = x1 ^ q0;
    q0 = (q0 << 24) | (q0 >> 40);
    q0 = q0 ^ q1 ^ (q1 << 16);
    q1 = (q1 << 37) | (q1 >> 27);
    st[1] = s;
    st[2] = q0;
    st[3] = q1;
    std::memcpy(ba.data.data(), st, 32);
    return Value::Int64(static_cast<int64_t>(z));
  };
  reg("caml_lxm_next", 1, lxm_next);
  reg("caml_lxm_next_unboxed", 1, lxm_next);

  // =========================================================================
  // 8. Obj, Lazy, Atomic, Weak/Ephemeron, Domain, Mutex, Gc, Digest, Marshal
  // =========================================================================

  reg("%obj_is_int", 1, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].IsImmediate());
  });

  reg("caml_obj_tag", 1, [&intern, maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    return Value::Int(ObjTag(a[0], intern));
  });

  reg("%obj_size", 1, [maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    return Value::Int(ObjSize(a[0]));
  });

  reg("%obj_field", 2, [&intern, maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    return ObjField(a[0], a[1].AsInt(), intern);
  });

  reg("%obj_set_field", 3, [maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    ObjSetField(a[0], a[1].AsInt(), a[2]);
    return Value::Unit();
  });

  reg("caml_obj_block", 2, [](absl::Span<const Value> a) {
    int tag = static_cast<int>(a[0].AsInt());
    int64_t size = a[1].AsInt();
    std::vector<Value> fields(static_cast<size_t>(std::max<int64_t>(0, size)),
                              Value::Int(0));
    if (tag == 254) {
      return Value::FloatArray(std::vector<double>(
          static_cast<size_t>(std::max<int64_t>(0, size)), 0.0));
    }
    return Value::RawBlock(tag, std::move(fields));
  });

  reg("caml_obj_with_tag", 2, [&intern](absl::Span<const Value> a) {
    int new_tag = static_cast<int>(a[0].AsInt());
    const Value& v = a[1];
    int64_t sz = ObjSize(v);
    std::vector<Value> fields;
    fields.reserve(static_cast<size_t>(std::max<int64_t>(0, sz)));
    for (int64_t i = 0; i < sz; ++i) {
      fields.push_back(ObjField(v, i, intern));
    }
    return Value::RawBlock(new_tag, std::move(fields));
  });

  reg("caml_obj_dup", 1, [maybe_sync_argv](absl::Span<const Value> a) {
    maybe_sync_argv(a[0]);
    const Value& v = a[0];
    switch (v.kind()) {
      case Value::Kind::kString:
        return Value::String(v.AsString()->data);
      case Value::Kind::kTuple:
        return Value::Tuple(v.AsTuple()->elements);
      case Value::Kind::kArray:
        return Value::Array(v.AsArray()->elements);
      case Value::Kind::kFloatArray:
        return Value::FloatArray(v.AsFloatArray()->elements);
      case Value::Kind::kRecord:
        return Value::Record(v.AsRecord()->fields, v.AsRecord()->tag);
      case Value::Kind::kConstructor: {
        const auto& c = v.AsConstructor();
        Value dup = Value::Constructor(c->ctor, c->exn_stamp, c->arg,
                                       c->is_poly_var);
        dup.AsConstructor()->tag = c->tag;
        return dup;
      }
      case Value::Kind::kRawBlock:
        return Value::RawBlock(v.AsRawBlock()->tag, v.AsRawBlock()->fields);
      default:
        return v;
    }
  });

  reg("caml_obj_truncate", 2, [](absl::Span<const Value> a) {
    int64_t new_size = a[1].AsInt();
    if (new_size < 0) return Value::Unit();
    if (a[0].IsArray()) {
      auto& elems = a[0].AsArray()->elements;
      if (static_cast<size_t>(new_size) < elems.size()) {
        elems.resize(static_cast<size_t>(new_size));
      }
    } else if (a[0].IsRawBlock()) {
      auto& fields = a[0].AsRawBlock()->fields;
      if (static_cast<size_t>(new_size) < fields.size()) {
        fields.resize(static_cast<size_t>(new_size));
      }
    }
    return Value::Unit();
  });

  reg("caml_obj_reachable_words", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_obj_raw_field", 2, [&intern](absl::Span<const Value> a) {
    Value f = ObjField(a[0], a[1].AsInt(), intern);
    return Value::Nativeint(f.IsIntLike() ? f.AsInt() : 0);
  });
  reg("caml_obj_set_raw_field", 3, [](absl::Span<const Value> a) {
    ObjSetField(a[0], a[1].AsInt(), Value::Int(a[2].AsNativeint()));
    return Value::Unit();
  });
  reg("caml_obj_add_offset", 2, [](absl::Span<const Value>) -> Value {
    LOG(FATAL) << "caml_obj_add_offset is not supported.";
  });
  reg("caml_set_oo_id", 1, [](absl::Span<const Value> a) { return a[0]; });
  reg("caml_get_public_method", 2, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("%send", 3, [](absl::Span<const Value>) -> Value {
    LOG(FATAL) << "%send (object method dispatch) is not supported.";
  });
  reg("%sendcache", 5, [](absl::Span<const Value>) -> Value {
    LOG(FATAL) << "%sendcache (object method dispatch) is not supported.";
  });
  reg("%sendself", 3, [](absl::Span<const Value>) -> Value {
    LOG(FATAL) << "%sendself (object method dispatch) is not supported.";
  });

  // Lazy primitives (`stdlib/camlinternalLazy.ml`).
  reg("%lazy_force", 1, [&eval](absl::Span<const Value> a) {
    return eval.ForceLazy(a[0]);
  });

  reg("caml_lazy_make_forward", 1, [](absl::Span<const Value> a) {
    auto lz = std::make_shared<LazyVal>();
    lz->forced = true;
    lz->value = a[0];
    return Value::Lazy(std::move(lz));
  });
  reg("caml_lazy_update_to_forcing", 1, [](absl::Span<const Value> a) {
    if (!a[0].IsLazy()) return Value::Int(2);
    const auto& l = a[0].AsLazy();
    if (!l->forced && !l->evaluating) {
      l->evaluating = true;
      return Value::Int(0);
    }
    if (l->evaluating) {
      return Value::Int(1);
    }
    return Value::Int(2);
  });
  reg("caml_lazy_reset_to_lazy", 1, [](absl::Span<const Value> a) {
    if (a[0].IsLazy()) {
      a[0].AsLazy()->forced = false;
      a[0].AsLazy()->evaluating = false;
    }
    return Value::Unit();
  });
  reg("caml_lazy_update_to_forward", 1, [](absl::Span<const Value> a) {
    if (a[0].IsLazy()) {
      a[0].AsLazy()->evaluating = false;
      a[0].AsLazy()->forced = true;
    }
    return Value::Unit();
  });
  reg("caml_lazy_read_result", 1, [](absl::Span<const Value> a) {
    if (a[0].IsLazy()) {
      return a[0].AsLazy()->value;
    }
    return a[0];
  });

  // Atomic primitives (`stdlib/atomic.ml`).
  auto atomic_make = [sym_contents](absl::Span<const Value> a) {
    return Value::Ref(a[0], sym_contents);
  };
  auto atomic_load = [](absl::Span<const Value> a) {
    return GetRefField0(a[0]);
  };
  auto atomic_set = [](absl::Span<const Value> a) {
    SetRefField0(a[0], a[1]);
    return Value::Unit();
  };
  auto atomic_exchange = [](absl::Span<const Value> a) {
    Value old = GetRefField0(a[0]);
    SetRefField0(a[0], a[1]);
    return old;
  };
  auto atomic_cas = [phys_eq](absl::Span<const Value> a) {
    Value cur = GetRefField0(a[0]);
    if (phys_eq(cur, a[1])) {
      SetRefField0(a[0], a[2]);
      return Value::Bool(true);
    }
    return Value::Bool(false);
  };
  auto atomic_fetch_add = [](absl::Span<const Value> a) {
    int64_t old = GetRefField0(a[0]).AsInt();
    int64_t delta = a[1].AsInt();
    SetRefField0(a[0], Value::Int(SignExtend63(static_cast<uint64_t>(old) +
                                               static_cast<uint64_t>(delta))));
    return Value::Int(old);
  };

  reg("%atomic_make", 1, atomic_make);
  reg("caml_atomic_make", 1, atomic_make);
  reg("caml_atomic_make_contended", 1, atomic_make);
  reg("%atomic_load", 1, atomic_load);
  reg("caml_atomic_load", 1, atomic_load);
  reg("%atomic_set", 2, atomic_set);
  reg("caml_atomic_set", 2, atomic_set);
  reg("%atomic_exchange", 2, atomic_exchange);
  reg("caml_atomic_exchange", 2, atomic_exchange);
  reg("%atomic_cas", 3, atomic_cas);
  reg("caml_atomic_cas", 3, atomic_cas);
  reg("%atomic_fetch_add", 2, atomic_fetch_add);
  reg("caml_atomic_fetch_add", 2, atomic_fetch_add);

  // Weak and Ephemeron primitives (`stdlib/weak.ml`, `stdlib/obj.ml`).
  // Layout: RawBlock(251, size = l + 2) where fields[0] = link,
  // fields[1] = data option, and fields[2 + i] = key[i] option.
  auto ephe_create = [&intern](absl::Span<const Value> a) {
    int64_t len = a[0].AsInt();
    if (len < 0) {
      throw OcamlException(MakeExnInvalidArgument(intern, "Weak.create"));
    }
    Value none = MakeNone(intern);
    std::vector<Value> fields(static_cast<size_t>(len + 2), none);
    return Value::RawBlock(251, std::move(fields));
  };
  auto ephe_set_key = [&intern](absl::Span<const Value> a) {
    int64_t idx = a[1].AsInt();
    ObjSetField(a[0], idx + 2, MakeSome(intern, a[2]));
    return Value::Unit();
  };
  auto ephe_unset_key = [&intern](absl::Span<const Value> a) {
    int64_t idx = a[1].AsInt();
    ObjSetField(a[0], idx + 2, MakeNone(intern));
    return Value::Unit();
  };
  auto ephe_get_key = [&intern](absl::Span<const Value> a) {
    int64_t idx = a[1].AsInt();
    return ObjField(a[0], idx + 2, intern);
  };
  auto ephe_check_key = [&intern](absl::Span<const Value> a) {
    int64_t idx = a[1].AsInt();
    Value opt = ObjField(a[0], idx + 2, intern);
    return Value::Bool(opt.IsConstructor() &&
                       intern.Resolve(opt.AsConstructor()->ctor) == "Some");
  };
  auto ephe_blit_key = [&intern](absl::Span<const Value> a) {
    int64_t ofs1 = a[1].AsInt();
    int64_t ofs2 = a[3].AsInt();
    int64_t len = a[4].AsInt();
    std::vector<Value> tmp;
    tmp.reserve(static_cast<size_t>(std::max<int64_t>(0, len)));
    for (int64_t i = 0; i < len; ++i) {
      tmp.push_back(ObjField(a[0], ofs1 + i + 2, intern));
    }
    for (int64_t i = 0; i < len; ++i) {
      ObjSetField(a[2], ofs2 + i + 2, tmp[i]);
    }
    return Value::Unit();
  };
  auto ephe_get_data = [&intern](absl::Span<const Value> a) {
    return ObjField(a[0], 1, intern);
  };
  auto ephe_set_data = [&intern](absl::Span<const Value> a) {
    ObjSetField(a[0], 1, MakeSome(intern, a[1]));
    return Value::Unit();
  };
  auto ephe_unset_data = [&intern](absl::Span<const Value> a) {
    ObjSetField(a[0], 1, MakeNone(intern));
    return Value::Unit();
  };
  auto ephe_check_data = [&intern](absl::Span<const Value> a) {
    Value opt = ObjField(a[0], 1, intern);
    return Value::Bool(opt.IsConstructor() &&
                       intern.Resolve(opt.AsConstructor()->ctor) == "Some");
  };
  auto ephe_blit_data = [&intern](absl::Span<const Value> a) {
    ObjSetField(a[1], 1, ObjField(a[0], 1, intern));
    return Value::Unit();
  };

  reg("caml_weak_create", 1, ephe_create);
  reg("caml_ephe_create", 1, ephe_create);
  reg("caml_ephe_set_key", 3, ephe_set_key);
  reg("caml_ephe_unset_key", 2, ephe_unset_key);
  reg("caml_weak_get", 2, ephe_get_key);
  reg("caml_weak_get_copy", 2, ephe_get_key);
  reg("caml_ephe_get_key", 2, ephe_get_key);
  reg("caml_ephe_get_key_copy", 2, ephe_get_key);
  reg("caml_weak_check", 2, ephe_check_key);
  reg("caml_ephe_check_key", 2, ephe_check_key);
  reg("caml_weak_blit", 5, ephe_blit_key);
  reg("caml_ephe_blit_key", 5, ephe_blit_key);
  reg("caml_ephe_get_data", 1, ephe_get_data);
  reg("caml_ephe_get_data_copy", 1, ephe_get_data);
  reg("caml_ephe_set_data", 2, ephe_set_data);
  reg("caml_ephe_unset_data", 1, ephe_unset_data);
  reg("caml_ephe_check_data", 1, ephe_check_data);
  reg("caml_ephe_blit_data", 2, ephe_blit_data);

  // Domain and DLS (`stdlib/domain.ml`).
  auto dls_state = std::make_shared<Value>(Value::Array({}));
  reg("%dls_get", 1, [dls_state](absl::Span<const Value>) {
    return *dls_state;
  });
  reg("caml_domain_dls_get", 1, [dls_state](absl::Span<const Value>) {
    return *dls_state;
  });
  reg("caml_domain_dls_set", 1, [dls_state](absl::Span<const Value> a) {
    *dls_state = a[0];
    return Value::Unit();
  });
  reg("caml_domain_dls_compare_and_set", 2,
      [dls_state, phys_eq](absl::Span<const Value> a) {
        if (phys_eq(*dls_state, a[0])) {
          *dls_state = a[1];
          return Value::Bool(true);
        }
        return Value::Bool(false);
      });
  reg("caml_ml_domain_id", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_ml_domain_index", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_ml_domain_cpu_relax", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_recommended_domain_count", 1, [](absl::Span<const Value>) {
    return Value::Int(1);
  });
  reg("caml_domain_spawn", 2, [](absl::Span<const Value>) -> Value {
    LOG(FATAL) << "caml_domain_spawn is not supported in single-domain bootstrap.";
  });

  // Mutex and Condition (`stdlib/mutex.ml`, `stdlib/condition.ml`).
  reg("caml_ml_mutex_new", 1, [sym_contents](absl::Span<const Value>) {
    return Value::Ref(Value::Bool(false), sym_contents);
  });
  reg("caml_ml_mutex_lock", 1, [](absl::Span<const Value> a) {
    SetRefField0(a[0], Value::Bool(true));
    return Value::Unit();
  });
  reg("caml_ml_mutex_try_lock", 1, [](absl::Span<const Value> a) {
    if (GetRefField0(a[0]).AsBool()) return Value::Bool(false);
    SetRefField0(a[0], Value::Bool(true));
    return Value::Bool(true);
  });
  reg("caml_ml_mutex_unlock", 1, [](absl::Span<const Value> a) {
    SetRefField0(a[0], Value::Bool(false));
    return Value::Unit();
  });
  reg("caml_ml_condition_new", 1, [sym_contents](absl::Span<const Value>) {
    return Value::Ref(Value::Int(0), sym_contents);
  });
  reg("caml_ml_condition_wait", 2, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_ml_condition_signal", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_ml_condition_broadcast", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });

  // Gc primitives (`stdlib/gc.ml`).
  auto make_gc_stat = [&intern](absl::Span<const Value>) {
    constexpr absl::string_view kFields[] = {
        "minor_words",       "promoted_words",  "major_words",
        "minor_collections", "major_collections", "heap_words",
        "heap_chunks",       "live_words",      "live_blocks",
        "free_words",        "free_blocks",     "largest_free",
        "fragments",         "compactions",     "top_heap_words",
        "stack_size",        "forced_major_collections",
    };
    std::vector<std::pair<SymbolId, Value>> fields;
    fields.reserve(std::size(kFields));
    for (size_t i = 0; i < std::size(kFields); ++i) {
      SymbolId sym = intern.Intern(kFields[i]);
      fields.emplace_back(sym, (i < 3) ? Value::Float(0.0) : Value::Int(0));
    }
    return Value::Record(std::move(fields));
  };
  reg("caml_gc_stat", 1, make_gc_stat);
  reg("caml_gc_quick_stat", 1, make_gc_stat);
  reg("caml_gc_counters", 1, [](absl::Span<const Value>) {
    return Value::Tuple(
        {Value::Float(0.0), Value::Float(0.0), Value::Float(0.0)});
  });
  reg("caml_gc_minor_words", 1, [](absl::Span<const Value>) {
    return Value::Float(0.0);
  });
  reg("caml_gc_minor_words_unboxed", 1, [](absl::Span<const Value>) {
    return Value::Float(0.0);
  });
  reg("caml_gc_get", 1, [&intern](absl::Span<const Value>) {
    constexpr absl::string_view kFields[] = {
        "minor_heap_size",    "major_heap_increment", "space_overhead",
        "verbose",            "max_overhead",         "stack_limit",
        "allocation_policy",  "window_size",          "custom_major_ratio",
        "custom_minor_ratio", "custom_minor_max_size",
    };
    constexpr int64_t kDefaults[] = {
        262144, 15, 120, 0, 500, 1048576, 0, 1, 44, 100, 8192,
    };
    std::vector<std::pair<SymbolId, Value>> fields;
    fields.reserve(std::size(kFields));
    for (size_t i = 0; i < std::size(kFields); ++i) {
      SymbolId sym = intern.Intern(kFields[i]);
      fields.emplace_back(sym, Value::Int(kDefaults[i]));
    }
    return Value::Record(std::move(fields));
  });
  reg("caml_gc_set", 1, [](absl::Span<const Value>) { return Value::Unit(); });
  reg("caml_gc_minor", 1, [](absl::Span<const Value>) { return Value::Unit(); });
  reg("caml_gc_major_slice", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_gc_major", 1, [](absl::Span<const Value>) { return Value::Unit(); });
  reg("caml_gc_full_major", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_gc_compaction", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_get_minor_free", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_get_major_bucket", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_get_major_credit", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_gc_huge_fallback_count", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_final_register", 2, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_final_register_called_without_value", 2, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_final_release", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_memprof_start", 3, [](absl::Span<const Value>) {
    return Value::Constructor(0);
  });
  reg("caml_memprof_set", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_memprof_stop", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_memprof_discard", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_eventlog_pause", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_eventlog_resume", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });

  // Digest primitives: MD5 (`runtime/md5.c`) and BLAKE2b (`runtime/blake2.c`).
  auto md5_bytes = [](absl::Span<const Value> a) {
    const std::string& s = a[0].AsString()->data;
    int64_t ofs = a[1].AsInt();
    int64_t len = a[2].AsInt();
    MD5Context ctx;
    caml_MD5Init(&ctx);
    if (len > 0 && ofs >= 0 &&
        static_cast<size_t>(ofs + len) <= s.size()) {
      caml_MD5Update(&ctx, reinterpret_cast<const unsigned char*>(s.data() + ofs),
                     static_cast<size_t>(len));
    }
    unsigned char digest[16];
    caml_MD5Final(digest, &ctx);
    return Value::String(absl::string_view(reinterpret_cast<const char*>(digest), 16));
  };
  reg("caml_md5_string", 3, md5_bytes);
  reg("caml_md5_bytes", 3, md5_bytes);
  reg("caml_md5_chan", 2, [&intern](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    int64_t len = a[1].AsInt();
    MD5Context ctx;
    caml_MD5Init(&ctx);
    uint8_t buf[4096];
    if (len < 0) {
      while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), ch.fp);
        if (n > 0) caml_MD5Update(&ctx, buf, n);
        if (n < sizeof(buf)) break;
      }
    } else {
      size_t rem = static_cast<size_t>(len);
      while (rem > 0) {
        size_t chunk = std::min(rem, sizeof(buf));
        size_t n = std::fread(buf, 1, chunk, ch.fp);
        if (n == 0) {
          throw OcamlException(MakeExnEndOfFile(intern));
        }
        caml_MD5Update(&ctx, buf, n);
        rem -= n;
      }
    }
    unsigned char digest[16];
    caml_MD5Final(digest, &ctx);
    return Value::String(absl::string_view(reinterpret_cast<const char*>(digest), 16));
  });

  reg("caml_blake2_create", 2, [](absl::Span<const Value> a) {
    size_t hashlen =
        static_cast<size_t>(std::clamp<int64_t>(a[0].AsInt(), 1, 64));
    const std::string& key = a[1].AsString()->data;
    BLAKE2_context ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    size_t keylen = std::min<size_t>(key.size(), 64);
    caml_BLAKE2Init(&ctx, hashlen, keylen,
                    reinterpret_cast<const unsigned char*>(key.data()));
    return Value::String(
        absl::string_view(reinterpret_cast<const char*>(&ctx), sizeof(ctx)));
  });
  reg("caml_blake2_update", 4, [](absl::Span<const Value> a) {
    std::string& state_bytes = a[0].AsString()->data;
    const std::string& buf = a[1].AsString()->data;
    int64_t ofs = a[2].AsInt();
    int64_t len = a[3].AsInt();
    if (state_bytes.size() == sizeof(BLAKE2_context) && len > 0 && ofs >= 0 &&
        static_cast<size_t>(ofs + len) <= buf.size()) {
      BLAKE2_context ctx;
      std::memcpy(&ctx, state_bytes.data(), sizeof(ctx));
      caml_BLAKE2Update(&ctx, reinterpret_cast<const unsigned char*>(buf.data() + ofs),
                        static_cast<size_t>(len));
      std::memcpy(state_bytes.data(), &ctx, sizeof(ctx));
    }
    return Value::Unit();
  });
  reg("caml_blake2_final", 2, [](absl::Span<const Value> a) {
    std::string& state_bytes = a[0].AsString()->data;
    int64_t hashlen = a[1].AsInt();
    if (state_bytes.size() == sizeof(BLAKE2_context) && hashlen > 0 &&
        static_cast<size_t>(hashlen) <= 64) {
      BLAKE2_context ctx;
      std::memcpy(&ctx, state_bytes.data(), sizeof(ctx));
      std::string out(static_cast<size_t>(hashlen), '\0');
      caml_BLAKE2Final(&ctx, out.size(),
                       reinterpret_cast<unsigned char*>(out.data()));
      std::memcpy(state_bytes.data(), &ctx, sizeof(ctx));
      return Value::String(std::move(out));
    }
    return Value::String(
        std::string(static_cast<size_t>(std::max<int64_t>(0, hashlen)), '\0'));
  });
  auto blake2_direct = [](absl::Span<const Value> a) {
    int64_t hashlen = a[0].AsInt();
    const std::string& key = a[1].AsString()->data;
    const std::string& buf = a[2].AsString()->data;
    int64_t ofs = a[3].AsInt();
    int64_t len = a[4].AsInt();
    if (hashlen <= 0 || static_cast<size_t>(hashlen) > 64) {
      return Value::String("");
    }
    size_t outlen = static_cast<size_t>(hashlen);
    BLAKE2_context ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    size_t keylen = std::min<size_t>(key.size(), 64);
    caml_BLAKE2Init(&ctx, outlen, keylen,
                    reinterpret_cast<const unsigned char*>(key.data()));
    if (len > 0 && ofs >= 0 &&
        static_cast<size_t>(ofs + len) <= buf.size()) {
      caml_BLAKE2Update(&ctx, reinterpret_cast<const unsigned char*>(buf.data() + ofs),
                        static_cast<size_t>(len));
    }
    std::string out(outlen, '\0');
    caml_BLAKE2Final(&ctx, out.size(),
                     reinterpret_cast<unsigned char*>(out.data()));
    return Value::String(std::move(out));
  };
  reg("caml_blake2_string", 5, blake2_direct);
  reg("caml_blake2_bytes", 5, blake2_direct);

  // Hash primitives (`stdlib/hashtbl.ml`, `stdlib/string.ml`).
  auto hash_fn = [&intern](absl::Span<const Value> a) {
    int64_t meaningful = a[0].AsInt();
    int64_t total = a[1].AsInt();
    uint32_t seed = static_cast<uint32_t>(a[2].AsInt());
    int64_t h = HashValue(meaningful, total, seed, a[3], &intern);
    return Value::Int(h & 0x3FFFFFFFLL);
  };
  reg("caml_hash", 4, hash_fn);
  reg("caml_hash_exn", 4, hash_fn);
  reg("caml_hash_univ_param", 3, [&intern](absl::Span<const Value> a) {
    int64_t meaningful = a[0].AsInt();
    int64_t total = a[1].AsInt();
    int64_t h = HashValue(meaningful, total, 0, a[2], &intern);
    return Value::Int(h & 0x3FFFFFFFLL);
  });
  reg("caml_string_hash", 2, [&intern](absl::Span<const Value> a) {
    uint32_t seed = static_cast<uint32_t>(a[0].AsInt());
    int64_t h = HashValue(10, 100, seed, a[1], &intern);
    return Value::Int(h & 0x3FFFFFFFLL);
  });

  // Marshal primitives (`stdlib/marshal.ml` & `utils/compression.ml`).
  auto parse_marshal_flags = [](const Value& flags_list) -> int64_t {
    (void)flags_list;
    return 0;
  };
  reg("caml_output_value", 3,
      [&intern, parse_marshal_flags](absl::Span<const Value> a) {
        ChannelVal& ch = *a[0].AsChannel();
        std::string bytes =
            MarshalToString(a[1], parse_marshal_flags(a[2]), intern);
        if (!bytes.empty() && ch.fp != nullptr) {
          std::fwrite(bytes.data(), 1, bytes.size(), ch.fp);
        }
        return Value::Unit();
      });
  reg("caml_output_value_to_string", 2,
      [&intern, parse_marshal_flags](absl::Span<const Value> a) {
        return Value::String(
            MarshalToString(a[0], parse_marshal_flags(a[1]), intern));
      });
  reg("caml_output_value_to_bytes", 2,
      [&intern, parse_marshal_flags](absl::Span<const Value> a) {
        return Value::String(
            MarshalToString(a[0], parse_marshal_flags(a[1]), intern));
      });
  reg("caml_output_value_to_buffer", 5,
      [&intern, parse_marshal_flags](absl::Span<const Value> a) {
        std::string& buf = a[0].AsString()->data;
        int64_t ofs = a[1].AsInt();
        int64_t max_len = a[2].AsInt();
        std::string bytes =
            MarshalToString(a[3], parse_marshal_flags(a[4]), intern);
        if (ofs < 0 || max_len < 0 ||
            static_cast<size_t>(ofs) + bytes.size() > buf.size() ||
            bytes.size() > static_cast<size_t>(max_len)) {
          throw OcamlException(
              MakeExnFailure(intern, "Marshal.to_buffer: buffer overflow"));
        }
        std::memcpy(buf.data() + ofs, bytes.data(), bytes.size());
        return Value::Int(static_cast<int64_t>(bytes.size()));
      });
  reg("caml_input_value", 1, [&intern](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    if (ch.fp == nullptr) {
      throw OcamlException(MakeExnEndOfFile(intern));
    }
    uint8_t header[16];
    if (std::fread(header, 1, 16, ch.fp) != 16) {
      throw OcamlException(MakeExnEndOfFile(intern));
    }
    int64_t data_len = MarshalDataSize(
        absl::string_view(reinterpret_cast<const char*>(header), 16), 0,
        intern);
    std::string full(16 + static_cast<size_t>(data_len), '\0');
    std::memcpy(full.data(), header, 16);
    if (data_len > 0) {
      if (std::fread(full.data() + 16, 1, static_cast<size_t>(data_len),
                     ch.fp) != static_cast<size_t>(data_len)) {
        throw OcamlException(MakeExnEndOfFile(intern));
      }
    }
    return UnmarshalFromBytes(full, 0, intern);
  });
  auto input_val_from_bytes = [&intern](absl::Span<const Value> a) {
    return UnmarshalFromBytes(a[0].AsString()->data,
                              static_cast<size_t>(a[1].AsInt()), intern);
  };
  reg("caml_input_value_from_bytes", 2, input_val_from_bytes);
  reg("caml_input_value_from_string", 2, input_val_from_bytes);
  reg("caml_marshal_data_size", 2, [&intern](absl::Span<const Value> a) {
    return Value::Int(MarshalDataSize(a[0].AsString()->data,
                                      static_cast<size_t>(a[1].AsInt()),
                                      intern));
  });
  reg("caml_zstd_initialize", 1, [](absl::Span<const Value>) {
    return Value::Bool(false);
  });


  // =========================================================================
  // 9. Channels (`caml_ml_*`) and System (`caml_sys_*`, Backtrace, Callback)
  // =========================================================================

  auto out_channels = std::make_shared<std::vector<Value>>();
  auto channel_buffered =
      std::make_shared<absl::flat_hash_map<const void*, bool>>();

  reg("caml_ml_open_descriptor_in", 1, [](absl::Span<const Value> a) {
    int fd = static_cast<int>(a[0].AsInt());
    auto ch = std::make_shared<ChannelVal>();
    ch->fp = (fd == 0) ? stdin : fdopen(fd, "rb");
    ch->fd = fd;
    ch->is_input = true;
    ch->is_output = false;
    ch->owns_fp = (fd != 0);
    ch->binary_mode = true;
    return Value::Channel(std::move(ch));
  });

  reg("caml_ml_open_descriptor_out", 1,
      [out_channels](absl::Span<const Value> a) {
        int fd = static_cast<int>(a[0].AsInt());
        auto ch = std::make_shared<ChannelVal>();
        ch->fd = fd;
        ch->is_input = false;
        ch->is_output = true;
        ch->binary_mode = true;
        if (fd == 1) {
          ch->fp = stdout;
          ch->owns_fp = false;
        } else if (fd == 2) {
          ch->fp = stderr;
          ch->owns_fp = false;
        } else {
          ch->fp = fdopen(fd, "wb");
          ch->owns_fp = true;
        }
        Value v = Value::Channel(std::move(ch));
        out_channels->push_back(v);
        return v;
      });

  reg("caml_ml_close_channel", 1, [out_channels](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    if (ch.fp != nullptr) {
      if (ch.owns_fp && ch.fp != stdin && ch.fp != stdout && ch.fp != stderr) {
        std::fclose(ch.fp);
      } else {
        std::fflush(ch.fp);
      }
      ch.fp = nullptr;
      ch.owns_fp = false;
      ch.closed = true;
    }
    out_channels->erase(
        std::remove_if(out_channels->begin(), out_channels->end(),
                       [&a](const Value& v) {
                         return v.RawPtr() == a[0].RawPtr();
                       }),
        out_channels->end());
    return Value::Unit();
  });

  reg("caml_ml_out_channels_list", 1,
      [out_channels, &intern](absl::Span<const Value>) {
        std::vector<Value> open_list;
        for (const Value& ch : *out_channels) {
          if (ch.IsChannel() && ch.AsChannel()->fp != nullptr) {
            open_list.push_back(ch);
          }
        }
        return MakeList(intern, open_list);
      });

  reg("caml_ml_flush", 1, [](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    if (ch.fp != nullptr) {
      std::fflush(ch.fp);
    }
    return Value::Unit();
  });

  auto ml_output = [](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    const std::string& buf = a[1].AsString()->data;
    int64_t ofs = a[2].AsInt();
    int64_t len = a[3].AsInt();
    if (ch.fp != nullptr && len > 0 && ofs >= 0 &&
        static_cast<size_t>(ofs + len) <= buf.size()) {
      std::fwrite(buf.data() + ofs, 1, static_cast<size_t>(len), ch.fp);
    }
    return Value::Unit();
  };
  reg("caml_ml_output", 4, ml_output);
  reg("caml_ml_output_bytes", 4, ml_output);

  reg("caml_ml_output_bigarray", 4, [ba_bytes_const](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    const auto& ba = *a[1].AsBigarray1();
    int64_t ofs = a[2].AsInt();
    int64_t len = a[3].AsInt();
    if (ch.fp != nullptr && len > 0 && ofs >= 0 &&
        static_cast<size_t>(ofs + len) <= ba.data.size()) {
      std::fwrite(ba_bytes_const(ba) + ofs, 1, static_cast<size_t>(len), ch.fp);
    }
    return Value::Unit();
  });

  reg("caml_ml_output_char", 2, [](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    if (ch.fp != nullptr) {
      std::fputc(static_cast<int>(a[1].AsChar()), ch.fp);
    }
    return Value::Unit();
  });

  reg("caml_ml_output_int", 2, [](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    if (ch.fp != nullptr) {
      uint32_t v = static_cast<uint32_t>(a[1].AsInt());
      uint8_t b[4] = {
          static_cast<uint8_t>((v >> 24) & 0xFF),
          static_cast<uint8_t>((v >> 16) & 0xFF),
          static_cast<uint8_t>((v >> 8) & 0xFF),
          static_cast<uint8_t>(v & 0xFF),
      };
      std::fwrite(b, 1, 4, ch.fp);
    }
    return Value::Unit();
  });

  reg("caml_ml_input", 4, [](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    std::string& buf = a[1].AsString()->data;
    int64_t ofs = a[2].AsInt();
    int64_t len = a[3].AsInt();
    if (ch.fp == nullptr || len <= 0 || ofs < 0 ||
        static_cast<size_t>(ofs + len) > buf.size()) {
      return Value::Int(0);
    }
    size_t n = std::fread(buf.data() + ofs, 1, static_cast<size_t>(len), ch.fp);
    return Value::Int(static_cast<int64_t>(n));
  });

  reg("caml_ml_input_bigarray", 4, [ba_bytes_mut](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    auto& ba = *a[1].AsBigarray1();
    int64_t ofs = a[2].AsInt();
    int64_t len = a[3].AsInt();
    if (ch.fp == nullptr || len <= 0 || ofs < 0 ||
        static_cast<size_t>(ofs + len) > ba.data.size()) {
      return Value::Int(0);
    }
    size_t n =
        std::fread(ba_bytes_mut(ba) + ofs, 1, static_cast<size_t>(len), ch.fp);
    return Value::Int(static_cast<int64_t>(n));
  });

  reg("caml_ml_input_char", 1, [&intern](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    if (ch.fp == nullptr) {
      throw OcamlException(MakeExnEndOfFile(intern));
    }
    int c = std::fgetc(ch.fp);
    if (c == EOF) {
      throw OcamlException(MakeExnEndOfFile(intern));
    }
    return Value::Char(static_cast<uint8_t>(c));
  });

  reg("caml_ml_input_int", 1, [&intern](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    if (ch.fp == nullptr) {
      throw OcamlException(MakeExnEndOfFile(intern));
    }
    uint8_t b[4];
    if (std::fread(b, 1, 4, ch.fp) != 4) {
      throw OcamlException(MakeExnEndOfFile(intern));
    }
    int32_t v = static_cast<int32_t>((static_cast<uint32_t>(b[0]) << 24) |
                                     (static_cast<uint32_t>(b[1]) << 16) |
                                     (static_cast<uint32_t>(b[2]) << 8) |
                                     static_cast<uint32_t>(b[3]));
    return Value::Int(static_cast<int64_t>(v));
  });

  reg("caml_ml_input_scan_line", 1, [&intern](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    if (ch.fp == nullptr) {
      throw OcamlException(MakeExnEndOfFile(intern));
    }
    int64_t start_pos = ftello(ch.fp);
    if (start_pos < 0) {
      int c = std::fgetc(ch.fp);
      if (c == EOF) throw OcamlException(MakeExnEndOfFile(intern));
      std::ungetc(c, ch.fp);
      return Value::Int(1);
    }
    int64_t count = 0;
    bool found_nl = false;
    while (count < 4096) {
      int c = std::fgetc(ch.fp);
      if (c == EOF) break;
      ++count;
      if (c == '\n') {
        found_nl = true;
        break;
      }
    }
    fseeko(ch.fp, start_pos, SEEK_SET);
    if (count == 0) {
      throw OcamlException(MakeExnEndOfFile(intern));
    }
    return Value::Int(found_nl ? count : -count);
  });

  auto ml_seek = [&intern](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    int64_t pos = a[1].AsInt();
    if (ch.fp == nullptr || fseeko(ch.fp, pos, SEEK_SET) != 0) {
      throw OcamlException(MakeExnSysError(intern, std::strerror(errno)));
    }
    return Value::Unit();
  };
  reg("caml_ml_seek_in", 2, ml_seek);
  reg("caml_ml_seek_in_64", 2, ml_seek);
  reg("caml_ml_seek_out", 2, ml_seek);
  reg("caml_ml_seek_out_64", 2, ml_seek);

  auto ml_pos = [](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    if (ch.fp == nullptr) return Value::Int(0);
    int64_t p = ftello(ch.fp);
    return Value::Int(p < 0 ? 0 : p);
  };
  auto ml_pos64 = [](absl::Span<const Value> a) {
    ChannelVal& ch = *a[0].AsChannel();
    if (ch.fp == nullptr) return Value::Int64(0);
    int64_t p = ftello(ch.fp);
    return Value::Int64(p < 0 ? 0 : p);
  };
  reg("caml_ml_pos_in", 1, ml_pos);
  reg("caml_ml_pos_out", 1, ml_pos);
  reg("caml_ml_pos_in_64", 1, ml_pos64);
  reg("caml_ml_pos_out_64", 1, ml_pos64);

  auto ml_chan_size = [](absl::Span<const Value> a, bool as_i64) {
    ChannelVal& ch = *a[0].AsChannel();
    if (ch.fp == nullptr) {
      return as_i64 ? Value::Int64(0) : Value::Int(0);
    }
    std::fflush(ch.fp);
    struct stat st;
    int fd = fileno(ch.fp);
    int64_t sz = 0;
    if (fd >= 0 && fstat(fd, &st) == 0) {
      sz = static_cast<int64_t>(st.st_size);
    }
    return as_i64 ? Value::Int64(sz) : Value::Int(sz);
  };
  reg("caml_ml_channel_size", 1, [ml_chan_size](absl::Span<const Value> a) {
    return ml_chan_size(a, false);
  });
  reg("caml_ml_channel_size_64", 1, [ml_chan_size](absl::Span<const Value> a) {
    return ml_chan_size(a, true);
  });

  reg("caml_ml_set_binary_mode", 2, [](absl::Span<const Value> a) {
    a[0].AsChannel()->binary_mode = a[1].AsBool();
    return Value::Unit();
  });
  reg("caml_ml_is_binary_mode", 1, [](absl::Span<const Value> a) {
    return Value::Bool(a[0].AsChannel()->binary_mode);
  });
  reg("caml_ml_set_buffered", 2, [channel_buffered](absl::Span<const Value> a) {
    (*channel_buffered)[a[0].RawPtr()] = a[1].AsBool();
    return Value::Unit();
  });
  reg("caml_ml_is_buffered", 1, [channel_buffered](absl::Span<const Value> a) {
    auto it = channel_buffered->find(a[0].RawPtr());
    return Value::Bool(it == channel_buffered->end() ? true : it->second);
  });
  reg("caml_ml_set_channel_name", 2, [](absl::Span<const Value> a) {
    a[0].AsChannel()->name = a[1].AsString()->data;
    return Value::Unit();
  });

  // System primitives (`stdlib/sys.ml.in`).
  reg("caml_sys_open", 3, [&intern](absl::Span<const Value> a) {
    const std::string& path = a[0].AsString()->data;
    std::vector<Value> flags_list = ExtractList(a[1]);
    int mode = static_cast<int>(a[2].AsInt());
    int flags = 0;
    for (const Value& f : flags_list) {
      if (f.IsConstructor()) {
        absl::string_view cname = intern.Resolve(f.AsConstructor()->ctor);
        if (cname == "Open_rdonly") flags |= O_RDONLY;
        else if (cname == "Open_wronly") flags |= O_WRONLY;
        else if (cname == "Open_append") flags |= O_APPEND;
        else if (cname == "Open_creat") flags |= O_CREAT;
        else if (cname == "Open_trunc") flags |= O_TRUNC;
        else if (cname == "Open_excl") flags |= O_EXCL;
        else if (cname == "Open_nonblock") flags |= O_NONBLOCK;
      } else if (f.IsIntLike()) {
        switch (f.AsInt()) {
          case 0: flags |= O_RDONLY; break;
          case 1: flags |= O_WRONLY; break;
          case 2: flags |= O_APPEND; break;
          case 3: flags |= O_CREAT; break;
          case 4: flags |= O_TRUNC; break;
          case 5: flags |= O_EXCL; break;
          case 8: flags |= O_NONBLOCK; break;
          default: break;
        }
      }
    }
    int fd = ::open(path.c_str(), flags, mode);
    if (fd < 0) {
      throw OcamlException(
          MakeExnSysError(intern, absl::StrCat(path, ": ", std::strerror(errno))));
    }
    return Value::Int(fd);
  });

  reg("caml_sys_close", 1, [](absl::Span<const Value> a) {
    int fd = static_cast<int>(a[0].AsInt());
    if (fd > 2) ::close(fd);
    return Value::Unit();
  });

  reg("caml_sys_file_exists", 1, [](absl::Span<const Value> a) {
    struct stat st;
    return Value::Bool(stat(a[0].AsString()->data.c_str(), &st) == 0);
  });

  reg("caml_sys_is_directory", 1, [&intern](absl::Span<const Value> a) {
    const std::string& p = a[0].AsString()->data;
    struct stat st;
    if (stat(p.c_str(), &st) != 0) {
      throw OcamlException(
          MakeExnSysError(intern, absl::StrCat(p, ": ", std::strerror(errno))));
    }
    return Value::Bool(S_ISDIR(st.st_mode));
  });

  reg("caml_sys_is_regular_file", 1, [&intern](absl::Span<const Value> a) {
    const std::string& p = a[0].AsString()->data;
    struct stat st;
    if (stat(p.c_str(), &st) != 0) {
      throw OcamlException(
          MakeExnSysError(intern, absl::StrCat(p, ": ", std::strerror(errno))));
    }
    return Value::Bool(S_ISREG(st.st_mode));
  });

  reg("caml_sys_remove", 1, [&intern](absl::Span<const Value> a) {
    const std::string& p = a[0].AsString()->data;
    if (unlink(p.c_str()) != 0) {
      throw OcamlException(
          MakeExnSysError(intern, absl::StrCat(p, ": ", std::strerror(errno))));
    }
    return Value::Unit();
  });

  reg("caml_sys_rename", 2, [&intern](absl::Span<const Value> a) {
    const std::string& oldp = a[0].AsString()->data;
    const std::string& newp = a[1].AsString()->data;
    if (std::rename(oldp.c_str(), newp.c_str()) != 0) {
      throw OcamlException(
          MakeExnSysError(intern, absl::StrCat(oldp, ": ", std::strerror(errno))));
    }
    return Value::Unit();
  });

  reg("caml_sys_chdir", 1, [&intern](absl::Span<const Value> a) {
    const std::string& p = a[0].AsString()->data;
    if (chdir(p.c_str()) != 0) {
      throw OcamlException(
          MakeExnSysError(intern, absl::StrCat(p, ": ", std::strerror(errno))));
    }
    return Value::Unit();
  });

  reg("caml_sys_mkdir", 2, [&intern](absl::Span<const Value> a) {
    const std::string& p = a[0].AsString()->data;
    int mode = static_cast<int>(a[1].AsInt());
    if (mkdir(p.c_str(), mode) != 0) {
      throw OcamlException(
          MakeExnSysError(intern, absl::StrCat(p, ": ", std::strerror(errno))));
    }
    return Value::Unit();
  });

  reg("caml_sys_rmdir", 1, [&intern](absl::Span<const Value> a) {
    const std::string& p = a[0].AsString()->data;
    if (rmdir(p.c_str()) != 0) {
      throw OcamlException(
          MakeExnSysError(intern, absl::StrCat(p, ": ", std::strerror(errno))));
    }
    return Value::Unit();
  });

  reg("caml_sys_getcwd", 1, [&intern](absl::Span<const Value>) {
    char buf[4096];
    if (getcwd(buf, sizeof(buf)) == nullptr) {
      throw OcamlException(MakeExnSysError(intern, std::strerror(errno)));
    }
    return Value::String(buf);
  });

  reg("caml_sys_read_directory", 1, [&intern](absl::Span<const Value> a) {
    const std::string& p = a[0].AsString()->data;
    DIR* d = opendir(p.c_str());
    if (d == nullptr) {
      throw OcamlException(
          MakeExnSysError(intern, absl::StrCat(p, ": ", std::strerror(errno))));
    }
    std::vector<Value> entries;
    while (struct dirent* ent = readdir(d)) {
      absl::string_view name(ent->d_name);
      if (name != "." && name != "..") {
        entries.push_back(Value::String(name));
      }
    }
    closedir(d);
    return Value::Array(std::move(entries));
  });

  auto sys_getenv = [&intern](absl::Span<const Value> a) {
    const char* val = std::getenv(a[0].AsString()->data.c_str());
    if (val == nullptr) {
      throw OcamlException(MakeExnNotFound(intern));
    }
    return Value::String(val);
  };
  reg("caml_sys_getenv", 1, sys_getenv);
  reg("caml_sys_unsafe_getenv", 1, sys_getenv);

  // Note: `external argv : string array = "%sys_argv"` in `stdlib/sys.ml.in` is
  // a 0-arity value external.
  reg("%sys_argv", 0, [sys_argv_val, sync_sys_argv](absl::Span<const Value>) {
    sync_sys_argv();
    return sys_argv_val;
  });
  reg("caml_sys_argv", 1, [sys_argv_val, sync_sys_argv](absl::Span<const Value>) {
    sync_sys_argv();
    return sys_argv_val;
  });
  reg("caml_sys_get_argv", 1,
      [&eval, sys_argv_val, sync_sys_argv](absl::Span<const Value>) {
        sync_sys_argv();
        std::string exe =
            eval.argv().empty() ? "ocamlc" : eval.argv().front();
        return Value::Tuple({Value::String(exe), sys_argv_val});
      });
  reg("caml_sys_modify_argv", 1,
      [sys_argv_arr](absl::Span<const Value> a) {
        if (a[0].IsArray()) {
          sys_argv_arr->elements = a[0].AsArray()->elements;
        }
        return Value::Unit();
      });
  reg("caml_sys_executable_name", 1, [&eval](absl::Span<const Value>) {
    std::string exe = eval.argv().empty() ? "ocamlc" : eval.argv().front();
    return Value::String(exe);
  });

  auto sys_time = [](absl::Span<const Value>) {
    return Value::Float(static_cast<double>(std::clock()) / CLOCKS_PER_SEC);
  };
  reg("caml_sys_time", 1, sys_time);
  reg("caml_sys_time_unboxed", 1, sys_time);
  reg("caml_sys_time_include_children", 1, sys_time);
  reg("caml_sys_time_include_children_unboxed", 1, sys_time);

  reg("caml_sys_system_command", 1, [](absl::Span<const Value> a) {
    int rc = std::system(a[0].AsString()->data.c_str());
    if (rc == -1) return Value::Int(127);
    if (WIFEXITED(rc)) return Value::Int(WEXITSTATUS(rc));
    return Value::Int(rc);
  });

  reg("caml_sys_random_seed", 1, [](absl::Span<const Value>) {
    return Value::Array(
        {Value::Int(0x12345678), Value::Int(0x9ABCDEF0), Value::Int(0x13579BDF),
         Value::Int(0x2468ACE0)});
  });

  reg("caml_sys_get_config", 1, [](absl::Span<const Value>) {
    return Value::Tuple(
        {Value::String("Unix"), Value::Int(64), Value::Bool(false)});
  });

  auto const_false = [](absl::Span<const Value>) { return Value::Bool(false); };
  auto const_true = [](absl::Span<const Value>) { return Value::Bool(true); };
  reg("%big_endian", 1, const_false);
  reg("caml_sys_const_big_endian", 1, const_false);
  reg("%word_size", 1, [](absl::Span<const Value>) { return Value::Int(64); });
  reg("caml_sys_const_word_size", 1,
      [](absl::Span<const Value>) { return Value::Int(64); });
  reg("%int_size", 1, [](absl::Span<const Value>) { return Value::Int(63); });
  reg("caml_sys_const_int_size", 1,
      [](absl::Span<const Value>) { return Value::Int(63); });
  reg("%max_wosize", 1, [](absl::Span<const Value>) {
    return Value::Int((1LL << 54) - 1);
  });
  reg("caml_sys_const_max_wosize", 1, [](absl::Span<const Value>) {
    return Value::Int((1LL << 54) - 1);
  });
  reg("%ostype_unix", 1, const_true);
  reg("caml_sys_const_ostype_unix", 1, const_true);
  reg("%ostype_win32", 1, const_false);
  reg("caml_sys_const_ostype_win32", 1, const_false);
  reg("%ostype_cygwin", 1, const_false);
  reg("caml_sys_const_ostype_cygwin", 1, const_false);
  auto backend_type = [&intern](absl::Span<const Value>) {
    return Value::Constructor(intern.Intern("Bytecode"));
  };
  reg("%backend_type", 1, backend_type);
  reg("caml_sys_const_backend_type", 1, backend_type);
  reg("caml_sys_const_naked_pointers_checked", 1, const_false);
  reg("caml_runtime_variant", 1, [](absl::Span<const Value>) {
    return Value::String("");
  });
  reg("caml_runtime_parameters", 1, [](absl::Span<const Value>) {
    return Value::String("");
  });
  reg("caml_install_signal_handler", 2, [&intern](absl::Span<const Value>) {
    return Value::Constructor(intern.Intern("Signal_default"));
  });
  reg("caml_ml_enable_runtime_warnings", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_ml_runtime_warnings_enabled", 1, const_false);

  reg("caml_sys_isatty", 1, [](absl::Span<const Value> a) {
    if (!a[0].IsChannel() || a[0].AsChannel()->fp == nullptr) {
      return Value::Bool(false);
    }
    return Value::Bool(isatty(fileno(a[0].AsChannel()->fp)) != 0);
  });
  reg("caml_terminfo_rows", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });

  reg("caml_sys_exit", 1, [out_channels](absl::Span<const Value> a) -> Value {
    for (const Value& ch : *out_channels) {
      if (ch.IsChannel() && ch.AsChannel()->fp != nullptr) {
        std::fflush(ch.AsChannel()->fp);
      }
    }
    throw OcamlSysExit(static_cast<int>(a[0].AsInt()));
  });

  // Callback & Backtrace primitives (`stdlib/callback.ml`, `stdlib/printexc.ml`).
  reg("caml_register_named_value", 2, [&eval](absl::Span<const Value> a) {
    if (a[0].IsString()) {
      eval.RegisterNamedValue(a[0].AsString()->data, a[1]);
    }
    return Value::Unit();
  });
  auto backtrace_active = std::make_shared<bool>(false);
  reg("caml_record_backtrace", 1, [backtrace_active](absl::Span<const Value> a) {
    *backtrace_active = a[0].AsBool();
    return Value::Unit();
  });
  reg("caml_backtrace_status", 1, [backtrace_active](absl::Span<const Value>) {
    return Value::Bool(*backtrace_active);
  });
  reg("caml_get_exception_raw_backtrace", 1, [](absl::Span<const Value>) {
    return Value::Array({});
  });
  reg("caml_get_exception_backtrace", 1, [&intern](absl::Span<const Value>) {
    return MakeNone(intern);
  });
  reg("caml_raw_backtrace_length", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_raw_backtrace_slot", 2, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_raw_backtrace_next_slot", 1, [&intern](absl::Span<const Value>) {
    return MakeNone(intern);
  });
  reg("caml_convert_raw_backtrace", 1, [](absl::Span<const Value>) {
    return Value::Array({});
  });
  reg("caml_convert_raw_backtrace_slot", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_get_current_callstack", 1, [](absl::Span<const Value>) {
    return Value::Array({});
  });
  reg("caml_ml_debug_info_status", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_restore_raw_backtrace", 2, [](absl::Span<const Value>) {
    return Value::Unit();
  });

  // Effect, Dynlink, and Meta stubs (`stdlib/effect.ml`, `bytecomp/symtable.ml`).
  reg("%perform", 1, [](absl::Span<const Value>) -> Value {
    LOG(FATAL) << "%perform is not supported in bootstrap.";
  });
  reg("%reperform", 3, [](absl::Span<const Value>) -> Value {
    LOG(FATAL) << "%reperform is not supported in bootstrap.";
  });
  reg("%resume", 4, [](absl::Span<const Value>) -> Value {
    LOG(FATAL) << "%resume is not supported in bootstrap.";
  });
  reg("%runstack", 3, [](absl::Span<const Value>) -> Value {
    LOG(FATAL) << "%runstack is not supported in bootstrap.";
  });
  reg("caml_continuation_use_noexc", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_continuation_use_and_update_handler_noexc", 4,
      [](absl::Span<const Value>) { return Value::Unit(); });
  reg("caml_alloc_stack", 3, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_get_continuation_callstack", 2, [](absl::Span<const Value>) {
    return Value::Array({});
  });

  reg("caml_dynlink_open_lib", 2, [](absl::Span<const Value>) {
    return Value::Nativeint(0);
  });
  reg("caml_dynlink_close_lib", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_dynlink_lookup_symbol", 2, [](absl::Span<const Value>) {
    return Value::Nativeint(0);
  });
  reg("caml_dynlink_add_primitive", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_dynlink_get_current_libs", 1, [](absl::Span<const Value>) {
    return Value::Array({});
  });
  reg("caml_dynlink_get_bytecode_sections", 1,
      [&intern](absl::Span<const Value>) {
        Value empty_list = MakeList(intern, {});
        Value empty_gmap = Value::Record(
            {{intern.Intern("cnt"), Value::Int(0)},
             {intern.Intern("tbl"), Value::Constructor(intern.Intern("Empty"))}});
        return Value::Record({{intern.Intern("symb"), empty_gmap},
                              {intern.Intern("crcs"), empty_list},
                              {intern.Intern("prim"), empty_list},
                              {intern.Intern("dlpt"), empty_list}});
      });
  reg("caml_get_global_data", 1, [](absl::Span<const Value>) {
    return Value::Array({});
  });
  reg("caml_realloc_global", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_get_section_table", 1, [&intern](absl::Span<const Value>) {
    return MakeList(intern, {});
  });
  reg("caml_reify_bytecode", 4, [](absl::Span<const Value>) {
    return Value::Tuple({Value::Nativeint(0), Value::Unit()});
  });
  reg("caml_static_release_bytecode", 2, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_invoke_traced_function", 3, [](absl::Span<const Value>) -> Value {
    LOG(FATAL) << "caml_invoke_traced_function is not supported.";
  });

  // =========================================================================
  // 10. Lexer and Parser Table Engines (`runtime/lexing.c`, `runtime/parsing.c`)
  // =========================================================================

  SymbolId sym_lex_base = intern.Intern("lex_base");
  SymbolId sym_lex_backtrk = intern.Intern("lex_backtrk");
  SymbolId sym_lex_default = intern.Intern("lex_default");
  SymbolId sym_lex_trans = intern.Intern("lex_trans");
  SymbolId sym_lex_check = intern.Intern("lex_check");
  SymbolId sym_lex_base_code = intern.Intern("lex_base_code");
  SymbolId sym_lex_backtrk_code = intern.Intern("lex_backtrk_code");
  SymbolId sym_lex_default_code = intern.Intern("lex_default_code");
  SymbolId sym_lex_trans_code = intern.Intern("lex_trans_code");
  SymbolId sym_lex_check_code = intern.Intern("lex_check_code");
  SymbolId sym_lex_code = intern.Intern("lex_code");

  SymbolId sym_lex_buffer = intern.Intern("lex_buffer");
  SymbolId sym_lex_buffer_len = intern.Intern("lex_buffer_len");
  SymbolId sym_lex_start_pos = intern.Intern("lex_start_pos");
  SymbolId sym_lex_curr_pos = intern.Intern("lex_curr_pos");
  SymbolId sym_lex_last_pos = intern.Intern("lex_last_pos");
  SymbolId sym_lex_last_action = intern.Intern("lex_last_action");
  SymbolId sym_lex_eof_reached = intern.Intern("lex_eof_reached");
  SymbolId sym_lex_mem = intern.Intern("lex_mem");

  auto get_rec_field = [](RecordVal& r, SymbolId sym,
                          size_t fallback_idx) -> Value& {
    if (Value* p = r.FindMutable(sym)) return *p;
    CHECK_LT(fallback_idx, r.fields.size());
    return r.fields[fallback_idx].second;
  };

  auto run_lex_engine = [=, &intern](const Value& tbl_val, int64_t start_state,
                                     const Value& buf_val,
                                     bool is_new_engine) -> Value {
    RecordVal& tbl = *tbl_val.AsRecord();
    RecordVal& buf = *buf_val.AsRecord();

    const std::string& lex_base = get_rec_field(tbl, sym_lex_base, 0).AsString()->data;
    const std::string& lex_backtrk = get_rec_field(tbl, sym_lex_backtrk, 1).AsString()->data;
    const std::string& lex_default = get_rec_field(tbl, sym_lex_default, 2).AsString()->data;
    const std::string& lex_trans = get_rec_field(tbl, sym_lex_trans, 3).AsString()->data;
    const std::string& lex_check = get_rec_field(tbl, sym_lex_check, 4).AsString()->data;
    const std::string& lex_base_code = get_rec_field(tbl, sym_lex_base_code, 5).AsString()->data;
    const std::string& lex_backtrk_code = get_rec_field(tbl, sym_lex_backtrk_code, 6).AsString()->data;
    const std::string& lex_default_code = get_rec_field(tbl, sym_lex_default_code, 7).AsString()->data;
    const std::string& lex_trans_code = get_rec_field(tbl, sym_lex_trans_code, 8).AsString()->data;
    const std::string& lex_check_code = get_rec_field(tbl, sym_lex_check_code, 9).AsString()->data;
    const std::string& lex_code = get_rec_field(tbl, sym_lex_code, 10).AsString()->data;

    int state = static_cast<int>(start_state);
    if (state >= 0) {
      Value curr_pos = get_rec_field(buf, sym_lex_curr_pos, 4);
      get_rec_field(buf, sym_lex_last_pos, 5) = curr_pos;
      get_rec_field(buf, sym_lex_start_pos, 3) = curr_pos;
      get_rec_field(buf, sym_lex_last_action, 6) = Value::Int(-1);
    } else {
      state = -state - 1;
    }

    while (true) {
      int base = ReadLeShort(lex_base, state);
      if (base < 0) {
        if (is_new_engine) {
          int pc_off = ReadLeUShort(lex_base_code, state);
          RunLexTag(lex_code, static_cast<size_t>(pc_off),
                    get_rec_field(buf, sym_lex_mem, 8).AsArray()->elements);
        }
        return Value::Int(-base - 1);
      }

      int backtrk = ReadLeShort(lex_backtrk, state);
      if (backtrk >= 0) {
        if (is_new_engine) {
          int pc_off = ReadLeUShort(lex_backtrk_code, state);
          RunLexTag(lex_code, static_cast<size_t>(pc_off),
                    get_rec_field(buf, sym_lex_mem, 8).AsArray()->elements);
        }
        get_rec_field(buf, sym_lex_last_pos, 5) =
            get_rec_field(buf, sym_lex_curr_pos, 4);
        get_rec_field(buf, sym_lex_last_action, 6) = Value::Int(backtrk);
      }

      int64_t curr_pos = get_rec_field(buf, sym_lex_curr_pos, 4).AsInt();
      int64_t buf_len = get_rec_field(buf, sym_lex_buffer_len, 2).AsInt();
      if (curr_pos >= buf_len) {
        if (!get_rec_field(buf, sym_lex_eof_reached, 7).AsBool()) {
          return Value::Int(-state - 1);
        } else {
          int c = 256;
          int pstate = state;
          if (ReadLeShort(lex_check, base + c) == state) {
            state = ReadLeShort(lex_trans, base + c);
          } else {
            state = ReadLeShort(lex_default, state);
          }
          if (state < 0) {
            get_rec_field(buf, sym_lex_curr_pos, 4) =
                get_rec_field(buf, sym_lex_last_pos, 5);
            int64_t act = get_rec_field(buf, sym_lex_last_action, 6).AsInt();
            if (act == -1) {
              throw OcamlException(
                  MakeExnFailure(intern, "lexing: empty token"));
            }
            return Value::Int(act);
          }
          if (is_new_engine) {
            int base_code = ReadLeUShort(lex_base_code, pstate);
            int pc_off = (ReadLeShort(lex_check_code, base_code + c) == pstate)
                             ? ReadLeUShort(lex_trans_code, base_code + c)
                             : ReadLeUShort(lex_default_code, pstate);
            if (pc_off > 0) {
              RunLexMem(
                  lex_code, static_cast<size_t>(pc_off),
                  get_rec_field(buf, sym_lex_mem, 8).AsArray()->elements,
                  get_rec_field(buf, sym_lex_curr_pos, 4).AsInt());
            }
          }
          get_rec_field(buf, sym_lex_eof_reached, 7) = Value::Bool(false);
        }
      } else {
        const std::string& lex_buffer =
            get_rec_field(buf, sym_lex_buffer, 1).AsString()->data;
        int c = static_cast<uint8_t>(lex_buffer[curr_pos]);
        get_rec_field(buf, sym_lex_curr_pos, 4) = Value::Int(curr_pos + 1);
        int pstate = state;
        if (ReadLeShort(lex_check, base + c) == state) {
          state = ReadLeShort(lex_trans, base + c);
        } else {
          state = ReadLeShort(lex_default, state);
        }
        if (state < 0) {
          get_rec_field(buf, sym_lex_curr_pos, 4) =
              get_rec_field(buf, sym_lex_last_pos, 5);
          int64_t act = get_rec_field(buf, sym_lex_last_action, 6).AsInt();
          if (act == -1) {
            throw OcamlException(
                MakeExnFailure(intern, "lexing: empty token"));
          }
          return Value::Int(act);
        }
        if (is_new_engine) {
          int base_code = ReadLeUShort(lex_base_code, pstate);
          int pc_off = (ReadLeShort(lex_check_code, base_code + c) == pstate)
                           ? ReadLeUShort(lex_trans_code, base_code + c)
                           : ReadLeUShort(lex_default_code, pstate);
          if (pc_off > 0) {
            RunLexMem(
                lex_code, static_cast<size_t>(pc_off),
                get_rec_field(buf, sym_lex_mem, 8).AsArray()->elements,
                get_rec_field(buf, sym_lex_curr_pos, 4).AsInt());
          }
        }
      }
    }
  };

  reg("caml_lex_engine", 3, [run_lex_engine](absl::Span<const Value> a) {
    return run_lex_engine(a[0], a[1].AsInt(), a[2], /*is_new_engine=*/false);
  });
  reg("caml_new_lex_engine", 3, [run_lex_engine](absl::Span<const Value> a) {
    return run_lex_engine(a[0], a[1].AsInt(), a[2], /*is_new_engine=*/true);
  });

  // Parser engine (`runtime/parsing.c` & `stdlib/parsing.ml`).
  SymbolId sym_defred = intern.Intern("defred");
  SymbolId sym_sindex = intern.Intern("sindex");
  SymbolId sym_rindex = intern.Intern("rindex");
  SymbolId sym_gindex = intern.Intern("gindex");
  SymbolId sym_tablesize = intern.Intern("tablesize");
  SymbolId sym_table = intern.Intern("table");
  SymbolId sym_check = intern.Intern("check");
  SymbolId sym_dgoto = intern.Intern("dgoto");
  SymbolId sym_lhs = intern.Intern("lhs");
  SymbolId sym_len = intern.Intern("len");
  SymbolId sym_transl_const = intern.Intern("transl_const");
  SymbolId sym_transl_block = intern.Intern("transl_block");

  SymbolId sym_s_stack = intern.Intern("s_stack");
  SymbolId sym_v_stack = intern.Intern("v_stack");
  SymbolId sym_symb_start_stack = intern.Intern("symb_start_stack");
  SymbolId sym_symb_end_stack = intern.Intern("symb_end_stack");
  SymbolId sym_stacksize = intern.Intern("stacksize");
  SymbolId sym_stackbase = intern.Intern("stackbase");
  SymbolId sym_curr_char = intern.Intern("curr_char");
  SymbolId sym_lval = intern.Intern("lval");
  SymbolId sym_symb_start = intern.Intern("symb_start");
  SymbolId sym_symb_end = intern.Intern("symb_end");
  SymbolId sym_asp = intern.Intern("asp");
  SymbolId sym_rule_len = intern.Intern("rule_len");
  SymbolId sym_rule_number = intern.Intern("rule_number");
  SymbolId sym_sp = intern.Intern("sp");
  SymbolId sym_state = intern.Intern("state");
  SymbolId sym_errflag = intern.Intern("errflag");

  SymbolId sym_Start = intern.Intern("Start");
  SymbolId sym_Token_read = intern.Intern("Token_read");
  SymbolId sym_Stacks_grown_1 = intern.Intern("Stacks_grown_1");
  SymbolId sym_Stacks_grown_2 = intern.Intern("Stacks_grown_2");
  SymbolId sym_Semantic_action_computed =
      intern.Intern("Semantic_action_computed");
  SymbolId sym_Error_detected = intern.Intern("Error_detected");

  SymbolId sym_Read_token = intern.Intern("Read_token");
  SymbolId sym_Raise_parse_error = intern.Intern("Raise_parse_error");
  SymbolId sym_Grow_stacks_1 = intern.Intern("Grow_stacks_1");
  SymbolId sym_Grow_stacks_2 = intern.Intern("Grow_stacks_2");
  SymbolId sym_Compute_semantic_action =
      intern.Intern("Compute_semantic_action");
  SymbolId sym_Call_error_function = intern.Intern("Call_error_function");

  auto parse_cmd_kind = [=](const Value& cmd) -> int {
    if (cmd.IsIntLike()) return static_cast<int>(cmd.AsInt());
    if (cmd.IsConstructor()) {
      SymbolId c = cmd.AsConstructor()->ctor;
      if (c == sym_Start || c == sym_Read_token) return 0;
      if (c == sym_Token_read || c == sym_Raise_parse_error) return 1;
      if (c == sym_Stacks_grown_1 || c == sym_Grow_stacks_1) return 2;
      if (c == sym_Stacks_grown_2 || c == sym_Grow_stacks_2) return 3;
      if (c == sym_Semantic_action_computed ||
          c == sym_Compute_semantic_action) {
        return 4;
      }
      if (c == sym_Error_detected || c == sym_Call_error_function) return 5;
      return cmd.AsConstructor()->tag;
    }
    return 0;
  };

  auto resolve_const_token_tag = [&intern](const ConstructorVal& c) -> int {
    if (c.tag != 0) return c.tag;
    absl::string_view name = intern.Resolve(c.ctor);
    static constexpr absl::string_view kLexConstTokens[] = {
        "Trule",      "Tparse",   "Tparse_shortest", "Tand",
        "Tequal",     "Tend",     "Tor",             "Tunderscore",
        "Teof",       "Tlbracket","Trbracket",       "Trefill",
        "Tstar",      "Tmaybe",   "Tplus",           "Tlparen",
        "Trparen",    "Tcaret",   "Tdash",           "Tlet",
        "Tas",        "Thash",
    };
    for (size_t i = 0; i < std::size(kLexConstTokens); ++i) {
      if (name == kLexConstTokens[i]) return static_cast<int>(i);
    }
    return c.tag;
  };

  constexpr int kErrTab = 256;
  enum class ParseResumePoint {
    kStart = 0,
    kAfterGrow1,
    kAfterReadToken,
    kAfterErrorFunction,
    kAfterGrow2,
    kAfterSemanticAction,
  };
  struct SavedParserFrame {
    ParseResumePoint resume = ParseResumePoint::kStart;
    int state = 0;
    int sp = 0;
    int errflag = 0;
    int n = 0;
    int n1 = 0;
    int n2 = 0;
    int m = 0;
    int state1 = 0;
  };
  auto parser_frames =
      std::make_shared<absl::flat_hash_map<const void*, SavedParserFrame>>();

  reg("caml_parse_engine", 4,
      [=, &intern](absl::Span<const Value> a) -> Value {
        RecordVal& tables = *a[0].AsRecord();
        RecordVal& env = *a[1].AsRecord();
        int cmd = parse_cmd_kind(a[2]);
        const Value& arg = a[3];

        const std::string& defred =
            get_rec_field(tables, sym_defred, 2).AsString()->data;
        const std::string& sindex =
            get_rec_field(tables, sym_sindex, 3).AsString()->data;
        const std::string& rindex =
            get_rec_field(tables, sym_rindex, 4).AsString()->data;
        const std::string& gindex =
            get_rec_field(tables, sym_gindex, 5).AsString()->data;
        int tablesize =
            static_cast<int>(get_rec_field(tables, sym_tablesize, 6).AsInt());
        const std::string& table =
            get_rec_field(tables, sym_table, 7).AsString()->data;
        const std::string& check =
            get_rec_field(tables, sym_check, 8).AsString()->data;
        const std::string& dgoto =
            get_rec_field(tables, sym_dgoto, 9).AsString()->data;
        const std::string& lhs =
            get_rec_field(tables, sym_lhs, 10).AsString()->data;
        const std::string& len =
            get_rec_field(tables, sym_len, 11).AsString()->data;

        SavedParserFrame& f = (*parser_frames)[&env];
        int state = f.state;
        int sp = f.sp;
        int errflag = f.errflag;
        int n = f.n;
        int n1 = f.n1;
        int n2 = f.n2;
        int m = f.m;
        int state1 = f.state1;

        auto save_and_return = [&](ParseResumePoint pt, SymbolId ret_ctor) {
          get_rec_field(env, sym_sp, 13) = Value::Int(sp);
          get_rec_field(env, sym_state, 14) = Value::Int(state);
          get_rec_field(env, sym_errflag, 15) = Value::Int(errflag);
          f.resume = pt;
          f.state = state;
          f.sp = sp;
          f.errflag = errflag;
          f.n = n;
          f.n1 = n1;
          f.n2 = n2;
          f.m = m;
          f.state1 = state1;
          return Value::Constructor(ret_ctor);
        };

        switch (cmd) {
          case 0:  // Start.
            state = 0;
            sp = static_cast<int>(get_rec_field(env, sym_sp, 13).AsInt());
            errflag = 0;
            goto loop;

          case 1:  // Token_read.
            sp = static_cast<int>(get_rec_field(env, sym_sp, 13).AsInt());
            state = static_cast<int>(get_rec_field(env, sym_state, 14).AsInt());
            errflag =
                static_cast<int>(get_rec_field(env, sym_errflag, 15).AsInt());
            if (arg.IsConstructor()) {
              const auto& c = arg.AsConstructor();
              if (!c->arg.has_value()) {
                const auto& tc =
                    get_rec_field(tables, sym_transl_const, 0).AsArray()->elements;
                int tag = resolve_const_token_tag(*c);
                CHECK_GE(tag, 0);
                CHECK_LT(static_cast<size_t>(tag), tc.size());
                get_rec_field(env, sym_curr_char, 6) = tc[tag];
                get_rec_field(env, sym_lval, 7) = Value::Int(0);
              } else {
                const auto& tb =
                    get_rec_field(tables, sym_transl_block, 1).AsArray()->elements;
                int tag = ObjTag(arg, intern);
                CHECK_GE(tag, 0);
                CHECK_LT(static_cast<size_t>(tag), tb.size());
                get_rec_field(env, sym_curr_char, 6) = tb[tag];
                get_rec_field(env, sym_lval, 7) = *c->arg;
              }
            } else if (arg.IsIntLike()) {
              const auto& tc =
                  get_rec_field(tables, sym_transl_const, 0).AsArray()->elements;
              int tag = static_cast<int>(arg.AsInt());
              CHECK_GE(tag, 0);
              CHECK_LT(static_cast<size_t>(tag), tc.size());
              get_rec_field(env, sym_curr_char, 6) = tc[tag];
              get_rec_field(env, sym_lval, 7) = Value::Int(0);
            }
            goto after_read_token;

          case 2:  // Stacks_grown_1.
            sp = static_cast<int>(get_rec_field(env, sym_sp, 13).AsInt());
            state = static_cast<int>(get_rec_field(env, sym_state, 14).AsInt());
            errflag =
                static_cast<int>(get_rec_field(env, sym_errflag, 15).AsInt());
            goto after_grow_1;

          case 3:  // Stacks_grown_2.
            sp = static_cast<int>(get_rec_field(env, sym_sp, 13).AsInt());
            state = static_cast<int>(get_rec_field(env, sym_state, 14).AsInt());
            errflag =
                static_cast<int>(get_rec_field(env, sym_errflag, 15).AsInt());
            goto after_grow_2;

          case 4:  // Semantic_action_computed.
            sp = static_cast<int>(get_rec_field(env, sym_sp, 13).AsInt());
            state = static_cast<int>(get_rec_field(env, sym_state, 14).AsInt());
            errflag =
                static_cast<int>(get_rec_field(env, sym_errflag, 15).AsInt());
            get_rec_field(env, sym_s_stack, 0).AsArray()->elements[sp] =
                Value::Int(state);
            get_rec_field(env, sym_v_stack, 1).AsArray()->elements[sp] = arg;
            get_rec_field(env, sym_symb_start_stack, 2).AsArray()->elements[sp] =
                get_rec_field(env, sym_symb_start, 8);
            get_rec_field(env, sym_symb_end_stack, 3).AsArray()->elements[sp] =
                get_rec_field(env, sym_symb_end, 9);
            goto loop;

          case 5:  // Error_detected.
            sp = static_cast<int>(get_rec_field(env, sym_sp, 13).AsInt());
            state = static_cast<int>(get_rec_field(env, sym_state, 14).AsInt());
            errflag =
                static_cast<int>(get_rec_field(env, sym_errflag, 15).AsInt());
            goto recover;

          default:
            return Value::Constructor(sym_Raise_parse_error);
        }

      loop:
        n = ReadLeShort(defred, state);
        if (n != 0) goto reduce;
        if (get_rec_field(env, sym_curr_char, 6).AsInt() >= 0) goto testshift;
        return save_and_return(ParseResumePoint::kAfterReadToken,
                               sym_Read_token);

      after_read_token:
      testshift:
        n1 = ReadLeShort(sindex, state);
        n2 = n1 + static_cast<int>(get_rec_field(env, sym_curr_char, 6).AsInt());
        if (n1 != 0 && n2 >= 0 && n2 <= tablesize &&
            ReadLeShort(check, n2) ==
                get_rec_field(env, sym_curr_char, 6).AsInt()) {
          goto shift;
        }
        n1 = ReadLeShort(rindex, state);
        n2 = n1 + static_cast<int>(get_rec_field(env, sym_curr_char, 6).AsInt());
        if (n1 != 0 && n2 >= 0 && n2 <= tablesize &&
            ReadLeShort(check, n2) ==
                get_rec_field(env, sym_curr_char, 6).AsInt()) {
          n = ReadLeShort(table, n2);
          goto reduce;
        }
        if (errflag > 0) goto recover;
        return save_and_return(ParseResumePoint::kAfterErrorFunction,
                               sym_Call_error_function);

      recover:
        if (errflag < 3) {
          errflag = 3;
          while (true) {
            state1 = static_cast<int>(
                get_rec_field(env, sym_s_stack, 0).AsArray()->elements[sp].AsInt());
            n1 = ReadLeShort(sindex, state1);
            n2 = n1 + kErrTab;
            if (n1 != 0 && n2 >= 0 && n2 <= tablesize &&
                ReadLeShort(check, n2) == kErrTab) {
              goto shift_recover;
            }
            if (sp <=
                static_cast<int>(get_rec_field(env, sym_stackbase, 5).AsInt())) {
              return Value::Constructor(sym_Raise_parse_error);
            }
            sp--;
          }
        } else {
          if (get_rec_field(env, sym_curr_char, 6).AsInt() == 0) {
            return Value::Constructor(sym_Raise_parse_error);
          }
          get_rec_field(env, sym_curr_char, 6) = Value::Int(-1);
          goto loop;
        }

      shift:
        get_rec_field(env, sym_curr_char, 6) = Value::Int(-1);
        if (errflag > 0) errflag--;
      shift_recover:
        state = ReadLeShort(table, n2);
        sp++;
        if (sp <
            static_cast<int>(get_rec_field(env, sym_stacksize, 4).AsInt())) {
          goto push;
        }
        return save_and_return(ParseResumePoint::kAfterGrow1,
                               sym_Grow_stacks_1);

      after_grow_1:
      push:
        get_rec_field(env, sym_s_stack, 0).AsArray()->elements[sp] =
            Value::Int(state);
        get_rec_field(env, sym_v_stack, 1).AsArray()->elements[sp] =
            get_rec_field(env, sym_lval, 7);
        get_rec_field(env, sym_symb_start_stack, 2).AsArray()->elements[sp] =
            get_rec_field(env, sym_symb_start, 8);
        get_rec_field(env, sym_symb_end_stack, 3).AsArray()->elements[sp] =
            get_rec_field(env, sym_symb_end, 9);
        goto loop;

      reduce:
        m = ReadLeShort(len, n);
        get_rec_field(env, sym_asp, 10) = Value::Int(sp);
        get_rec_field(env, sym_rule_number, 12) = Value::Int(n);
        get_rec_field(env, sym_rule_len, 11) = Value::Int(m);
        sp = sp - m + 1;
        m = ReadLeShort(lhs, n);
        state1 = static_cast<int>(
            get_rec_field(env, sym_s_stack, 0).AsArray()->elements[sp - 1].AsInt());
        n1 = ReadLeShort(gindex, m);
        n2 = n1 + state1;
        if (n1 != 0 && n2 >= 0 && n2 <= tablesize &&
            ReadLeShort(check, n2) == state1) {
          state = ReadLeShort(table, n2);
        } else {
          state = ReadLeShort(dgoto, m);
        }
        if (sp <
            static_cast<int>(get_rec_field(env, sym_stacksize, 4).AsInt())) {
          goto semantic_action;
        }
        return save_and_return(ParseResumePoint::kAfterGrow2,
                               sym_Grow_stacks_2);

      after_grow_2:
      semantic_action:
        return save_and_return(ParseResumePoint::kAfterSemanticAction,
                               sym_Compute_semantic_action);
      });

  reg("caml_set_parser_trace", 1, [](absl::Span<const Value>) {
    return Value::Bool(false);
  });

  // Additional OCaml 5.7+ standard library and compiler primitives.
  reg("caml_sys_proc_self_exe", 1, [&intern](absl::Span<const Value>) {
    return MakeNone(intern);
  });
  reg("caml_sys_io_buffer_size", 1, [](absl::Span<const Value>) {
    return Value::Int(65536);
  });
  reg("caml_sys_filepath_exists", 1, [](absl::Span<const Value> a) {
    struct stat st;
    return Value::Bool(::stat(a[0].AsString()->data.c_str(), &st) == 0);
  });
  reg("caml_sys_getenv_opt", 1, [&intern](absl::Span<const Value> a) {
    const char* v = std::getenv(a[0].AsString()->data.c_str());
    if (v == nullptr) return MakeNone(intern);
    return MakeSome(intern, Value::String(v));
  });
  reg("caml_sys_temp_dir_name", 1, [](absl::Span<const Value>) {
    return Value::String("/tmp");
  });
  auto stdlib_default_fn = [](absl::Span<const Value>) {
    return Value::String(".");
  };
  reg("%standard_library_default", 1, stdlib_default_fn);
  reg("caml_sys_const_standard_library_default", 1, stdlib_default_fn);
  reg("caml_sys_get_stdlib_dirs", 1, [&intern](absl::Span<const Value> a) {
    return Value::Tuple({a[0], MakeNone(intern)});
  });
  reg("caml_sys_convert_signal_number", 1, [](absl::Span<const Value> a) {
    return a[0];
  });
  reg("caml_sys_rev_convert_signal_number", 1, [](absl::Span<const Value> a) {
    return a[0];
  });
  reg("caml_uniform_array_make", 2, [](absl::Span<const Value> a) {
    int64_t len = std::max<int64_t>(0, a[0].AsInt());
    std::vector<Value> elems(static_cast<size_t>(len), a[1]);
    return Value::Array(std::move(elems));
  });
  reg("%check_array_bound", 2, [&intern](absl::Span<const Value> a) {
    int64_t idx = a[1].AsInt();
    int64_t len = static_cast<int64_t>(a[0].AsArray()->elements.size());
    if (idx < 0 || idx >= len) {
      throw OcamlException(
          MakeExnInvalidArgument(intern, "index out of bounds"));
    }
    return Value::Unit();
  });
  reg("%atomic_unsafe_index", 2, [](absl::Span<const Value> a) {
    return Value::Tuple({a[0], a[1]});
  });
  reg("%atomic_load_loc", 1, [&intern](absl::Span<const Value> a) {
    if (a[0].IsTuple()) {
      const auto& elems = a[0].AsTuple()->elements;
      if (elems[0].IsRecord()) {
        SymbolId f = intern.Intern(elems[1].AsString()->data);
        const Value* v = elems[0].AsRecord()->Find(f);
        CHECK(v != nullptr);
        return *v;
      }
      if (elems[0].IsArray()) {
        return elems[0].AsArray()->elements.at(
            static_cast<size_t>(elems[1].AsInt()));
      }
    }
    const Value* v = a[0].AsRecord()->Find(intern.Intern("contents"));
    CHECK(v != nullptr);
    return *v;
  });
  reg("%atomic_exchange_loc", 2, [&intern](absl::Span<const Value> a) {
    if (a[0].IsTuple()) {
      const auto& elems = a[0].AsTuple()->elements;
      if (elems[0].IsRecord()) {
        SymbolId f = intern.Intern(elems[1].AsString()->data);
        const Value* cur = elems[0].AsRecord()->Find(f);
        CHECK(cur != nullptr);
        Value old = *cur;
        elems[0].AsRecord()->Set(f, a[1]);
        return old;
      }
      if (elems[0].IsArray()) {
        size_t idx = static_cast<size_t>(elems[1].AsInt());
        Value old = elems[0].AsArray()->elements.at(idx);
        elems[0].AsArray()->elements[idx] = a[1];
        return old;
      }
    }
    SymbolId f = intern.Intern("contents");
    const Value* cur = a[0].AsRecord()->Find(f);
    CHECK(cur != nullptr);
    Value old = *cur;
    a[0].AsRecord()->Set(f, a[1]);
    return old;
  });
  reg("%atomic_cas_loc", 3, [&intern](absl::Span<const Value> a) {
    if (a[0].IsTuple()) {
      const auto& elems = a[0].AsTuple()->elements;
      if (elems[0].IsRecord()) {
        SymbolId f = intern.Intern(elems[1].AsString()->data);
        const Value* cur = elems[0].AsRecord()->Find(f);
        CHECK(cur != nullptr);
        if (EqualValues(*cur, a[1])) {
          elems[0].AsRecord()->Set(f, a[2]);
          return Value::Bool(true);
        }
        return Value::Bool(false);
      }
      if (elems[0].IsArray()) {
        size_t idx = static_cast<size_t>(elems[1].AsInt());
        if (EqualValues(elems[0].AsArray()->elements.at(idx), a[1])) {
          elems[0].AsArray()->elements[idx] = a[2];
          return Value::Bool(true);
        }
        return Value::Bool(false);
      }
    }
    SymbolId f = intern.Intern("contents");
    const Value* cur = a[0].AsRecord()->Find(f);
    CHECK(cur != nullptr);
    if (EqualValues(*cur, a[1])) {
      a[0].AsRecord()->Set(f, a[2]);
      return Value::Bool(true);
    }
    return Value::Bool(false);
  });
  reg("%atomic_fetch_add_loc", 2, [&intern](absl::Span<const Value> a) {
    int64_t delta = a[1].AsInt();
    if (a[0].IsTuple()) {
      const auto& elems = a[0].AsTuple()->elements;
      if (elems[0].IsRecord()) {
        SymbolId f = intern.Intern(elems[1].AsString()->data);
        const Value* cur = elems[0].AsRecord()->Find(f);
        CHECK(cur != nullptr);
        Value old = *cur;
        elems[0].AsRecord()->Set(
            f, Value::Int(SignExtend63(static_cast<uint64_t>(old.AsInt()) +
                                       static_cast<uint64_t>(delta))));
        return old;
      }
      if (elems[0].IsArray()) {
        size_t idx = static_cast<size_t>(elems[1].AsInt());
        Value old = elems[0].AsArray()->elements.at(idx);
        elems[0].AsArray()->elements[idx] =
            Value::Int(SignExtend63(static_cast<uint64_t>(old.AsInt()) +
                                    static_cast<uint64_t>(delta)));
        return old;
      }
    }
    SymbolId f = intern.Intern("contents");
    const Value* cur = a[0].AsRecord()->Find(f);
    CHECK(cur != nullptr);
    Value old = *cur;
    a[0].AsRecord()->Set(
        f, Value::Int(SignExtend63(static_cast<uint64_t>(old.AsInt()) +
                                   static_cast<uint64_t>(delta))));
    return old;
  });
  reg("%int32_to_float", 1, [](absl::Span<const Value> a) {
    return Value::Float(static_cast<double>(a[0].AsInt32()));
  });
  reg("%int64_of_float", 1, [](absl::Span<const Value> a) {
    return Value::Int64(static_cast<int64_t>(a[0].AsFloat()));
  });
  reg("%int64_to_float", 1, [](absl::Span<const Value> a) {
    return Value::Float(static_cast<double>(a[0].AsInt64()));
  });
  reg("%nativeint_of_float", 1, [](absl::Span<const Value> a) {
    return Value::Nativeint(static_cast<int64_t>(a[0].AsFloat()));
  });
  reg("%nativeint_to_float", 1, [](absl::Span<const Value> a) {
    return Value::Float(static_cast<double>(a[0].AsNativeint()));
  });
  reg("%todo", 1, [&intern](absl::Span<const Value>) -> Value {
    throw OcamlException(MakeExnFailure(intern, "Todo"));
  });
  reg("caml_int_clz", 1, [](absl::Span<const Value> a) {
    uint64_t u = (static_cast<uint64_t>(a[0].AsInt()) << 1) | 1ULL;
    return Value::Int(__builtin_clzll(u));
  });
  reg("caml_int_ctz", 1, [](absl::Span<const Value> a) {
    uint64_t u = static_cast<uint64_t>(a[0].AsInt()) >> 1;
    return Value::Int(u == 0 ? 63 : __builtin_ctzll(u));
  });
  reg("caml_int_popcount", 1, [](absl::Span<const Value> a) {
    uint64_t u = static_cast<uint64_t>(a[0].AsInt()) & 0x7FFFFFFFFFFFFFFFULL;
    return Value::Int(__builtin_popcountll(u));
  });
  reg("caml_gc_tweak_set", 2, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_gc_tweak_get", 1, [](absl::Span<const Value>) {
    return Value::Int(0);
  });
  reg("caml_gc_tweak_list_active", 1, [&intern](absl::Span<const Value>) {
    return MakeList(intern, {});
  });
  reg("caml_ml_gc_ramp_up", 1, [&eval](absl::Span<const Value> a) {
    std::vector<Value> u = {Value::Unit()};
    Value r = eval.ApplyValues(a[0], u);
    return Value::Tuple({std::move(r), Value::Int(0)});
  });
  reg("caml_ml_gc_ramp_down", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_memprof_is_sampling", 1, [](absl::Span<const Value>) {
    return Value::Bool(false);
  });
  reg("caml_runtime_hashtbl_randomize", 1, [](absl::Span<const Value>) {
    return Value::Unit();
  });
  reg("caml_runtime_hashtbl_is_randomized", 1, [](absl::Span<const Value>) {
    return Value::Bool(false);
  });
  reg("caml_domain_count", 1, [](absl::Span<const Value>) {
    return Value::Int(1);
  });
  reg("caml_dynlink_parse_ld_conf", 1, [](absl::Span<const Value>) {
    return Value::Array({});
  });
  reg("caml_acos_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::acos(a[0].AsFloat())); });
  reg("caml_acosh_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::acosh(a[0].AsFloat())); });
  reg("caml_asin_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::asin(a[0].AsFloat())); });
  reg("caml_asinh_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::asinh(a[0].AsFloat())); });
  reg("caml_atan_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::atan(a[0].AsFloat())); });
  reg("caml_atan2_float", 2, [](absl::Span<const Value> a) { return Value::Float(std::atan2(a[0].AsFloat(), a[1].AsFloat())); });
  reg("caml_atanh_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::atanh(a[0].AsFloat())); });
  reg("caml_cbrt_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::cbrt(a[0].AsFloat())); });
  reg("caml_ceil_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::ceil(a[0].AsFloat())); });
  reg("caml_copysign_float", 2, [](absl::Span<const Value> a) { return Value::Float(std::copysign(a[0].AsFloat(), a[1].AsFloat())); });
  reg("caml_cos_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::cos(a[0].AsFloat())); });
  reg("caml_cosh_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::cosh(a[0].AsFloat())); });
  reg("caml_erf_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::erf(a[0].AsFloat())); });
  reg("caml_erfc_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::erfc(a[0].AsFloat())); });
  reg("caml_exp_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::exp(a[0].AsFloat())); });
  reg("caml_exp2_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::exp2(a[0].AsFloat())); });
  reg("caml_expm1_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::expm1(a[0].AsFloat())); });
  reg("caml_floor_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::floor(a[0].AsFloat())); });
  reg("caml_fmod_float", 2, [](absl::Span<const Value> a) { return Value::Float(std::fmod(a[0].AsFloat(), a[1].AsFloat())); });
  reg("caml_hypot_float", 2, [](absl::Span<const Value> a) { return Value::Float(std::hypot(a[0].AsFloat(), a[1].AsFloat())); });
  reg("caml_log_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::log(a[0].AsFloat())); });
  reg("caml_log10_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::log10(a[0].AsFloat())); });
  reg("caml_log1p_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::log1p(a[0].AsFloat())); });
  reg("caml_log2_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::log2(a[0].AsFloat())); });
  reg("caml_nextafter_float", 2, [](absl::Span<const Value> a) { return Value::Float(std::nextafter(a[0].AsFloat(), a[1].AsFloat())); });
  reg("caml_power_float", 2, [](absl::Span<const Value> a) { return Value::Float(std::pow(a[0].AsFloat(), a[1].AsFloat())); });
  reg("caml_round_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::round(a[0].AsFloat())); });
  reg("caml_sin_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::sin(a[0].AsFloat())); });
  reg("caml_sinh_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::sinh(a[0].AsFloat())); });
  reg("caml_sqrt_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::sqrt(a[0].AsFloat())); });
  reg("caml_tan_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::tan(a[0].AsFloat())); });
  reg("caml_tanh_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::tanh(a[0].AsFloat())); });
  reg("caml_trunc_float", 1, [](absl::Span<const Value> a) { return Value::Float(std::trunc(a[0].AsFloat())); });
}

}  // namespace ocaml_boot
