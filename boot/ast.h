// Copyright 2026 Google LLC.
// Lowered abstract syntax tree for the OCaml 5.3 bootstrap interpreter.

#ifndef OCAML_BOOT_AST_H_
#define OCAML_BOOT_AST_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "intern.h"

namespace ocaml_boot {

struct Expr;
struct Pattern;
struct ModuleExpr;
struct StructureItem;

using ExprPtr = std::unique_ptr<Expr>;
using PatternPtr = std::unique_ptr<Pattern>;
using ModuleExprPtr = std::unique_ptr<ModuleExpr>;
using Structure = std::vector<StructureItem>;

// Tracks source location information for diagnostics and %loc_* primitives.
struct SourceLoc {
  std::string filename;
  int line = 1;       // 1-based line number.
  int start_col = 0;  // 0-based column offset from beginning of line.
  int end_col = 0;    // 0-based end column offset from beginning of line.
  int bol_offset = 0;
  int start_byte = 0;
  int end_byte = 0;
  std::string module_name;
  std::string function_name;
};

// Enumeration of compiler-builtin location macro kinds.
enum class LocMacroKind {
  kLocLOC,
  kLocFILE,
  kLocLINE,
  kLocPOS,
  kLocMODULE,
  kLocFUNCTION,
  kLocLOC_OF,
};

// Parameter passing kind for function definitions.
enum class ParamKind {
  kPositional,
  kLabeled,
  kOptional,
};

// Argument passing kind at function call sites.
enum class ArgKind {
  kPositional,
  kLabeled,
  kOptional,
};

// Represents a single formal parameter of an OCaml function.
struct FuncParam {
  ParamKind kind = ParamKind::kPositional;
  SymbolId label = kInvalidSymbol;
  PatternPtr pattern;
  ExprPtr default_expr;
};

// Represents a single argument passed in a function application.
struct ApplyArg {
  ArgKind kind = ArgKind::kPositional;
  SymbolId label = kInvalidSymbol;
  ExprPtr expr;
};

// Represents a single `let` or `and` binding (`pattern = expr`).
struct LetBinding {
  PatternPtr pattern;
  ExprPtr expr;
  SourceLoc loc;
};

// Represents a single case in a `match`, `function`, or `try` expression.
struct MatchCase {
  PatternPtr pattern;
  ExprPtr guard;
  ExprPtr body;
  bool is_exception = false;
};

// Represents a lowered OCaml pattern.
struct Pattern {
  struct Wildcard {};

  struct Var {
    SymbolId name = kInvalidSymbol;
  };

  struct Const {
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
    };

    Kind kind = Kind::kUnit;
    int64_t int_val = 0;
    double float_val = 0.0;
    uint8_t char_val = 0;
    bool bool_val = false;
    std::string str_val;

    static Const Unit() {
      Const c;
      c.kind = Kind::kUnit;
      return c;
    }
    static Const Bool(bool v) {
      Const c;
      c.kind = Kind::kBool;
      c.bool_val = v;
      return c;
    }
    static Const Char(uint8_t v) {
      Const c;
      c.kind = Kind::kChar;
      c.char_val = v;
      return c;
    }
    static Const Int(int64_t v) {
      Const c;
      c.kind = Kind::kInt;
      c.int_val = v;
      return c;
    }
    static Const Int32(int32_t v) {
      Const c;
      c.kind = Kind::kInt32;
      c.int_val = v;
      return c;
    }
    static Const Int64(int64_t v) {
      Const c;
      c.kind = Kind::kInt64;
      c.int_val = v;
      return c;
    }
    static Const Nativeint(int64_t v) {
      Const c;
      c.kind = Kind::kNativeint;
      c.int_val = v;
      return c;
    }
    static Const Float(double v) {
      Const c;
      c.kind = Kind::kFloat;
      c.float_val = v;
      return c;
    }
    static Const String(std::string v) {
      Const c;
      c.kind = Kind::kString;
      c.str_val = std::move(v);
      return c;
    }
  };

  struct Tuple {
    std::vector<PatternPtr> elements;
  };

  struct Construct {
    std::vector<SymbolId> module_path;
    SymbolId ctor_name = kInvalidSymbol;
    bool is_poly_var = false;
    PatternPtr arg;
  };

  struct Record {
    std::vector<std::pair<SymbolId, PatternPtr>> fields;
  };

  struct Array {
    std::vector<PatternPtr> elements;
  };

  struct Or {
    PatternPtr left;
    PatternPtr right;
  };

  struct Alias {
    PatternPtr pat;
    SymbolId name = kInvalidSymbol;
  };

  struct Range {
    char lo = 0;
    char hi = 0;
  };

  struct Lazy {
    PatternPtr pat;
  };

  struct Exception {
    PatternPtr pat;
  };

  struct UnpackModule {
    SymbolId name = kInvalidSymbol;
  };

  using Variant =
      std::variant<Wildcard, Var, Const, Tuple, Construct, Record, Array, Or,
                   Alias, Range, Lazy, Exception, UnpackModule>;

  Variant node;
  SourceLoc loc;

  Pattern() : node(Wildcard{}) {}
  explicit Pattern(Variant n, SourceLoc l = {})
      : node(std::move(n)), loc(std::move(l)) {}
};

// Helper to construct a heap-allocated Pattern node.
template <typename T>
PatternPtr MakePattern(T&& node, SourceLoc loc = {}) {
  return std::make_unique<Pattern>(Pattern::Variant(std::forward<T>(node)),
                                   std::move(loc));
}

// Represents a lowered OCaml expression.
struct Expr {
  struct Int {
    int64_t val = 0;
  };

  struct Int32 {
    int32_t val = 0;
  };

  struct Int64 {
    int64_t val = 0;
  };

  struct Nativeint {
    int64_t val = 0;
  };

  struct Float {
    double val = 0.0;
  };

  struct Char {
    uint8_t val = 0;
  };

  struct Bool {
    bool val = false;
  };

  struct Unit {};

  struct String {
    std::string val;
  };

  struct LocMacro {
    LocMacroKind kind = LocMacroKind::kLocLOC;
    ExprPtr arg;
  };

  struct Var {
    std::vector<SymbolId> module_path;
    SymbolId name = kInvalidSymbol;
  };

  struct Construct {
    std::vector<SymbolId> module_path;
    SymbolId ctor_name = kInvalidSymbol;
    bool is_poly_var = false;
    ExprPtr arg;
  };

  struct Tuple {
    std::vector<ExprPtr> elements;
  };

  struct Record {
    ExprPtr with_base;
    std::vector<std::pair<SymbolId, ExprPtr>> fields;
  };

  struct FieldGet {
    ExprPtr rec;
    SymbolId field = kInvalidSymbol;
    std::vector<SymbolId> module_path;
  };

  struct FieldSet {
    ExprPtr rec;
    SymbolId field = kInvalidSymbol;
    ExprPtr val;
    std::vector<SymbolId> module_path;
  };

  struct ArrayLit {
    std::vector<ExprPtr> elements;
  };

  struct Function {
    std::vector<FuncParam> params;
    ExprPtr body;
    SymbolId func_name = kInvalidSymbol;
  };

  struct Match {
    ExprPtr scrutinee;
    std::vector<MatchCase> cases;
  };

  struct Try {
    ExprPtr body;
    std::vector<MatchCase> cases;
  };

  struct Apply {
    ExprPtr fn;
    std::vector<ApplyArg> args;
  };

  struct Let {
    bool is_rec = false;
    std::vector<LetBinding> bindings;
    ExprPtr body;
  };

  struct Seq {
    ExprPtr first;
    ExprPtr second;
  };

  struct If {
    ExprPtr cond;
    ExprPtr then_br;
    ExprPtr else_br;
  };

  struct While {
    ExprPtr cond;
    ExprPtr body;
  };

  struct For {
    SymbolId var = kInvalidSymbol;
    ExprPtr start;
    ExprPtr end;
    bool downto = false;
    ExprPtr body;
  };

  struct Lazy {
    ExprPtr expr;
  };

  struct Assert {
    ExprPtr expr;
  };

  struct LocalOpen {
    ModuleExprPtr mod_expr;
    ExprPtr body;
  };

  struct LetModule {
    SymbolId name = kInvalidSymbol;
    ModuleExprPtr mod_expr;
    ExprPtr body;
  };

  struct LetException {
    SymbolId name = kInvalidSymbol;
    std::vector<SymbolId> alias_path;
    ExprPtr body;
  };

  struct PackModule {
    ModuleExprPtr mod_expr;
  };

  struct ObjectDummy {};

  using Variant =
      std::variant<Int, Int32, Int64, Nativeint, Float, Char, Bool, Unit,
                   String, LocMacro, Var, Construct, Tuple, Record, FieldGet,
                   FieldSet, ArrayLit, Function, Match, Try, Apply, Let, Seq,
                   If, While, For, Lazy, Assert, LocalOpen, LetModule,
                   LetException, PackModule, ObjectDummy>;

  Variant node;
  SourceLoc loc;

  Expr() : node(Unit{}) {}
  explicit Expr(Variant n, SourceLoc l = {})
      : node(std::move(n)), loc(std::move(l)) {}
};

// Helper to construct a heap-allocated Expr node.
template <typename T>
ExprPtr MakeExpr(T&& node, SourceLoc loc = {}) {
  return std::make_unique<Expr>(Expr::Variant(std::forward<T>(node)),
                                std::move(loc));
}

// Represents a lowered OCaml module expression.
struct ModuleExpr {
  struct Struct {
    std::vector<StructureItem> items;
  };

  struct ModPath {
    std::vector<SymbolId> path;
  };

  struct Functor {
    std::vector<std::optional<SymbolId>> param_names;
    ModuleExprPtr body;
  };

  struct ModApply {
    ModuleExprPtr functor;
    ModuleExprPtr arg;  // Nullptr represents unit argument `()`.
  };

  struct Unpack {
    ExprPtr expr;
  };

  using Variant = std::variant<Struct, ModPath, Functor, ModApply, Unpack>;

  Variant node;
  SourceLoc loc;

  ModuleExpr();
  explicit ModuleExpr(Variant n, SourceLoc l = {});
};

// Helper to construct a heap-allocated ModuleExpr node.
template <typename T>
ModuleExprPtr MakeModuleExpr(T&& node, SourceLoc loc = {}) {
  return std::make_unique<ModuleExpr>(
      ModuleExpr::Variant(std::forward<T>(node)), std::move(loc));
}

// Represents a top-level or structure-level item in an `.ml` file.
struct StructureItem {
  struct EvalExpr {
    ExprPtr expr;
  };

  struct ValueDef {
    bool is_rec = false;
    std::vector<LetBinding> bindings;
  };

  struct ExternalDef {
    SymbolId name = kInvalidSymbol;
    std::string prim_name;
  };

  struct ExceptionDef {
    SymbolId name = kInvalidSymbol;
    std::vector<SymbolId> alias_path;
  };

  struct TypeExtDef {
    // Each pair is (ctor_name, alias_path); alias_path is empty for new ctors.
    std::vector<std::pair<SymbolId, std::vector<SymbolId>>> ctors;
  };

  struct OpenDef {
    ModuleExprPtr mod_expr;
  };

  struct IncludeDef {
    ModuleExprPtr mod_expr;
  };

  struct ModuleDef {
    SymbolId name = kInvalidSymbol;
    ModuleExprPtr mod_expr;
  };

  struct ModuleRecDef {
    std::vector<std::pair<SymbolId, ModuleExprPtr>> modules;
  };

  using Variant =
      std::variant<EvalExpr, ValueDef, ExternalDef, ExceptionDef, TypeExtDef,
                   OpenDef, IncludeDef, ModuleDef, ModuleRecDef>;

  Variant node;
  SourceLoc loc;

  StructureItem() : node(EvalExpr{}) {}
  explicit StructureItem(Variant n, SourceLoc l = {})
      : node(std::move(n)), loc(std::move(l)) {}
};

inline ModuleExpr::ModuleExpr() : node(Struct{}) {}

inline ModuleExpr::ModuleExpr(Variant n, SourceLoc l)
    : node(std::move(n)), loc(std::move(l)) {}

}  // namespace ocaml_boot

#endif  // OCAML_BOOT_AST_H_
