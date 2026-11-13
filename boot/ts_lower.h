// Copyright 2026 Google LLC.
// Tree-sitter OCaml parser and AST lowering pass for the OCaml 5.3 bootstrap
// interpreter.

#ifndef OCAML_BOOT_TS_LOWER_H_
#define OCAML_BOOT_TS_LOWER_H_

#include <optional>
#include <string>

#include "third_party/absl/container/flat_hash_set.h"
#include "third_party/absl/status/statusor.h"
#include "third_party/absl/strings/string_view.h"
#include "ast.h"
#include "intern.h"

namespace ocaml_boot {

// Computes the canonical OCaml module name from a file path (for example,
// `"stdlib/stdlib__List.ml"` -> `"Stdlib__List"`).
std::string ModuleNameFromFilename(absl::string_view filename);

// Parses an OCaml `.ml` source buffer with tree-sitter-ocaml and lowers it into
// a `Structure`. If `module_name` is empty, it is derived from `filename`.
absl::StatusOr<Structure> ParseAndLower(absl::string_view source,
                                        absl::string_view filename,
                                        InternTable& intern,
                                        absl::string_view module_name = "");

// Convenience wrapper that crashes on parse failure and returns the lowered
// `Structure` directly.
Structure LowerOcamlSource(absl::string_view source, absl::string_view filename,
                           InternTable& intern,
                           absl::string_view module_name = "");

// Reads an `.ml` file from disk, parses it, and lowers it into a `Structure`.
absl::StatusOr<Structure> ParseAndLowerFile(absl::string_view filepath,
                                            InternTable& intern,
                                            absl::string_view module_name = "");

// Parses an OCaml `.mli` interface buffer with tree-sitter-ocaml-interface and
// returns the set of top-level exported value symbols (`val` and `external`).
// Returns `std::nullopt` if the `.mli` contains a top-level `include`.
std::optional<absl::flat_hash_set<SymbolId>> ParseInterfaceExportedValues(
    absl::string_view mli_source, InternTable& intern);

// Parses an OCamllex `.mll` source buffer with tree-sitter-ocamllex and lowers
// it into an OCaml `Parser` module source string defining `lexer_definition`.
absl::StatusOr<std::string> ParseAndLowerOcamllex(
    absl::string_view mll_source, absl::string_view filename);

}  // namespace ocaml_boot

#endif  // OCAML_BOOT_TS_LOWER_H_
