// Copyright 2026 Google LLC.
// Implementation of the OCaml 5.3 bootstrap AST evaluator.

#include "eval.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "third_party/absl/log/check.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/string_view.h"
#include "third_party/absl/types/span.h"
#include "ast.h"
#include "intern.h"
#include "prims.h"
#include "value.h"

namespace ocaml_boot {
namespace {

// Recursively collects all variable symbols bound by a pattern.
void CollectPatternVars(const Pattern& pat, std::vector<SymbolId>* vars) {
  std::visit(
      [&](const auto& node) {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, Pattern::Var>) {
          if (node.name != kInvalidSymbol) {
            vars->push_back(node.name);
          }
        } else if constexpr (std::is_same_v<T, Pattern::Tuple>) {
          for (const auto& elem : node.elements) {
            if (elem != nullptr) {
              CollectPatternVars(*elem, vars);
            }
          }
        } else if constexpr (std::is_same_v<T, Pattern::Construct>) {
          if (node.arg != nullptr) {
            CollectPatternVars(*node.arg, vars);
          }
        } else if constexpr (std::is_same_v<T, Pattern::Record>) {
          for (const auto& [field, sub] : node.fields) {
            if (sub != nullptr) {
              CollectPatternVars(*sub, vars);
            }
          }
        } else if constexpr (std::is_same_v<T, Pattern::Array>) {
          for (const auto& elem : node.elements) {
            if (elem != nullptr) {
              CollectPatternVars(*elem, vars);
            }
          }
        } else if constexpr (std::is_same_v<T, Pattern::Or>) {
          if (node.left != nullptr) {
            CollectPatternVars(*node.left, vars);
          }
        } else if constexpr (std::is_same_v<T, Pattern::Alias>) {
          if (node.pat != nullptr) {
            CollectPatternVars(*node.pat, vars);
          }
          if (node.name != kInvalidSymbol) {
            vars->push_back(node.name);
          }
        } else if constexpr (std::is_same_v<T, Pattern::Lazy>) {
          if (node.pat != nullptr) {
            CollectPatternVars(*node.pat, vars);
          }
        } else if constexpr (std::is_same_v<T, Pattern::Exception>) {
          if (node.pat != nullptr) {
            CollectPatternVars(*node.pat, vars);
          }
        }
      },
      pat.node);
}

// Pre-allocates a placeholder runtime Value matching the top-level syntactic
// shape of a `let rec` right-hand-side expression so self/mutual references can
// be patched in place after evaluation.
Value CreateLetRecPlaceholder(const Expr& expr) {
  return std::visit(
      [&](const auto& node) -> Value {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, Expr::Function>) {
          return Value::Closure(std::make_shared<ClosureVal>());
        } else if constexpr (std::is_same_v<T, Expr::Lazy>) {
          return Value::Lazy(std::make_shared<LazyVal>());
        } else if constexpr (std::is_same_v<T, Expr::Construct>) {
          return Value::Constructor(
              node.ctor_name, 0,
              node.arg != nullptr ? std::optional<Value>(Value::Unit())
                                  : std::nullopt,
              node.is_poly_var);
        } else if constexpr (std::is_same_v<T, Expr::Tuple>) {
          return Value::Tuple(
              std::vector<Value>(node.elements.size(), Value::Unit()));
        } else if constexpr (std::is_same_v<T, Expr::Record>) {
          std::vector<std::pair<SymbolId, Value>> fields;
          fields.reserve(node.fields.size());
          for (const auto& [k, _] : node.fields) {
            fields.emplace_back(k, Value::Unit());
          }
          return Value::Record(std::move(fields));
        } else if constexpr (std::is_same_v<T, Expr::ArrayLit>) {
          return Value::Array(
              std::vector<Value>(node.elements.size(), Value::Unit()));
        } else {
          return Value::Closure(std::make_shared<ClosureVal>());
        }
      },
      expr.node);
}

// Back-patches a `let rec` placeholder value in place when the evaluated value
// shares the same heap representation kind, and updates the environment cell.
void PatchLetRecValue(Value* slot, const Value& evaluated) {
  if (slot == nullptr) {
    return;
  }
  if (slot->IsClosure() && evaluated.IsClosure()) {
    *slot->AsClosure() = *evaluated.AsClosure();
    return;
  }
  if (slot->IsLazy() && evaluated.IsLazy()) {
    *slot->AsLazy() = *evaluated.AsLazy();
    return;
  }
  if (slot->IsConstructor() && evaluated.IsConstructor()) {
    *slot->AsConstructor() = *evaluated.AsConstructor();
    return;
  }
  if (slot->IsTuple() && evaluated.IsTuple()) {
    *slot->AsTuple() = *evaluated.AsTuple();
    return;
  }
  if (slot->IsRecord() && evaluated.IsRecord()) {
    *slot->AsRecord() = *evaluated.AsRecord();
    return;
  }
  if (slot->IsArray() && evaluated.IsArray()) {
    *slot->AsArray() = *evaluated.AsArray();
    return;
  }
  *slot = evaluated;
}

// Formats a `SourceLoc` into OCaml's `%loc_LOC` string format:
// `File "foo.ml", line 10, characters 4-12`.
std::string FormatLocString(const SourceLoc& loc) {
  return absl::StrCat("File \"", loc.filename, "\", line ", loc.line,
                      ", characters ", loc.start_col, "-", loc.end_col);
}

}  // namespace

Evaluator::Evaluator(InternTable& intern)
    : intern_(intern), root_env_(std::make_shared<Env>()) {
  sym_some_ = intern_.Intern("Some");
  sym_none_ = intern_.Intern("None");
  sym_error_ = intern_.Intern("Error");
  sym_format_ = intern_.Intern("Format");
  sym_fmt_ebb_ = intern_.Intern("Fmt_EBB");
  sym_fmt_ebb_of_string_ = intern_.Intern("fmt_ebb_of_string");
  sym_camlinternal_format_ = intern_.Intern("CamlinternalFormat");
  sym_stdlib_camlinternal_format_ =
      intern_.Intern("Stdlib__CamlinternalFormat");
  sym_contents_ = intern_.Intern("contents");

  // Pre-populate the 12 built-in runtime exceptions in `root_env_` so they are
  // always in scope even before `Stdlib` is loaded.
  constexpr absl::string_view kBuiltinExceptions[] = {
      "Out_of_memory",
      "Sys_error",
      "Failure",
      "Invalid_argument",
      "End_of_file",
      "Division_by_zero",
      "Not_found",
      "Match_failure",
      "Stack_overflow",
      "Sys_blocked_io",
      "Assert_failure",
      "Undefined_recursive_module",
  };
  for (absl::string_view exn_name : kBuiltinExceptions) {
    root_env_->DefineExnStamp(intern_.Intern(exn_name),
                              WellKnownExnStamp(exn_name));
  }

  argv_.push_back("ocaml_boot");
  RegisterPrimitives(*this);
}

void Evaluator::RegisterNamedValue(absl::string_view name, Value val) {
  named_values_[std::string(name)] = std::move(val);
}

const Value* Evaluator::LookupNamedValue(absl::string_view name) const {
  auto it = named_values_.find(name);
  if (it == named_values_.end()) {
    return nullptr;
  }
  return &it->second;
}

void Evaluator::RegisterPrimitive(
    absl::string_view name, int arity,
    std::function<Value(absl::Span<const Value>)> fn) {
  auto p = std::make_shared<PrimitiveVal>();
  p->name = std::string(name);
  p->arity = arity;
  p->fn = std::move(fn);
  primitives_[p->name] = std::move(p);
}

Value Evaluator::LookupPrimitive(absl::string_view prim_name) {
  auto it = primitives_.find(prim_name);
  if (it != primitives_.end()) {
    if (it->second->arity == 0) {
      return it->second->fn({});
    }
    return Value::Primitive(it->second);
  }
  // Create a stub primitive that reports an informative error if called.
  auto p = std::make_shared<PrimitiveVal>();
  p->name = std::string(prim_name);
  p->arity = 1;
  InternTable* intern_ptr = &intern_;
  std::string name_copy(prim_name);
  p->fn = [intern_ptr, name_copy](absl::Span<const Value>) -> Value {
    throw OcamlException(MakeExnFailure(
        *intern_ptr, absl::StrCat("Unimplemented primitive: ", name_copy)));
  };
  return Value::Primitive(std::move(p));
}

Value Evaluator::ForceLazy(const Value& v) {
  if (v.IsLazy()) {
    const auto& l = v.AsLazy();
    if (l->forced) {
      return l->value;
    }
    if (l->evaluating) {
      SymbolId undef_sym = intern_.Intern("Undefined");
      int64_t stamp = 0;
      if (auto m = root_env_->LookupModule(
              intern_.Intern("CamlinternalLazy"))) {
        stamp = m->LookupExportedExnStamp(undef_sym);
      }
      if (stamp == 0 && stdlib_env_ != nullptr) {
        if (auto m = stdlib_env_->LookupModule(intern_.Intern("Lazy"))) {
          stamp = m->LookupExportedExnStamp(undef_sym);
        }
      }
      throw OcamlException(Value::Constructor(undef_sym, stamp));
    }
    l->evaluating = true;
    try {
      Value res = l->thunk ? l->thunk() : EvalExpr(*l->expr, l->env);
      res = ForceLazy(res);
      l->value = res;
      l->forced = true;
      l->evaluating = false;
      return l->value;
    } catch (...) {
      l->evaluating = false;
      throw;
    }
  }
  if (v.IsRawBlock()) {
    const auto& rb = v.AsRawBlock();
    // Forward_tag == 250, Lazy_tag == 246.
    if (rb->tag == 250 && !rb->fields.empty()) {
      return rb->fields[0];
    }
    if (rb->tag == 246 && !rb->fields.empty()) {
      Value closure = rb->fields[0];
      rb->tag = 244;  // Forcing_tag.
      try {
        std::vector<Value> unit_arg = {Value::Unit()};
        Value res = ApplyValues(closure, unit_arg);
        rb->fields[0] = res;
        rb->tag = 250;  // Forward_tag.
        return res;
      } catch (...) {
        rb->tag = 246;
        throw;
      }
    }
  }
  return v;
}

std::optional<Value> Evaluator::ComputeFormatOfString(
    const std::string& str, const std::shared_ptr<Env>& env) {
  auto cache_it = format_cache_.find(str);
  if (cache_it != format_cache_.end()) {
    return cache_it->second;
  }

  std::shared_ptr<Env> fmt_mod = nullptr;
  if (env != nullptr) {
    fmt_mod = env->LookupModule(sym_camlinternal_format_);
    if (fmt_mod == nullptr) {
      fmt_mod = env->LookupModule(sym_stdlib_camlinternal_format_);
    }
  }
  if (fmt_mod == nullptr && root_env_ != nullptr) {
    fmt_mod = root_env_->LookupModule(sym_camlinternal_format_);
    if (fmt_mod == nullptr) {
      fmt_mod = root_env_->LookupModule(sym_stdlib_camlinternal_format_);
    }
  }
  if (fmt_mod == nullptr && stdlib_env_ != nullptr) {
    fmt_mod = stdlib_env_->LookupModule(sym_camlinternal_format_);
    if (fmt_mod == nullptr) {
      fmt_mod = stdlib_env_->LookupModule(sym_stdlib_camlinternal_format_);
    }
  }
  if (fmt_mod == nullptr) {
    return std::nullopt;
  }

  const Value* fn = fmt_mod->LookupExportedValue(sym_fmt_ebb_of_string_);
  if (fn == nullptr) {
    fn = fmt_mod->LookupValue(sym_fmt_ebb_of_string_);
  }
  if (fn == nullptr) {
    return std::nullopt;
  }

  std::vector<Value> args = {Value::String(str)};
  Value ebb = ApplyValues(*fn, args);
  if (ebb.IsConstructor() && ebb.AsConstructor()->ctor == sym_fmt_ebb_ &&
      ebb.AsConstructor()->arg.has_value()) {
    Value fmt = *ebb.AsConstructor()->arg;
    format_cache_.emplace(str, fmt);
    return fmt;
  }
  return std::nullopt;
}

bool Evaluator::MatchPattern(
    const Pattern& pat, const Value& raw_val, const std::shared_ptr<Env>& env,
    std::vector<std::pair<SymbolId, Value>>* bindings,
    std::vector<std::pair<SymbolId, std::shared_ptr<Env>>>* mod_bindings) {
  const Value& val =
      (raw_val.IsLazy() && raw_val.AsLazy()->forced &&
       !std::holds_alternative<Pattern::Lazy>(pat.node))
          ? raw_val.AsLazy()->value
          : raw_val;

  return std::visit(
      [&](const auto& node) -> bool {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, Pattern::Wildcard>) {
          return true;
        } else if constexpr (std::is_same_v<T, Pattern::Var>) {
          if (node.name != kInvalidSymbol && bindings != nullptr) {
            bindings->emplace_back(node.name, val);
          }
          return true;
        } else if constexpr (std::is_same_v<T, Pattern::Const>) {
          switch (node.kind) {
            case Pattern::Const::Kind::kUnit:
              return val.IsIntLike() && val.AsInt() == 0;
            case Pattern::Const::Kind::kBool:
              return val.IsIntLike() && (val.AsInt() != 0) == node.bool_val;
            case Pattern::Const::Kind::kChar:
              return val.IsIntLike() && val.AsChar() == node.char_val;
            case Pattern::Const::Kind::kInt:
              return val.IsIntLike() && val.AsInt() == node.int_val;
            case Pattern::Const::Kind::kInt32:
              return (val.IsInt32() || val.IsIntLike()) &&
                     val.AsInt32() == static_cast<int32_t>(node.int_val);
            case Pattern::Const::Kind::kInt64:
              return (val.IsInt64() || val.IsIntLike()) &&
                     val.AsInt64() == node.int_val;
            case Pattern::Const::Kind::kNativeint:
              return (val.IsNativeint() || val.IsIntLike()) &&
                     val.AsNativeint() == node.int_val;
            case Pattern::Const::Kind::kFloat:
              return val.IsFloat() && val.AsFloat() == node.float_val;
            case Pattern::Const::Kind::kString:
              return val.IsString() && val.AsString()->data == node.str_val;
          }
          return false;
        } else if constexpr (std::is_same_v<T, Pattern::Range>) {
          if (!val.IsIntLike()) {
            return false;
          }
          uint8_t c = val.AsChar();
          uint8_t lo = static_cast<uint8_t>(node.lo);
          uint8_t hi = static_cast<uint8_t>(node.hi);
          return c >= lo && c <= hi;
        } else if constexpr (std::is_same_v<T, Pattern::Tuple>) {
          if (!val.IsTuple()) {
            return false;
          }
          const auto& elems = val.AsTuple()->elements;
          if (elems.size() != node.elements.size()) {
            return false;
          }
          for (size_t i = 0; i < elems.size(); ++i) {
            if (!MatchPattern(*node.elements[i], elems[i], env, bindings,
                              mod_bindings)) {
              return false;
            }
          }
          return true;
        } else if constexpr (std::is_same_v<T, Pattern::Construct>) {
          // Format6 string hook: when matching `Format (p_fmt, p_str)` against
          // a `String` value, dynamically parse the format string with
          // `CamlinternalFormat.fmt_ebb_of_string`.
          if (node.ctor_name == sym_format_ && val.IsString() &&
              node.arg != nullptr) {
            auto fmt_opt = ComputeFormatOfString(val.AsString()->data, env);
            if (!fmt_opt.has_value()) {
              return false;
            }
            Value pair = Value::Tuple({*fmt_opt, val});
            return MatchPattern(*node.arg, pair, env, bindings, mod_bindings);
          }

          if (!val.IsConstructor()) {
            return false;
          }
          const auto& c = val.AsConstructor();
          if (c->is_poly_var != node.is_poly_var) {
            return false;
          }

          int64_t pat_stamp = 0;
          if (!node.is_poly_var && env != nullptr) {
            pat_stamp = env->LookupExnStamp(node.module_path, node.ctor_name);
          }
          bool is_non_exn_error =
              (c->ctor == sym_error_ && node.ctor_name == sym_error_ &&
               !c->is_raised_exn);
          if (!is_non_exn_error && (pat_stamp != 0 || c->exn_stamp != 0)) {
            if (pat_stamp != 0 && c->exn_stamp != 0) {
              if (pat_stamp != c->exn_stamp) {
                return false;
              }
            } else if (c->ctor != node.ctor_name) {
              return false;
            } else {
              return false;
            }
          } else if (c->ctor != node.ctor_name) {
            return false;
          }

          if (node.arg == nullptr) {
            return !c->arg.has_value();
          }
          if (!c->arg.has_value()) {
            return false;
          }
          return MatchPattern(*node.arg, *c->arg, env, bindings, mod_bindings);
        } else if constexpr (std::is_same_v<T, Pattern::Record>) {
          if (!val.IsRecord()) {
            return false;
          }
          const auto& rec = val.AsRecord();
          for (const auto& [field_sym, field_pat] : node.fields) {
            const Value* fv = rec->Find(field_sym);
            if (fv == nullptr) {
              return false;
            }
            if (!MatchPattern(*field_pat, *fv, env, bindings, mod_bindings)) {
              return false;
            }
          }
          return true;
        } else if constexpr (std::is_same_v<T, Pattern::Array>) {
          if (val.IsArray()) {
            const auto& elems = val.AsArray()->elements;
            if (elems.size() != node.elements.size()) {
              return false;
            }
            for (size_t i = 0; i < elems.size(); ++i) {
              if (!MatchPattern(*node.elements[i], elems[i], env, bindings,
                                mod_bindings)) {
                return false;
              }
            }
            return true;
          }
          if (val.IsFloatArray()) {
            const auto& elems = val.AsFloatArray()->elements;
            if (elems.size() != node.elements.size()) {
              return false;
            }
            for (size_t i = 0; i < elems.size(); ++i) {
              if (!MatchPattern(*node.elements[i], Value::Float(elems[i]), env,
                                bindings, mod_bindings)) {
                return false;
              }
            }
            return true;
          }
          return false;
        } else if constexpr (std::is_same_v<T, Pattern::Or>) {
          size_t saved_val_size = bindings != nullptr ? bindings->size() : 0;
          size_t saved_mod_size =
              mod_bindings != nullptr ? mod_bindings->size() : 0;
          if (MatchPattern(*node.left, val, env, bindings, mod_bindings)) {
            return true;
          }
          if (bindings != nullptr) {
            bindings->resize(saved_val_size);
          }
          if (mod_bindings != nullptr) {
            mod_bindings->resize(saved_mod_size);
          }
          return MatchPattern(*node.right, val, env, bindings, mod_bindings);
        } else if constexpr (std::is_same_v<T, Pattern::Alias>) {
          if (!MatchPattern(*node.pat, val, env, bindings, mod_bindings)) {
            return false;
          }
          if (node.name != kInvalidSymbol && bindings != nullptr) {
            bindings->emplace_back(node.name, val);
          }
          return true;
        } else if constexpr (std::is_same_v<T, Pattern::Lazy>) {
          Value forced = ForceLazy(raw_val);
          return MatchPattern(*node.pat, forced, env, bindings, mod_bindings);
        } else if constexpr (std::is_same_v<T, Pattern::Exception>) {
          return false;
        } else if constexpr (std::is_same_v<T, Pattern::UnpackModule>) {
          if (!val.IsModule()) {
            return false;
          }
          if (node.name != kInvalidSymbol && mod_bindings != nullptr) {
            mod_bindings->emplace_back(node.name, val.AsModule()->env);
          }
          return true;
        }
      },
      pat.node);
}

Value Evaluator::ApplyValues(const Value& fn, absl::Span<const Value> args) {
  std::vector<AppliedArg> applied;
  applied.reserve(args.size());
  for (const Value& v : args) {
    AppliedArg a;
    a.kind = ArgKind::kPositional;
    a.value = v;
    applied.push_back(std::move(a));
  }
  return Apply(fn, std::move(applied));
}

Value Evaluator::Apply(const Value& fn, std::vector<AppliedArg> args) {
  if (args.empty()) {
    return fn;
  }

  if (fn.IsPrimitive()) {
    const auto& prim = fn.AsPrimitive();
    std::vector<Value> all_vals = prim->bound_args;
    all_vals.reserve(all_vals.size() + args.size());
    for (auto& a : args) {
      all_vals.push_back(std::move(a.value));
    }
    int arity = prim->arity;
    if (arity < 0) {
      return prim->fn(all_vals);
    }
    if (static_cast<int>(all_vals.size()) < arity) {
      auto partial = std::make_shared<PrimitiveVal>();
      partial->name = prim->name;
      partial->arity = prim->arity;
      partial->fn = prim->fn;
      partial->bound_args = std::move(all_vals);
      return Value::Primitive(std::move(partial));
    }
    if (static_cast<int>(all_vals.size()) == arity) {
      return prim->fn(all_vals);
    }
    absl::Span<const Value> first_batch(all_vals.data(), arity);
    Value res = prim->fn(first_batch);
    std::vector<AppliedArg> rem_args;
    rem_args.reserve(all_vals.size() - arity);
    for (size_t i = static_cast<size_t>(arity); i < all_vals.size(); ++i) {
      AppliedArg ra;
      ra.kind = ArgKind::kPositional;
      ra.value = std::move(all_vals[i]);
      rem_args.push_back(std::move(ra));
    }
    return Apply(res, std::move(rem_args));
  }

  if (!fn.IsClosure()) {
    throw OcamlException(MakeExnInvalidArgument(
        intern_, absl::StrCat("Attempted to apply non-function value: ",
                              DebugString(fn, &intern_))));
  }

  const auto& clos = fn.AsClosure();
  if (clos->native_fn) {
    std::vector<AppliedArg> combined = clos->applied_args;
    for (auto& a : args) {
      combined.push_back(std::move(a));
    }
    return clos->native_fn(combined);
  }

  CHECK(clos->params != nullptr);
  const auto& params = *clos->params;
  const size_t num_params = params.size();

  std::vector<AppliedArg> pool = clos->applied_args;
  pool.reserve(pool.size() + args.size());
  for (auto& a : args) {
    pool.push_back(std::move(a));
  }

  std::vector<bool> arg_used(pool.size(), false);
  std::vector<bool> param_filled(num_params, false);
  std::vector<Value> param_vals(num_params);
  bool consumed_positional = false;

  // Step 1: Match labeled (`~lbl`) and optional (`?lbl`) arguments in `pool`
  // against formal parameters with matching labels.
  for (size_t ai = 0; ai < pool.size(); ++ai) {
    if (pool[ai].kind == ArgKind::kPositional) {
      continue;
    }
    for (size_t pi = 0; pi < num_params; ++pi) {
      if (param_filled[pi]) {
        continue;
      }
      if ((params[pi].kind == ParamKind::kLabeled ||
           params[pi].kind == ParamKind::kOptional) &&
          params[pi].label == pool[ai].label) {
        param_filled[pi] = true;
        arg_used[ai] = true;
        if (params[pi].kind == ParamKind::kOptional) {
          if (pool[ai].kind == ArgKind::kLabeled) {
            param_vals[pi] =
                Value::Constructor(sym_some_, 0, pool[ai].value, false);
          } else {
            param_vals[pi] = pool[ai].value;
          }
        } else {
          param_vals[pi] = pool[ai].value;
        }
        break;
      }
    }
  }

  // Step 2: Walk parameters in order, consuming positional arguments and
  // defaulting preceding optional parameters when a positional argument is
  // supplied.
  for (size_t pi = 0; pi < num_params; ++pi) {
    if (param_filled[pi]) {
      continue;
    }
    if (params[pi].kind == ParamKind::kPositional) {
      size_t pos_ai = pool.size();
      for (size_t ai = 0; ai < pool.size(); ++ai) {
        if (!arg_used[ai] && pool[ai].kind == ArgKind::kPositional) {
          pos_ai = ai;
          break;
        }
      }
      if (pos_ai < pool.size()) {
        param_filled[pi] = true;
        param_vals[pi] = pool[pos_ai].value;
        arg_used[pos_ai] = true;
        consumed_positional = true;
        // Default all earlier unconsumed Optional parameters.
        for (size_t prev = 0; prev < pi; ++prev) {
          if (!param_filled[prev] &&
              params[prev].kind == ParamKind::kOptional) {
            param_filled[prev] = true;
            param_vals[prev] =
                Value::Constructor(sym_none_, 0, std::nullopt, false);
          }
        }
      }
    }
  }

  // Step 3: If at least one positional argument was consumed (or is waiting in
  // `pool` for a curried function returned by `body`) and all remaining
  // unconsumed parameters in `params` are Optional, default those Optional
  // parameters as well.
  bool has_unused_positional = false;
  for (size_t ai = 0; ai < pool.size(); ++ai) {
    if (!arg_used[ai] && pool[ai].kind == ArgKind::kPositional) {
      has_unused_positional = true;
      break;
    }
  }
  if (consumed_positional || has_unused_positional) {
    bool all_non_opt_filled = true;
    for (size_t pi = 0; pi < num_params; ++pi) {
      if (!param_filled[pi] && params[pi].kind != ParamKind::kOptional) {
        all_non_opt_filled = false;
        break;
      }
    }
    if (all_non_opt_filled) {
      for (size_t pi = 0; pi < num_params; ++pi) {
        if (!param_filled[pi] && params[pi].kind == ParamKind::kOptional) {
          param_filled[pi] = true;
          param_vals[pi] =
              Value::Constructor(sym_none_, 0, std::nullopt, false);
        }
      }
    }
  }

  // Fallback: if an application passed a labeled/optional argument, but no
  // parameter matched its label (e.g. a higher-order function `let id x = x`
  // receiving a labeled argument or a function whose parameter had no label in
  // untyped AST), bind any remaining unused arguments to unfilled positional
  // parameters if all labeled/optional parameters are already satisfied.
  bool any_labeled_or_opt_unfilled = false;
  for (size_t pi = 0; pi < num_params; ++pi) {
    if (!param_filled[pi] && params[pi].kind != ParamKind::kPositional) {
      any_labeled_or_opt_unfilled = true;
      break;
    }
  }
  if (!any_labeled_or_opt_unfilled) {
    for (size_t pi = 0; pi < num_params; ++pi) {
      if (!param_filled[pi] && params[pi].kind == ParamKind::kPositional) {
        for (size_t ai = 0; ai < pool.size(); ++ai) {
          if (!arg_used[ai]) {
            param_filled[pi] = true;
            param_vals[pi] = pool[ai].value;
            arg_used[ai] = true;
            break;
          }
        }
      }
    }
  }

  bool all_filled = true;
  for (size_t pi = 0; pi < num_params; ++pi) {
    if (!param_filled[pi]) {
      all_filled = false;
      break;
    }
  }

  if (!all_filled) {
    // Partial application: return a new closure accumulating `pool`.
    auto partial = std::make_shared<ClosureVal>();
    partial->params = clos->params;
    partial->owned_params = clos->owned_params;
    partial->body = clos->body;
    partial->env = clos->env;
    partial->name = clos->name;
    partial->applied_args = std::move(pool);
    return Value::Closure(std::move(partial));
  }

  // All parameters of this closure are satisfied! Bind them in a fresh child
  // environment of `clos->env` in parameter order (evaluating default_expr for
  // defaulted optional parameters).
  auto call_env = std::make_shared<Env>(clos->env);
  for (size_t pi = 0; pi < num_params; ++pi) {
    Value v = std::move(param_vals[pi]);
    if (params[pi].kind == ParamKind::kOptional &&
        params[pi].default_expr != nullptr) {
      if (v.IsConstructor() && v.AsConstructor()->ctor == sym_none_ &&
          !v.AsConstructor()->arg.has_value()) {
        v = EvalExpr(*params[pi].default_expr, call_env);
      } else if (v.IsConstructor() && v.AsConstructor()->ctor == sym_some_ &&
                 v.AsConstructor()->arg.has_value()) {
        v = *v.AsConstructor()->arg;
      }
    }
    if (params[pi].pattern != nullptr) {
      std::vector<std::pair<SymbolId, Value>> bindings;
      std::vector<std::pair<SymbolId, std::shared_ptr<Env>>> mod_bindings;
      if (!MatchPattern(*params[pi].pattern, v, call_env, &bindings,
                        &mod_bindings)) {
        const SourceLoc& loc = params[pi].pattern->loc;
        throw OcamlException(
            MakeExnMatchFailure(intern_, loc.filename, loc.line, loc.start_col));
      }
      for (auto& [sym, bval] : bindings) {
        call_env->DefineValue(sym, std::move(bval));
      }
      for (auto& [msym, menv] : mod_bindings) {
        call_env->DefineModule(msym, std::move(menv));
      }
    }
  }

  Value result = EvalExpr(*clos->body, call_env);

  // Collect any leftover arguments (over-application) and apply `result` to
  // them.
  std::vector<AppliedArg> leftover;
  for (size_t ai = 0; ai < pool.size(); ++ai) {
    if (!arg_used[ai]) {
      leftover.push_back(std::move(pool[ai]));
    }
  }
  if (!leftover.empty()) {
    return Apply(result, std::move(leftover));
  }
  return result;
}

void Evaluator::EvalLetBindings(bool is_rec,
                                const std::vector<LetBinding>& bindings,
                                const std::shared_ptr<Env>& rhs_env,
                                const std::shared_ptr<Env>& target_env,
                                Env* export_env) {
  if (!is_rec) {
    std::vector<std::pair<SymbolId, Value>> all_val_bindings;
    std::vector<std::pair<SymbolId, std::shared_ptr<Env>>> all_mod_bindings;
    for (const LetBinding& lb : bindings) {
      Value v = EvalExpr(*lb.expr, rhs_env);
      if (lb.pattern != nullptr) {
        if (!MatchPattern(*lb.pattern, v, rhs_env, &all_val_bindings,
                          &all_mod_bindings)) {
          throw OcamlException(MakeExnMatchFailure(
              intern_, lb.loc.filename, lb.loc.line, lb.loc.start_col));
        }
      }
    }
    for (auto& [sym, val] : all_val_bindings) {
      if (export_env != nullptr) {
        export_env->DefineValue(sym, val);
      }
      target_env->DefineValue(sym, std::move(val));
    }
    for (auto& [msym, menv] : all_mod_bindings) {
      if (export_env != nullptr) {
        export_env->DefineModule(msym, menv);
      }
      target_env->DefineModule(msym, std::move(menv));
    }
    return;
  }

  // `let rec`:
  // 1. Pre-bind all pattern variables in `target_env` to placeholder cells.
  for (const LetBinding& lb : bindings) {
    if (lb.pattern == nullptr) continue;
    if (const auto* var_pat = std::get_if<Pattern::Var>(&lb.pattern->node)) {
      if (var_pat->name != kInvalidSymbol) {
        target_env->DefineValue(var_pat->name,
                                CreateLetRecPlaceholder(*lb.expr));
      }
    } else {
      std::vector<SymbolId> vars;
      CollectPatternVars(*lb.pattern, &vars);
      for (SymbolId v : vars) {
        target_env->DefineValue(v, Value::Unit());
      }
    }
  }

  // 2. Evaluate each RHS in `target_env` and back-patch the pre-allocated cells
  // in place so closures and recursive values observe the final definitions.
  for (const LetBinding& lb : bindings) {
    Value v = EvalExpr(*lb.expr, target_env);
    if (lb.pattern == nullptr) continue;
    if (const auto* var_pat = std::get_if<Pattern::Var>(&lb.pattern->node)) {
      if (var_pat->name != kInvalidSymbol) {
        Value* slot = target_env->LookupValueMutable(var_pat->name);
        PatchLetRecValue(slot, v);
        if (export_env != nullptr && slot != nullptr) {
          export_env->DefineValue(var_pat->name, *slot);
        }
      }
    } else {
      std::vector<std::pair<SymbolId, Value>> pat_bindings;
      std::vector<std::pair<SymbolId, std::shared_ptr<Env>>> mod_bindings;
      if (!MatchPattern(*lb.pattern, v, target_env, &pat_bindings,
                        &mod_bindings)) {
        throw OcamlException(MakeExnMatchFailure(
            intern_, lb.loc.filename, lb.loc.line, lb.loc.start_col));
      }
      for (auto& [sym, bval] : pat_bindings) {
        if (Value* slot = target_env->LookupValueMutable(sym)) {
          PatchLetRecValue(slot, bval);
          if (export_env != nullptr) {
            export_env->DefineValue(sym, *slot);
          }
        } else {
          if (export_env != nullptr) {
            export_env->DefineValue(sym, bval);
          }
          target_env->DefineValue(sym, std::move(bval));
        }
      }
      for (auto& [msym, menv] : mod_bindings) {
        if (export_env != nullptr) {
          export_env->DefineModule(msym, menv);
        }
        target_env->DefineModule(msym, std::move(menv));
      }
    }
  }
}

Value Evaluator::EvalExpr(const Expr& expr, const std::shared_ptr<Env>& env) {
  return std::visit(
      [&](const auto& node) -> Value {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, Expr::Int>) {
          return Value::Int(node.val);
        } else if constexpr (std::is_same_v<T, Expr::Int32>) {
          return Value::Int32(node.val);
        } else if constexpr (std::is_same_v<T, Expr::Int64>) {
          return Value::Int64(node.val);
        } else if constexpr (std::is_same_v<T, Expr::Nativeint>) {
          return Value::Nativeint(node.val);
        } else if constexpr (std::is_same_v<T, Expr::Float>) {
          return Value::Float(node.val);
        } else if constexpr (std::is_same_v<T, Expr::Char>) {
          return Value::Char(node.val);
        } else if constexpr (std::is_same_v<T, Expr::Bool>) {
          return Value::Bool(node.val);
        } else if constexpr (std::is_same_v<T, Expr::Unit>) {
          return Value::Unit();
        } else if constexpr (std::is_same_v<T, Expr::String>) {
          return Value::String(node.val);
        } else if constexpr (std::is_same_v<T, Expr::LocMacro>) {
          const SourceLoc& loc = expr.loc;
          switch (node.kind) {
            case LocMacroKind::kLocLOC:
              return Value::String(FormatLocString(loc));
            case LocMacroKind::kLocFILE:
              return Value::String(loc.filename);
            case LocMacroKind::kLocLINE:
              return Value::Int(loc.line);
            case LocMacroKind::kLocPOS:
              return Value::Tuple({Value::String(loc.filename),
                                   Value::Int(loc.line),
                                   Value::Int(loc.start_col),
                                   Value::Int(loc.end_col)});
            case LocMacroKind::kLocMODULE:
              return Value::String(loc.module_name);
            case LocMacroKind::kLocFUNCTION:
              return Value::String(loc.function_name);
            case LocMacroKind::kLocLOC_OF: {
              Value arg_val = node.arg != nullptr ? EvalExpr(*node.arg, env)
                                                  : Value::Unit();
              return Value::Tuple(
                  {Value::String(FormatLocString(loc)), std::move(arg_val)});
            }
          }
          return Value::Unit();
        } else if constexpr (std::is_same_v<T, Expr::Var>) {
          const Value* v = env->LookupValue(node.module_path, node.name);
          if (v != nullptr) {
            return *v;
          }
          if (stdlib_env_ != nullptr) {
            v = stdlib_env_->LookupValue(node.module_path, node.name);
            if (v != nullptr) {
              return *v;
            }
          }
          if (root_env_ != nullptr) {
            v = root_env_->LookupValue(node.module_path, node.name);
            if (v != nullptr) {
              return *v;
            }
          }
          std::string path_str;
          for (SymbolId m : node.module_path) {
            absl::StrAppend(&path_str, intern_.Resolve(m), ".");
          }
          absl::StrAppend(&path_str, intern_.Resolve(node.name));
          throw OcamlException(MakeExnFailure(
              intern_, absl::StrCat("Unbound value: ", path_str, " at ",
                                    expr.loc.filename, ":", expr.loc.line)));
        } else if constexpr (std::is_same_v<T, Expr::Construct>) {
          std::optional<Value> arg_val;
          if (node.arg != nullptr) {
            arg_val = EvalExpr(*node.arg, env);
          }
          int64_t stamp = 0;
          if (!node.is_poly_var) {
            stamp = env->LookupExnStamp(node.module_path, node.ctor_name);
            if (stamp == 0 && stdlib_env_ != nullptr) {
              stamp =
                  stdlib_env_->LookupExnStamp(node.module_path, node.ctor_name);
            }
          }
          return Value::Constructor(node.ctor_name, stamp, std::move(arg_val),
                                    node.is_poly_var);
        } else if constexpr (std::is_same_v<T, Expr::Tuple>) {
          std::vector<Value> elems;
          elems.reserve(node.elements.size());
          for (const auto& e : node.elements) {
            elems.push_back(EvalExpr(*e, env));
          }
          return Value::Tuple(std::move(elems));
        } else if constexpr (std::is_same_v<T, Expr::Record>) {
          std::vector<std::pair<SymbolId, Value>> fields;
          int tag = 0;
          if (node.with_base != nullptr) {
            Value base = EvalExpr(*node.with_base, env);
            CHECK(base.IsRecord());
            fields = base.AsRecord()->fields;
            tag = base.AsRecord()->tag;
          }
          for (const auto& [f_sym, f_expr] : node.fields) {
            Value fv = EvalExpr(*f_expr, env);
            bool found = false;
            for (auto& kv : fields) {
              if (kv.first == f_sym) {
                kv.second = std::move(fv);
                found = true;
                break;
              }
            }
            if (!found) {
              if (fields.size() == 1 && fields[0].first == kInvalidSymbol) {
                fields[0].first = f_sym;
                fields[0].second = std::move(fv);
              } else {
                fields.emplace_back(f_sym, std::move(fv));
              }
            }
          }
          return Value::Record(std::move(fields), tag);
        } else if constexpr (std::is_same_v<T, Expr::FieldGet>) {
          Value rec = EvalExpr(*node.rec, env);
          if (rec.IsRecord()) {
            const Value* fv = rec.AsRecord()->Find(node.field);
            if (fv != nullptr) {
              return *fv;
            }
          }
          if (rec.IsTuple() && node.field == sym_contents_ &&
              !rec.AsTuple()->elements.empty()) {
            return rec.AsTuple()->elements[0];
          }
          throw OcamlException(MakeExnFailure(
              intern_,
              absl::StrCat("Missing record field: ",
                           intern_.Resolve(node.field), " on ",
                           DebugString(rec, &intern_), " at ",
                           expr.loc.filename, ":", expr.loc.line)));
        } else if constexpr (std::is_same_v<T, Expr::FieldSet>) {
          Value rec = EvalExpr(*node.rec, env);
          Value val = EvalExpr(*node.val, env);
          CHECK(rec.IsRecord())
              << "FieldSet on non-record at " << expr.loc.filename << ":"
              << expr.loc.line;
          rec.AsRecord()->Set(node.field, std::move(val));
          return Value::Unit();
        } else if constexpr (std::is_same_v<T, Expr::ArrayLit>) {
          std::vector<Value> elems;
          elems.reserve(node.elements.size());
          for (const auto& e : node.elements) {
            elems.push_back(EvalExpr(*e, env));
          }
          return Value::Array(std::move(elems));
        } else if constexpr (std::is_same_v<T, Expr::Function>) {
          auto c = std::make_shared<ClosureVal>();
          c->params = &node.params;
          c->body = node.body.get();
          c->env = env;
          c->name = node.func_name;
          return Value::Closure(std::move(c));
        } else if constexpr (std::is_same_v<T, Expr::Match>) {
          Value scrut;
          bool raised = false;
          OcamlException caught_exn(Value::Unit());
          try {
            scrut = EvalExpr(*node.scrutinee, env);
          } catch (const OcamlException& ex) {
            raised = true;
            caught_exn = ex;
          }

          if (raised) {
            for (const MatchCase& mc : node.cases) {
              if (!mc.is_exception) continue;
              std::vector<std::pair<SymbolId, Value>> bindings;
              std::vector<std::pair<SymbolId, std::shared_ptr<Env>>>
                  mod_bindings;
              if (MatchPattern(*mc.pattern, caught_exn.value(), env, &bindings,
                               &mod_bindings)) {
                auto branch_env = std::make_shared<Env>(env);
                for (auto& [sym, bval] : bindings) {
                  branch_env->DefineValue(sym, std::move(bval));
                }
                for (auto& [msym, menv] : mod_bindings) {
                  branch_env->DefineModule(msym, std::move(menv));
                }
                if (mc.guard != nullptr) {
                  Value g = EvalExpr(*mc.guard, branch_env);
                  if (!g.AsBool()) {
                    continue;
                  }
                }
                return EvalExpr(*mc.body, branch_env);
              }
            }
            throw caught_exn;
          }

          for (const MatchCase& mc : node.cases) {
            if (mc.is_exception) continue;
            std::vector<std::pair<SymbolId, Value>> bindings;
            std::vector<std::pair<SymbolId, std::shared_ptr<Env>>> mod_bindings;
            if (MatchPattern(*mc.pattern, scrut, env, &bindings,
                             &mod_bindings)) {
              auto branch_env = std::make_shared<Env>(env);
              for (auto& [sym, bval] : bindings) {
                branch_env->DefineValue(sym, std::move(bval));
              }
              for (auto& [msym, menv] : mod_bindings) {
                branch_env->DefineModule(msym, std::move(menv));
              }
              if (mc.guard != nullptr) {
                Value g = EvalExpr(*mc.guard, branch_env);
                if (!g.AsBool()) {
                  continue;
                }
              }
              return EvalExpr(*mc.body, branch_env);
            }
          }
          throw OcamlException(MakeExnMatchFailure(
              intern_, expr.loc.filename, expr.loc.line, expr.loc.start_col));
        } else if constexpr (std::is_same_v<T, Expr::Try>) {
          try {
            return EvalExpr(*node.body, env);
          } catch (const OcamlException& ex) {
            for (const MatchCase& mc : node.cases) {
              std::vector<std::pair<SymbolId, Value>> bindings;
              std::vector<std::pair<SymbolId, std::shared_ptr<Env>>>
                  mod_bindings;
              if (MatchPattern(*mc.pattern, ex.value(), env, &bindings,
                               &mod_bindings)) {
                auto branch_env = std::make_shared<Env>(env);
                for (auto& [sym, bval] : bindings) {
                  branch_env->DefineValue(sym, std::move(bval));
                }
                for (auto& [msym, menv] : mod_bindings) {
                  branch_env->DefineModule(msym, std::move(menv));
                }
                if (mc.guard != nullptr) {
                  Value g = EvalExpr(*mc.guard, branch_env);
                  if (!g.AsBool()) {
                    continue;
                  }
                }
                return EvalExpr(*mc.body, branch_env);
              }
            }
            throw;
          }
        } else if constexpr (std::is_same_v<T, Expr::Apply>) {
          // Evaluate arguments and function expression.
          Value fn = EvalExpr(*node.fn, env);
          std::vector<AppliedArg> args;
          args.reserve(node.args.size());
          for (const ApplyArg& a : node.args) {
            AppliedArg aa;
            aa.kind = a.kind;
            aa.label = a.label;
            aa.value = EvalExpr(*a.expr, env);
            args.push_back(std::move(aa));
          }
          return Apply(fn, std::move(args));
        } else if constexpr (std::is_same_v<T, Expr::Let>) {
          auto let_env = std::make_shared<Env>(env);
          EvalLetBindings(node.is_rec, node.bindings, env, let_env);
          return EvalExpr(*node.body, let_env);
        } else if constexpr (std::is_same_v<T, Expr::Seq>) {
          EvalExpr(*node.first, env);
          return EvalExpr(*node.second, env);
        } else if constexpr (std::is_same_v<T, Expr::If>) {
          Value c = EvalExpr(*node.cond, env);
          if (c.AsBool()) {
            return EvalExpr(*node.then_br, env);
          }
          return node.else_br != nullptr ? EvalExpr(*node.else_br, env)
                                         : Value::Unit();
        } else if constexpr (std::is_same_v<T, Expr::While>) {
          while (EvalExpr(*node.cond, env).AsBool()) {
            EvalExpr(*node.body, env);
          }
          return Value::Unit();
        } else if constexpr (std::is_same_v<T, Expr::For>) {
          int64_t start_v = EvalExpr(*node.start, env).AsInt();
          int64_t end_v = EvalExpr(*node.end, env).AsInt();
          auto loop_env = std::make_shared<Env>(env);
          if (!node.downto) {
            for (int64_t i = start_v; i <= end_v; ++i) {
              loop_env->DefineValue(node.var, Value::Int(i));
              EvalExpr(*node.body, loop_env);
            }
          } else {
            for (int64_t i = start_v; i >= end_v; --i) {
              loop_env->DefineValue(node.var, Value::Int(i));
              EvalExpr(*node.body, loop_env);
            }
          }
          return Value::Unit();
        } else if constexpr (std::is_same_v<T, Expr::Lazy>) {
          auto l = std::make_shared<LazyVal>();
          l->expr = node.expr.get();
          l->env = env;
          return Value::Lazy(std::move(l));
        } else if constexpr (std::is_same_v<T, Expr::Assert>) {
          Value c = EvalExpr(*node.expr, env);
          if (!c.AsBool()) {
            throw OcamlException(MakeExnAssertFailure(
                intern_, expr.loc.filename, expr.loc.line, expr.loc.start_col));
          }
          return Value::Unit();
        } else if constexpr (std::is_same_v<T, Expr::LocalOpen>) {
          auto open_env = std::make_shared<Env>(env);
          auto mod = EvalModule(*node.mod_expr, env);
          if (mod != nullptr) {
            open_env->OpenModule(std::move(mod));
          }
          return EvalExpr(*node.body, open_env);
        } else if constexpr (std::is_same_v<T, Expr::LetModule>) {
          auto mod_env = std::make_shared<Env>(env);
          auto mod = EvalModule(*node.mod_expr, env);
          if (node.name != kInvalidSymbol && mod != nullptr) {
            mod_env->DefineModule(node.name, std::move(mod));
          }
          return EvalExpr(*node.body, mod_env);
        } else if constexpr (std::is_same_v<T, Expr::LetException>) {
          auto exn_env = std::make_shared<Env>(env);
          int64_t stamp = 0;
          if (!node.alias_path.empty()) {
            absl::Span<const SymbolId> mod_path(node.alias_path.data(),
                                                node.alias_path.size() - 1);
            stamp = env->LookupExnStamp(mod_path, node.alias_path.back());
          }
          if (stamp == 0) {
            stamp = AllocateExnStamp(intern_.Resolve(node.name));
          }
          exn_env->DefineExnStamp(node.name, stamp);
          return EvalExpr(*node.body, exn_env);
        } else if constexpr (std::is_same_v<T, Expr::PackModule>) {
          auto mod = EvalModule(*node.mod_expr, env);
          return Value::Module(std::move(mod));
        } else if constexpr (std::is_same_v<T, Expr::ObjectDummy>) {
          return Value::DummyObject(NextObjectId());
        }
      },
      expr.node);
}

std::shared_ptr<Env> Evaluator::ApplyFunctor(
    const std::shared_ptr<Env>& func_mod, const std::shared_ptr<Env>& arg_mod) {
  CHECK(func_mod != nullptr && func_mod->IsFunctor());
  const FunctorDef& fdef = *func_mod->functor();
  auto body_env = std::make_shared<Env>(fdef.lexical_env);
  if (fdef.param_index < fdef.param_names.size() &&
      fdef.param_names[fdef.param_index].has_value() && arg_mod != nullptr) {
    body_env->DefineModule(*fdef.param_names[fdef.param_index], arg_mod);
  }
  if (fdef.param_index + 1 < fdef.param_names.size()) {
    auto next_func = std::make_shared<Env>(body_env);
    FunctorDef next_def = fdef;
    next_def.param_index = fdef.param_index + 1;
    next_def.lexical_env = body_env;
    next_func->set_functor(std::move(next_def));
    return next_func;
  }
  return EvalModule(*fdef.body, body_env);
}

std::shared_ptr<Env> Evaluator::EvalModule(const ModuleExpr& mod_expr,
                                           const std::shared_ptr<Env>& env) {
  return std::visit(
      [&](const auto& node) -> std::shared_ptr<Env> {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ModuleExpr::Struct>) {
          auto struct_env = std::make_shared<Env>(env);
          EvalStructure(node.items, struct_env);
          return struct_env;
        } else if constexpr (std::is_same_v<T, ModuleExpr::ModPath>) {
          auto m = env->LookupModulePath(node.path);
          if (m == nullptr && stdlib_env_ != nullptr) {
            m = stdlib_env_->LookupModulePath(node.path);
          }
          if (m == nullptr && root_env_ != nullptr) {
            m = root_env_->LookupModulePath(node.path);
          }
          return m;
        } else if constexpr (std::is_same_v<T, ModuleExpr::Functor>) {
          if (node.param_names.empty()) {
            return EvalModule(*node.body, env);
          }
          auto func_env = std::make_shared<Env>(env);
          FunctorDef fdef;
          fdef.param_names = node.param_names;
          fdef.param_index = 0;
          fdef.body = node.body.get();
          fdef.lexical_env = env;
          func_env->set_functor(std::move(fdef));
          return func_env;
        } else if constexpr (std::is_same_v<T, ModuleExpr::ModApply>) {
          auto func_mod = EvalModule(*node.functor, env);
          std::shared_ptr<Env> arg_mod =
              node.arg != nullptr ? EvalModule(*node.arg, env)
                                  : std::make_shared<Env>(env);
          return ApplyFunctor(func_mod, arg_mod);
        } else if constexpr (std::is_same_v<T, ModuleExpr::Unpack>) {
          Value v = EvalExpr(*node.expr, env);
          CHECK(v.IsModule());
          return v.AsModule()->env;
        }
      },
      mod_expr.node);
}

void Evaluator::EvalStructure(const Structure& items,
                              const std::shared_ptr<Env>& env) {
  auto lex_env = std::make_shared<Env>(env->parent());
  lex_env->IncludeModule(*env);
  for (const auto& om : env->opened_modules()) {
    lex_env->OpenModule(om);
  }

  auto is_val_in_scope = [&](SymbolId sym) {
    return lex_env->LookupValue(sym) != nullptr ||
           (stdlib_env_ != nullptr &&
            stdlib_env_->LookupValue(sym) != nullptr) ||
           (root_env_ != nullptr && root_env_->LookupValue(sym) != nullptr);
  };

  for (const StructureItem& item : items) {
    std::visit(
        [&](const auto& node) {
          using T = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<T, StructureItem::EvalExpr>) {
            if (node.expr != nullptr) {
              EvalExpr(*node.expr, lex_env);
            }
          } else if constexpr (std::is_same_v<T, StructureItem::ValueDef>) {
            std::vector<SymbolId> vars;
            for (const LetBinding& lb : node.bindings) {
              if (lb.pattern != nullptr) {
                CollectPatternVars(*lb.pattern, &vars);
              }
            }
            bool shadows = false;
            for (SymbolId sym : vars) {
              if (is_val_in_scope(sym)) {
                shadows = true;
                break;
              }
            }
            if (node.is_rec) {
              if (shadows) {
                lex_env = std::make_shared<Env>(lex_env);
              }
              EvalLetBindings(true, node.bindings, lex_env, lex_env, env.get());
            } else {
              auto rhs_env = lex_env;
              if (shadows) {
                lex_env = std::make_shared<Env>(lex_env);
              }
              EvalLetBindings(false, node.bindings, rhs_env, lex_env,
                              env.get());
            }
          } else if constexpr (std::is_same_v<T, StructureItem::ExternalDef>) {
            if (node.name != kInvalidSymbol) {
              if (is_val_in_scope(node.name)) {
                lex_env = std::make_shared<Env>(lex_env);
              }
              Value prim = LookupPrimitive(node.prim_name);
              lex_env->DefineValue(node.name, prim);
              env->DefineValue(node.name, std::move(prim));
            }
          } else if constexpr (std::is_same_v<T, StructureItem::ExceptionDef>) {
            if (node.name != kInvalidSymbol) {
              int64_t stamp = 0;
              if (!node.alias_path.empty()) {
                absl::Span<const SymbolId> mod_path(
                    node.alias_path.data(), node.alias_path.size() - 1);
                stamp =
                    lex_env->LookupExnStamp(mod_path, node.alias_path.back());
                if (stamp == 0 && stdlib_env_ != nullptr) {
                  stamp = stdlib_env_->LookupExnStamp(mod_path,
                                                      node.alias_path.back());
                }
              }
              if (stamp == 0) {
                stamp = AllocateExnStamp(intern_.Resolve(node.name));
              }
              lex_env->DefineExnStamp(node.name, stamp);
              env->DefineExnStamp(node.name, stamp);
            }
          } else if constexpr (std::is_same_v<T, StructureItem::TypeExtDef>) {
            for (const auto& [ctor_sym, alias_path] : node.ctors) {
              if (ctor_sym == kInvalidSymbol) continue;
              int64_t stamp = 0;
              if (!alias_path.empty()) {
                absl::Span<const SymbolId> mod_path(alias_path.data(),
                                                    alias_path.size() - 1);
                stamp = lex_env->LookupExnStamp(mod_path, alias_path.back());
                if (stamp == 0 && stdlib_env_ != nullptr) {
                  stamp =
                      stdlib_env_->LookupExnStamp(mod_path, alias_path.back());
                }
              }
              if (stamp == 0) {
                stamp = AllocateExnStamp(intern_.Resolve(ctor_sym));
              }
              lex_env->DefineExnStamp(ctor_sym, stamp);
              env->DefineExnStamp(ctor_sym, stamp);
            }
          } else if constexpr (std::is_same_v<T, StructureItem::OpenDef>) {
            if (node.mod_expr != nullptr) {
              auto m = EvalModule(*node.mod_expr, lex_env);
              if (m != nullptr) {
                lex_env = std::make_shared<Env>(lex_env);
                lex_env->OpenModule(m);
                env->OpenModule(std::move(m));
              }
            }
          } else if constexpr (std::is_same_v<T, StructureItem::IncludeDef>) {
            if (node.mod_expr != nullptr) {
              auto m = EvalModule(*node.mod_expr, lex_env);
              if (m != nullptr) {
                lex_env = std::make_shared<Env>(lex_env);
                lex_env->IncludeModule(*m);
                env->IncludeModule(*m);
                if (m->IsFunctor()) {
                  lex_env->set_functor(*m->functor());
                  env->set_functor(*m->functor());
                }
              }
            }
          } else if constexpr (std::is_same_v<T, StructureItem::ModuleDef>) {
            if (node.mod_expr != nullptr && node.name != kInvalidSymbol) {
              auto m = EvalModule(*node.mod_expr, lex_env);
              if (m != nullptr) {
                if (lex_env->LookupModule(node.name) != nullptr) {
                  lex_env = std::make_shared<Env>(lex_env);
                }
                lex_env->DefineModule(node.name, m);
                env->DefineModule(node.name, std::move(m));
              }
            }
          } else if constexpr (std::is_same_v<T, StructureItem::ModuleRecDef>) {
            // Pre-create module environments so mutually recursive modules can
            // resolve each other by name, then populate their contents in
            // place.
            lex_env = std::make_shared<Env>(lex_env);
            std::vector<std::shared_ptr<Env>> mod_envs;
            mod_envs.reserve(node.modules.size());
            for (const auto& [m_sym, _] : node.modules) {
              auto m_env = std::make_shared<Env>(lex_env);
              if (m_sym != kInvalidSymbol) {
                lex_env->DefineModule(m_sym, m_env);
                env->DefineModule(m_sym, m_env);
              }
              mod_envs.push_back(std::move(m_env));
            }
            for (size_t i = 0; i < node.modules.size(); ++i) {
              const auto& [m_sym, m_expr] = node.modules[i];
              if (m_expr == nullptr) continue;
              if (const auto* st =
                      std::get_if<ModuleExpr::Struct>(&m_expr->node)) {
                EvalStructure(st->items, mod_envs[i]);
              } else {
                auto evaluated = EvalModule(*m_expr, lex_env);
                if (evaluated != nullptr) {
                  mod_envs[i]->IncludeModule(*evaluated);
                  if (evaluated->IsFunctor()) {
                    mod_envs[i]->set_functor(*evaluated->functor());
                  }
                }
              }
            }
          }
        },
        item.node);
  }
}

std::shared_ptr<Env> Evaluator::LoadModule(absl::string_view module_name,
                                           const Structure& items,
                                           bool open_stdlib,
                                           absl::string_view alias_name) {
  auto mod_env = std::make_shared<Env>(root_env_);
  if (open_stdlib && stdlib_env_ != nullptr) {
    mod_env->OpenModule(stdlib_env_);
  }
  EvalStructure(items, mod_env);

  if (!module_name.empty()) {
    SymbolId mod_sym = intern_.Intern(module_name);
    root_env_->DefineModule(mod_sym, mod_env);
    if (stdlib_env_ != nullptr && stdlib_env_ != mod_env) {
      stdlib_env_->DefineModule(mod_sym, mod_env);
    }
  }
  if (!alias_name.empty()) {
    SymbolId alias_sym = intern_.Intern(alias_name);
    root_env_->DefineModule(alias_sym, mod_env);
    if (stdlib_env_ != nullptr) {
      stdlib_env_->DefineModule(alias_sym, mod_env);
    }
  }
  return mod_env;
}

}  // namespace ocaml_boot
