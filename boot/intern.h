// Copyright 2026 Google LLC.
// Intern table mapping strings to compact 32-bit symbol identifiers.

#ifndef OCAML_BOOT_INTERN_H_
#define OCAML_BOOT_INTERN_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include "third_party/absl/container/flat_hash_map.h"
#include "third_party/absl/log/check.h"
#include "third_party/absl/strings/string_view.h"

namespace ocaml_boot {

// Compact identifier for interned identifiers, constructors, and labels.
using SymbolId = uint32_t;

// Sentinel symbol ID representing an empty or unset symbol.
inline constexpr SymbolId kInvalidSymbol = 0;

// Interns strings into monotonically increasing SymbolId values.
class InternTable {
 public:
  InternTable() {
    // Reserve index 0 for the empty string / kInvalidSymbol.
    strings_.emplace_back("");
    map_.emplace(strings_.back(), kInvalidSymbol);
  }

  // Interns the string view and returns its unique SymbolId.
  SymbolId Intern(absl::string_view s) {
    auto it = map_.find(s);
    if (it != map_.end()) {
      return it->second;
    }
    SymbolId id = static_cast<SymbolId>(strings_.size());
    strings_.emplace_back(s);
    map_.emplace(strings_.back(), id);
    return id;
  }

  // Looks up a string without inserting it if absent.
  std::optional<SymbolId> Find(absl::string_view s) const {
    auto it = map_.find(s);
    if (it == map_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  // Resolves a SymbolId back to its interned string view.
  absl::string_view Resolve(SymbolId id) const {
    DCHECK_LT(id, strings_.size());
    return strings_[id];
  }

  // Returns the total number of interned symbols including kInvalidSymbol.
  size_t Size() const { return strings_.size(); }

 private:
  // Uses std::deque so string storage addresses remain stable on growth.
  std::deque<std::string> strings_;
  absl::flat_hash_map<absl::string_view, SymbolId> map_;
};

}  // namespace ocaml_boot

#endif  // OCAML_BOOT_INTERN_H_
