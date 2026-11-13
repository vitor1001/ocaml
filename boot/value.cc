// Copyright 2026 Google LLC.
// Implementation of Value, Env, exceptions, structural comparison, and hashing.

#include "value.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "third_party/absl/container/flat_hash_set.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_join.h"
#include "third_party/absl/strings/string_view.h"
#include "third_party/absl/types/span.h"
#include "intern.h"

namespace ocaml_boot {

Value Value::String(absl::string_view s) {
  return Value(Kind::kString, std::make_shared<StringVal>(s));
}

Value Value::Bytes(absl::string_view s) {
  return Value(Kind::kString, std::make_shared<StringVal>(s));
}

Value Value::Bytes(size_t len, char fill) {
  return Value(Kind::kString, std::make_shared<StringVal>(len, fill));
}

Value Value::Tuple(std::vector<Value> elements) {
  return Value(Kind::kTuple, std::make_shared<TupleVal>(std::move(elements)));
}

Value Value::Constructor(SymbolId ctor, int64_t exn_stamp,
                         std::optional<Value> arg, bool is_poly_var) {
  auto c = std::make_shared<ConstructorVal>();
  c->ctor = ctor;
  c->exn_stamp = exn_stamp;
  c->arg = std::move(arg);
  c->is_poly_var = is_poly_var;
  return Value(Kind::kConstructor, std::move(c));
}

Value Value::Record(std::vector<std::pair<SymbolId, Value>> fields, int tag) {
  return Value(Kind::kRecord,
               std::make_shared<RecordVal>(std::move(fields), tag));
}

Value Value::Ref(Value initial, SymbolId contents_sym) {
  std::vector<std::pair<SymbolId, Value>> fields;
  fields.emplace_back(contents_sym, std::move(initial));
  return Value(Kind::kRecord, std::make_shared<RecordVal>(std::move(fields)));
}

Value Value::Array(std::vector<Value> elements) {
  return Value(Kind::kArray, std::make_shared<ArrayVal>(std::move(elements)));
}

Value Value::FloatArray(std::vector<double> elements) {
  return Value(Kind::kFloatArray,
               std::make_shared<FloatArrayVal>(std::move(elements)));
}

Value Value::RawBlock(int tag, std::vector<Value> fields) {
  return Value(Kind::kRawBlock,
               std::make_shared<RawBlockVal>(tag, std::move(fields)));
}

Value Value::Bigarray1(std::vector<uint8_t> data, int kind, int layout) {
  return Value(Kind::kBigarray1,
               std::make_shared<Bigarray1Val>(std::move(data), kind, layout));
}

Value Value::Closure(std::shared_ptr<ClosureVal> c) {
  return Value(Kind::kClosure, std::move(c));
}

Value Value::Primitive(std::shared_ptr<PrimitiveVal> p) {
  return Value(Kind::kPrimitive, std::move(p));
}

Value Value::Module(std::shared_ptr<Env> env) {
  return Value(Kind::kModule, std::make_shared<ModuleVal>(std::move(env)));
}

Value Value::Lazy(std::shared_ptr<LazyVal> l) {
  return Value(Kind::kLazy, std::move(l));
}

Value Value::Channel(std::shared_ptr<ChannelVal> ch) {
  return Value(Kind::kChannel, std::move(ch));
}

Value Value::DummyObject(int64_t id) {
  return Value(Kind::kDummyObject, id);
}

bool Value::IsImmediate() const {
  if (IsIntLike()) {
    return true;
  }
  if (kind_ == Kind::kConstructor) {
    const auto& c = AsConstructor();
    return !c->arg.has_value() && c->exn_stamp == 0;
  }
  return false;
}

void Env::DefineValue(SymbolId name, Value val) {
  auto it = values_.find(name);
  if (it != values_.end()) {
    // Allocate a fresh cell so existing closures capturing the previous
    // binding of a shadowed variable keep the old value.
    it->second = std::make_shared<Value>(std::move(val));
  } else {
    values_.emplace(name, std::make_shared<Value>(std::move(val)));
  }
}

void Env::DefineModule(SymbolId name, std::shared_ptr<Env> mod) {
  modules_[name] = std::move(mod);
}

void Env::DefineExnStamp(SymbolId name, int64_t stamp) {
  exn_stamps_[name] = stamp;
}

void Env::OpenModule(std::shared_ptr<Env> mod) {
  if (mod != nullptr) {
    opened_modules_.push_back(std::move(mod));
  }
}

void Env::IncludeModule(const Env& mod) {
  for (const auto& [k, v] : mod.values_) {
    values_[k] = std::make_shared<Value>(*v);
  }
  for (const auto& [k, m] : mod.modules_) {
    modules_[k] = m;
  }
  for (const auto& [k, s] : mod.exn_stamps_) {
    exn_stamps_[k] = s;
  }
}

void Env::FilterExportedValues(const absl::flat_hash_set<SymbolId>& allowed) {
  absl::erase_if(values_, [&](const auto& kv) {
    return !allowed.contains(kv.first);
  });
  absl::erase_if(modules_, [&](const auto& kv) {
    return !allowed.contains(kv.first);
  });
}

const Value* Env::LookupExportedValue(SymbolId name) const {
  auto it = values_.find(name);
  if (it != values_.end()) {
    return it->second.get();
  }
  return nullptr;
}

Value* Env::LookupValueMutable(SymbolId name) {
  auto it = values_.find(name);
  if (it != values_.end()) {
    return it->second.get();
  }
  return nullptr;
}

const Value* Env::LookupValue(SymbolId name) const {
  for (const Env* cur = this; cur != nullptr; cur = cur->parent_.get()) {
    auto it = cur->values_.find(name);
    if (it != cur->values_.end()) {
      return it->second.get();
    }
    for (auto oit = cur->opened_modules_.rbegin();
         oit != cur->opened_modules_.rend(); ++oit) {
      if (const Value* v = (*oit)->LookupExportedValue(name)) {
        return v;
      }
    }
  }
  return nullptr;
}

std::shared_ptr<Env> Env::LookupExportedModule(SymbolId name) const {
  auto it = modules_.find(name);
  if (it != modules_.end()) {
    return it->second;
  }
  return nullptr;
}

std::shared_ptr<Env> Env::LookupModule(SymbolId name) const {
  for (const Env* cur = this; cur != nullptr; cur = cur->parent_.get()) {
    auto it = cur->modules_.find(name);
    if (it != cur->modules_.end()) {
      return it->second;
    }
    for (auto oit = cur->opened_modules_.rbegin();
         oit != cur->opened_modules_.rend(); ++oit) {
      if (auto m = (*oit)->LookupExportedModule(name)) {
        return m;
      }
    }
  }
  return nullptr;
}

std::shared_ptr<Env> Env::LookupModulePath(
    absl::Span<const SymbolId> module_path) const {
  if (module_path.empty()) {
    return nullptr;
  }
  std::shared_ptr<Env> cur = LookupModule(module_path[0]);
  for (size_t i = 1; i < module_path.size() && cur != nullptr; ++i) {
    auto next = cur->LookupExportedModule(module_path[i]);
    if (next == nullptr) {
      next = cur->LookupModule(module_path[i]);
    }
    cur = std::move(next);
  }
  return cur;
}

const Value* Env::LookupValue(absl::Span<const SymbolId> module_path,
                              SymbolId name) const {
  if (module_path.empty()) {
    return LookupValue(name);
  }
  std::shared_ptr<Env> mod = LookupModulePath(module_path);
  if (mod == nullptr) {
    return nullptr;
  }
  if (const Value* v = mod->LookupExportedValue(name)) {
    return v;
  }
  return mod->LookupValue(name);
}

int64_t Env::LookupExportedExnStamp(SymbolId name) const {
  auto it = exn_stamps_.find(name);
  if (it != exn_stamps_.end()) {
    return it->second;
  }
  return 0;
}

int64_t Env::LookupExnStamp(SymbolId name) const {
  for (const Env* cur = this; cur != nullptr; cur = cur->parent_.get()) {
    auto it = cur->exn_stamps_.find(name);
    if (it != cur->exn_stamps_.end()) {
      return it->second;
    }
    for (auto oit = cur->opened_modules_.rbegin();
         oit != cur->opened_modules_.rend(); ++oit) {
      int64_t s = (*oit)->LookupExportedExnStamp(name);
      if (s != 0) {
        return s;
      }
    }
  }
  return 0;
}

int64_t Env::LookupExnStamp(absl::Span<const SymbolId> module_path,
                            SymbolId name) const {
  if (module_path.empty()) {
    return LookupExnStamp(name);
  }
  std::shared_ptr<Env> mod = LookupModulePath(module_path);
  if (mod == nullptr) {
    return 0;
  }
  int64_t s = mod->LookupExportedExnStamp(name);
  if (s != 0) {
    return s;
  }
  return mod->LookupExnStamp(name);
}

int64_t WellKnownExnStamp(absl::string_view name) {
  if (name == "Out_of_memory") return 1;
  if (name == "Sys_error") return 2;
  if (name == "Failure") return 3;
  if (name == "Invalid_argument") return 4;
  if (name == "End_of_file") return 5;
  if (name == "Division_by_zero") return 6;
  if (name == "Not_found") return 7;
  if (name == "Match_failure") return 8;
  if (name == "Stack_overflow") return 9;
  if (name == "Sys_blocked_io") return 10;
  if (name == "Assert_failure") return 11;
  if (name == "Undefined_recursive_module") return 12;
  return 0;
}

int64_t AllocateExnStamp(absl::string_view name) {
  static std::atomic<int64_t> next_stamp{100};
  static absl::flat_hash_set<int64_t> claimed_well_known;
  int64_t wk = WellKnownExnStamp(name);
  if (wk != 0 && claimed_well_known.insert(wk).second) {
    return wk;
  }
  return next_stamp.fetch_add(1, std::memory_order_relaxed);
}

Value MakeExnNotFound(InternTable& intern) {
  return Value::Constructor(intern.Intern("Not_found"),
                            WellKnownExnStamp("Not_found"));
}

Value MakeExnEndOfFile(InternTable& intern) {
  return Value::Constructor(intern.Intern("End_of_file"),
                            WellKnownExnStamp("End_of_file"));
}

Value MakeExnDivisionByZero(InternTable& intern) {
  return Value::Constructor(intern.Intern("Division_by_zero"),
                            WellKnownExnStamp("Division_by_zero"));
}

Value MakeExnOutOfMemory(InternTable& intern) {
  return Value::Constructor(intern.Intern("Out_of_memory"),
                            WellKnownExnStamp("Out_of_memory"));
}

Value MakeExnSysBlockedIo(InternTable& intern) {
  return Value::Constructor(intern.Intern("Sys_blocked_io"),
                            WellKnownExnStamp("Sys_blocked_io"));
}

Value MakeExnInvalidArgument(InternTable& intern, absl::string_view msg) {
  return Value::Constructor(intern.Intern("Invalid_argument"),
                            WellKnownExnStamp("Invalid_argument"),
                            Value::String(msg));
}

Value MakeExnFailure(InternTable& intern, absl::string_view msg) {
  return Value::Constructor(intern.Intern("Failure"),
                            WellKnownExnStamp("Failure"), Value::String(msg));
}

Value MakeExnSysError(InternTable& intern, absl::string_view msg) {
  return Value::Constructor(intern.Intern("Sys_error"),
                            WellKnownExnStamp("Sys_error"), Value::String(msg));
}

Value MakeExnMatchFailure(InternTable& intern, absl::string_view file,
                          int64_t line, int64_t col) {
  return Value::Constructor(
      intern.Intern("Match_failure"), WellKnownExnStamp("Match_failure"),
      Value::Tuple({Value::String(file), Value::Int(line), Value::Int(col)}));
}

Value MakeExnAssertFailure(InternTable& intern, absl::string_view file,
                           int64_t line, int64_t col) {
  return Value::Constructor(
      intern.Intern("Assert_failure"), WellKnownExnStamp("Assert_failure"),
      Value::Tuple({Value::String(file), Value::Int(line), Value::Int(col)}));
}

Value MakeExnUndefinedRecursiveModule(InternTable& intern,
                                      absl::string_view file, int64_t line,
                                      int64_t col) {
  return Value::Constructor(
      intern.Intern("Undefined_recursive_module"),
      WellKnownExnStamp("Undefined_recursive_module"),
      Value::Tuple({Value::String(file), Value::Int(line), Value::Int(col)}));
}

namespace {

// Unwraps already-forced Lazy values during structural comparison.
const Value& UnwrapForcedLazy(const Value& v) {
  const Value* cur = &v;
  while (cur->IsLazy() && cur->AsLazy()->forced) {
    cur = &cur->AsLazy()->value;
  }
  return *cur;
}

int TypeRank(const Value& v) {
  if (v.IsIntLike()) {
    return 0;
  }
  switch (v.kind()) {
    case Value::Kind::kInt32:
      return 1;
    case Value::Kind::kInt64:
      return 2;
    case Value::Kind::kNativeint:
      return 3;
    case Value::Kind::kFloat:
      return 4;
    case Value::Kind::kString:
      return 5;
    case Value::Kind::kConstructor:
      return 6;
    case Value::Kind::kTuple:
      return 7;
    case Value::Kind::kRecord:
      return 8;
    case Value::Kind::kArray:
      return 9;
    case Value::Kind::kFloatArray:
      return 10;
    case Value::Kind::kRawBlock:
      return 11;
    case Value::Kind::kBigarray1:
      return 12;
    case Value::Kind::kDummyObject:
      return 13;
    default:
      return 20 + static_cast<int>(v.kind());
  }
}

}  // namespace

int64_t CompareValues(const Value& raw_a, const Value& raw_b, bool total,
                      const InternTable* intern) {
  const Value& a = UnwrapForcedLazy(raw_a);
  const Value& b = UnwrapForcedLazy(raw_b);

  if (a.IsIntLike() && b.IsIntLike()) {
    int64_t va = a.AsInt();
    int64_t vb = b.AsInt();
    return (va < vb) ? -1 : (va > vb ? 1 : 0);
  }

  if (a.RawPtr() != nullptr && a.RawPtr() == b.RawPtr() && total) {
    return 0;
  }

  int ra = TypeRank(a);
  int rb = TypeRank(b);
  if (ra != rb) {
    return (ra < rb) ? -1 : 1;
  }

  switch (a.kind()) {
    case Value::Kind::kInt32: {
      int32_t va = a.AsInt32();
      int32_t vb = b.AsInt32();
      return (va < vb) ? -1 : (va > vb ? 1 : 0);
    }
    case Value::Kind::kInt64:
    case Value::Kind::kNativeint: {
      int64_t va = a.AsInt();
      int64_t vb = b.AsInt();
      return (va < vb) ? -1 : (va > vb ? 1 : 0);
    }
    case Value::Kind::kFloat: {
      double fa = a.AsFloat();
      double fb = b.AsFloat();
      if (std::isnan(fa) || std::isnan(fb)) {
        if (!total) {
          return INT64_MIN;
        }
        if (std::isnan(fa) && std::isnan(fb)) {
          return 0;
        }
        return std::isnan(fa) ? -1 : 1;
      }
      return (fa < fb) ? -1 : (fa > fb ? 1 : 0);
    }
    case Value::Kind::kString: {
      const std::string& sa = a.AsString()->data;
      const std::string& sb = b.AsString()->data;
      int cmp = sa.compare(sb);
      return (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);
    }
    case Value::Kind::kConstructor: {
      const auto& ca = a.AsConstructor();
      const auto& cb = b.AsConstructor();
      bool imm_a = !ca->arg.has_value() && ca->exn_stamp == 0;
      bool imm_b = !cb->arg.has_value() && cb->exn_stamp == 0;
      if (imm_a != imm_b) {
        return imm_a ? -1 : 1;
      }
      if (ca->exn_stamp != 0 || cb->exn_stamp != 0) {
        if (ca->exn_stamp != cb->exn_stamp) {
          return (ca->exn_stamp < cb->exn_stamp) ? -1 : 1;
        }
      }
      if (ca->ctor != cb->ctor) {
        if (intern != nullptr) {
          int cmp =
              intern->Resolve(ca->ctor).compare(intern->Resolve(cb->ctor));
          if (cmp != 0) {
            return (cmp < 0) ? -1 : 1;
          }
        }
        return (ca->ctor < cb->ctor) ? -1 : 1;
      }
      if (ca->arg.has_value() != cb->arg.has_value()) {
        return ca->arg.has_value() ? 1 : -1;
      }
      if (ca->arg.has_value()) {
        return CompareValues(*ca->arg, *cb->arg, total, intern);
      }
      return 0;
    }
    case Value::Kind::kTuple: {
      const auto& ta = a.AsTuple()->elements;
      const auto& tb = b.AsTuple()->elements;
      if (ta.size() != tb.size()) {
        return (ta.size() < tb.size()) ? -1 : 1;
      }
      for (size_t i = 0; i < ta.size(); ++i) {
        int64_t c = CompareValues(ta[i], tb[i], total, intern);
        if (c != 0) {
          return c;
        }
      }
      return 0;
    }
    case Value::Kind::kRecord: {
      const auto& fa = a.AsRecord();
      const auto& fb = b.AsRecord();
      if (fa->tag != fb->tag) {
        return (fa->tag < fb->tag) ? -1 : 1;
      }
      if (fa->fields.size() != fb->fields.size()) {
        return (fa->fields.size() < fb->fields.size()) ? -1 : 1;
      }
      for (size_t i = 0; i < fa->fields.size(); ++i) {
        int64_t c = CompareValues(fa->fields[i].second, fb->fields[i].second,
                                  total, intern);
        if (c != 0) {
          return c;
        }
      }
      return 0;
    }
    case Value::Kind::kArray: {
      const auto& aa = a.AsArray()->elements;
      const auto& ab = b.AsArray()->elements;
      if (aa.size() != ab.size()) {
        return (aa.size() < ab.size()) ? -1 : 1;
      }
      for (size_t i = 0; i < aa.size(); ++i) {
        int64_t c = CompareValues(aa[i], ab[i], total, intern);
        if (c != 0) {
          return c;
        }
      }
      return 0;
    }
    case Value::Kind::kFloatArray: {
      const auto& fa = a.AsFloatArray()->elements;
      const auto& fb = b.AsFloatArray()->elements;
      if (fa.size() != fb.size()) {
        return (fa.size() < fb.size()) ? -1 : 1;
      }
      for (size_t i = 0; i < fa.size(); ++i) {
        int64_t c =
            CompareValues(Value::Float(fa[i]), Value::Float(fb[i]), total);
        if (c != 0) {
          return c;
        }
      }
      return 0;
    }
    case Value::Kind::kRawBlock: {
      const auto& ba = a.AsRawBlock();
      const auto& bb = b.AsRawBlock();
      if (ba->tag != bb->tag) {
        return (ba->tag < bb->tag) ? -1 : 1;
      }
      if (ba->fields.size() != bb->fields.size()) {
        return (ba->fields.size() < bb->fields.size()) ? -1 : 1;
      }
      for (size_t i = 0; i < ba->fields.size(); ++i) {
        int64_t c =
            CompareValues(ba->fields[i], bb->fields[i], total, intern);
        if (c != 0) {
          return c;
        }
      }
      return 0;
    }
    case Value::Kind::kBigarray1: {
      const auto& ba = a.AsBigarray1()->data;
      const auto& bb = b.AsBigarray1()->data;
      if (ba.size() != bb.size()) {
        return (ba.size() < bb.size()) ? -1 : 1;
      }
      int cmp = std::memcmp(ba.data(), bb.data(), ba.size());
      return (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);
    }
    case Value::Kind::kDummyObject: {
      int64_t ida = a.AsDummyObjectId();
      int64_t idb = b.AsDummyObjectId();
      return (ida < idb) ? -1 : (ida > idb ? 1 : 0);
    }
    default: {
      if (a.RawPtr() == b.RawPtr()) {
        return 0;
      }
      return (a.RawPtr() < b.RawPtr()) ? -1 : 1;
    }
  }
}

bool EqualValues(const Value& a, const Value& b, const InternTable* intern) {
  return CompareValues(a, b, /*total=*/false, intern) == 0;
}

namespace {

inline uint32_t Rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline void HashMixU32(uint32_t& h, uint32_t d) {
  d *= 0xcc9e2d51U;
  d = Rotl32(d, 15);
  d *= 0x1b873593U;
  h ^= d;
  h = Rotl32(h, 13);
  h = h * 5 + 0xe6546b64U;
}

inline void HashMixU64(uint32_t& h, uint64_t d) {
  HashMixU32(h, static_cast<uint32_t>(d));
  HashMixU32(h, static_cast<uint32_t>(d >> 32));
}

void HashMixString(uint32_t& h, absl::string_view s) {
  size_t len = s.size();
  size_t i = 0;
  for (; i + 4 <= len; i += 4) {
    uint32_t w = static_cast<uint8_t>(s[i]) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(s[i + 1])) << 8) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(s[i + 2])) << 16) |
                 (static_cast<uint32_t>(static_cast<uint8_t>(s[i + 3])) << 24);
    HashMixU32(h, w);
  }
  uint32_t rem = 0;
  switch (len & 3) {
    case 3:
      rem = static_cast<uint32_t>(static_cast<uint8_t>(s[i + 2])) << 16;
      [[fallthrough]];
    case 2:
      rem |= static_cast<uint32_t>(static_cast<uint8_t>(s[i + 1])) << 8;
      [[fallthrough]];
    case 1:
      rem |= static_cast<uint8_t>(s[i]);
      HashMixU32(h, rem);
      break;
    default:
      break;
  }
  HashMixU32(h, static_cast<uint32_t>(len));
}

}  // namespace

int64_t HashValue(int64_t meaningful, int64_t limit, uint32_t seed,
                  const Value& root, const InternTable* intern) {
  uint32_t h = seed;
  std::vector<const Value*> queue;
  queue.push_back(&root);
  size_t head = 0;

  while (head < queue.size() && meaningful > 0 && limit > 0) {
    const Value& v = UnwrapForcedLazy(*queue[head++]);
    if (v.IsIntLike()) {
      HashMixU32(h, static_cast<uint32_t>((v.AsInt() << 1) | 1));
      --meaningful;
      continue;
    }
    switch (v.kind()) {
      case Value::Kind::kInt32:
        HashMixU32(h, static_cast<uint32_t>(v.AsInt32()));
        --meaningful;
        break;
      case Value::Kind::kInt64:
      case Value::Kind::kNativeint:
        HashMixU64(h, static_cast<uint64_t>(v.AsInt()));
        --meaningful;
        break;
      case Value::Kind::kFloat: {
        double d = v.AsFloat();
        if (d == 0.0) {
          d = 0.0;
        } else if (std::isnan(d)) {
          HashMixU64(h, 0x7FF8000000000001ULL);
          --meaningful;
          break;
        }
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(double));
        HashMixU64(h, bits);
        --meaningful;
        break;
      }
      case Value::Kind::kString:
        HashMixString(h, v.AsString()->data);
        --meaningful;
        break;
      case Value::Kind::kConstructor: {
        const auto& c = v.AsConstructor();
        if (c->exn_stamp != 0) {
          HashMixU32(h, static_cast<uint32_t>(c->exn_stamp));
        } else if (intern != nullptr) {
          HashMixString(h, intern->Resolve(c->ctor));
        } else {
          HashMixU32(h, c->ctor);
        }
        --meaningful;
        if (c->arg.has_value()) {
          --limit;
          queue.push_back(&(*c->arg));
        }
        break;
      }
      case Value::Kind::kTuple: {
        const auto& elems = v.AsTuple()->elements;
        HashMixU32(h, static_cast<uint32_t>(elems.size() << 10));
        --meaningful;
        --limit;
        for (const auto& e : elems) {
          queue.push_back(&e);
        }
        break;
      }
      case Value::Kind::kRecord: {
        const auto& fields = v.AsRecord()->fields;
        HashMixU32(h, static_cast<uint32_t>((fields.size() << 10) |
                                            (v.AsRecord()->tag & 0xFF)));
        --meaningful;
        --limit;
        for (const auto& kv : fields) {
          queue.push_back(&kv.second);
        }
        break;
      }
      case Value::Kind::kArray: {
        const auto& elems = v.AsArray()->elements;
        HashMixU32(h, static_cast<uint32_t>(elems.size() << 10));
        --meaningful;
        --limit;
        for (const auto& e : elems) {
          queue.push_back(&e);
        }
        break;
      }
      case Value::Kind::kFloatArray: {
        const auto& elems = v.AsFloatArray()->elements;
        for (double d : elems) {
          if (meaningful <= 0) break;
          uint64_t bits = 0;
          if (d == 0.0) d = 0.0;
          std::memcpy(&bits, &d, sizeof(double));
          HashMixU64(h, bits);
          --meaningful;
        }
        break;
      }
      case Value::Kind::kRawBlock: {
        const auto& rb = v.AsRawBlock();
        HashMixU32(
            h, static_cast<uint32_t>((rb->fields.size() << 10) | rb->tag));
        --meaningful;
        --limit;
        for (const auto& f : rb->fields) {
          queue.push_back(&f);
        }
        break;
      }
      case Value::Kind::kDummyObject:
        HashMixU32(h, static_cast<uint32_t>(v.AsDummyObjectId()));
        --meaningful;
        break;
      default:
        --limit;
        break;
    }
  }

  h ^= h >> 16;
  h *= 0x85ebca6bU;
  h ^= h >> 13;
  h *= 0xc2b2ae35U;
  h ^= h >> 16;
  return static_cast<int64_t>(h & 0x3FFFFFFFU);
}

std::string DebugString(const Value& v, const InternTable* intern) {
  switch (v.kind()) {
    case Value::Kind::kUnit:
      return "()";
    case Value::Kind::kBool:
      return v.AsBool() ? "true" : "false";
    case Value::Kind::kChar:
      return absl::StrCat("'", std::string(1, static_cast<char>(v.AsChar())),
                          "'");
    case Value::Kind::kInt:
      return absl::StrCat(v.AsInt());
    case Value::Kind::kInt32:
      return absl::StrCat(v.AsInt32(), "l");
    case Value::Kind::kInt64:
      return absl::StrCat(v.AsInt64(), "L");
    case Value::Kind::kNativeint:
      return absl::StrCat(v.AsNativeint(), "n");
    case Value::Kind::kFloat:
      return absl::StrCat(v.AsFloat());
    case Value::Kind::kString:
      return absl::StrCat("\"", v.AsString()->data, "\"");
    case Value::Kind::kTuple: {
      std::vector<std::string> parts;
      for (const auto& e : v.AsTuple()->elements) {
        parts.push_back(DebugString(e, intern));
      }
      return absl::StrCat("(", absl::StrJoin(parts, ", "), ")");
    }
    case Value::Kind::kConstructor: {
      const auto& c = v.AsConstructor();
      std::string name = intern != nullptr
                             ? std::string(intern->Resolve(c->ctor))
                             : absl::StrCat("ctor#", c->ctor);
      if (c->is_poly_var) {
        name = absl::StrCat("`", name);
      }
      if (c->arg.has_value()) {
        return absl::StrCat(name, "(", DebugString(*c->arg, intern), ")");
      }
      return name;
    }
    case Value::Kind::kRecord: {
      std::vector<std::string> parts;
      for (const auto& [k, fv] : v.AsRecord()->fields) {
        std::string fname = intern != nullptr ? std::string(intern->Resolve(k))
                                              : absl::StrCat("f#", k);
        parts.push_back(
            absl::StrCat(fname, " = ", DebugString(fv, intern)));
      }
      return absl::StrCat("{", absl::StrJoin(parts, "; "), "}");
    }
    case Value::Kind::kArray: {
      std::vector<std::string> parts;
      for (const auto& e : v.AsArray()->elements) {
        parts.push_back(DebugString(e, intern));
      }
      return absl::StrCat("[|", absl::StrJoin(parts, "; "), "|]");
    }
    case Value::Kind::kFloatArray:
      return absl::StrCat("<floatarray len=", v.AsFloatArray()->elements.size(),
                          ">");
    case Value::Kind::kRawBlock:
      return absl::StrCat("<rawblock tag=", v.AsRawBlock()->tag,
                          " size=", v.AsRawBlock()->fields.size(), ">");
    case Value::Kind::kBigarray1:
      return absl::StrCat("<bigarray1 size=", v.AsBigarray1()->data.size(),
                          ">");
    case Value::Kind::kClosure:
      return "<fun>";
    case Value::Kind::kPrimitive:
      return absl::StrCat("<prim:", v.AsPrimitive()->name, ">");
    case Value::Kind::kModule:
      return "<module>";
    case Value::Kind::kLazy:
      return v.AsLazy()->forced
                 ? absl::StrCat("lazy(", DebugString(v.AsLazy()->value, intern),
                                ")")
                 : "<lazy>";
    case Value::Kind::kChannel:
      return "<channel>";
    case Value::Kind::kDummyObject:
      return absl::StrCat("<object#", v.AsDummyObjectId(), ">");
  }
  return "<unknown>";
}

}  // namespace ocaml_boot
