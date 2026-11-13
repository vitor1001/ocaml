// Copyright 2026 Google LLC.
// Evaluator for the OCaml 5.3 bootstrap interpreter.

#ifndef OCAML_BOOT_EVAL_H_
#define OCAML_BOOT_EVAL_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "third_party/absl/container/flat_hash_map.h"
#include "third_party/absl/strings/string_view.h"
#include "third_party/absl/types/span.h"
#include "ast.h"
#include "intern.h"
#include "value.h"

namespace ocaml_boot {

// Evaluates lowered OCaml AST structures, expressions, patterns, closures,
// exceptions, and modules/functors.
class Evaluator {
 public:
  explicit Evaluator(InternTable& intern);

  InternTable& intern() { return intern_; }
  const InternTable& intern() const { return intern_; }

  std::shared_ptr<Env> root_env() const { return root_env_; }
  std::shared_ptr<Env> stdlib_env() const { return stdlib_env_; }
  void set_stdlib_env(std::shared_ptr<Env> env) {
    stdlib_env_ = std::move(env);
  }

  void SetArgv(std::vector<std::string> argv) { argv_ = std::move(argv); }
  const std::vector<std::string>& argv() const { return argv_; }

  // Evaluates an expression in the given lexical environment.
  Value EvalExpr(const Expr& expr, const std::shared_ptr<Env>& env);

  // Evaluates a sequence of structure items into `env`.
  void EvalStructure(const Structure& items, const std::shared_ptr<Env>& env);

  // Evaluates a module expression in `env` and returns the resulting module
  // environment.
  std::shared_ptr<Env> EvalModule(const ModuleExpr& mod_expr,
                                  const std::shared_ptr<Env>& env);

  // Evaluates `items` as a top-level module named `module_name`, registering it
  // in `root_env_` (and `stdlib_env_` when `alias_name` is non-empty).
  std::shared_ptr<Env> LoadModule(absl::string_view module_name,
                                  const Structure& items,
                                  bool open_stdlib = true,
                                  absl::string_view alias_name = "");

  // Matches `val` against `pat` in `env`, appending variable bindings to
  // `bindings` and submodule bindings (from `(module M)`) to `mod_bindings`.
  bool MatchPattern(
      const Pattern& pat, const Value& val, const std::shared_ptr<Env>& env,
      std::vector<std::pair<SymbolId, Value>>* bindings,
      std::vector<std::pair<SymbolId, std::shared_ptr<Env>>>* mod_bindings =
          nullptr);

  // Applies a closure or primitive `fn` to a list of labeled/optional/positional
  // arguments.
  Value Apply(const Value& fn, std::vector<AppliedArg> args);

  // Applies a closure or primitive `fn` to a span of positional `Value`
  // arguments.
  Value ApplyValues(const Value& fn, absl::Span<const Value> args);

  // Forces a `Lazy` value (or returns `v` unchanged if it is not a `Lazy` or
  // `lazy_tag` / `forward_tag` raw block).
  Value ForceLazy(const Value& v);

  // Registers a named value for `Callback.register` / `caml_register_named_value`.
  void RegisterNamedValue(absl::string_view name, Value val);

  // Looks up a named value registered via `RegisterNamedValue`.
  const Value* LookupNamedValue(absl::string_view name) const;

  // Registers an external primitive with the given name and arity.
  void RegisterPrimitive(
      absl::string_view name, int arity,
      std::function<Value(absl::Span<const Value>)> fn);

  // Looks up or creates a `Value::Primitive` for `prim_name`.
  Value LookupPrimitive(absl::string_view prim_name);

  // Allocates the next unique object ID for `Oo.id` / `caml_set_oo_id` /
  // `ObjectDummy`.
  int64_t NextObjectId() { return ++next_object_id_; }

 private:
  void EvalLetBindings(bool is_rec, const std::vector<LetBinding>& bindings,
                       const std::shared_ptr<Env>& rhs_env,
                       const std::shared_ptr<Env>& target_env,
                       Env* export_env = nullptr);

  std::shared_ptr<Env> ApplyFunctor(const std::shared_ptr<Env>& func_mod,
                                    const std::shared_ptr<Env>& arg_mod);

  std::optional<Value> ComputeFormatOfString(const std::string& str,
                                             const std::shared_ptr<Env>& env);

  InternTable& intern_;
  std::shared_ptr<Env> root_env_;
  std::shared_ptr<Env> stdlib_env_;
  std::vector<std::string> argv_;

  SymbolId sym_some_ = kInvalidSymbol;
  SymbolId sym_none_ = kInvalidSymbol;
  SymbolId sym_error_ = kInvalidSymbol;
  SymbolId sym_format_ = kInvalidSymbol;
  SymbolId sym_fmt_ebb_ = kInvalidSymbol;
  SymbolId sym_fmt_ebb_of_string_ = kInvalidSymbol;
  SymbolId sym_camlinternal_format_ = kInvalidSymbol;
  SymbolId sym_stdlib_camlinternal_format_ = kInvalidSymbol;
  SymbolId sym_contents_ = kInvalidSymbol;

  int64_t next_object_id_ = 0;

  absl::flat_hash_map<std::string, std::shared_ptr<PrimitiveVal>> primitives_;
  absl::flat_hash_map<std::string, Value> named_values_;
  absl::flat_hash_map<std::string, Value> format_cache_;
};

}  // namespace ocaml_boot

#endif  // OCAML_BOOT_EVAL_H_
