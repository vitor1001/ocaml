// Copyright 2026 Google LLC.
// Bootstrap interpreter driver: runs .ml files,
// bootstrap ocamllex, make_opcodes, cvt_emit, and ocamlc (including fork()
// zygote batch mode).

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "third_party/absl/flags/flag.h"
#include "third_party/absl/flags/parse.h"
#include "third_party/absl/log/check.h"
#include "third_party/absl/log/log.h"
#include "third_party/absl/status/status.h"
#include "third_party/absl/status/statusor.h"
#include "third_party/absl/strings/ascii.h"
#include "third_party/absl/strings/match.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_replace.h"
#include "third_party/absl/strings/str_split.h"
#include "third_party/absl/strings/string_view.h"
#include "ast.h"
#include "eval.h"
#include "intern.h"
#include "prims.h"
#include "ts_lower.h"
#include "value.h"

ABSL_FLAG(std::string, ocaml_dir, ".",
          "Path to the OCaml source root directory.");
ABSL_FLAG(std::string, mode, "run",
          "Execution mode: run, ocamllex, make_opcodes, cvt_emit, or ocamlc.");
ABSL_FLAG(std::string, eval, "",
          "Inline OCaml source code to evaluate after loading modules.");
ABSL_FLAG(bool, nostdlib, false,
          "If true, do not automatically load the OCaml standard library.");
ABSL_FLAG(std::string, batch_cmds_file, "",
          "Optional file of newline-separated argument lists to execute using "
          "fork() zygote mode after initializing the compiler.");

namespace ocaml_boot {
namespace {

constexpr absl::string_view kStdlibTopoBasenames[] = {
    "either",         "sys",         "int32",            "obj",
    "type",           "atomic",      "camlinternalLazy", "lazy",
    "seq",            "option",      "pair",             "result",
    "bool",           "char",        "uchar",            "list",
    "int",            "bytes",       "string",           "unit",
    "marshal",        "array",       "iarray",           "float",
    "int64",          "nativeint",   "lexing",           "parsing",
    "repr",           "set",         "map",              "stack",
    "queue",          "buffer",      "mutex",            "condition",
    "semaphore",      "domain",      "camlinternalFormat",
    "printf",         "arg",         "printexc",         "fun",
    "gc",             "complex",     "bigarray",         "in_channel",
    "out_channel",    "digest",      "random",           "hashtbl",
    "weak",           "format",      "scanf",            "callback",
    "camlinternalOO", "oo",          "camlinternalMod",  "dynarray",
    "pqueue",         "ephemeron",   "filename",         "arrayLabels",
    "listLabels",     "bytesLabels", "stringLabels",     "moreLabels",
    "stdLabels",      "effect",
};

constexpr absl::string_view kOcamlcommonMlSources[] = {
    "utils/config.ml",
    "utils/build_path_prefix_map.ml",
    "utils/format_doc.ml",
    "utils/utf8_lexeme.ml",
    "utils/misc.ml",
    "utils/identifiable.ml",
    "utils/numbers.ml",
    "utils/arg_helper.ml",
    "utils/local_store.ml",
    "utils/load_path.ml",
    "utils/profile.ml",
    "utils/clflags.ml",
    "utils/terminfo.ml",
    "utils/ccomp.ml",
    "utils/warnings.ml",
    "utils/consistbl.ml",
    "utils/linkdeps.ml",
    "utils/strongly_connected_components.ml",
    "utils/targetint.ml",
    "utils/int_replace_polymorphic_compare.ml",
    "utils/domainstate.ml",
    "utils/binutils.ml",
    "utils/lazy_backtrack.ml",
    "utils/diffing.ml",
    "utils/diffing_with_keys.ml",
    "utils/stable_matching.ml",
    "utils/compression.ml",
    "parsing/location.ml",
    "parsing/unit_info.ml",
    "parsing/asttypes.ml",
    "parsing/longident.ml",
    "parsing/docstrings.ml",
    "parsing/syntaxerr.ml",
    "parsing/ast_helper.ml",
    "parsing/ast_iterator.ml",
    "parsing/builtin_attributes.ml",
    "parsing/camlinternalMenhirLib.ml",
    "parsing/parser.ml",
    "parsing/lexer.ml",
    "parsing/pprintast.ml",
    "parsing/parse.ml",
    "parsing/printast.ml",
    "parsing/ast_mapper.ml",
    "parsing/attr_helper.ml",
    "parsing/ast_invariants.ml",
    "parsing/depend.ml",
    "typing/ident.ml",
    "typing/path.ml",
    "typing/type_immediacy.ml",
    "typing/primitive.ml",
    "typing/shape.ml",
    "typing/types.ml",
    "typing/data_types.ml",
    "typing/rawprinttyp.ml",
    "typing/gprinttyp.ml",
    "typing/btype.ml",
    "typing/oprint.ml",
    "typing/subst.ml",
    "typing/predef.ml",
    "typing/datarepr.ml",
    "typing/typing_recovery.ml",
    "file_formats/cmi_format.ml",
    "typing/persistent_env.ml",
    "typing/env.ml",
    "typing/errortrace.ml",
    "typing/typedtree.ml",
    "typing/signature_group.ml",
    "typing/printtyped.ml",
    "typing/ctype.ml",
    "typing/out_type.ml",
    "typing/printtyp.ml",
    "typing/errortrace_report.ml",
    "typing/includeclass.ml",
    "typing/mtype.ml",
    "typing/envaux.ml",
    "typing/includecore.ml",
    "typing/tast_iterator.ml",
    "typing/tast_mapper.ml",
    "typing/stypes.ml",
    "typing/shape_reduce.ml",
    "file_formats/cmt_format.ml",
    "typing/cmt2annot.ml",
    "typing/typing_recovery_state.ml",
    "typing/untypeast.ml",
    "typing/includemod.ml",
    "typing/signature_matching.ml",
    "typing/includemod_errorprinter.ml",
    "typing/typetexp.ml",
    "typing/printpat.ml",
    "typing/patterns.ml",
    "typing/parmatch.ml",
    "typing/typedecl_properties.ml",
    "typing/typedecl_variance.ml",
    "typing/typedecl_unboxed.ml",
    "typing/typedecl_immediacy.ml",
    "typing/typedecl_separability.ml",
    "lambda/debuginfo.ml",
    "lambda/lambda.ml",
    "typing/typeopt.ml",
    "typing/typedecl.ml",
    "typing/value_rec_check.ml",
    "typing/typecore.ml",
    "typing/typeclass.ml",
    "typing/typemod.ml",
    "lambda/printlambda.ml",
    "lambda/switch.ml",
    "lambda/matching.ml",
    "lambda/value_rec_compiler.ml",
    "lambda/translobj.ml",
    "lambda/translattribute.ml",
    "lambda/translprim.ml",
    "lambda/translcore.ml",
    "lambda/translclass.ml",
    "lambda/translmod.ml",
    "lambda/tmc.ml",
    "lambda/simplif.ml",
    "lambda/runtimedef.ml",
    "bytecomp/meta.ml",
    "bytecomp/opcodes.ml",
    "bytecomp/bytesections.ml",
    "bytecomp/dll.ml",
    "bytecomp/symtable.ml",
    "driver/pparse.ml",
    "driver/compenv.ml",
    "driver/main_args.ml",
    "driver/compmisc.ml",
    "driver/makedepend.ml",
    "driver/compile_common.ml",
};

constexpr absl::string_view kOcamlbytecompMlSources[] = {
    "bytecomp/byterntm.ml",
    "bytecomp/opnames.ml",
    "bytecomp/instruct.ml",
    "bytecomp/bytegen.ml",
    "bytecomp/printinstr.ml",
    "bytecomp/emitcode.ml",
    "bytecomp/bytelink.ml",
    "bytecomp/bytelibrarian.ml",
    "bytecomp/bytepackager.ml",
    "driver/errors.ml",
    "driver/compile.ml",
    "driver/maindriver.ml",
};

bool FileExists(absl::string_view path) {
  std::ifstream ifs((std::string(path)));
  return ifs.good();
}

absl::StatusOr<std::string> ReadFileToString(absl::string_view path) {
  std::ifstream ifs{std::string(path), std::ios::binary};
  if (!ifs) {
    return absl::NotFoundError(absl::StrCat("Cannot open file: ", path));
  }
  std::ostringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

std::string CapitalizeModule(absl::string_view base) {
  if (base.empty()) {
    return "";
  }
  std::string res(base);
  res[0] = absl::ascii_toupper(res[0]);
  return res;
}

// Reads an OCaml source file from `ocaml_dir`.
absl::StatusOr<std::string> LoadSourceContent(absl::string_view ocaml_dir,
                                              absl::string_view rel_path) {
  return ReadFileToString(absl::StrCat(ocaml_dir, "/", rel_path));
}

// Loads and evaluates a single module into `eval`, keeping its AST alive in
// `ast_arena`.
absl::Status EvalModuleFromSource(
    Evaluator& eval, std::vector<std::unique_ptr<Structure>>& ast_arena,
    absl::string_view code, absl::string_view filename,
    absl::string_view module_name, bool open_stdlib = true,
    absl::string_view alias_name = "") {
  auto lowered = ParseAndLower(code, filename, eval.intern(), module_name);
  if (!lowered.ok()) {
    return lowered.status();
  }
  ast_arena.push_back(std::make_unique<Structure>(std::move(*lowered)));
  try {
    eval.LoadModule(module_name, *ast_arena.back(), open_stdlib, alias_name);
  } catch (const OcamlException& ex) {
    return absl::InternalError(
        absl::StrCat("Uncaught OCaml exception while loading ", module_name,
                     " (", filename, "): ",
                     DebugString(ex.value(), &eval.intern())));
  }
  return absl::OkStatus();
}

// Loads the OCaml 5.3 standard library into `eval`.
absl::Status LoadStdlib(Evaluator& eval,
                        std::vector<std::unique_ptr<Structure>>& ast_arena,
                        absl::string_view ocaml_dir) {
  auto fmt_basics =
      LoadSourceContent(ocaml_dir, "stdlib/camlinternalFormatBasics.ml");
  if (!fmt_basics.ok()) return fmt_basics.status();
  if (auto st = EvalModuleFromSource(
          eval, ast_arena, *fmt_basics, "camlinternalFormatBasics.ml",
          "CamlinternalFormatBasics", /*open_stdlib=*/false);
      !st.ok()) {
    return st;
  }

  auto stdlib_src = LoadSourceContent(ocaml_dir, "stdlib/stdlib.ml");
  if (!stdlib_src.ok()) return stdlib_src.status();
  // Strip the trailing (*MODULE_ALIASES*) block from stdlib.ml because those
  // submodule aliases are populated dynamically as each stdlib module loads.
  std::string stdlib_trimmed = *stdlib_src;
  size_t alias_pos = stdlib_trimmed.find("(*MODULE_ALIASES*)");
  if (alias_pos != std::string::npos) {
    stdlib_trimmed.resize(alias_pos);
  }
  auto stdlib_ast =
      ParseAndLower(stdlib_trimmed, "stdlib.ml", eval.intern(), "Stdlib");
  if (!stdlib_ast.ok()) return stdlib_ast.status();
  ast_arena.push_back(std::make_unique<Structure>(std::move(*stdlib_ast)));
  auto stdlib_env = eval.LoadModule("Stdlib", *ast_arena.back(),
                                    /*open_stdlib=*/false);
  eval.set_stdlib_env(stdlib_env);

  for (absl::string_view base : kStdlibTopoBasenames) {
    std::string rel = absl::StrCat("stdlib/", base, ".ml");
    if (base != "sys" && !FileExists(absl::StrCat(ocaml_dir, "/", rel))) {
      continue;
    }
    auto src = LoadSourceContent(ocaml_dir, rel);
    if (!src.ok()) return src.status();
    std::string cap = CapitalizeModule(base);
    if (absl::StartsWith(base, "camlinternal")) {
      if (auto st = EvalModuleFromSource(eval, ast_arena, *src,
                                         absl::StrCat(base, ".ml"), cap,
                                         /*open_stdlib=*/true);
          !st.ok()) {
        return st;
      }
    } else {
      std::string prefixed = absl::StrCat("Stdlib__", cap);
      if (auto st = EvalModuleFromSource(eval, ast_arena, *src,
                                         absl::StrCat(base, ".ml"), prefixed,
                                         /*open_stdlib=*/true, cap);
          !st.ok()) {
        return st;
      }
    }
  }
  return absl::OkStatus();
}

// Resets `Stdlib.Arg.current` to `0` before running a new `Arg.parse` pass.
void ResetArgCurrent(Evaluator& eval) {
  auto arg_mod =
      eval.root_env()->LookupModule(eval.intern().Intern("Stdlib__Arg"));
  if (arg_mod == nullptr) {
    arg_mod = eval.root_env()->LookupModule(eval.intern().Intern("Arg"));
  }
  if (arg_mod == nullptr) {
    return;
  }
  const Value* cur =
      arg_mod->LookupExportedValue(eval.intern().Intern("current"));
  if (cur == nullptr) {
    return;
  }
  if (cur->IsRecord()) {
    cur->AsRecord()->Set(eval.intern().Intern("contents"), Value::Int(0));
  } else if (cur->IsTuple() && !cur->AsTuple()->elements.empty()) {
    cur->AsTuple()->elements[0] = Value::Int(0);
  } else if (cur->IsRawBlock() && !cur->AsRawBlock()->fields.empty()) {
    cur->AsRawBlock()->fields[0] = Value::Int(0);
  }
}

// Invokes `Pervasives.do_at_exit ()` and flushes standard streams.
void RunAtExit(Evaluator& eval) {
  if (const Value* at_exit = eval.LookupNamedValue("Pervasives.do_at_exit")) {
    try {
      std::vector<Value> unit_arg = {Value::Unit()};
      eval.ApplyValues(*at_exit, unit_arg);
    } catch (...) {
    }
  }
  std::fflush(nullptr);
}

// Runs the bootstrap ocamllex inside the interpreter to compile `in_mll` to
// `out_ml`.
absl::Status RunBootstrapOcamllex(
    Evaluator& eval, std::vector<std::unique_ptr<Structure>>& ast_arena,
    absl::string_view ocaml_dir, const std::vector<std::string>& argv) {
  std::string in_mll;
  for (size_t i = 1; i < argv.size(); ++i) {
    if (argv[i] == "-o") {
      ++i;
      continue;
    }
    if (!absl::StartsWith(argv[i], "-")) {
      in_mll = argv[i];
    }
  }
  CHECK(!in_mll.empty()) << "RunBootstrapOcamllex requires an input .mll path.";
  auto mll_src = ReadFileToString(in_mll);
  if (!mll_src.ok()) return mll_src.status();
  auto lowered_parser_ml = ParseAndLowerOcamllex(*mll_src, in_mll);
  if (!lowered_parser_ml.ok()) return lowered_parser_ml.status();

  constexpr absl::string_view kLexMods[] = {
      "lex/cset.ml",           "lex/syntax.ml",  "lex/parser.ml",
      "lex/lexer.ml",          "lex/table.ml",   "lex/lexgen.ml",
      "lex/exhaustiveness.ml", "lex/compact.ml", "lex/common.ml",
      "lex/output.ml",         "lex/outputbis.ml",
  };
  for (absl::string_view rel : kLexMods) {
    std::string mod_name = ModuleNameFromFilename(rel);
    std::string code;
    if (rel == "lex/parser.ml") {
      code = *lowered_parser_ml;
    } else if (rel == "lex/lexer.ml") {
      code =
          "exception Lexical_error of string * string * int * int\n"
          "let main _ = ()\n";
    } else {
      auto src = LoadSourceContent(ocaml_dir, rel);
      if (!src.ok()) {
        continue;
      }
      code = std::move(*src);
    }
    if (auto st = EvalModuleFromSource(eval, ast_arena, code, rel, mod_name);
        !st.ok()) {
      return st;
    }
  }
  ResetArgCurrent(eval);
  eval.SetArgv(argv);
  auto main_src = LoadSourceContent(ocaml_dir, "lex/main.ml");
  if (!main_src.ok()) return main_src.status();
  try {
    if (auto st = EvalModuleFromSource(eval, ast_arena, *main_src,
                                       "lex/main.ml", "Main");
        !st.ok()) {
      return st;
    }
  } catch (const OcamlSysExit& ex) {
    if (ex.exit_code() != 0) {
      return absl::InternalError(
          absl::StrCat("ocamllex exited with code ", ex.exit_code()));
    }
  }
  ResetArgCurrent(eval);
  std::fflush(nullptr);
  return absl::OkStatus();
}

// Loads Ocamlcommon + Ocamlbytecomp into `eval`.
absl::Status LoadOcamlcLibraries(
    Evaluator& eval, std::vector<std::unique_ptr<Structure>>& ast_arena,
    absl::string_view ocaml_dir) {
  // Pre-register empty module environments for `.mli`-only type modules so
  // `open Parsetree`, `open Outcometree`, `open Cmo_format`, etc. succeed.
  constexpr absl::string_view kTypeOnlyModules[] = {
      "Parsetree", "Annot", "Value_rec_types", "Outcometree", "Cmo_format",
      "Instruct",
  };
  for (absl::string_view m : kTypeOnlyModules) {
    auto empty_env = std::make_shared<Env>(eval.root_env());
    eval.root_env()->DefineModule(eval.intern().Intern(m), empty_env);
  }

  auto filter_by_mli = [&](absl::string_view rel, absl::string_view mod_name) {
    auto mli_src = LoadSourceContent(ocaml_dir, absl::StrCat(rel, "i"));
    if (!mli_src.ok()) {
      return;
    }
    auto allowed = ParseInterfaceExportedValues(*mli_src, eval.intern());
    if (!allowed.has_value()) {
      return;
    }
    if (auto mod_env =
            eval.root_env()->LookupModule(eval.intern().Intern(mod_name))) {
      mod_env->FilterExportedValues(*allowed);
    }
  };

  if (!FileExists(absl::StrCat(ocaml_dir, "/parsing/lexer.ml"))) {
    std::string tmp_lexer = absl::StrCat(ocaml_dir, "/parsing/lexer.ml");
    std::vector<std::string> lex_argv = {
        "ocamllex", "-q", "-o", tmp_lexer,
        absl::StrCat(ocaml_dir, "/parsing/lexer.mll")};
    if (auto st = RunBootstrapOcamllex(eval, ast_arena, ocaml_dir, lex_argv);
        !st.ok()) {
      return st;
    }
  }
  if (FileExists(absl::StrCat(ocaml_dir, "/bytecomp/byterntm.mll")) &&
      !FileExists(absl::StrCat(ocaml_dir, "/bytecomp/byterntm.ml"))) {
    std::string tmp_byterntm = absl::StrCat(ocaml_dir, "/bytecomp/byterntm.ml");
    std::vector<std::string> lex_argv = {
        "ocamllex", "-q", "-o", tmp_byterntm,
        absl::StrCat(ocaml_dir, "/bytecomp/byterntm.mll")};
    if (auto st = RunBootstrapOcamllex(eval, ast_arena, ocaml_dir, lex_argv);
        !st.ok()) {
      return st;
    }
  }

  for (absl::string_view rel : kOcamlcommonMlSources) {
    auto src = LoadSourceContent(ocaml_dir, rel);
    if (!src.ok()) {
      continue;
    }
    std::string mod_name = ModuleNameFromFilename(rel);
    absl::string_view alias =
        (rel == "parsing/camlinternalMenhirLib.ml") ? "MenhirLib" : "";
    if (auto st =
            EvalModuleFromSource(eval, ast_arena, *src, rel, mod_name,
                                 /*open_stdlib=*/true, alias);
        !st.ok()) {
      return st;
    }
    filter_by_mli(rel, mod_name);
  }

  for (absl::string_view rel : kOcamlbytecompMlSources) {
    auto src = LoadSourceContent(ocaml_dir, rel);
    if (!src.ok()) {
      continue;
    }
    std::string mod_name = ModuleNameFromFilename(rel);
    if (auto st = EvalModuleFromSource(eval, ast_arena, *src, rel, mod_name);
        !st.ok()) {
      return st;
    }
    filter_by_mli(rel, mod_name);
  }
  return absl::OkStatus();
}

// Splits a batch command line into arguments while respecting single and double
// quotes.
std::vector<std::string> SplitCommandLine(absl::string_view line) {
  std::vector<std::string> args;
  std::string cur;
  bool in_token = false;
  char quote = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (quote != 0) {
      if (c == quote) {
        quote = 0;
      } else if (quote == '"' && c == '\\' && i + 1 < line.size()) {
        cur.push_back(line[++i]);
      } else {
        cur.push_back(c);
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
      in_token = true;
    } else if (absl::ascii_isspace(static_cast<unsigned char>(c))) {
      if (in_token) {
        args.push_back(std::move(cur));
        cur.clear();
        in_token = false;
      }
    } else {
      cur.push_back(c);
      in_token = true;
    }
  }
  if (in_token) {
    args.push_back(std::move(cur));
  }
  return args;
}

int RunSingleMainStructure(Evaluator& eval, const Structure& main_struct,
                           const std::vector<std::string>& argv) {
  ResetArgCurrent(eval);
  eval.SetArgv(argv);
  int exit_code = 0;
  try {
    eval.LoadModule("Main", main_struct, /*open_stdlib=*/true);
  } catch (const OcamlSysExit& ex) {
    exit_code = ex.exit_code();
  } catch (const OcamlException& ex) {
    std::cerr << "Uncaught OCaml exception: "
              << DebugString(ex.value(), &eval.intern()) << "\n";
    return 2;
  }
  RunAtExit(eval);
  return exit_code;
}

}  // namespace
}  // namespace ocaml_boot

int main(int argc, char** argv) {
  struct rlimit rl;
  if (::getrlimit(RLIMIT_STACK, &rl) == 0) {
    constexpr rlim_t kMinStack = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    if (rl.rlim_cur < kMinStack) {
      rl.rlim_cur = (rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= kMinStack)
                        ? kMinStack
                        : rl.rlim_max;
      ::setrlimit(RLIMIT_STACK, &rl);
    }
  }
  std::vector<char*> positional = absl::ParseCommandLine(argc, argv);
  std::string ocaml_dir = absl::GetFlag(FLAGS_ocaml_dir);
  std::string mode = absl::GetFlag(FLAGS_mode);
  std::string eval_code = absl::GetFlag(FLAGS_eval);
  bool nostdlib = absl::GetFlag(FLAGS_nostdlib);
  std::string batch_file = absl::GetFlag(FLAGS_batch_cmds_file);

  ocaml_boot::InternTable intern;
  ocaml_boot::Evaluator eval(intern);
  std::vector<std::unique_ptr<ocaml_boot::Structure>> ast_arena;

  std::vector<std::string> target_argv;
  for (size_t i = 0; i < positional.size(); ++i) {
    target_argv.emplace_back(positional[i]);
  }
  if (target_argv.empty()) {
    target_argv.emplace_back("ocaml_boot");
  }
  eval.SetArgv(target_argv);

  if (!nostdlib) {
    absl::Status st = ocaml_boot::LoadStdlib(eval, ast_arena, ocaml_dir);
    if (!st.ok()) {
      std::cerr << "Failed to load Stdlib: " << st << "\n";
      return 1;
    }
  }

  try {
    if (mode == "ocamllex") {
      absl::Status st = ocaml_boot::RunBootstrapOcamllex(
          eval, ast_arena, ocaml_dir, target_argv);
      if (!st.ok()) {
        std::cerr << "ocamllex failed: " << st << "\n";
        return 1;
      }
      return 0;
    }

    if (mode == "make_opcodes" || mode == "cvt_emit") {
      std::string tool_ml = absl::StrCat(ocaml_dir, "/tools/", mode, ".ml");
      if (!ocaml_boot::FileExists(tool_ml)) {
        std::string tmp_ml = absl::StrCat("/tmp/ocaml_boot_", mode, ".ml");
        std::vector<std::string> lex_argv = {
            "ocamllex", "-q", "-o", tmp_ml,
            absl::StrCat(ocaml_dir, "/tools/", mode, ".mll")};
        absl::Status st = ocaml_boot::RunBootstrapOcamllex(
            eval, ast_arena, ocaml_dir, lex_argv);
        if (!st.ok()) {
          std::cerr << "Failed to lex " << mode << ".mll: " << st << "\n";
          return 1;
        }
        tool_ml = tmp_ml;
      }
      auto src = ocaml_boot::ReadFileToString(tool_ml);
      if (!src.ok()) {
        std::cerr << src.status() << "\n";
        return 1;
      }
      auto lowered = ocaml_boot::ParseAndLower(
          *src, tool_ml, intern, ocaml_boot::ModuleNameFromFilename(tool_ml));
      if (!lowered.ok()) {
        std::cerr << lowered.status() << "\n";
        return 1;
      }
      ast_arena.push_back(
          std::make_unique<ocaml_boot::Structure>(std::move(*lowered)));
      return ocaml_boot::RunSingleMainStructure(eval, *ast_arena.back(),
                                                 target_argv);
    }

    if (mode == "ocamlc") {
      absl::Status st =
          ocaml_boot::LoadOcamlcLibraries(eval, ast_arena, ocaml_dir);
      if (!st.ok()) {
        std::cerr << "Failed to load ocamlc libraries: " << st << "\n";
        return 1;
      }
      auto main_src =
          ocaml_boot::LoadSourceContent(ocaml_dir, "driver/main.ml");
      if (!main_src.ok()) {
        std::cerr << main_src.status() << "\n";
        return 1;
      }
      auto main_ast = ocaml_boot::ParseAndLower(*main_src, "driver/main.ml",
                                                 intern, "Main");
      if (!main_ast.ok()) {
        std::cerr << main_ast.status() << "\n";
        return 1;
      }
      ast_arena.push_back(
          std::make_unique<ocaml_boot::Structure>(std::move(*main_ast)));

      if (!batch_file.empty()) {
        auto batch_content = ocaml_boot::ReadFileToString(batch_file);
        if (!batch_content.ok()) {
          std::cerr << batch_content.status() << "\n";
          return 1;
        }
        const size_t max_bg_jobs = std::max<size_t>(
            32, std::thread::hardware_concurrency());
        absl::flat_hash_map<pid_t, std::string> bg_pids;
        auto reap_one = [&]() -> int {
          int status = 0;
          pid_t done = ::waitpid(-1, &status, 0);
          if (done < 0) {
            std::perror("waitpid");
            return 1;
          }
          std::string cmd_str = bg_pids[done];
          bg_pids.erase(done);
          if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            std::cerr << "Batch command failed: " << cmd_str << "\n";
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
          }
          return 0;
        };
        auto reap_all = [&]() -> int {
          int fail_rc = 0;
          while (!bg_pids.empty()) {
            int rc = reap_one();
            if (rc != 0 && fail_rc == 0) {
              fail_rc = rc;
            }
          }
          return fail_rc;
        };
        for (absl::string_view line :
             absl::StrSplit(*batch_content, '\n', absl::SkipWhitespace())) {
          line = absl::StripAsciiWhitespace(line);
          if (line.empty() || absl::StartsWith(line, "#")) {
            continue;
          }
          if (line == "WAIT") {
            if (int rc = reap_all(); rc != 0) {
              return rc;
            }
            continue;
          }
          if (absl::StartsWith(line, "CD ")) {
            if (int rc = reap_all(); rc != 0) {
              return rc;
            }
            std::string dir(
                absl::StripAsciiWhitespace(line.substr(3)));
            if (::chdir(dir.c_str()) != 0) {
              std::perror("chdir");
              return 1;
            }
            continue;
          }
          bool is_bg = false;
          if (absl::StartsWith(line, "& ")) {
            is_bg = true;
            line = absl::StripAsciiWhitespace(line.substr(2));
          } else {
            if (int rc = reap_all(); rc != 0) {
              return rc;
            }
          }
          if (is_bg && bg_pids.size() >= max_bg_jobs) {
            if (int rc = reap_one(); rc != 0) {
              return rc;
            }
          }
          std::vector<std::string> cmd_argv =
              ocaml_boot::SplitCommandLine(line);
          std::fflush(nullptr);
          pid_t pid = ::fork();
          if (pid < 0) {
            std::perror("fork");
            return 1;
          }
          if (pid == 0) {
            int rc = ocaml_boot::RunSingleMainStructure(
                eval, *ast_arena.back(), cmd_argv);
            std::fflush(nullptr);
            ::_exit(rc);
          }
          if (is_bg) {
            bg_pids.emplace(pid, std::string(line));
          } else {
            int status = 0;
            if (::waitpid(pid, &status, 0) < 0) {
              std::perror("waitpid");
              return 1;
            }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
              std::cerr << "Batch command failed: " << line << "\n";
              return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            }
          }
        }
        if (int rc = reap_all(); rc != 0) {
          return rc;
        }
        return 0;
      }

      return ocaml_boot::RunSingleMainStructure(eval, *ast_arena.back(),
                                                 target_argv);
    }

    // Default "run" mode: evaluate positional .ml files and optional --eval.
    for (size_t i = 1; i < positional.size(); ++i) {
      std::string path = positional[i];
      auto src = ocaml_boot::ReadFileToString(path);
      if (!src.ok()) {
        std::cerr << src.status() << "\n";
        return 1;
      }
      std::string mod_name = ocaml_boot::ModuleNameFromFilename(path);
      absl::Status st = ocaml_boot::EvalModuleFromSource(
          eval, ast_arena, *src, path, mod_name, !nostdlib);
      if (!st.ok()) {
        std::cerr << st << "\n";
        return 1;
      }
    }

    if (!eval_code.empty()) {
      absl::Status st = ocaml_boot::EvalModuleFromSource(
          eval, ast_arena, eval_code, "<eval>", "Eval", !nostdlib);
      if (!st.ok()) {
        std::cerr << st << "\n";
        return 1;
      }
    }
    ocaml_boot::RunAtExit(eval);
  } catch (const ocaml_boot::OcamlSysExit& ex) {
    ocaml_boot::RunAtExit(eval);
    return ex.exit_code();
  } catch (const ocaml_boot::OcamlException& ex) {
    std::cerr << "Uncaught OCaml exception: "
              << ocaml_boot::DebugString(ex.value(), &intern) << "\n";
    return 2;
  }

  return 0;
}
