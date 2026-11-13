// Copyright 2026 Google LLC.
// Implementation of the Tree-sitter OCaml parser and AST lowering pass.

#include "ts_lower.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "third_party/absl/container/flat_hash_map.h"
#include "third_party/absl/container/flat_hash_set.h"
#include "third_party/absl/log/check.h"
#include "third_party/absl/log/log.h"
#include "third_party/absl/status/status.h"
#include "third_party/absl/status/statusor.h"
#include "third_party/absl/strings/ascii.h"
#include "third_party/absl/strings/match.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/absl/strings/string_view.h"
#include "ast.h"
#include "intern.h"
#include "tree_sitter/api.h"

extern "C" const TSLanguage* tree_sitter_ocaml(void);
extern "C" const TSLanguage* tree_sitter_ocaml_interface(void);
extern "C" const TSLanguage* tree_sitter_ocamllex(void);

namespace ocaml_boot {
namespace {

// Converts a single hexadecimal character to its integer value (0..15).
int HexDigitVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

// Appends a Unicode code point as UTF-8 bytes to `out`.
void AppendUtf8(uint32_t cp, std::string* out) {
  if (cp <= 0x7F) {
    out->push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Unescapes an OCaml string or character literal body (without outer quotes).
std::string UnescapeOcamlString(absl::string_view s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '\\') {
      out.push_back(s[i]);
      ++i;
      continue;
    }
    ++i;
    if (i >= s.size()) {
      break;
    }
    char c = s[i];
    switch (c) {
      case '\\':
      case '"':
      case '\'':
      case ' ':
        out.push_back(c);
        ++i;
        break;
      case 'n':
        out.push_back('\n');
        ++i;
        break;
      case 'r':
        out.push_back('\r');
        ++i;
        break;
      case 't':
        out.push_back('\t');
        ++i;
        break;
      case 'b':
        out.push_back('\b');
        ++i;
        break;
      case '\n':
        ++i;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
          ++i;
        }
        break;
      case '\r':
        if (i + 1 < s.size() && s[i + 1] == '\n') {
          i += 2;
          while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            ++i;
          }
        } else {
          out.push_back('\\');
          out.push_back(c);
          ++i;
        }
        break;
      case 'x':
        if (i + 2 < s.size() && absl::ascii_isxdigit(s[i + 1]) &&
            absl::ascii_isxdigit(s[i + 2])) {
          int v = (HexDigitVal(s[i + 1]) << 4) | HexDigitVal(s[i + 2]);
          out.push_back(static_cast<char>(static_cast<uint8_t>(v)));
          i += 3;
        } else {
          out.push_back('\\');
          out.push_back(c);
          ++i;
        }
        break;
      case 'o':
        if (i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '3' &&
            s[i + 2] >= '0' && s[i + 2] <= '7' && s[i + 3] >= '0' &&
            s[i + 3] <= '7') {
          int v = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 +
                  (s[i + 3] - '0');
          out.push_back(static_cast<char>(static_cast<uint8_t>(v)));
          i += 4;
        } else {
          out.push_back('\\');
          out.push_back(c);
          ++i;
        }
        break;
      case 'u':
        if (i + 2 < s.size() && s[i + 1] == '{') {
          size_t j = i + 2;
          uint32_t cp = 0;
          while (j < s.size() && absl::ascii_isxdigit(s[j])) {
            cp = (cp << 4) | static_cast<uint32_t>(HexDigitVal(s[j]));
            ++j;
          }
          if (j < s.size() && s[j] == '}') {
            AppendUtf8(cp, &out);
            i = j + 1;
            break;
          }
        }
        out.push_back('\\');
        out.push_back(c);
        ++i;
        break;
      default:
        if (c >= '0' && c <= '9' && i + 2 < s.size() &&
            absl::ascii_isdigit(s[i + 1]) && absl::ascii_isdigit(s[i + 2])) {
          int v =
              (c - '0') * 100 + (s[i + 1] - '0') * 10 + (s[i + 2] - '0');
          out.push_back(static_cast<char>(static_cast<uint8_t>(v)));
          i += 3;
        } else {
          out.push_back('\\');
          out.push_back(c);
          ++i;
        }
        break;
    }
  }
  return out;
}

// Extracts the raw string payload inside an OCaml quoted string `{id|...|id}`.
std::string ExtractQuotedString(absl::string_view raw) {
  size_t first_bar = raw.find('|');
  size_t last_bar = raw.rfind('|');
  if (first_bar == absl::string_view::npos ||
      last_bar == absl::string_view::npos || last_bar <= first_bar) {
    return "";
  }
  return std::string(raw.substr(first_bar + 1, last_bar - first_bar - 1));
}

// Unescapes an OCaml character literal `'...'` or `#'...'`.
uint8_t UnescapeOcamlChar(absl::string_view raw) {
  if (!raw.empty() && raw[0] == '#') {
    raw.remove_prefix(1);
  }
  if (raw.size() < 2 || raw.front() != '\'' || raw.back() != '\'') {
    return 0;
  }
  std::string s = UnescapeOcamlString(raw.substr(1, raw.size() - 2));
  return s.empty() ? 0 : static_cast<uint8_t>(s[0]);
}

// Strips the OCaml 5.3 raw identifier prefix `\#` if present.
absl::string_view CleanIdent(absl::string_view s) {
  if (absl::StartsWith(s, "\\#")) {
    return s.substr(2);
  }
  return s;
}

// Parses an OCaml numeric literal (with optional folded negation) into a
// `Pattern::Const`.
Pattern::Const ParseNumberConst(absl::string_view raw, bool negate) {
  if (!raw.empty() && raw[0] == '#') {
    raw.remove_prefix(1);
  }
  if (!raw.empty() && (raw[0] == '-' || raw[0] == '+')) {
    if (raw[0] == '-') {
      negate = !negate;
    }
    raw.remove_prefix(1);
  }

  std::string clean;
  clean.reserve(raw.size());
  for (char c : raw) {
    if (c != '_') {
      clean.push_back(c);
    }
  }
  if (clean.empty()) {
    return Pattern::Const::Int(0);
  }

  bool is_hex = clean.size() >= 2 && clean[0] == '0' &&
                (clean[1] == 'x' || clean[1] == 'X');
  bool is_oct = clean.size() >= 2 && clean[0] == '0' &&
                (clean[1] == 'o' || clean[1] == 'O');
  bool is_bin = clean.size() >= 2 && clean[0] == '0' &&
                (clean[1] == 'b' || clean[1] == 'B');

  char suffix = 0;
  char last = clean.back();
  if (last == 'l' || last == 'L' || last == 'n') {
    suffix = last;
    clean.pop_back();
  } else if (!is_hex &&
             ((last >= 'g' && last <= 'z') || (last >= 'G' && last <= 'Z'))) {
    clean.pop_back();
  } else if (is_hex &&
             ((last >= 'g' && last <= 'z' && last != 'p') ||
              (last >= 'G' && last <= 'Z' && last != 'P'))) {
    clean.pop_back();
  }

  bool is_float = false;
  if (suffix == 0) {
    if (is_hex) {
      for (size_t i = 2; i < clean.size(); ++i) {
        if (clean[i] == '.' || clean[i] == 'p' || clean[i] == 'P') {
          is_float = true;
          break;
        }
      }
    } else if (!is_oct && !is_bin) {
      for (char c : clean) {
        if (c == '.' || c == 'e' || c == 'E') {
          is_float = true;
          break;
        }
      }
    }
  }

  if (is_float) {
    double d = std::strtod(clean.c_str(), nullptr);
    if (negate) {
      d = -d;
    }
    return Pattern::Const::Float(d);
  }

  uint64_t base = 10;
  size_t idx = 0;
  if (is_hex) {
    base = 16;
    idx = 2;
  } else if (is_oct) {
    base = 8;
    idx = 2;
  } else if (is_bin) {
    base = 2;
    idx = 2;
  }

  uint64_t uval = 0;
  for (; idx < clean.size(); ++idx) {
    uval = uval * base + static_cast<uint64_t>(HexDigitVal(clean[idx]));
  }
  if (negate) {
    uval = 0ULL - uval;
  }

  if (suffix == 'l') {
    return Pattern::Const::Int32(static_cast<int32_t>(uval));
  }
  if (suffix == 'L') {
    return Pattern::Const::Int64(static_cast<int64_t>(uval));
  }
  if (suffix == 'n') {
    return Pattern::Const::Nativeint(static_cast<int64_t>(uval));
  }
  return Pattern::Const::Int(static_cast<int64_t>(uval));
}

// Converts a `Pattern::Const` into the corresponding `ExprPtr`.
ExprPtr ConstToExpr(const Pattern::Const& c, SourceLoc loc) {
  switch (c.kind) {
    case Pattern::Const::Kind::kUnit:
      return MakeExpr(Expr::Unit{}, std::move(loc));
    case Pattern::Const::Kind::kBool:
      return MakeExpr(Expr::Bool{c.bool_val}, std::move(loc));
    case Pattern::Const::Kind::kChar:
      return MakeExpr(Expr::Char{c.char_val}, std::move(loc));
    case Pattern::Const::Kind::kInt:
      return MakeExpr(Expr::Int{c.int_val}, std::move(loc));
    case Pattern::Const::Kind::kInt32:
      return MakeExpr(Expr::Int32{static_cast<int32_t>(c.int_val)},
                      std::move(loc));
    case Pattern::Const::Kind::kInt64:
      return MakeExpr(Expr::Int64{c.int_val}, std::move(loc));
    case Pattern::Const::Kind::kNativeint:
      return MakeExpr(Expr::Nativeint{c.int_val}, std::move(loc));
    case Pattern::Const::Kind::kFloat:
      return MakeExpr(Expr::Float{c.float_val}, std::move(loc));
    case Pattern::Const::Kind::kString:
      return MakeExpr(Expr::String{c.str_val}, std::move(loc));
  }
  return MakeExpr(Expr::Unit{}, std::move(loc));
}

// Stateful lowering visitor converting a Tree-sitter OCaml CST to the AST.
class Lowerer {
 public:
  Lowerer(absl::string_view source, absl::string_view filename,
          absl::string_view module_name, InternTable& intern)
      : source_(source),
        filename_(filename),
        module_name_(module_name),
        intern_(intern) {
    // Seed cross-module polymorphic variant types used in `#type` patterns
    // across the OCaml compiler (`typing/patterns.ml`, `typing/parmatch.ml`,
    // `lambda/matching.ml`, and `typing/typemod.ml`).
    PolyTagList simple_view = {
        {"Any", false},      {"Constant", true}, {"Tuple", true},
        {"Construct", true}, {"Variant", true},  {"Record", true},
        {"Array", true},     {"Lazy", true},
    };
    PolyTagList half_simple_view = simple_view;
    half_simple_view.emplace_back("Or", true);
    PolyTagList general_view = half_simple_view;
    general_view.emplace_back("Var", true);
    general_view.emplace_back("Alias", true);

    poly_variant_types_["Simple.view"] = simple_view;
    poly_variant_types_["Patterns.Simple.view"] = simple_view;
    poly_variant_types_["view"] = simple_view;
    poly_variant_types_["Half_simple.view"] = half_simple_view;
    poly_variant_types_["Patterns.Half_simple.view"] = half_simple_view;
    poly_variant_types_["General.view"] = general_view;
    poly_variant_types_["Patterns.General.view"] = general_view;
    poly_variant_types_["bound_info"] = {
        {"Exported", false},
        {"Shadowable", true},
    };
    for (const auto& [k, v] : GlobalPolyVariantTypes()) {
      poly_variant_types_[k] = v;
    }
  }

  static absl::flat_hash_map<std::string, std::vector<std::pair<std::string, bool>>>&
  GlobalPolyVariantTypes() {
    static absl::flat_hash_map<std::string, std::vector<std::pair<std::string, bool>>> m;
    return m;
  }

  Structure LowerCompilationUnit(TSNode root) {
    UpdateLineDirectives(root);
    Structure items;
    uint32_t count = ts_node_child_count(root);
    for (uint32_t i = 0; i < count; ++i) {
      TSNode child = ts_node_child(root, i);
      LowerStructureItem(child, &items);
    }
    return items;
  }

  std::optional<absl::flat_hash_set<SymbolId>> ExtractInterfaceExportedValues(
      TSNode root) {
    absl::flat_hash_set<SymbolId> exported;
    for (TSNode c : GetNamedChildren(root)) {
      absl::string_view ct = NodeType(c);
      if (ct == "include_module_type") {
        return std::nullopt;
      }
      if (ct == "value_specification" || ct == "external") {
        for (TSNode sub : GetNamedChildren(c)) {
          absl::string_view st = NodeType(sub);
          if (st == "value_name" || st == "parenthesized_operator") {
            exported.insert(ExtractNameSymbol(sub));
            break;
          }
        }
      } else if (ct == "module_definition") {
        for (TSNode mb : GetNamedChildren(c)) {
          if (NodeType(mb) == "module_binding") {
            for (TSNode sub : GetNamedChildren(mb)) {
              if (NodeType(sub) == "module_name") {
                exported.insert(ExtractNameSymbol(sub));
                break;
              }
            }
          }
        }
      }
    }
    return exported;
  }

 private:
  struct LineDirective {
    uint32_t byte_offset = 0;
    uint32_t directive_row = 0;
    int target_line = 1;
    std::string target_file;
  };

  // Scans the CST for `# <line> "<file>"` line number directives.
  void UpdateLineDirectives(TSNode node) {
    if (ts_node_is_null(node)) return;
    absl::string_view type = NodeType(node);
    if (type == "line_number_directive") {
      absl::string_view text = NodeText(node);
      size_t i = 0;
      if (i < text.size() && text[i] == '#') ++i;
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
      size_t num_start = i;
      while (i < text.size() && absl::ascii_isdigit(text[i])) ++i;
      if (i > num_start) {
        int line_num = std::atoi(std::string(text.substr(num_start, i - num_start)).c_str());
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        std::string file = filename_;
        if (i < text.size() && text[i] == '"') {
          size_t f_start = i + 1;
          size_t f_end = text.find('"', f_start);
          if (f_end != absl::string_view::npos) {
            file = std::string(text.substr(f_start, f_end - f_start));
          }
        }
        TSPoint end_pt = ts_node_end_point(node);
        uint32_t end_row = end_pt.row;
        if (!text.empty() && text.back() != '\n') {
          ++end_row;
        }
        line_directives_.push_back(LineDirective{
            ts_node_end_byte(node), end_row, line_num, std::move(file)});
      }
      return;
    }
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i) {
      UpdateLineDirectives(ts_node_child(node, i));
    }
  }

  absl::string_view NodeType(TSNode node) const {
    if (ts_node_is_null(node)) return "";
    return ts_node_type(node);
  }

  absl::string_view NodeText(TSNode node) const {
    if (ts_node_is_null(node)) return "";
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= source_.size() || end <= start) return "";
    return source_.substr(start, end - start);
  }

  SourceLoc GetLoc(TSNode node) const {
    SourceLoc loc;
    loc.filename = filename_;
    loc.module_name = module_name_;
    loc.function_name = current_function_name_;
    if (ts_node_is_null(node)) {
      return loc;
    }
    TSPoint start_pt = ts_node_start_point(node);
    uint32_t start_byte = ts_node_start_byte(node);
    uint32_t end_byte = ts_node_end_byte(node);
    loc.line = static_cast<int>(start_pt.row) + 1;
    for (const auto& dir : line_directives_) {
      if (dir.byte_offset <= start_byte) {
        loc.filename = dir.target_file;
        loc.line = dir.target_line +
                   static_cast<int>(start_pt.row - dir.directive_row);
      } else {
        break;
      }
    }
    loc.start_col = static_cast<int>(start_pt.column);
    loc.start_byte = static_cast<int>(start_byte);
    loc.end_byte = static_cast<int>(end_byte);
    loc.end_col = loc.start_col + (loc.end_byte - loc.start_byte);
    loc.bol_offset = loc.start_byte - loc.start_col;
    return loc;
  }

  bool IsExtraOrAttribute(TSNode node) const {
    if (ts_node_is_null(node) || ts_node_is_extra(node)) {
      return true;
    }
    absl::string_view t = NodeType(node);
    return t == "comment" || t == "line_number_directive" ||
           t == "attribute" || t == "item_attribute" ||
           t == "floating_attribute" || t == "attribute_id";
  }

  // Returns all named children that are not comments, directives, or
  // attributes.
  std::vector<TSNode> GetNamedChildren(TSNode node) const {
    std::vector<TSNode> out;
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      TSNode c = ts_node_child(node, i);
      if (!ts_node_is_named(c) || IsExtraOrAttribute(c)) {
        continue;
      }
      out.push_back(c);
    }
    return out;
  }

  TSNode GetField(TSNode node, absl::string_view field_name) const {
    return ts_node_child_by_field_name(
        node, field_name.data(), static_cast<uint32_t>(field_name.size()));
  }

  bool HasAnonToken(TSNode node, absl::string_view tok) const {
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      TSNode c = ts_node_child(node, i);
      if (!ts_node_is_named(c) && !ts_node_is_extra(c) &&
          NodeText(c) == tok) {
        return true;
      }
    }
    return false;
  }

  // Extracts the string name of an identifier, constructor, or parenthesized
  // operator node.
  std::string ExtractNameString(TSNode node) const {
    absl::string_view t = NodeType(node);
    if (t == "parenthesized_operator") {
      std::string op;
      uint32_t count = ts_node_child_count(node);
      for (uint32_t i = 0; i < count; ++i) {
        TSNode c = ts_node_child(node, i);
        if (IsExtraOrAttribute(c)) continue;
        absl::string_view ct = NodeText(c);
        if ((i == 0 && ct == "(") || (i + 1 == count && ct == ")")) {
          continue;
        }
        absl::StrAppend(&op, ct);
      }
      return op;
    }
    if (t == "constructor_name") {
      absl::string_view raw = NodeText(node);
      if (raw.size() >= 2 && raw.front() == '[' && raw.back() == ']') {
        return "[]";
      }
      if (raw.size() >= 2 && raw.front() == '(' && raw.back() == ')') {
        return "()";
      }
      return std::string(raw);
    }
    if (t == "tag") {
      absl::string_view raw = NodeText(node);
      if (!raw.empty() && raw[0] == '`') {
        raw.remove_prefix(1);
      }
      return std::string(CleanIdent(raw));
    }
    return std::string(CleanIdent(NodeText(node)));
  }

  SymbolId ExtractNameSymbol(TSNode node) {
    return intern_.Intern(ExtractNameString(node));
  }

  // Appends all `module_name` components inside a `module_path` or
  // `extended_module_path` node.
  void CollectModulePath(TSNode node, std::vector<SymbolId>* path) {
    if (ts_node_is_null(node)) return;
    absl::string_view t = NodeType(node);
    if (t == "module_name") {
      path->push_back(ExtractNameSymbol(node));
      return;
    }
    if (t == "module_path" || t == "extended_module_path") {
      for (TSNode c : GetNamedChildren(node)) {
        CollectModulePath(c, path);
      }
    }
  }

  // Splits a path node (`value_path`, `constructor_path`, `field_path`,
  // `indexing_operator_path`, `type_constructor_path`) into its module prefix
  // and final name.
  void SplitPath(TSNode node, std::vector<SymbolId>* module_path,
                 SymbolId* final_name, std::string* final_str = nullptr) {
    module_path->clear();
    *final_name = kInvalidSymbol;
    if (ts_node_is_null(node)) return;
    absl::string_view t = NodeType(node);
    if (t == "value_name" || t == "value_pattern" ||
        t == "parenthesized_operator" || t == "constructor_name" ||
        t == "field_name" || t == "indexing_operator" ||
        t == "type_constructor" || t == "module_name") {
      std::string s = ExtractNameString(node);
      *final_name = intern_.Intern(s);
      if (final_str != nullptr) *final_str = std::move(s);
      return;
    }
    std::vector<TSNode> children = GetNamedChildren(node);
    for (size_t i = 0; i < children.size(); ++i) {
      TSNode c = children[i];
      absl::string_view ct = NodeType(c);
      if (i + 1 < children.size() &&
          (ct == "module_path" || ct == "extended_module_path" ||
           ct == "module_name")) {
        CollectModulePath(c, module_path);
      } else {
        std::string s = ExtractNameString(c);
        *final_name = intern_.Intern(s);
        if (final_str != nullptr) *final_str = std::move(s);
      }
    }
  }

  // Qualifies unqualified non-builtin constructor patterns inside a
  // `local_open_pattern` `M.(p)`.
  void QualifyPatternConstructors(Pattern* pat,
                                  const std::vector<SymbolId>& mod_path) {
    if (pat == nullptr || mod_path.empty()) return;
    std::visit(
        [&](auto& n) {
          using T = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<T, Pattern::Tuple> ||
                        std::is_same_v<T, Pattern::Array>) {
            for (auto& el : n.elements) {
              QualifyPatternConstructors(el.get(), mod_path);
            }
          } else if constexpr (std::is_same_v<T, Pattern::Construct>) {
            absl::string_view cname = intern_.Resolve(n.ctor_name);
            if (!n.is_poly_var && n.module_path.empty() && cname != "[]" &&
                cname != "::" && cname != "Some" && cname != "None") {
              n.module_path = mod_path;
            }
            if (n.arg != nullptr) {
              QualifyPatternConstructors(n.arg.get(), mod_path);
            }
          } else if constexpr (std::is_same_v<T, Pattern::Record>) {
            for (auto& kv : n.fields) {
              QualifyPatternConstructors(kv.second.get(), mod_path);
            }
          } else if constexpr (std::is_same_v<T, Pattern::Or>) {
            QualifyPatternConstructors(n.left.get(), mod_path);
            QualifyPatternConstructors(n.right.get(), mod_path);
          } else if constexpr (std::is_same_v<T, Pattern::Alias> ||
                               std::is_same_v<T, Pattern::Lazy> ||
                               std::is_same_v<T, Pattern::Exception>) {
            QualifyPatternConstructors(n.pat.get(), mod_path);
          }
        },
        pat->node);
  }

 public:
  PatternPtr LowerPattern(TSNode node) {
    SourceLoc loc = GetLoc(node);
    if (ts_node_is_null(node)) {
      return MakePattern(Pattern::Wildcard{}, std::move(loc));
    }
    absl::string_view type = NodeType(node);
    absl::string_view text = NodeText(node);

    if (type == "_" || text == "_") {
      return MakePattern(Pattern::Wildcard{}, std::move(loc));
    }
    if (type == "value_pattern" || type == "value_name" ||
        type == "parenthesized_operator") {
      std::string name = ExtractNameString(node);
      if (name == "_") {
        return MakePattern(Pattern::Wildcard{}, std::move(loc));
      }
      return MakePattern(Pattern::Var{intern_.Intern(name)}, std::move(loc));
    }
    if (type == "unit") {
      return MakePattern(Pattern::Const::Unit(), std::move(loc));
    }
    if (type == "boolean") {
      bool b = (text == "true" || text == "#true");
      return MakePattern(Pattern::Const::Bool(b), std::move(loc));
    }
    if (type == "character") {
      return MakePattern(Pattern::Const::Char(UnescapeOcamlChar(text)),
                         std::move(loc));
    }
    if (type == "string") {
      std::string s =
          text.size() >= 2
              ? UnescapeOcamlString(text.substr(1, text.size() - 2))
              : "";
      return MakePattern(Pattern::Const::String(std::move(s)), std::move(loc));
    }
    if (type == "quoted_string") {
      return MakePattern(Pattern::Const::String(ExtractQuotedString(text)),
                         std::move(loc));
    }
    if (type == "number" || type == "signed_number") {
      return MakePattern(ParseNumberConst(text, /*negate=*/false),
                         std::move(loc));
    }
    if (type == "range_pattern") {
      TSNode left = GetField(node, "left");
      TSNode right = GetField(node, "right");
      char lo = static_cast<char>(UnescapeOcamlChar(NodeText(left)));
      char hi = static_cast<char>(UnescapeOcamlChar(NodeText(right)));
      return MakePattern(Pattern::Range{lo, hi}, std::move(loc));
    }
    if (type == "parenthesized_pattern" || type == "typed_pattern") {
      TSNode pat_field = GetField(node, "pattern");
      if (!ts_node_is_null(pat_field)) {
        return LowerPattern(pat_field);
      }
      auto children = GetNamedChildren(node);
      if (!children.empty()) {
        return LowerPattern(children[0]);
      }
      return MakePattern(Pattern::Const::Unit(), std::move(loc));
    }
    if (type == "tuple_pattern") {
      std::vector<PatternPtr> elems;
      for (TSNode c : GetNamedChildren(node)) {
        absl::string_view ct = NodeType(c);
        if (ct == "labeled_tuple_element_pattern" ||
            ct == "labeled_tuple_element_binding_pattern") {
          TSNode p = GetField(c, "pattern");
          if (!ts_node_is_null(p)) {
            elems.push_back(LowerPattern(p));
          } else {
            for (TSNode sub : GetNamedChildren(c)) {
              if (NodeType(sub) == "label_name") {
                elems.push_back(MakePattern(
                    Pattern::Var{ExtractNameSymbol(sub)}, GetLoc(sub)));
                break;
              }
            }
          }
        } else {
          elems.push_back(LowerPattern(c));
        }
      }
      return MakePattern(Pattern::Tuple{std::move(elems)}, std::move(loc));
    }
    if (type == "constructor_path") {
      std::vector<SymbolId> mod_path;
      SymbolId ctor_sym = kInvalidSymbol;
      std::string ctor_str;
      SplitPath(node, &mod_path, &ctor_sym, &ctor_str);
      if (mod_path.empty()) {
        if (ctor_str == "true") {
          return MakePattern(Pattern::Const::Bool(true), std::move(loc));
        }
        if (ctor_str == "false") {
          return MakePattern(Pattern::Const::Bool(false), std::move(loc));
        }
        if (ctor_str == "()") {
          return MakePattern(Pattern::Const::Unit(), std::move(loc));
        }
      }
      return MakePattern(
          Pattern::Construct{std::move(mod_path), ctor_sym, false, nullptr},
          std::move(loc));
    }
    if (type == "tag") {
      return MakePattern(
          Pattern::Construct{{}, ExtractNameSymbol(node), true, nullptr},
          std::move(loc));
    }
    if (type == "constructor_pattern") {
      std::vector<SymbolId> mod_path;
      SymbolId ctor_sym = kInvalidSymbol;
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "constructor_path") {
          SplitPath(c, &mod_path, &ctor_sym);
          break;
        }
      }
      TSNode arg_node = GetField(node, "pattern");
      PatternPtr arg =
          ts_node_is_null(arg_node) ? nullptr : LowerPattern(arg_node);
      return MakePattern(Pattern::Construct{std::move(mod_path), ctor_sym,
                                            false, std::move(arg)},
                         std::move(loc));
    }
    if (type == "tag_pattern") {
      SymbolId tag_sym = kInvalidSymbol;
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "tag") {
          tag_sym = ExtractNameSymbol(c);
          break;
        }
      }
      TSNode arg_node = GetField(node, "pattern");
      PatternPtr arg =
          ts_node_is_null(arg_node) ? nullptr : LowerPattern(arg_node);
      return MakePattern(
          Pattern::Construct{{}, tag_sym, true, std::move(arg)},
          std::move(loc));
    }
    if (type == "cons_pattern") {
      PatternPtr left = LowerPattern(GetField(node, "left"));
      PatternPtr right = LowerPattern(GetField(node, "right"));
      std::vector<PatternPtr> elems;
      elems.push_back(std::move(left));
      elems.push_back(std::move(right));
      PatternPtr tup = MakePattern(Pattern::Tuple{std::move(elems)}, loc);
      return MakePattern(
          Pattern::Construct{{}, intern_.Intern("::"), false, std::move(tup)},
          std::move(loc));
    }
    if (type == "list_pattern") {
      auto children = GetNamedChildren(node);
      SymbolId nil_sym = intern_.Intern("[]");
      SymbolId cons_sym = intern_.Intern("::");
      PatternPtr acc =
          MakePattern(Pattern::Construct{{}, nil_sym, false, nullptr}, loc);
      for (auto it = children.rbegin(); it != children.rend(); ++it) {
        PatternPtr elem = LowerPattern(*it);
        std::vector<PatternPtr> pair;
        pair.push_back(std::move(elem));
        pair.push_back(std::move(acc));
        PatternPtr tup = MakePattern(Pattern::Tuple{std::move(pair)}, loc);
        acc = MakePattern(
            Pattern::Construct{{}, cons_sym, false, std::move(tup)}, loc);
      }
      return acc;
    }
    if (type == "array_pattern" || type == "iarray_pattern") {
      std::vector<PatternPtr> elems;
      for (TSNode c : GetNamedChildren(node)) {
        elems.push_back(LowerPattern(c));
      }
      return MakePattern(Pattern::Array{std::move(elems)}, std::move(loc));
    }
    if (type == "record_pattern") {
      std::vector<std::pair<SymbolId, PatternPtr>> fields;
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) != "field_pattern") continue;
        std::vector<SymbolId> f_mod_path;
        SymbolId f_sym = kInvalidSymbol;
        for (TSNode sub : GetNamedChildren(c)) {
          if (NodeType(sub) == "field_path") {
            SplitPath(sub, &f_mod_path, &f_sym);
            break;
          }
        }
        TSNode pat_node = GetField(c, "pattern");
        PatternPtr f_pat =
            ts_node_is_null(pat_node)
                ? MakePattern(Pattern::Var{f_sym}, GetLoc(c))
                : LowerPattern(pat_node);
        fields.emplace_back(f_sym, std::move(f_pat));
      }
      return MakePattern(Pattern::Record{std::move(fields)}, std::move(loc));
    }
    if (type == "or_pattern") {
      auto children = GetNamedChildren(node);
      if (children.empty()) {
        return MakePattern(Pattern::Wildcard{}, std::move(loc));
      }
      PatternPtr acc = LowerPattern(children[0]);
      for (size_t i = 1; i < children.size(); ++i) {
        PatternPtr rhs = LowerPattern(children[i]);
        acc = MakePattern(Pattern::Or{std::move(acc), std::move(rhs)}, loc);
      }
      return acc;
    }
    if (type == "alias_pattern") {
      PatternPtr inner = LowerPattern(GetField(node, "pattern"));
      SymbolId alias_sym = ExtractNameSymbol(GetField(node, "alias"));
      return MakePattern(Pattern::Alias{std::move(inner), alias_sym},
                         std::move(loc));
    }
    if (type == "lazy_pattern") {
      return MakePattern(Pattern::Lazy{LowerPattern(GetField(node, "pattern"))},
                         std::move(loc));
    }
    if (type == "exception_pattern") {
      TSNode inner_node = GetField(node, "pattern");
      PatternPtr inner = LowerPattern(inner_node);
      if (NodeType(inner_node) == "or_pattern") {
        // In OCaml, unparenthesized `exception P1 | P2` binds `exception` only
        // to the leftmost branch `P1`, whereas tree-sitter-ocaml parses
        // `or_pattern` inside `exception_pattern`.
        PatternPtr* cur = &inner;
        while (*cur != nullptr &&
               std::holds_alternative<Pattern::Or>((*cur)->node)) {
          cur = &std::get<Pattern::Or>((*cur)->node).left;
        }
        if (*cur != nullptr) {
          SourceLoc cur_loc = (*cur)->loc;
          *cur = MakePattern(Pattern::Exception{std::move(*cur)},
                             std::move(cur_loc));
        }
        return inner;
      }
      return MakePattern(Pattern::Exception{std::move(inner)}, std::move(loc));
    }
    if (type == "package_pattern") {
      SymbolId mod_sym = kInvalidSymbol;
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "module_name") {
          std::string s = ExtractNameString(c);
          if (s != "_") {
            mod_sym = intern_.Intern(s);
          }
          break;
        }
      }
      return MakePattern(Pattern::UnpackModule{mod_sym}, std::move(loc));
    }
    if (type == "local_open_pattern") {
      std::vector<SymbolId> mod_path;
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "module_path") {
          CollectModulePath(c, &mod_path);
          break;
        }
      }
      PatternPtr inner = LowerPattern(GetField(node, "pattern"));
      QualifyPatternConstructors(inner.get(), mod_path);
      return inner;
    }
    if (type == "polymorphic_variant_pattern") {
      std::string type_path;
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "type_constructor_path") {
          type_path = std::string(absl::StripAsciiWhitespace(NodeText(c)));
          break;
        }
      }
      if (type_path.empty() && text.size() > 1 && text[0] == '#') {
        type_path = std::string(absl::StripAsciiWhitespace(text.substr(1)));
      }
      auto it = poly_variant_types_.find(type_path);
      if (it == poly_variant_types_.end()) {
        size_t dot = type_path.rfind('.');
        if (dot != std::string::npos) {
          it = poly_variant_types_.find(type_path.substr(dot + 1));
        }
      }
      CHECK(it != poly_variant_types_.end() && !it->second.empty())
          << "Unknown polymorphic_variant_pattern #" << type_path << " at "
          << loc.filename << ":" << loc.line;
      PatternPtr acc = nullptr;
      for (const auto& [tag_name, has_arg] : it->second) {
        PatternPtr arg =
            has_arg ? MakePattern(Pattern::Wildcard{}, loc) : nullptr;
        PatternPtr tag_pat = MakePattern(
            Pattern::Construct{{}, intern_.Intern(tag_name), true,
                               std::move(arg)},
            loc);
        if (acc == nullptr) {
          acc = std::move(tag_pat);
        } else {
          acc = MakePattern(Pattern::Or{std::move(acc), std::move(tag_pat)},
                            loc);
        }
      }
      return acc;
    }

    return MakePattern(Pattern::Wildcard{}, std::move(loc));
  }

 private:
  // Lowers a formal function parameter node into a `FuncParam`.
  std::optional<FuncParam> LowerFuncParam(TSNode node) {
    absl::string_view type = NodeType(node);
    if (type == "abstract_type") {
      return std::nullopt;
    }
    if (type != "parameter") {
      FuncParam fp;
      fp.kind = ParamKind::kPositional;
      fp.pattern = LowerPattern(node);
      return fp;
    }

    FuncParam fp;
    if (HasAnonToken(node, "~")) {
      fp.kind = ParamKind::kLabeled;
    } else if (HasAnonToken(node, "?")) {
      fp.kind = ParamKind::kOptional;
    } else {
      fp.kind = ParamKind::kPositional;
    }

    for (TSNode c : GetNamedChildren(node)) {
      if (NodeType(c) == "label_name") {
        fp.label = ExtractNameSymbol(c);
        break;
      }
    }

    TSNode pat_node = GetField(node, "pattern");
    fp.pattern = ts_node_is_null(pat_node)
                     ? MakePattern(Pattern::Wildcard{}, GetLoc(node))
                     : LowerPattern(pat_node);

    if (fp.kind != ParamKind::kPositional && fp.label == kInvalidSymbol) {
      if (const auto* var = std::get_if<Pattern::Var>(&fp.pattern->node)) {
        fp.label = var->name;
      } else if (const auto* alias =
                     std::get_if<Pattern::Alias>(&fp.pattern->node)) {
        fp.label = alias->name;
      }
    }

    TSNode def_node = GetField(node, "default");
    if (!ts_node_is_null(def_node)) {
      fp.default_expr = LowerExpr(def_node);
    }
    return fp;
  }

  // Lowers an application argument node into an `ApplyArg`.
  ApplyArg LowerApplyArg(TSNode node) {
    ApplyArg arg;
    if (NodeType(node) == "labeled_argument") {
      if (HasAnonToken(node, "?")) {
        arg.kind = ArgKind::kOptional;
      } else {
        arg.kind = ArgKind::kLabeled;
      }
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "label_name") {
          arg.label = ExtractNameSymbol(c);
          break;
        }
      }
      TSNode expr_node = GetField(node, "expression");
      if (!ts_node_is_null(expr_node)) {
        arg.expr = LowerExpr(expr_node);
      } else {
        arg.expr = MakeExpr(Expr::Var{{}, arg.label}, GetLoc(node));
      }
      return arg;
    }
    arg.kind = ArgKind::kPositional;
    arg.expr = LowerExpr(node);
    return arg;
  }

  // Returns true if `pat` contains any `Pattern::Exception` node.
  bool HasExceptionPattern(const Pattern* pat) const {
    if (pat == nullptr) return false;
    if (std::holds_alternative<Pattern::Exception>(pat->node)) return true;
    if (const auto* or_p = std::get_if<Pattern::Or>(&pat->node)) {
      return HasExceptionPattern(or_p->left.get()) ||
             HasExceptionPattern(or_p->right.get());
    }
    if (const auto* alias_p = std::get_if<Pattern::Alias>(&pat->node)) {
      return HasExceptionPattern(alias_p->pat.get());
    }
    return false;
  }

  // Recursively unwraps any nested `Pattern::Exception` wrappers inside `pat`.
  PatternPtr UnwrapExceptionPatterns(PatternPtr pat) {
    if (pat == nullptr) return nullptr;
    if (auto* exn = std::get_if<Pattern::Exception>(&pat->node)) {
      return UnwrapExceptionPatterns(std::move(exn->pat));
    }
    if (auto* or_p = std::get_if<Pattern::Or>(&pat->node)) {
      or_p->left = UnwrapExceptionPatterns(std::move(or_p->left));
      or_p->right = UnwrapExceptionPatterns(std::move(or_p->right));
      return pat;
    }
    if (auto* alias_p = std::get_if<Pattern::Alias>(&pat->node)) {
      alias_p->pat = UnwrapExceptionPatterns(std::move(alias_p->pat));
      return pat;
    }
    return pat;
  }

  // Filters `pat` into either its `exception` branch patterns
  // (`want_exception == true`) or its normal value branch patterns
  // (`want_exception == false`), returning `nullptr` if none exist.
  PatternPtr SplitExceptionPatterns(PatternPtr pat, bool want_exception) {
    if (pat == nullptr) return nullptr;
    if (auto* exn = std::get_if<Pattern::Exception>(&pat->node)) {
      if (want_exception) {
        return UnwrapExceptionPatterns(std::move(exn->pat));
      }
      return nullptr;
    }
    if (auto* or_p = std::get_if<Pattern::Or>(&pat->node)) {
      PatternPtr l =
          SplitExceptionPatterns(std::move(or_p->left), want_exception);
      PatternPtr r =
          SplitExceptionPatterns(std::move(or_p->right), want_exception);
      if (l != nullptr && r != nullptr) {
        return MakePattern(Pattern::Or{std::move(l), std::move(r)}, pat->loc);
      }
      return l != nullptr ? std::move(l) : std::move(r);
    }
    if (auto* alias_p = std::get_if<Pattern::Alias>(&pat->node)) {
      PatternPtr inner =
          SplitExceptionPatterns(std::move(alias_p->pat), want_exception);
      if (inner == nullptr) return nullptr;
      return MakePattern(Pattern::Alias{std::move(inner), alias_p->name},
                         pat->loc);
    }
    return want_exception ? nullptr : std::move(pat);
  }

  // Lowers all `match_case` children of a `match`, `function`, or `try` node.
  std::vector<MatchCase> LowerMatchCases(TSNode node) {
    std::vector<MatchCase> cases;
    for (TSNode c : GetNamedChildren(node)) {
      if (NodeType(c) != "match_case") continue;
      TSNode pat_node = GetField(c, "pattern");
      PatternPtr raw_pat = LowerPattern(pat_node);

      auto lower_case_with_pat = [&](PatternPtr pat, bool is_exn) {
        MatchCase mc;
        mc.pattern = std::move(pat);
        mc.is_exception = is_exn;
        for (TSNode sub : GetNamedChildren(c)) {
          if (NodeType(sub) == "guard") {
            mc.guard = LowerExpr(GetField(sub, "expression"));
            break;
          }
        }
        TSNode body_node = GetField(c, "body");
        if (ts_node_is_null(body_node) ||
            NodeType(body_node) == "refutation_case") {
          mc.body = MakeExpr(
              Expr::Assert{MakeExpr(Expr::Bool{false}, GetLoc(c))}, GetLoc(c));
        } else {
          mc.body = LowerExpr(body_node);
        }
        cases.push_back(std::move(mc));
      };

      if (!HasExceptionPattern(raw_pat.get())) {
        lower_case_with_pat(std::move(raw_pat), /*is_exn=*/false);
        continue;
      }

      PatternPtr val_pat = SplitExceptionPatterns(
          LowerPattern(pat_node), /*want_exception=*/false);
      PatternPtr exn_pat =
          SplitExceptionPatterns(std::move(raw_pat), /*want_exception=*/true);
      if (exn_pat != nullptr) {
        lower_case_with_pat(std::move(exn_pat), /*is_exn=*/true);
      }
      if (val_pat != nullptr) {
        lower_case_with_pat(std::move(val_pat), /*is_exn=*/false);
      }
    }
    return cases;
  }

  // Lowers a single `let_binding` node into a `LetBinding`.
  LetBinding LowerLetBinding(TSNode node) {
    LetBinding lb;
    lb.loc = GetLoc(node);
    lb.pattern = LowerPattern(GetField(node, "pattern"));

    std::string prev_func_name = current_function_name_;
    SymbolId bound_var_sym = kInvalidSymbol;
    if (const auto* v = std::get_if<Pattern::Var>(&lb.pattern->node)) {
      bound_var_sym = v->name;
      current_function_name_ =
          absl::StrCat(module_name_, ".", intern_.Resolve(bound_var_sym));
    }

    std::vector<FuncParam> params;
    for (TSNode c : GetNamedChildren(node)) {
      absl::string_view ct = NodeType(c);
      if (ct == "parameter" || ct == "abstract_type") {
        if (auto fp = LowerFuncParam(c)) {
          params.push_back(std::move(*fp));
        }
      }
    }

    TSNode body_node = GetField(node, "body");
    ExprPtr body = ts_node_is_null(body_node)
                       ? MakeExpr(Expr::Unit{}, lb.loc)
                       : LowerExpr(body_node);

    current_function_name_ = std::move(prev_func_name);

    if (!params.empty()) {
      lb.expr = MakeExpr(
          Expr::Function{std::move(params), std::move(body), bound_var_sym},
          lb.loc);
    } else {
      if (auto* fn = std::get_if<Expr::Function>(&body->node)) {
        if (fn->func_name == kInvalidSymbol) {
          fn->func_name = bound_var_sym;
        }
      }
      lb.expr = std::move(body);
    }
    return lb;
  }

  // Helper to construct a call expression `M1.M2.fn arg1 arg2 ...`.
  ExprPtr MakeQualifiedCall(std::vector<SymbolId> mod_path, SymbolId fn_name,
                            std::vector<ExprPtr> pos_args, SourceLoc loc) {
    ExprPtr fn = MakeExpr(Expr::Var{std::move(mod_path), fn_name}, loc);
    std::vector<ApplyArg> args;
    args.reserve(pos_args.size());
    for (auto& e : pos_args) {
      ApplyArg a;
      a.kind = ArgKind::kPositional;
      a.expr = std::move(e);
      args.push_back(std::move(a));
    }
    return MakeExpr(Expr::Apply{std::move(fn), std::move(args)},
                    std::move(loc));
  }

  // Checks if `(mod_path, name_str)` refers to a built-in zero-arg location
  // macro (`__LOC__`, `__FILE__`, `__LINE__`, `__MODULE__`, `__POS__`,
  // `__FUNCTION__`).
  std::optional<LocMacroKind> MatchZeroArgLocMacro(
      const std::vector<SymbolId>& mod_path,
      absl::string_view name_str) const {
    if (!mod_path.empty() &&
        (mod_path.size() > 1 || intern_.Resolve(mod_path[0]) != "Stdlib")) {
      return std::nullopt;
    }
    if (name_str == "__LOC__" || name_str == "%loc_LOC") {
      return LocMacroKind::kLocLOC;
    }
    if (name_str == "__FILE__" || name_str == "%loc_FILE") {
      return LocMacroKind::kLocFILE;
    }
    if (name_str == "__LINE__" || name_str == "%loc_LINE") {
      return LocMacroKind::kLocLINE;
    }
    if (name_str == "__MODULE__" || name_str == "%loc_MODULE") {
      return LocMacroKind::kLocMODULE;
    }
    if (name_str == "__POS__" || name_str == "%loc_POS") {
      return LocMacroKind::kLocPOS;
    }
    if (name_str == "__FUNCTION__" || name_str == "%loc_FUNCTION") {
      return LocMacroKind::kLocFUNCTION;
    }
    return std::nullopt;
  }

 public:
  ExprPtr LowerExpr(TSNode node) {
    SourceLoc loc = GetLoc(node);
    if (ts_node_is_null(node)) {
      return MakeExpr(Expr::Unit{}, std::move(loc));
    }
    absl::string_view type = NodeType(node);
    absl::string_view text = NodeText(node);

    if (type == "unit") {
      return MakeExpr(Expr::Unit{}, std::move(loc));
    }
    if (type == "boolean") {
      bool b = (text == "true" || text == "#true");
      return MakeExpr(Expr::Bool{b}, std::move(loc));
    }
    if (type == "character") {
      return MakeExpr(Expr::Char{UnescapeOcamlChar(text)}, std::move(loc));
    }
    if (type == "string") {
      std::string s =
          text.size() >= 2
              ? UnescapeOcamlString(text.substr(1, text.size() - 2))
              : "";
      return MakeExpr(Expr::String{std::move(s)}, std::move(loc));
    }
    if (type == "quoted_string") {
      return MakeExpr(Expr::String{ExtractQuotedString(text)}, std::move(loc));
    }
    if (type == "number" || type == "signed_number") {
      return ConstToExpr(ParseNumberConst(text, /*negate=*/false),
                         std::move(loc));
    }
    if (type == "parenthesized_expression" || type == "typed_expression" ||
        type == "stack_expression" || type == "exclave_expression") {
      TSNode inner = GetField(node, "expression");
      if (!ts_node_is_null(inner)) {
        return LowerExpr(inner);
      }
      auto children = GetNamedChildren(node);
      if (!children.empty()) {
        return LowerExpr(children[0]);
      }
      return MakeExpr(Expr::Unit{}, std::move(loc));
    }
    if (type == "value_path") {
      std::vector<SymbolId> mod_path;
      SymbolId val_sym = kInvalidSymbol;
      std::string val_str;
      SplitPath(node, &mod_path, &val_sym, &val_str);
      if (auto macro = MatchZeroArgLocMacro(mod_path, val_str)) {
        return MakeExpr(Expr::LocMacro{*macro, nullptr}, std::move(loc));
      }
      return MakeExpr(Expr::Var{std::move(mod_path), val_sym}, std::move(loc));
    }
    if (type == "constructor_path") {
      std::vector<SymbolId> mod_path;
      SymbolId ctor_sym = kInvalidSymbol;
      std::string ctor_str;
      SplitPath(node, &mod_path, &ctor_sym, &ctor_str);
      if (mod_path.empty()) {
        if (ctor_str == "true") {
          return MakeExpr(Expr::Bool{true}, std::move(loc));
        }
        if (ctor_str == "false") {
          return MakeExpr(Expr::Bool{false}, std::move(loc));
        }
        if (ctor_str == "()") {
          return MakeExpr(Expr::Unit{}, std::move(loc));
        }
      }
      return MakeExpr(
          Expr::Construct{std::move(mod_path), ctor_sym, false, nullptr},
          std::move(loc));
    }
    if (type == "tag") {
      return MakeExpr(
          Expr::Construct{{}, ExtractNameSymbol(node), true, nullptr},
          std::move(loc));
    }
    if (type == "tuple_expression") {
      std::vector<ExprPtr> elems;
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "labeled_tuple_element") {
          TSNode e = GetField(c, "expression");
          if (!ts_node_is_null(e)) {
            elems.push_back(LowerExpr(e));
          } else {
            for (TSNode sub : GetNamedChildren(c)) {
              if (NodeType(sub) == "label_name") {
                elems.push_back(
                    MakeExpr(Expr::Var{{}, ExtractNameSymbol(sub)}, GetLoc(sub)));
                break;
              }
            }
          }
        } else {
          elems.push_back(LowerExpr(c));
        }
      }
      return MakeExpr(Expr::Tuple{std::move(elems)}, std::move(loc));
    }
    if (type == "cons_expression") {
      ExprPtr left = LowerExpr(GetField(node, "left"));
      ExprPtr right = LowerExpr(GetField(node, "right"));
      std::vector<ExprPtr> pair;
      pair.push_back(std::move(left));
      pair.push_back(std::move(right));
      ExprPtr tup = MakeExpr(Expr::Tuple{std::move(pair)}, loc);
      return MakeExpr(
          Expr::Construct{{}, intern_.Intern("::"), false, std::move(tup)},
          std::move(loc));
    }
    if (type == "list_expression") {
      auto children = GetNamedChildren(node);
      SymbolId nil_sym = intern_.Intern("[]");
      SymbolId cons_sym = intern_.Intern("::");
      ExprPtr acc =
          MakeExpr(Expr::Construct{{}, nil_sym, false, nullptr}, loc);
      for (auto it = children.rbegin(); it != children.rend(); ++it) {
        ExprPtr elem = LowerExpr(*it);
        std::vector<ExprPtr> pair;
        pair.push_back(std::move(elem));
        pair.push_back(std::move(acc));
        ExprPtr tup = MakeExpr(Expr::Tuple{std::move(pair)}, loc);
        acc = MakeExpr(
            Expr::Construct{{}, cons_sym, false, std::move(tup)}, loc);
      }
      return acc;
    }
    if (type == "array_expression" || type == "iarray_expression") {
      std::vector<ExprPtr> elems;
      for (TSNode c : GetNamedChildren(node)) {
        elems.push_back(LowerExpr(c));
      }
      return MakeExpr(Expr::ArrayLit{std::move(elems)}, std::move(loc));
    }
    if (type == "record_expression") {
      TSNode base_node = GetField(node, "record");
      ExprPtr with_base =
          ts_node_is_null(base_node) ? nullptr : LowerExpr(base_node);
      std::vector<std::pair<SymbolId, ExprPtr>> fields;
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) != "field_expression") continue;
        std::vector<SymbolId> f_mod_path;
        SymbolId f_sym = kInvalidSymbol;
        for (TSNode sub : GetNamedChildren(c)) {
          if (NodeType(sub) == "field_path") {
            SplitPath(sub, &f_mod_path, &f_sym);
            break;
          }
        }
        TSNode body_node = GetField(c, "body");
        ExprPtr f_expr =
            ts_node_is_null(body_node)
                ? MakeExpr(Expr::Var{{}, f_sym}, GetLoc(c))
                : LowerExpr(body_node);
        fields.emplace_back(f_sym, std::move(f_expr));
      }
      return MakeExpr(Expr::Record{std::move(with_base), std::move(fields)},
                      std::move(loc));
    }
    if (type == "field_get_expression") {
      ExprPtr rec = LowerExpr(GetField(node, "record"));
      std::vector<SymbolId> f_mod_path;
      SymbolId f_sym = kInvalidSymbol;
      SplitPath(GetField(node, "field"), &f_mod_path, &f_sym);
      return MakeExpr(
          Expr::FieldGet{std::move(rec), f_sym, std::move(f_mod_path)},
          std::move(loc));
    }
    if (type == "array_get_expression") {
      ExprPtr arr = LowerExpr(GetField(node, "array"));
      ExprPtr idx = LowerExpr(GetField(node, "index"));
      TSNode op_node = GetField(node, "operator");
      std::vector<ExprPtr> args;
      args.push_back(std::move(arr));
      args.push_back(std::move(idx));
      if (!ts_node_is_null(op_node)) {
        std::vector<SymbolId> mod_path;
        SymbolId op_sym = kInvalidSymbol;
        std::string op_str;
        SplitPath(op_node, &mod_path, &op_sym, &op_str);
        return MakeQualifiedCall(std::move(mod_path),
                                 intern_.Intern(absl::StrCat(".", op_str, "()")),
                                 std::move(args), std::move(loc));
      }
      return MakeQualifiedCall({intern_.Intern("Array")},
                               intern_.Intern("get"), std::move(args),
                               std::move(loc));
    }
    if (type == "string_get_expression") {
      ExprPtr str = LowerExpr(GetField(node, "string"));
      ExprPtr idx = LowerExpr(GetField(node, "index"));
      TSNode op_node = GetField(node, "operator");
      std::vector<ExprPtr> args;
      args.push_back(std::move(str));
      args.push_back(std::move(idx));
      if (!ts_node_is_null(op_node)) {
        std::vector<SymbolId> mod_path;
        SymbolId op_sym = kInvalidSymbol;
        std::string op_str;
        SplitPath(op_node, &mod_path, &op_sym, &op_str);
        return MakeQualifiedCall(std::move(mod_path),
                                 intern_.Intern(absl::StrCat(".", op_str, "[]")),
                                 std::move(args), std::move(loc));
      }
      return MakeQualifiedCall({intern_.Intern("String")},
                               intern_.Intern("get"), std::move(args),
                               std::move(loc));
    }
    if (type == "bigarray_get_expression") {
      ExprPtr ba = LowerExpr(GetField(node, "array"));
      ExprPtr idx = LowerExpr(GetField(node, "index"));
      TSNode op_node = GetField(node, "operator");
      std::vector<ExprPtr> args;
      args.push_back(std::move(ba));
      args.push_back(std::move(idx));
      if (!ts_node_is_null(op_node)) {
        std::vector<SymbolId> mod_path;
        SymbolId op_sym = kInvalidSymbol;
        std::string op_str;
        SplitPath(op_node, &mod_path, &op_sym, &op_str);
        return MakeQualifiedCall(std::move(mod_path),
                                 intern_.Intern(absl::StrCat(".", op_str, "{}")),
                                 std::move(args), std::move(loc));
      }
      return MakeQualifiedCall(
          {intern_.Intern("Bigarray"), intern_.Intern("Array1")},
          intern_.Intern("get"), std::move(args), std::move(loc));
    }
    if (type == "set_expression") {
      auto children = GetNamedChildren(node);
      if (children.empty()) {
        return MakeExpr(Expr::Unit{}, std::move(loc));
      }
      TSNode lhs = children[0];
      absl::string_view lhs_type = NodeType(lhs);
      ExprPtr val = LowerExpr(GetField(node, "body"));
      if (lhs_type == "field_get_expression") {
        ExprPtr rec = LowerExpr(GetField(lhs, "record"));
        std::vector<SymbolId> f_mod_path;
        SymbolId f_sym = kInvalidSymbol;
        SplitPath(GetField(lhs, "field"), &f_mod_path, &f_sym);
        return MakeExpr(Expr::FieldSet{std::move(rec), f_sym, std::move(val),
                                       std::move(f_mod_path)},
                        std::move(loc));
      }
      if (lhs_type == "array_get_expression") {
        ExprPtr arr = LowerExpr(GetField(lhs, "array"));
        ExprPtr idx = LowerExpr(GetField(lhs, "index"));
        TSNode op_node = GetField(lhs, "operator");
        std::vector<ExprPtr> args;
        args.push_back(std::move(arr));
        args.push_back(std::move(idx));
        args.push_back(std::move(val));
        if (!ts_node_is_null(op_node)) {
          std::vector<SymbolId> mod_path;
          SymbolId op_sym = kInvalidSymbol;
          std::string op_str;
          SplitPath(op_node, &mod_path, &op_sym, &op_str);
          return MakeQualifiedCall(
              std::move(mod_path),
              intern_.Intern(absl::StrCat(".", op_str, "()<-")),
              std::move(args), std::move(loc));
        }
        return MakeQualifiedCall({intern_.Intern("Array")},
                                 intern_.Intern("set"), std::move(args),
                                 std::move(loc));
      }
      if (lhs_type == "string_get_expression") {
        ExprPtr str = LowerExpr(GetField(lhs, "string"));
        ExprPtr idx = LowerExpr(GetField(lhs, "index"));
        TSNode op_node = GetField(lhs, "operator");
        std::vector<ExprPtr> args;
        args.push_back(std::move(str));
        args.push_back(std::move(idx));
        args.push_back(std::move(val));
        if (!ts_node_is_null(op_node)) {
          std::vector<SymbolId> mod_path;
          SymbolId op_sym = kInvalidSymbol;
          std::string op_str;
          SplitPath(op_node, &mod_path, &op_sym, &op_str);
          return MakeQualifiedCall(
              std::move(mod_path),
              intern_.Intern(absl::StrCat(".", op_str, "[]<-")),
              std::move(args), std::move(loc));
        }
        return MakeQualifiedCall({intern_.Intern("Bytes")},
                                 intern_.Intern("set"), std::move(args),
                                 std::move(loc));
      }
      if (lhs_type == "bigarray_get_expression") {
        ExprPtr ba = LowerExpr(GetField(lhs, "array"));
        ExprPtr idx = LowerExpr(GetField(lhs, "index"));
        TSNode op_node = GetField(lhs, "operator");
        std::vector<ExprPtr> args;
        args.push_back(std::move(ba));
        args.push_back(std::move(idx));
        args.push_back(std::move(val));
        if (!ts_node_is_null(op_node)) {
          std::vector<SymbolId> mod_path;
          SymbolId op_sym = kInvalidSymbol;
          std::string op_str;
          SplitPath(op_node, &mod_path, &op_sym, &op_str);
          return MakeQualifiedCall(
              std::move(mod_path),
              intern_.Intern(absl::StrCat(".", op_str, "{}<-")),
              std::move(args), std::move(loc));
        }
        return MakeQualifiedCall(
            {intern_.Intern("Bigarray"), intern_.Intern("Array1")},
            intern_.Intern("set"), std::move(args), std::move(loc));
      }
      return val;
    }
    if (type == "sign_expression" || type == "prefix_expression") {
      TSNode op_node = GetField(node, "operator");
      TSNode arg_node = GetField(node, "expression");
      absl::string_view op_str = NodeText(op_node);
      if ((op_str == "-" || op_str == "-." || op_str == "~-" ||
           op_str == "~-.") &&
          NodeType(arg_node) == "number") {
        return ConstToExpr(ParseNumberConst(NodeText(arg_node), /*negate=*/true),
                           std::move(loc));
      }
      if ((op_str == "+" || op_str == "+." || op_str == "~+" ||
           op_str == "~+.") &&
          NodeType(arg_node) == "number") {
        return ConstToExpr(
            ParseNumberConst(NodeText(arg_node), /*negate=*/false),
            std::move(loc));
      }
      std::string fn_op(op_str);
      if (op_str == "-") {
        fn_op = "~-";
      } else if (op_str == "+") {
        fn_op = "~+";
      } else if (op_str == "-.") {
        fn_op = "~-.";
      } else if (op_str == "+.") {
        fn_op = "~+.";
      }
      std::vector<ExprPtr> args;
      args.push_back(LowerExpr(arg_node));
      return MakeQualifiedCall({}, intern_.Intern(fn_op), std::move(args),
                               std::move(loc));
    }
    if (type == "infix_expression" || type == "hash_expression") {
      TSNode left_node = GetField(node, "left");
      TSNode op_node = GetField(node, "operator");
      TSNode right_node = GetField(node, "right");
      absl::string_view op_str = NodeText(op_node);
      if (op_str == "&&" || op_str == "&") {
        return MakeExpr(
            Expr::If{LowerExpr(left_node), LowerExpr(right_node),
                     MakeExpr(Expr::Bool{false}, loc)},
            std::move(loc));
      }
      if (op_str == "||" || op_str == "or") {
        return MakeExpr(
            Expr::If{LowerExpr(left_node), MakeExpr(Expr::Bool{true}, loc),
                     LowerExpr(right_node)},
            std::move(loc));
      }
      std::vector<ExprPtr> args;
      args.push_back(LowerExpr(left_node));
      args.push_back(LowerExpr(right_node));
      return MakeQualifiedCall({}, intern_.Intern(op_str), std::move(args),
                               std::move(loc));
    }
    if (type == "application_expression") {
      TSNode fn_node = GetField(node, "function");
      auto children = GetNamedChildren(node);
      std::vector<TSNode> arg_nodes;
      for (TSNode c : children) {
        if (!ts_node_eq(c, fn_node)) {
          arg_nodes.push_back(c);
        }
      }

      if (NodeType(fn_node) == "value_path" && arg_nodes.size() == 1) {
        std::vector<SymbolId> mod_path;
        SymbolId fn_sym = kInvalidSymbol;
        std::string fn_str;
        SplitPath(fn_node, &mod_path, &fn_sym, &fn_str);
        if (mod_path.empty() ||
            (mod_path.size() == 1 &&
             intern_.Resolve(mod_path[0]) == "Stdlib")) {
          TSNode arg0 = arg_nodes[0];
          SourceLoc arg_loc = GetLoc(arg0);
          if (fn_str == "__LOC_OF__" || fn_str == "%loc_LOC_OF") {
            return MakeExpr(
                Expr::LocMacro{LocMacroKind::kLocLOC_OF, LowerExpr(arg0)},
                std::move(arg_loc));
          }
          if (fn_str == "__LINE_OF__") {
            std::vector<ExprPtr> elems;
            elems.push_back(MakeExpr(
                Expr::LocMacro{LocMacroKind::kLocLINE, nullptr}, arg_loc));
            elems.push_back(LowerExpr(arg0));
            return MakeExpr(Expr::Tuple{std::move(elems)}, std::move(loc));
          }
          if (fn_str == "__POS_OF__") {
            std::vector<ExprPtr> elems;
            elems.push_back(MakeExpr(
                Expr::LocMacro{LocMacroKind::kLocPOS, nullptr}, arg_loc));
            elems.push_back(LowerExpr(arg0));
            return MakeExpr(Expr::Tuple{std::move(elems)}, std::move(loc));
          }
        }
      }

      ExprPtr fn = LowerExpr(fn_node);
      std::vector<ApplyArg> args;
      args.reserve(arg_nodes.size());
      for (TSNode a : arg_nodes) {
        args.push_back(LowerApplyArg(a));
      }

      if (auto* ctor = std::get_if<Expr::Construct>(&fn->node)) {
        if (ctor->arg == nullptr && !args.empty()) {
          if (args.size() == 1) {
            ctor->arg = std::move(args[0].expr);
          } else {
            std::vector<ExprPtr> elems;
            elems.reserve(args.size());
            for (auto& a : args) {
              elems.push_back(std::move(a.expr));
            }
            ctor->arg = MakeExpr(Expr::Tuple{std::move(elems)}, loc);
          }
          fn->loc = std::move(loc);
          return fn;
        }
      }

      return MakeExpr(Expr::Apply{std::move(fn), std::move(args)},
                      std::move(loc));
    }
    if (type == "fun_expression") {
      std::vector<FuncParam> params;
      for (TSNode c : GetNamedChildren(node)) {
        absl::string_view ct = NodeType(c);
        if (ct == "parameter" || ct == "abstract_type") {
          if (auto fp = LowerFuncParam(c)) {
            params.push_back(std::move(*fp));
          }
        }
      }
      ExprPtr body = LowerExpr(GetField(node, "body"));
      if (params.empty()) {
        return body;
      }
      return MakeExpr(Expr::Function{std::move(params), std::move(body),
                                     kInvalidSymbol},
                      std::move(loc));
    }
    if (type == "function_expression") {
      SymbolId arg_sym = intern_.Intern("__function_arg");
      FuncParam fp;
      fp.kind = ParamKind::kPositional;
      fp.pattern = MakePattern(Pattern::Var{arg_sym}, loc);
      std::vector<FuncParam> params;
      params.push_back(std::move(fp));
      ExprPtr scrutinee = MakeExpr(Expr::Var{{}, arg_sym}, loc);
      ExprPtr match_expr = MakeExpr(
          Expr::Match{std::move(scrutinee), LowerMatchCases(node)}, loc);
      return MakeExpr(Expr::Function{std::move(params), std::move(match_expr),
                                     kInvalidSymbol},
                      std::move(loc));
    }
    if (type == "match_expression") {
      TSNode scrut_node = GetField(node, "expression");
      ExprPtr scrutinee = LowerExpr(scrut_node);
      return MakeExpr(
          Expr::Match{std::move(scrutinee), LowerMatchCases(node)},
          std::move(loc));
    }
    if (type == "try_expression") {
      ExprPtr body = LowerExpr(GetField(node, "expression"));
      return MakeExpr(Expr::Try{std::move(body), LowerMatchCases(node)},
                      std::move(loc));
    }
    if (type == "let_expression") {
      ExprPtr body = LowerExpr(GetField(node, "body"));
      auto children = GetNamedChildren(node);
      if (children.empty()) {
        return body;
      }
      TSNode def_node = children[0];
      absl::string_view def_type = NodeType(def_node);
      if (def_type == "value_definition") {
        return LowerValueDefInExpr(def_node, std::move(body), std::move(loc));
      }
      if (def_type == "open_module") {
        ModuleExprPtr mod_expr = LowerModuleExpr(GetField(def_node, "module"));
        return MakeExpr(Expr::LocalOpen{std::move(mod_expr), std::move(body)},
                        std::move(loc));
      }
      if (def_type == "module_definition") {
        for (TSNode mb : GetNamedChildren(def_node)) {
          if (NodeType(mb) == "module_binding") {
            auto [m_sym, m_expr] = LowerModuleBinding(mb);
            if (m_expr != nullptr) {
              return MakeExpr(
                  Expr::LetModule{m_sym, std::move(m_expr), std::move(body)},
                  std::move(loc));
            }
          }
        }
        return body;
      }
      if (def_type == "exception_definition") {
        auto [exn_sym, alias_path] = LowerExceptionDecl(def_node);
        return MakeExpr(Expr::LetException{exn_sym, std::move(alias_path),
                                           std::move(body)},
                        std::move(loc));
      }
      return body;
    }
    if (type == "sequence_expression") {
      auto children = GetNamedChildren(node);
      if (children.empty()) {
        return MakeExpr(Expr::Unit{}, std::move(loc));
      }
      ExprPtr acc = LowerExpr(children.back());
      for (int i = static_cast<int>(children.size()) - 2; i >= 0; --i) {
        ExprPtr lhs = LowerExpr(children[i]);
        acc = MakeExpr(Expr::Seq{std::move(lhs), std::move(acc)}, loc);
      }
      return acc;
    }
    if (type == "if_expression") {
      ExprPtr cond = LowerExpr(GetField(node, "condition"));
      ExprPtr then_br = MakeExpr(Expr::Unit{}, loc);
      ExprPtr else_br = MakeExpr(Expr::Unit{}, loc);
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "then_clause") {
          then_br = LowerExpr(GetField(c, "expression"));
        } else if (NodeType(c) == "else_clause") {
          else_br = LowerExpr(GetField(c, "expression"));
        }
      }
      return MakeExpr(
          Expr::If{std::move(cond), std::move(then_br), std::move(else_br)},
          std::move(loc));
    }
    if (type == "while_expression") {
      ExprPtr cond = LowerExpr(GetField(node, "condition"));
      ExprPtr body = MakeExpr(Expr::Unit{}, loc);
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "do_clause") {
          auto do_children = GetNamedChildren(c);
          if (!do_children.empty()) {
            body = LowerExpr(do_children[0]);
          }
          break;
        }
      }
      return MakeExpr(Expr::While{std::move(cond), std::move(body)},
                      std::move(loc));
    }
    if (type == "for_expression") {
      PatternPtr name_pat = LowerPattern(GetField(node, "name"));
      SymbolId var_sym = intern_.Intern("_for_var");
      if (const auto* v = std::get_if<Pattern::Var>(&name_pat->node)) {
        var_sym = v->name;
      }
      ExprPtr start = LowerExpr(GetField(node, "from"));
      ExprPtr end = LowerExpr(GetField(node, "to"));
      bool is_downto = HasAnonToken(node, "downto");
      ExprPtr body = MakeExpr(Expr::Unit{}, loc);
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "do_clause") {
          auto do_children = GetNamedChildren(c);
          if (!do_children.empty()) {
            body = LowerExpr(do_children[0]);
          }
          break;
        }
      }
      return MakeExpr(Expr::For{var_sym, std::move(start), std::move(end),
                                is_downto, std::move(body)},
                      std::move(loc));
    }
    if (type == "assert_expression") {
      return MakeExpr(Expr::Assert{LowerExpr(GetField(node, "expression"))},
                      std::move(loc));
    }
    if (type == "lazy_expression") {
      return MakeExpr(Expr::Lazy{LowerExpr(GetField(node, "expression"))},
                      std::move(loc));
    }
    if (type == "local_open_expression") {
      std::vector<SymbolId> mod_path;
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "module_path") {
          CollectModulePath(c, &mod_path);
          break;
        }
      }
      ModuleExprPtr mod_expr =
          MakeModuleExpr(ModuleExpr::ModPath{std::move(mod_path)}, loc);
      ExprPtr body = LowerExpr(GetField(node, "expression"));
      return MakeExpr(Expr::LocalOpen{std::move(mod_expr), std::move(body)},
                      std::move(loc));
    }
    if (type == "package_expression") {
      ModuleExprPtr mod_expr = LowerModuleExpr(GetField(node, "module"));
      return MakeExpr(Expr::PackModule{std::move(mod_expr)}, std::move(loc));
    }
    if (type == "object_expression") {
      return MakeExpr(Expr::ObjectDummy{}, std::move(loc));
    }
    if (type == "extension") {
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "attribute_payload") {
          for (TSNode item : GetNamedChildren(c)) {
            if (NodeType(item) == "expression_item") {
              auto expr_children = GetNamedChildren(item);
              if (!expr_children.empty() &&
                  NodeType(expr_children[0]) == "field_get_expression") {
                TSNode fg = expr_children[0];
                ExprPtr rec = LowerExpr(GetField(fg, "record"));
                std::vector<SymbolId> f_mod_path;
                SymbolId f_sym = kInvalidSymbol;
                SplitPath(GetField(fg, "field"), &f_mod_path, &f_sym);
                std::vector<ExprPtr> elems;
                elems.push_back(std::move(rec));
                elems.push_back(MakeExpr(
                    Expr::String{std::string(intern_.Resolve(f_sym))}, loc));
                return MakeExpr(Expr::Tuple{std::move(elems)}, std::move(loc));
              }
            }
          }
        }
      }
      return MakeExpr(Expr::Unit{}, std::move(loc));
    }

    return MakeExpr(Expr::Unit{}, std::move(loc));
  }

 private:
  // Lowers a `value_definition` inside a `let ... in body` expression,
  // including `let*` / `let+` / `and*` / `and+` binding operators.
  ExprPtr LowerValueDefInExpr(TSNode val_def, ExprPtr body, SourceLoc loc) {
    bool is_rec = HasAnonToken(val_def, "rec");
    std::string let_op;
    std::string and_op;
    std::vector<LetBinding> bindings;
    for (TSNode c : GetNamedChildren(val_def)) {
      absl::string_view ct = NodeType(c);
      if (ct == "let_operator") {
        let_op = std::string(NodeText(c));
      } else if (ct == "let_and_operator") {
        and_op = std::string(NodeText(c));
      } else if (ct == "let_binding") {
        bindings.push_back(LowerLetBinding(c));
      }
    }

    if (!let_op.empty() && !bindings.empty()) {
      ExprPtr comb_expr = std::move(bindings[0].expr);
      PatternPtr comb_pat = std::move(bindings[0].pattern);
      for (size_t i = 1; i < bindings.size(); ++i) {
        std::vector<ExprPtr> and_args;
        and_args.push_back(std::move(comb_expr));
        and_args.push_back(std::move(bindings[i].expr));
        comb_expr = MakeQualifiedCall({}, intern_.Intern(and_op),
                                      std::move(and_args), loc);

        std::vector<PatternPtr> pat_pair;
        pat_pair.push_back(std::move(comb_pat));
        pat_pair.push_back(std::move(bindings[i].pattern));
        comb_pat = MakePattern(Pattern::Tuple{std::move(pat_pair)}, loc);
      }
      FuncParam fp;
      fp.kind = ParamKind::kPositional;
      fp.pattern = std::move(comb_pat);
      std::vector<FuncParam> params;
      params.push_back(std::move(fp));
      ExprPtr fn = MakeExpr(
          Expr::Function{std::move(params), std::move(body), kInvalidSymbol},
          loc);
      std::vector<ExprPtr> let_args;
      let_args.push_back(std::move(comb_expr));
      let_args.push_back(std::move(fn));
      return MakeQualifiedCall({}, intern_.Intern(let_op), std::move(let_args),
                               std::move(loc));
    }

    return MakeExpr(Expr::Let{is_rec, std::move(bindings), std::move(body)},
                    std::move(loc));
  }

  // Extracts functor parameter names from a sequence of `module_parameter`
  // nodes.
  std::vector<std::optional<SymbolId>> ExtractFunctorParams(
      const std::vector<TSNode>& children) {
    std::vector<std::optional<SymbolId>> params;
    for (TSNode c : children) {
      if (NodeType(c) != "module_parameter") continue;
      std::optional<SymbolId> param_sym = std::nullopt;
      for (TSNode sub : GetNamedChildren(c)) {
        if (NodeType(sub) == "module_name") {
          std::string s = ExtractNameString(sub);
          if (s != "_") {
            param_sym = intern_.Intern(s);
          }
          break;
        }
      }
      params.push_back(param_sym);
    }
    return params;
  }

  // Lowers a `module_binding` node into `(module_name, ModuleExprPtr)`.
  std::pair<SymbolId, ModuleExprPtr> LowerModuleBinding(TSNode mb) {
    SourceLoc loc = GetLoc(mb);
    SymbolId mod_sym = kInvalidSymbol;
    auto children = GetNamedChildren(mb);
    for (TSNode c : children) {
      if (NodeType(c) == "module_name") {
        mod_sym = ExtractNameSymbol(c);
        break;
      }
    }
    TSNode body_node = GetField(mb, "body");
    if (ts_node_is_null(body_node)) {
      return {mod_sym, nullptr};
    }
    ModuleExprPtr body = LowerModuleExpr(body_node);
    auto params = ExtractFunctorParams(children);
    if (!params.empty()) {
      body = MakeModuleExpr(
          ModuleExpr::Functor{std::move(params), std::move(body)},
          std::move(loc));
    }
    return {mod_sym, std::move(body)};
  }

  // Extracts `(exn_name, alias_path)` from an `exception_definition` node.
  std::pair<SymbolId, std::vector<SymbolId>> LowerExceptionDecl(
      TSNode exn_def) {
    SymbolId exn_sym = kInvalidSymbol;
    std::vector<SymbolId> alias_path;
    for (TSNode c : GetNamedChildren(exn_def)) {
      if (NodeType(c) != "constructor_declaration") continue;
      for (TSNode sub : GetNamedChildren(c)) {
        absl::string_view st = NodeType(sub);
        if (st == "constructor_name" && exn_sym == kInvalidSymbol) {
          exn_sym = ExtractNameSymbol(sub);
        } else if (st == "constructor_path") {
          SymbolId target_ctor = kInvalidSymbol;
          SplitPath(sub, &alias_path, &target_ctor);
          alias_path.push_back(target_ctor);
        }
      }
    }
    return {exn_sym, std::move(alias_path)};
  }

 public:
  ModuleExprPtr LowerModuleExpr(TSNode node) {
    SourceLoc loc = GetLoc(node);
    if (ts_node_is_null(node)) {
      return MakeModuleExpr(ModuleExpr::Struct{}, std::move(loc));
    }
    absl::string_view type = NodeType(node);

    if (type == "parenthesized_module_expression") {
      auto children = GetNamedChildren(node);
      if (!children.empty()) {
        return LowerModuleExpr(children[0]);
      }
      return MakeModuleExpr(ModuleExpr::Struct{}, std::move(loc));
    }
    if (type == "typed_module_expression") {
      return LowerModuleExpr(GetField(node, "module"));
    }
    if (type == "structure") {
      Structure items;
      uint32_t count = ts_node_child_count(node);
      for (uint32_t i = 0; i < count; ++i) {
        LowerStructureItem(ts_node_child(node, i), &items);
      }
      return MakeModuleExpr(ModuleExpr::Struct{std::move(items)},
                            std::move(loc));
    }
    if (type == "module_path") {
      std::vector<SymbolId> path;
      CollectModulePath(node, &path);
      return MakeModuleExpr(ModuleExpr::ModPath{std::move(path)},
                            std::move(loc));
    }
    if (type == "functor") {
      auto params = ExtractFunctorParams(GetNamedChildren(node));
      ModuleExprPtr body = LowerModuleExpr(GetField(node, "body"));
      return MakeModuleExpr(
          ModuleExpr::Functor{std::move(params), std::move(body)},
          std::move(loc));
    }
    if (type == "module_application") {
      ModuleExprPtr functor = LowerModuleExpr(GetField(node, "functor"));
      TSNode arg_node = GetField(node, "argument");
      ModuleExprPtr arg =
          ts_node_is_null(arg_node) ? nullptr : LowerModuleExpr(arg_node);
      return MakeModuleExpr(
          ModuleExpr::ModApply{std::move(functor), std::move(arg)},
          std::move(loc));
    }
    if (type == "packed_module") {
      ExprPtr val = LowerExpr(GetField(node, "value"));
      return MakeModuleExpr(ModuleExpr::Unpack{std::move(val)},
                            std::move(loc));
    }

    return MakeModuleExpr(ModuleExpr::Struct{}, std::move(loc));
  }

  void LowerStructureItem(TSNode node, Structure* items) {
    if (ts_node_is_null(node) || !ts_node_is_named(node) ||
        IsExtraOrAttribute(node)) {
      return;
    }
    SourceLoc loc = GetLoc(node);
    absl::string_view type = NodeType(node);

    if (type == "expression_item") {
      auto children = GetNamedChildren(node);
      if (!children.empty()) {
        items->emplace_back(
            StructureItem::EvalExpr{LowerExpr(children[0])}, std::move(loc));
      }
      return;
    }
    if (type == "value_definition") {
      bool is_rec = HasAnonToken(node, "rec");
      std::vector<LetBinding> bindings;
      for (TSNode c : GetNamedChildren(node)) {
        if (NodeType(c) == "let_binding") {
          bindings.push_back(LowerLetBinding(c));
        }
      }
      if (!bindings.empty()) {
        items->emplace_back(
            StructureItem::ValueDef{is_rec, std::move(bindings)},
            std::move(loc));
      }
      return;
    }
    if (type == "external") {
      SymbolId ext_sym = kInvalidSymbol;
      std::string prim_name;
      for (TSNode c : GetNamedChildren(node)) {
        absl::string_view ct = NodeType(c);
        if ((ct == "value_name" || ct == "parenthesized_operator") &&
            ext_sym == kInvalidSymbol) {
          ext_sym = ExtractNameSymbol(c);
        } else if (ct == "string" && prim_name.empty()) {
          absl::string_view raw = NodeText(c);
          if (raw.size() >= 2) {
            prim_name = UnescapeOcamlString(raw.substr(1, raw.size() - 2));
          }
        } else if (ct == "quoted_string" && prim_name.empty()) {
          prim_name = ExtractQuotedString(NodeText(c));
        }
      }
      if (ext_sym != kInvalidSymbol) {
        items->emplace_back(
            StructureItem::ExternalDef{ext_sym, std::move(prim_name)},
            std::move(loc));
      }
      return;
    }
    if (type == "exception_definition") {
      auto [exn_sym, alias_path] = LowerExceptionDecl(node);
      if (exn_sym != kInvalidSymbol) {
        items->emplace_back(
            StructureItem::ExceptionDef{exn_sym, std::move(alias_path)},
            std::move(loc));
      }
      return;
    }
    if (type == "type_definition") {
      std::vector<std::pair<SymbolId, std::vector<SymbolId>>> ext_ctors;
      for (TSNode tb : GetNamedChildren(node)) {
        if (NodeType(tb) != "type_binding") {
          continue;
        }
        TSNode body = GetField(tb, "body");
        if (ts_node_is_null(body)) {
          continue;
        }
        if (!HasAnonToken(tb, "+=") &&
            NodeType(body) == "polymorphic_variant_type") {
          TSNode name_node = GetField(tb, "name");
          std::string tname =
              std::string(absl::StripAsciiWhitespace(NodeText(name_node)));
          PolyTagList tags;
          for (TSNode spec : GetNamedChildren(body)) {
            absl::string_view st = NodeType(spec);
            if (st == "tag_specification") {
              auto spec_children = GetNamedChildren(spec);
              for (TSNode sc : spec_children) {
                if (NodeType(sc) == "tag") {
                  tags.emplace_back(ExtractNameString(sc),
                                    spec_children.size() > 1);
                  break;
                }
              }
            } else if (st == "type_constructor_path") {
              std::string inherited =
                  std::string(absl::StripAsciiWhitespace(NodeText(spec)));
              auto it = poly_variant_types_.find(inherited);
              if (it != poly_variant_types_.end()) {
                tags.insert(tags.end(), it->second.begin(), it->second.end());
              }
            }
          }
          if (!tags.empty() && !tname.empty() && tname != "view") {
            if (!module_name_.empty()) {
              poly_variant_types_[absl::StrCat(module_name_, ".", tname)] = tags;
              GlobalPolyVariantTypes()[absl::StrCat(module_name_, ".", tname)] = tags;
            }
            GlobalPolyVariantTypes()[tname] = tags;
            poly_variant_types_[tname] = std::move(tags);
          }
          continue;
        }
        if (!HasAnonToken(tb, "+=") ||
            NodeType(body) != "variant_declaration") {
          continue;
        }
        for (TSNode cd : GetNamedChildren(body)) {
          if (NodeType(cd) != "constructor_declaration") continue;
          SymbolId ctor_sym = kInvalidSymbol;
          std::vector<SymbolId> alias_path;
          for (TSNode sub : GetNamedChildren(cd)) {
            absl::string_view st = NodeType(sub);
            if (st == "constructor_name" && ctor_sym == kInvalidSymbol) {
              ctor_sym = ExtractNameSymbol(sub);
            } else if (st == "constructor_path") {
              SymbolId target_ctor = kInvalidSymbol;
              SplitPath(sub, &alias_path, &target_ctor);
              alias_path.push_back(target_ctor);
            }
          }
          if (ctor_sym != kInvalidSymbol) {
            ext_ctors.emplace_back(ctor_sym, std::move(alias_path));
          }
        }
      }
      if (!ext_ctors.empty()) {
        items->emplace_back(StructureItem::TypeExtDef{std::move(ext_ctors)},
                            std::move(loc));
      }
      return;
    }
    if (type == "open_module") {
      ModuleExprPtr mod_expr = LowerModuleExpr(GetField(node, "module"));
      items->emplace_back(StructureItem::OpenDef{std::move(mod_expr)},
                          std::move(loc));
      return;
    }
    if (type == "include_module") {
      ModuleExprPtr mod_expr = LowerModuleExpr(GetField(node, "module"));
      items->emplace_back(StructureItem::IncludeDef{std::move(mod_expr)},
                          std::move(loc));
      return;
    }
    if (type == "module_definition") {
      bool is_rec = HasAnonToken(node, "rec");
      std::vector<std::pair<SymbolId, ModuleExprPtr>> mods;
      for (TSNode mb : GetNamedChildren(node)) {
        if (NodeType(mb) == "module_binding") {
          auto [m_sym, m_expr] = LowerModuleBinding(mb);
          if (m_expr != nullptr) {
            mods.emplace_back(m_sym, std::move(m_expr));
          }
        }
      }
      if (is_rec && mods.size() > 1) {
        items->emplace_back(StructureItem::ModuleRecDef{std::move(mods)},
                            std::move(loc));
      } else {
        for (auto& [m_sym, m_expr] : mods) {
          items->emplace_back(
              StructureItem::ModuleDef{m_sym, std::move(m_expr)}, loc);
        }
      }
      return;
    }
  }

 private:
  using PolyTagList = std::vector<std::pair<std::string, bool>>;

  absl::string_view source_;
  std::string filename_;
  std::string module_name_;
  std::string current_function_name_;
  InternTable& intern_;
  std::vector<LineDirective> line_directives_;
  absl::flat_hash_map<std::string, PolyTagList> poly_variant_types_;
};

}  // namespace

std::string ModuleNameFromFilename(absl::string_view filename) {
  size_t slash = filename.find_last_of("/\\");
  absl::string_view base =
      (slash == absl::string_view::npos) ? filename : filename.substr(slash + 1);
  if (absl::EndsWith(base, ".mli")) {
    base.remove_suffix(4);
  } else if (absl::EndsWith(base, ".ml")) {
    base.remove_suffix(3);
  } else {
    size_t dot = base.find('.');
    if (dot != absl::string_view::npos) {
      base = base.substr(0, dot);
    }
  }
  std::string mod(base);
  if (!mod.empty()) {
    mod[0] = absl::ascii_toupper(mod[0]);
  }
  return mod;
}

absl::StatusOr<Structure> ParseAndLower(absl::string_view source,
                                        absl::string_view filename,
                                        InternTable& intern,
                                        absl::string_view module_name) {
  TSParser* parser = ts_parser_new();
  CHECK(parser != nullptr);
  bool ok = ts_parser_set_language(parser, tree_sitter_ocaml());
  CHECK(ok);

  TSTree* tree = ts_parser_parse_string(
      parser, nullptr, source.data(), static_cast<uint32_t>(source.size()));
  if (tree == nullptr) {
    ts_parser_delete(parser);
    return absl::InternalError(
        absl::StrCat("Tree-sitter failed to parse ", filename));
  }

  TSNode root = ts_tree_root_node(tree);
  std::string resolved_mod = module_name.empty()
                                 ? ModuleNameFromFilename(filename)
                                 : std::string(module_name);
  Lowerer lowerer(source, filename, resolved_mod, intern);
  Structure items = lowerer.LowerCompilationUnit(root);

  ts_tree_delete(tree);
  ts_parser_delete(parser);
  return items;
}

Structure LowerOcamlSource(absl::string_view source, absl::string_view filename,
                           InternTable& intern,
                           absl::string_view module_name) {
  auto res = ParseAndLower(source, filename, intern, module_name);
  CHECK_OK(res.status());
  return std::move(*res);
}

absl::StatusOr<Structure> ParseAndLowerFile(absl::string_view filepath,
                                            InternTable& intern,
                                            absl::string_view module_name) {
  std::ifstream ifs{std::string(filepath), std::ios::binary};
  if (!ifs) {
    return absl::NotFoundError(
        absl::StrCat("Could not open OCaml source file: ", filepath));
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return ParseAndLower(oss.str(), filepath, intern, module_name);
}

std::optional<absl::flat_hash_set<SymbolId>> ParseInterfaceExportedValues(
    absl::string_view mli_source, InternTable& intern) {
  TSParser* parser = ts_parser_new();
  CHECK(parser != nullptr);
  bool ok = ts_parser_set_language(parser, tree_sitter_ocaml_interface());
  CHECK(ok);

  TSTree* tree = ts_parser_parse_string(
      parser, nullptr, mli_source.data(),
      static_cast<uint32_t>(mli_source.size()));
  if (tree == nullptr) {
    ts_parser_delete(parser);
    return std::nullopt;
  }

  TSNode root = ts_tree_root_node(tree);
  Lowerer lowerer(mli_source, "<mli>", "", intern);
  auto exported = lowerer.ExtractInterfaceExportedValues(root);

  ts_tree_delete(tree);
  ts_parser_delete(parser);
  return exported;
}

namespace {

// Formats an arbitrary byte string as a valid OCaml string literal.
std::string EscapeOcamlStringLiteral(absl::string_view s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c >= 32 && c <= 126 && c != '"' && c != '\\') {
      out.push_back(static_cast<char>(c));
    } else {
      absl::StrAppendFormat(&out, "\\x%02x", static_cast<unsigned int>(c));
    }
  }
  out.push_back('"');
  return out;
}

// Lowers a Tree-sitter OCamllex CST into an OCaml `Parser` module source string
// defining `lexer_definition`.
class OcamllexLowerer {
 public:
  OcamllexLowerer(absl::string_view source, absl::string_view filename)
      : source_(source),
        escaped_filename_(EscapeOcamlStringLiteral(filename)) {}

  std::string Lower(TSNode root) {
    std::string out = R"ocaml(open Syntax
let named_regexps : (string, regular_expression) Hashtbl.t = Hashtbl.create 64
let regexp_for_string s =
  let rec re_string n =
    if n >= String.length s then Epsilon
    else if succ n = String.length s then
      Characters (Cset.singleton (Char.code s.[n]))
    else
      Sequence (Characters (Cset.singleton (Char.code s.[n])), re_string (succ n))
  in re_string 0
let rec remove_as = function
  | Bind (e, _) -> remove_as e
  | (Epsilon | Eof | Characters _) as e -> e
  | Sequence (e1, e2) -> Sequence (remove_as e1, remove_as e2)
  | Alternative (e1, e2) -> Alternative (remove_as e1, remove_as e2)
  | Repetition e -> Repetition (remove_as e)
let rec as_cset = function
  | Characters s -> s
  | Alternative (e1, e2) -> Cset.union (as_cset e1) (as_cset e2)
  | _ -> raise Cset.Bad
let re_plus e = Sequence (Repetition (remove_as e), e)
let re_diff e1 e2 = Characters (Cset.diff (as_cset e1) (as_cset e2))
)ocaml";

    std::vector<TSNode> children = GetNamedChildren(root);
    uint32_t first_entry_byte = static_cast<uint32_t>(source_.size());
    for (TSNode c : children) {
      if (NodeType(c) == "lexer_entry") {
        first_entry_byte = ts_node_start_byte(c);
        break;
      }
    }

    std::string header_loc(kEmptyLoc);
    std::string trailer_loc(kEmptyLoc);
    std::string refill_expr = "None";
    std::vector<std::string> entry_vars;

    for (TSNode c : children) {
      absl::string_view ct = NodeType(c);
      if (ct == "action") {
        if (ts_node_start_byte(c) < first_entry_byte) {
          header_loc = FormatActionLoc(c);
        } else {
          trailer_loc = FormatActionLoc(c);
        }
      } else if (ct == "refill_handler") {
        refill_expr = absl::StrCat("(Some ", FormatActionLoc(c), ")");
      } else if (ct == "named_regexp") {
        TSNode name_node = GetField(c, "name");
        TSNode re_node = GetField(c, "regexp");
        absl::StrAppend(&out, "let () = Hashtbl.add named_regexps ",
                        EscapeOcamlStringLiteral(NodeText(name_node)), " ",
                        LowerRegexp(re_node), "\n");
      } else if (ct == "lexer_entry") {
        std::string var_name = absl::StrCat("_entry_", entry_vars.size());
        absl::StrAppend(&out, "let ", var_name, " = ", LowerEntry(c), "\n");
        entry_vars.push_back(std::move(var_name));
      }
    }

    absl::StrAppend(&out, "let lexer_definition _ _ = {\n",
                    "  header = ", header_loc, ";\n",
                    "  refill_handler = ", refill_expr, ";\n",
                    "  entrypoints = [");
    for (size_t i = 0; i < entry_vars.size(); ++i) {
      if (i > 0) absl::StrAppend(&out, "; ");
      absl::StrAppend(&out, entry_vars[i]);
    }
    absl::StrAppend(&out, "];\n",
                    "  trailer = ", trailer_loc, ";\n",
                    "}\n");
    return out;
  }

 private:
  static constexpr absl::string_view kEmptyLoc =
      "{ loc_file = \"\"; start_pos = 0; end_pos = 0; start_line = 1; "
      "start_col = 0; end_line = 1; end_col = 0 }";

  absl::string_view NodeType(TSNode node) const {
    if (ts_node_is_null(node)) return "";
    return ts_node_type(node);
  }

  absl::string_view NodeText(TSNode node) const {
    if (ts_node_is_null(node)) return "";
    uint32_t start = ts_node_start_byte(node);
    uint32_t end = ts_node_end_byte(node);
    if (start >= source_.size() || end <= start) return "";
    return source_.substr(start, end - start);
  }

  TSNode GetField(TSNode node, absl::string_view field_name) const {
    return ts_node_child_by_field_name(
        node, field_name.data(), static_cast<uint32_t>(field_name.size()));
  }

  std::vector<TSNode> GetNamedChildren(TSNode node) const {
    std::vector<TSNode> out;
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      TSNode c = ts_node_child(node, i);
      if (!ts_node_is_named(c) || ts_node_is_extra(c) ||
          NodeType(c) == "comment") {
        continue;
      }
      out.push_back(c);
    }
    return out;
  }

  bool HasAnonToken(TSNode node, absl::string_view tok) const {
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      TSNode c = ts_node_child(node, i);
      if (!ts_node_is_named(c) && !ts_node_is_extra(c) &&
          NodeText(c) == tok) {
        return true;
      }
    }
    return false;
  }

  std::string FormatActionLoc(TSNode node) const {
    TSNode open_brace = {};
    TSNode close_brace = {};
    bool found_open = false;
    bool found_close = false;
    uint32_t count = ts_node_child_count(node);
    for (uint32_t i = 0; i < count; ++i) {
      TSNode c = ts_node_child(node, i);
      if (!ts_node_is_named(c) && !ts_node_is_extra(c)) {
        absl::string_view t = NodeText(c);
        if (t == "{" && !found_open) {
          open_brace = c;
          found_open = true;
        } else if (t == "}") {
          close_brace = c;
          found_close = true;
        }
      }
    }
    uint32_t start_pos =
        found_open ? ts_node_end_byte(open_brace) : ts_node_start_byte(node) + 1;
    uint32_t end_pos =
        found_close ? ts_node_start_byte(close_brace) : ts_node_end_byte(node) - 1;
    TSPoint pt =
        found_open ? ts_node_start_point(open_brace) : ts_node_start_point(node);
    TSPoint end_pt =
        found_close ? ts_node_start_point(close_brace) : ts_node_end_point(node);
    uint32_t start_line = pt.row + 1;
    uint32_t start_col = pt.column + 1;
    uint32_t end_line = end_pt.row + 1;
    uint32_t end_col = end_pt.column;
    return absl::StrCat("{ loc_file = ", escaped_filename_,
                        "; start_pos = ", start_pos, "; end_pos = ", end_pos,
                        "; start_line = ", start_line,
                        "; start_col = ", start_col,
                        "; end_line = ", end_line,
                        "; end_col = ", end_col, " }");
  }

  std::string FormatNodeLoc(TSNode node) const {
    uint32_t start_pos = ts_node_start_byte(node);
    uint32_t end_pos = ts_node_end_byte(node);
    TSPoint pt = ts_node_start_point(node);
    TSPoint end_pt = ts_node_end_point(node);
    uint32_t start_line = pt.row + 1;
    uint32_t start_col = pt.column;
    uint32_t end_line = end_pt.row + 1;
    uint32_t end_col = end_pt.column;
    return absl::StrCat("{ loc_file = ", escaped_filename_,
                        "; start_pos = ", start_pos, "; end_pos = ", end_pos,
                        "; start_line = ", start_line,
                        "; start_col = ", start_col,
                        "; end_line = ", end_line,
                        "; end_col = ", end_col, " }");
  }

  std::string LowerRegexp(TSNode node) const {
    absl::string_view type = NodeType(node);
    absl::string_view text = NodeText(node);
    if (type == "any") {
      return "(Characters Cset.all_chars)";
    }
    if (type == "eof") {
      return "Eof";
    }
    if (type == "regexp_name") {
      return absl::StrCat("(Hashtbl.find named_regexps ",
                          EscapeOcamlStringLiteral(text), ")");
    }
    if (type == "character") {
      int code = static_cast<int>(UnescapeOcamlChar(text));
      return absl::StrCat("(Characters (Cset.singleton ", code, "))");
    }
    if (type == "string") {
      std::string unescaped =
          text.size() >= 2
              ? UnescapeOcamlString(text.substr(1, text.size() - 2))
              : "";
      return absl::StrCat("(regexp_for_string ",
                          EscapeOcamlStringLiteral(unescaped), ")");
    }
    if (type == "character_set") {
      std::string acc;
      for (TSNode c : GetNamedChildren(node)) {
        absl::string_view ct = NodeType(c);
        std::string item;
        if (ct == "character") {
          int code = static_cast<int>(UnescapeOcamlChar(NodeText(c)));
          item = absl::StrCat("(Cset.singleton ", code, ")");
        } else if (ct == "character_range") {
          auto ch = GetNamedChildren(c);
          CHECK_EQ(ch.size(), 2u);
          int c1 = static_cast<int>(UnescapeOcamlChar(NodeText(ch[0])));
          int c2 = static_cast<int>(UnescapeOcamlChar(NodeText(ch[1])));
          item = absl::StrCat("(Cset.interval ", c1, " ", c2, ")");
        }
        if (!item.empty()) {
          if (acc.empty()) {
            acc = std::move(item);
          } else {
            acc = absl::StrCat("(Cset.union ", acc, " ", item, ")");
          }
        }
      }
      if (acc.empty()) {
        acc = "Cset.empty";
      }
      if (HasAnonToken(node, "^")) {
        acc = absl::StrCat("(Cset.complement ", acc, ")");
      }
      return absl::StrCat("(Characters ", acc, ")");
    }
    if (type == "regexp_difference") {
      auto ch = GetNamedChildren(node);
      CHECK_EQ(ch.size(), 2u);
      return absl::StrCat("(re_diff ", LowerRegexp(ch[0]), " ",
                          LowerRegexp(ch[1]), ")");
    }
    if (type == "regexp_repetition") {
      auto ch = GetNamedChildren(node);
      CHECK_EQ(ch.size(), 1u);
      std::string inner = LowerRegexp(ch[0]);
      if (HasAnonToken(node, "*")) {
        return absl::StrCat("(Repetition ", inner, ")");
      }
      if (HasAnonToken(node, "?")) {
        return absl::StrCat("(Alternative (Epsilon, ", inner, "))");
      }
      if (HasAnonToken(node, "+")) {
        return absl::StrCat("(re_plus ", inner, ")");
      }
      return inner;
    }
    if (type == "regexp_alternative") {
      auto ch = GetNamedChildren(node);
      CHECK_EQ(ch.size(), 2u);
      return absl::StrCat("(Alternative (", LowerRegexp(ch[0]), ", ",
                          LowerRegexp(ch[1]), "))");
    }
    if (type == "regexp_concatenation") {
      auto ch = GetNamedChildren(node);
      CHECK_EQ(ch.size(), 2u);
      return absl::StrCat("(Sequence (", LowerRegexp(ch[0]), ", ",
                          LowerRegexp(ch[1]), "))");
    }
    if (type == "parenthesized_regexp") {
      auto ch = GetNamedChildren(node);
      CHECK_EQ(ch.size(), 1u);
      return LowerRegexp(ch[0]);
    }
    if (type == "aliased_regexp") {
      auto ch = GetNamedChildren(node);
      CHECK_EQ(ch.size(), 2u);
      return absl::StrCat("(Bind (", LowerRegexp(ch[0]), ", (",
                          EscapeOcamlStringLiteral(NodeText(ch[1])), ", ",
                          FormatNodeLoc(ch[1]), ")))");
    }
    LOG(FATAL) << "Unhandled ocamllex regexp node type: " << type;
  }

  std::string LowerEntry(TSNode node) const {
    TSNode name_node = GetField(node, "name");
    std::string name_str = EscapeOcamlStringLiteral(NodeText(name_node));
    bool shortest = HasAnonToken(node, "shortest");
    std::vector<std::string> args;
    std::vector<std::string> clauses;
    for (TSNode c : GetNamedChildren(node)) {
      absl::string_view ct = NodeType(c);
      if (ct == "lexer_argument") {
        args.push_back(EscapeOcamlStringLiteral(NodeText(c)));
      } else if (ct == "lexer_case") {
        auto ch = GetNamedChildren(c);
        CHECK_EQ(ch.size(), 2u);
        clauses.push_back(
            absl::StrCat("(", LowerRegexp(ch[0]), ", ", FormatActionLoc(ch[1]), ")"));
      }
    }
    std::string out = absl::StrCat("{ name = ", name_str,
                                   "; shortest = ", shortest ? "true" : "false",
                                   "; args = [");
    for (size_t i = 0; i < args.size(); ++i) {
      if (i > 0) absl::StrAppend(&out, "; ");
      absl::StrAppend(&out, args[i]);
    }
    absl::StrAppend(&out, "]; body_location = ", FormatNodeLoc(node), "; clauses = [\n");
    for (size_t i = 0; i < clauses.size(); ++i) {
      absl::StrAppend(&out, "  ", clauses[i], ";\n");
    }
    absl::StrAppend(&out, "] }");
    return out;
  }

  absl::string_view source_;
  std::string escaped_filename_;
};

}  // namespace

absl::StatusOr<std::string> ParseAndLowerOcamllex(
    absl::string_view mll_source, absl::string_view filename) {
  TSParser* parser = ts_parser_new();
  CHECK(parser != nullptr);
  bool ok = ts_parser_set_language(parser, tree_sitter_ocamllex());
  CHECK(ok);

  TSTree* tree = ts_parser_parse_string(
      parser, nullptr, mll_source.data(),
      static_cast<uint32_t>(mll_source.size()));
  if (tree == nullptr) {
    ts_parser_delete(parser);
    return absl::InternalError(
        absl::StrCat("tree-sitter-ocamllex failed to parse ", filename));
  }

  TSNode root = ts_tree_root_node(tree);
  OcamllexLowerer lowerer(mll_source, filename);
  std::string out = lowerer.Lower(root);

  ts_tree_delete(tree);
  ts_parser_delete(parser);
  return out;
}

}  // namespace ocaml_boot
