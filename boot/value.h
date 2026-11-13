// Copyright 2026 Google LLC.
// Runtime value representation, lexical/module environments, exceptions, and
// structural comparison/hashing for the OCaml 5.3 bootstrap interpreter.

#ifndef OCAML_BOOT_VALUE_H_
#define OCAML_BOOT_VALUE_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "third_party/absl/container/flat_hash_map.h"
#include "third_party/absl/container/flat_hash_set.h"
#include "third_party/absl/log/check.h"
#include "third_party/absl/strings/string_view.h"
#include "third_party/absl/types/span.h"
#include "ast.h"
#include "intern.h"

namespace ocaml_boot {

class Env;
class Value;

struct StringVal;
struct TupleVal;
struct ConstructorVal;
struct RecordVal;
struct ArrayVal;
struct FloatArrayVal;
struct RawBlockVal;
struct Bigarray1Val;
struct ClosureVal;
struct PrimitiveVal;
struct ModuleVal;
struct LazyVal;
struct ChannelVal;
struct DummyObjectVal;

// Represents a runtime OCaml value in the interpreter.
class Value {
 public:
  enum class Kind {
    kUnit,
    kBool,
    kChar,
    kInt,
    kInt32,
    kInt64,
    kNativeint,
    kFloat,
    kString,
    kTuple,
    kConstructor,
    kRecord,
    kArray,
    kFloatArray,
    kRawBlock,
    kBigarray1,
    kClosure,
    kPrimitive,
    kModule,
    kLazy,
    kChannel,
    kDummyObject,
  };

  // Default constructor initializes to Unit `()`.
  Value() : kind_(Kind::kUnit), int_val_(0) {}

  // Static factory methods for immediate and boxed scalar values.
  static Value Unit() { return Value(Kind::kUnit, int64_t{0}); }
  static Value Bool(bool b) { return Value(Kind::kBool, b ? 1 : 0); }
  static Value Char(uint8_t c) {
    return Value(Kind::kChar, static_cast<int64_t>(c));
  }
  static Value Int(int64_t v) { return Value(Kind::kInt, v); }
  static Value Int32(int32_t v) {
    return Value(Kind::kInt32, static_cast<int64_t>(v));
  }
  static Value Int64(int64_t v) { return Value(Kind::kInt64, v); }
  static Value Nativeint(int64_t v) { return Value(Kind::kNativeint, v); }
  static Value Float(double d) {
    Value v;
    v.kind_ = Kind::kFloat;
    v.float_val_ = d;
    return v;
  }

  // Static factory methods for heap-allocated values.
  static Value String(absl::string_view s);
  static Value Bytes(absl::string_view s);
  static Value Bytes(size_t len, char fill = '\0');
  static Value Tuple(std::vector<Value> elements);
  static Value Constructor(SymbolId ctor, int64_t exn_stamp = 0,
                           std::optional<Value> arg = std::nullopt,
                           bool is_poly_var = false);
  static Value Record(std::vector<std::pair<SymbolId, Value>> fields,
                      int tag = 0);
  static Value Ref(Value initial, SymbolId contents_sym = kInvalidSymbol);
  static Value Array(std::vector<Value> elements);
  static Value FloatArray(std::vector<double> elements);
  static Value RawBlock(int tag, std::vector<Value> fields);
  static Value Bigarray1(std::vector<uint8_t> data, int kind = 0,
                         int layout = 0);
  static Value Closure(std::shared_ptr<ClosureVal> c);
  static Value Primitive(std::shared_ptr<PrimitiveVal> p);
  static Value Module(std::shared_ptr<Env> env);
  static Value Lazy(std::shared_ptr<LazyVal> l);
  static Value Channel(std::shared_ptr<ChannelVal> ch);
  static Value DummyObject(int64_t id = 0);

  Kind kind() const { return kind_; }

  bool IsUnit() const { return kind_ == Kind::kUnit; }
  bool IsBool() const { return kind_ == Kind::kBool; }
  bool IsChar() const { return kind_ == Kind::kChar; }
  bool IsInt() const { return kind_ == Kind::kInt; }
  bool IsInt32() const { return kind_ == Kind::kInt32; }
  bool IsInt64() const { return kind_ == Kind::kInt64; }
  bool IsNativeint() const { return kind_ == Kind::kNativeint; }
  bool IsFloat() const { return kind_ == Kind::kFloat; }
  bool IsString() const { return kind_ == Kind::kString; }
  bool IsTuple() const { return kind_ == Kind::kTuple; }
  bool IsConstructor() const { return kind_ == Kind::kConstructor; }
  bool IsRecord() const { return kind_ == Kind::kRecord; }
  bool IsArray() const { return kind_ == Kind::kArray; }
  bool IsFloatArray() const { return kind_ == Kind::kFloatArray; }
  bool IsRawBlock() const { return kind_ == Kind::kRawBlock; }
  bool IsBigarray1() const { return kind_ == Kind::kBigarray1; }
  bool IsClosure() const { return kind_ == Kind::kClosure; }
  bool IsPrimitive() const { return kind_ == Kind::kPrimitive; }
  bool IsModule() const { return kind_ == Kind::kModule; }
  bool IsLazy() const { return kind_ == Kind::kLazy; }
  bool IsChannel() const { return kind_ == Kind::kChannel; }
  bool IsDummyObject() const { return kind_ == Kind::kDummyObject; }

  // Returns true for values represented as immediate integers in OCaml's
  // runtime (`unit`, `bool`, `char`, `int`, and 0-arg non-exception ctors).
  bool IsImmediate() const;

  // Returns true if the value is an immediate integer-like scalar (`int`,
  // `char`, `bool`, or `unit`), which are interchangeable under `%identity`.
  bool IsIntLike() const {
    return kind_ == Kind::kInt || kind_ == Kind::kChar ||
           kind_ == Kind::kBool || kind_ == Kind::kUnit;
  }

  bool AsBool() const {
    DCHECK(IsIntLike());
    return int_val_ != 0;
  }

  uint8_t AsChar() const {
    DCHECK(IsIntLike());
    return static_cast<uint8_t>(int_val_ & 0xFF);
  }

  int64_t AsInt() const {
    DCHECK(IsIntLike() || kind_ == Kind::kInt32 || kind_ == Kind::kInt64 ||
           kind_ == Kind::kNativeint);
    return int_val_;
  }

  int32_t AsInt32() const {
    DCHECK(kind_ == Kind::kInt32 || IsIntLike());
    return static_cast<int32_t>(int_val_);
  }

  int64_t AsInt64() const {
    DCHECK(kind_ == Kind::kInt64 || IsIntLike());
    return int_val_;
  }

  int64_t AsNativeint() const {
    DCHECK(kind_ == Kind::kNativeint || IsIntLike());
    return int_val_;
  }

  double AsFloat() const {
    DCHECK_EQ(kind_, Kind::kFloat);
    return float_val_;
  }

  int64_t AsDummyObjectId() const {
    DCHECK_EQ(kind_, Kind::kDummyObject);
    return int_val_;
  }

  std::shared_ptr<StringVal> AsString() const {
    DCHECK_EQ(kind_, Kind::kString);
    return std::static_pointer_cast<StringVal>(ptr_);
  }

  std::shared_ptr<TupleVal> AsTuple() const {
    DCHECK_EQ(kind_, Kind::kTuple);
    return std::static_pointer_cast<TupleVal>(ptr_);
  }

  std::shared_ptr<ConstructorVal> AsConstructor() const {
    DCHECK_EQ(kind_, Kind::kConstructor);
    return std::static_pointer_cast<ConstructorVal>(ptr_);
  }

  std::shared_ptr<RecordVal> AsRecord() const {
    DCHECK_EQ(kind_, Kind::kRecord);
    return std::static_pointer_cast<RecordVal>(ptr_);
  }

  std::shared_ptr<ArrayVal> AsArray() const {
    DCHECK_EQ(kind_, Kind::kArray);
    return std::static_pointer_cast<ArrayVal>(ptr_);
  }

  std::shared_ptr<FloatArrayVal> AsFloatArray() const {
    DCHECK_EQ(kind_, Kind::kFloatArray);
    return std::static_pointer_cast<FloatArrayVal>(ptr_);
  }

  std::shared_ptr<RawBlockVal> AsRawBlock() const {
    DCHECK_EQ(kind_, Kind::kRawBlock);
    return std::static_pointer_cast<RawBlockVal>(ptr_);
  }

  std::shared_ptr<Bigarray1Val> AsBigarray1() const {
    DCHECK_EQ(kind_, Kind::kBigarray1);
    return std::static_pointer_cast<Bigarray1Val>(ptr_);
  }

  std::shared_ptr<ClosureVal> AsClosure() const {
    DCHECK_EQ(kind_, Kind::kClosure);
    return std::static_pointer_cast<ClosureVal>(ptr_);
  }

  std::shared_ptr<PrimitiveVal> AsPrimitive() const {
    DCHECK_EQ(kind_, Kind::kPrimitive);
    return std::static_pointer_cast<PrimitiveVal>(ptr_);
  }

  std::shared_ptr<ModuleVal> AsModule() const {
    DCHECK_EQ(kind_, Kind::kModule);
    return std::static_pointer_cast<ModuleVal>(ptr_);
  }

  std::shared_ptr<LazyVal> AsLazy() const {
    DCHECK_EQ(kind_, Kind::kLazy);
    return std::static_pointer_cast<LazyVal>(ptr_);
  }

  std::shared_ptr<ChannelVal> AsChannel() const {
    DCHECK_EQ(kind_, Kind::kChannel);
    return std::static_pointer_cast<ChannelVal>(ptr_);
  }

  const void* RawPtr() const { return ptr_.get(); }

 private:
  Value(Kind k, int64_t v) : kind_(k), int_val_(v) {}
  Value(Kind k, std::shared_ptr<void> p) : kind_(k), ptr_(std::move(p)) {}

  Kind kind_ = Kind::kUnit;
  union {
    int64_t int_val_ = 0;
    double float_val_;
  };
  std::shared_ptr<void> ptr_;
};

// Heap-allocated mutable string / bytes payload.
struct StringVal {
  std::string data;
  explicit StringVal(absl::string_view s) : data(s) {}
  StringVal(size_t len, char fill) : data(len, fill) {}
};

// Heap-allocated tuple payload.
struct TupleVal {
  std::vector<Value> elements;
  explicit TupleVal(std::vector<Value> elems) : elements(std::move(elems)) {}
};

// Heap-allocated variant or exception constructor payload.
struct ConstructorVal {
  SymbolId ctor = kInvalidSymbol;
  int64_t exn_stamp = 0;
  std::optional<Value> arg;
  bool is_poly_var = false;
  bool is_raised_exn = false;
  // Optional flattened fields when accessed via Obj.field / Obj.set_field.
  int tag = 0;
};

// Heap-allocated record payload with mutable fields.
struct RecordVal {
  std::vector<std::pair<SymbolId, Value>> fields;
  int tag = 0;

  RecordVal() = default;
  explicit RecordVal(std::vector<std::pair<SymbolId, Value>> f, int t = 0)
      : fields(std::move(f)), tag(t) {}

  // Looks up a field by SymbolId, returning nullptr if not found.
  const Value* Find(SymbolId field) const {
    for (const auto& kv : fields) {
      if (kv.first == field) {
        return &kv.second;
      }
    }
    if (fields.size() == 1 && fields[0].first == kInvalidSymbol) {
      return &fields[0].second;
    }
    return nullptr;
  }

  // Looks up a mutable field pointer by SymbolId.
  Value* FindMutable(SymbolId field) {
    for (auto& kv : fields) {
      if (kv.first == field) {
        return &kv.second;
      }
    }
    if (fields.size() == 1 && fields[0].first == kInvalidSymbol) {
      return &fields[0].second;
    }
    return nullptr;
  }

  // Sets or inserts a field by SymbolId.
  void Set(SymbolId field, Value val) {
    for (auto& kv : fields) {
      if (kv.first == field) {
        kv.second = std::move(val);
        return;
      }
    }
    if (fields.size() == 1 && fields[0].first == kInvalidSymbol) {
      fields[0].first = field;
      fields[0].second = std::move(val);
      return;
    }
    fields.emplace_back(field, std::move(val));
  }
};

// Heap-allocated mutable OCaml array payload.
struct ArrayVal {
  std::vector<Value> elements;
  explicit ArrayVal(std::vector<Value> elems) : elements(std::move(elems)) {}
};

// Heap-allocated unboxed float array payload.
struct FloatArrayVal {
  std::vector<double> elements;
  explicit FloatArrayVal(std::vector<double> elems)
      : elements(std::move(elems)) {}
};

// Heap-allocated untyped tagged block used by `Obj.new_block` and `Symtable`.
struct RawBlockVal {
  int tag = 0;
  std::vector<Value> fields;
  RawBlockVal(int t, std::vector<Value> f) : tag(t), fields(std::move(f)) {}
};

// Heap-allocated 1D byte bigarray used by `Emitcode`.
struct Bigarray1Val {
  std::vector<uint8_t> data;
  int kind = 0;
  int layout = 0;
  Bigarray1Val(std::vector<uint8_t> d, int k = 0, int l = 0)
      : data(std::move(d)), kind(k), layout(l) {}
};

// Represents an already-evaluated argument stored in a partial closure.
struct AppliedArg {
  ArgKind kind = ArgKind::kPositional;
  SymbolId label = kInvalidSymbol;
  Value value;
};

// Heap-allocated function closure payload.
struct ClosureVal {
  const std::vector<FuncParam>* params = nullptr;
  const Expr* body = nullptr;
  std::shared_ptr<Env> env;
  SymbolId name = kInvalidSymbol;
  std::vector<AppliedArg> applied_args;
  // Optional owned parameter list for dynamically synthesized closures.
  std::shared_ptr<std::vector<FuncParam>> owned_params;
  // Optional C++ callback for native closures.
  std::function<Value(const std::vector<AppliedArg>&)> native_fn;
};

// Heap-allocated external primitive function payload.
struct PrimitiveVal {
  std::string name;
  int arity = -1;
  std::function<Value(absl::Span<const Value>)> fn;
  std::vector<Value> bound_args;
};

// Heap-allocated first-class or structure module payload.
struct ModuleVal {
  std::shared_ptr<Env> env;
  explicit ModuleVal(std::shared_ptr<Env> e) : env(std::move(e)) {}
};

// Heap-allocated lazy value payload.
struct LazyVal {
  bool forced = false;
  bool evaluating = false;
  Value value;
  const Expr* expr = nullptr;
  std::shared_ptr<Env> env;
  std::function<Value()> thunk;
};

// Heap-allocated I/O channel (`in_channel` / `out_channel`) payload.
struct ChannelVal {
  FILE* fp = nullptr;
  int fd = -1;
  bool is_input = false;
  bool is_output = false;
  bool owns_fp = false;
  bool binary_mode = true;
  bool closed = false;
  std::string name;
};

// Represents a functor definition attached to a module `Env`.
struct FunctorDef {
  std::vector<std::optional<SymbolId>> param_names;
  size_t param_index = 0;
  const ModuleExpr* body = nullptr;
  std::shared_ptr<Env> lexical_env;
};

// Lexical and module environment mapping symbols to values, submodules, and
// exception stamps.
class Env : public std::enable_shared_from_this<Env> {
 public:
  explicit Env(std::shared_ptr<Env> parent = nullptr)
      : parent_(std::move(parent)) {}

  std::shared_ptr<Env> parent() const { return parent_; }
  void set_parent(std::shared_ptr<Env> p) { parent_ = std::move(p); }

  const std::optional<FunctorDef>& functor() const { return functor_; }
  void set_functor(FunctorDef f) { functor_ = std::move(f); }
  bool IsFunctor() const { return functor_.has_value(); }

  // Defines or overwrites a value binding in this environment.
  void DefineValue(SymbolId name, Value val);

  // Defines or overwrites a submodule binding in this environment.
  void DefineModule(SymbolId name, std::shared_ptr<Env> mod);

  // Defines an exception or extensible variant stamp in this environment.
  void DefineExnStamp(SymbolId name, int64_t stamp);

  // Appends a module to the list of opened modules in this scope (`open M`).
  void OpenModule(std::shared_ptr<Env> mod);

  // Copies all exported bindings from `mod` into this environment (`include M`).
  void IncludeModule(const Env& mod);

  // Removes any value bindings from this module environment whose symbol is not
  // in `allowed`.
  void FilterExportedValues(const absl::flat_hash_set<SymbolId>& allowed);

  // Looks up a value in this environment, opened modules, and parent scopes.
  const Value* LookupValue(SymbolId name) const;

  // Looks up a mutable pointer to a value in this environment (for `let rec`).
  Value* LookupValueMutable(SymbolId name);

  // Looks up a possibly module-qualified value (`M1.M2.x`).
  const Value* LookupValue(absl::Span<const SymbolId> module_path,
                           SymbolId name) const;

  // Looks up a value only in this module's own bindings (not walking parents).
  const Value* LookupExportedValue(SymbolId name) const;

  // Looks up a submodule in this environment, opened modules, and parents.
  std::shared_ptr<Env> LookupModule(SymbolId name) const;

  // Looks up a submodule only in this module's own bindings.
  std::shared_ptr<Env> LookupExportedModule(SymbolId name) const;

  // Resolves a sequence of module identifiers (`M1.M2...`).
  std::shared_ptr<Env> LookupModulePath(
      absl::Span<const SymbolId> module_path) const;

  // Looks up an exception or extensible constructor stamp.
  int64_t LookupExnStamp(SymbolId name) const;

  // Looks up a possibly module-qualified exception or extensible constructor.
  int64_t LookupExnStamp(absl::Span<const SymbolId> module_path,
                         SymbolId name) const;

  // Looks up an exception stamp only in this module's own bindings.
  int64_t LookupExportedExnStamp(SymbolId name) const;

  const absl::flat_hash_map<SymbolId, std::shared_ptr<Value>>& values() const {
    return values_;
  }
  const absl::flat_hash_map<SymbolId, std::shared_ptr<Env>>& modules() const {
    return modules_;
  }
  const absl::flat_hash_map<SymbolId, int64_t>& exn_stamps() const {
    return exn_stamps_;
  }
  const std::vector<std::shared_ptr<Env>>& opened_modules() const {
    return opened_modules_;
  }

 private:
  std::shared_ptr<Env> parent_;
  // Stores shared_ptr<Value> cells so `let rec` back-patching and exported
  // aliases never invalidate pointers on flat_hash_map rehash.
  absl::flat_hash_map<SymbolId, std::shared_ptr<Value>> values_;
  absl::flat_hash_map<SymbolId, std::shared_ptr<Env>> modules_;
  absl::flat_hash_map<SymbolId, int64_t> exn_stamps_;
  std::vector<std::shared_ptr<Env>> opened_modules_;
  std::optional<FunctorDef> functor_;
};

// C++ exception thrown when an OCaml exception is raised.
class OcamlException : public std::exception {
 public:
  explicit OcamlException(Value payload, bool is_reraise = false)
      : payload_(std::move(payload)), is_reraise_(is_reraise) {}

  const Value& value() const { return payload_; }
  Value& mutable_value() { return payload_; }
  bool is_reraise() const { return is_reraise_; }

  const char* what() const noexcept override { return "OcamlException"; }

 private:
  Value payload_;
  bool is_reraise_ = false;
};

// C++ exception thrown by `caml_sys_exit` so batch drivers and unit tests can
// intercept `Stdlib.exit` without terminating the host process.
class OcamlSysExit : public std::exception {
 public:
  explicit OcamlSysExit(int exit_code) : exit_code_(exit_code) {}
  int exit_code() const noexcept { return exit_code_; }
  const char* what() const noexcept override { return "OcamlSysExit"; }

 private:
  int exit_code_ = 0;
};

// Returns a canonical nonzero stamp for the 12 built-in OCaml runtime
// exceptions, or 0 if `name` is not one of the predefined runtime exceptions.
int64_t WellKnownExnStamp(absl::string_view name);

// Allocates a fresh unique exception/extensible-variant stamp (or returns the
// well-known stamp the first time a standard exception is registered).
int64_t AllocateExnStamp(absl::string_view name = "");

// Standard OCaml exception value constructors.
Value MakeExnNotFound(InternTable& intern);
Value MakeExnEndOfFile(InternTable& intern);
Value MakeExnDivisionByZero(InternTable& intern);
Value MakeExnOutOfMemory(InternTable& intern);
Value MakeExnSysBlockedIo(InternTable& intern);
Value MakeExnInvalidArgument(InternTable& intern, absl::string_view msg);
Value MakeExnFailure(InternTable& intern, absl::string_view msg);
Value MakeExnSysError(InternTable& intern, absl::string_view msg);
Value MakeExnMatchFailure(InternTable& intern, absl::string_view file,
                          int64_t line, int64_t col);
Value MakeExnAssertFailure(InternTable& intern, absl::string_view file,
                           int64_t line, int64_t col);
Value MakeExnUndefinedRecursiveModule(InternTable& intern,
                                      absl::string_view file, int64_t line,
                                      int64_t col);

// Structural comparison matching OCaml's `Stdlib.compare` (`total = true`) and
// relational operators `<`, `<=`, `>`, `>=` (`total = false`).
// Returns < 0 if a < b, 0 if a == b, > 0 if a > b, or INT64_MIN for unordered
// NaN when `total == false`.
int64_t CompareValues(const Value& a, const Value& b, bool total,
                      const InternTable* intern = nullptr);

// Structural equality matching OCaml's `=` operator.
bool EqualValues(const Value& a, const Value& b,
                 const InternTable* intern = nullptr);

// Computes OCaml's MurmurHash3-based `Hashtbl.hash` / `caml_hash`.
int64_t HashValue(int64_t meaningful, int64_t limit, uint32_t seed,
                  const Value& v, const InternTable* intern = nullptr);

// Formats a runtime Value for debugging and test assertions.
std::string DebugString(const Value& v, const InternTable* intern = nullptr);

}  // namespace ocaml_boot

#endif  // OCAML_BOOT_VALUE_H_
