#pragma once
#include <cstring>
#include <string_view>
#include <vector>
#include "third_party/absl/flags/flag.h"

namespace absl {
inline std::vector<char*> ParseCommandLine(int argc, char** argv) {
  std::vector<char*> positional;
  if (argc > 0) positional.push_back(argv[0]);
  bool after_double_dash = false;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (after_double_dash) {
      positional.push_back(argv[i]);
      continue;
    }
    if (arg == "--") {
      after_double_dash = true;
      continue;
    }
    if (arg.size() > 2 && arg.substr(0, 2) == "--") {
      std::string_view rest = arg.substr(2);
      size_t eq = rest.find('=');
      std::string_view fname = (eq == std::string_view::npos) ? rest : rest.substr(0, eq);
      bool matched = false;
      for (const auto& entry : GetFlagRegistry()) {
        if (entry.name == fname) {
          std::string_view fval;
          if (eq != std::string_view::npos) {
            fval = rest.substr(eq + 1);
          } else if (i + 1 < argc && argv[i + 1][0] != '-') {
            fval = argv[++i];
          }
          entry.parse_fn(entry.flag_ptr, fval);
          matched = true;
          break;
        }
      }
      if (matched) continue;
    }
    positional.push_back(argv[i]);
  }
  return positional;
}
}  // namespace absl
