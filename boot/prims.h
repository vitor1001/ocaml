// Copyright 2026 Google LLC.
// External primitive registration and binary marshaling interface for the
// OCaml 5.3 bootstrap interpreter.

#ifndef OCAML_BOOT_PRIMS_H_
#define OCAML_BOOT_PRIMS_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "third_party/absl/strings/string_view.h"
#include "intern.h"
#include "value.h"

namespace ocaml_boot {

class Evaluator;

// Registers all built-in (`%...`) and runtime (`caml_*`) external primitives on
// the given evaluator instance.
void RegisterPrimitives(Evaluator& eval);

// Serializes an OCaml runtime value into the OCaml `extern.c` small-magic
// binary format (`0x8495A6BE`).
std::string MarshalToString(const Value& v, int64_t flags, InternTable& intern);

// Deserializes an OCaml runtime value from `data` starting at `offset`.
// If `bytes_read` is non-null, writes the total number of bytes consumed
// (including the 20-byte header) into `*bytes_read`.
Value UnmarshalFromBytes(absl::string_view data, size_t offset,
                         InternTable& intern, size_t* bytes_read = nullptr);

// Reads the marshal header at `offset` in `data` and returns the payload size
// in bytes (matching `Marshal.data_size`).
int64_t MarshalDataSize(absl::string_view data, size_t offset,
                        InternTable& intern);

}  // namespace ocaml_boot

#endif  // OCAML_BOOT_PRIMS_H_
