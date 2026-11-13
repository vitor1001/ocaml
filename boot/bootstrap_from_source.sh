#!/bin/sh
# Bootstraps boot/ocamlc, boot/ocamllex, and boot/stdlib.cma from source using
# the standalone C++17 Tree-sitter interpreter in boot/ocaml_boot.
set -eu

ROOTDIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$ROOTDIR"

CXX="${CXX:-g++}"
CC="${CC:-gcc}"

if [ ! -x boot/ocaml_boot ]; then
  echo "  CXX boot/ocaml_boot"
  mkdir -p boot/.objs
  for src in value ts_lower eval prims marshal main; do
    "$CXX" -std=c++17 -O2 -fexceptions \
      -Iboot/shim -Iboot -Iboot/tree_sitter/lib/include \
      -c "boot/${src}.cc" -o "boot/.objs/${src}.o" &
  done
  "$CC" -O2 -Iboot/tree_sitter/lib/include -Iboot/tree_sitter/lib/src \
    -c boot/tree_sitter/lib/src/lib.c -o boot/.objs/ts_lib.o &
  "$CC" -O2 -Iboot/treesitter_ocaml/common -Iboot/treesitter_ocaml/grammars/ocaml/src \
    -c boot/treesitter_ocaml/grammars/ocaml/src/parser.c -o boot/.objs/ts_ocaml_parser.o &
  "$CC" -O2 -Iboot/treesitter_ocaml/common -Iboot/treesitter_ocaml/grammars/ocaml/src \
    -c boot/treesitter_ocaml/grammars/ocaml/src/scanner.c -o boot/.objs/ts_ocaml_scanner.o &
  "$CC" -O2 -Iboot/treesitter_ocaml/common -Iboot/treesitter_ocaml/grammars/interface/src \
    -c boot/treesitter_ocaml/grammars/interface/src/parser.c -o boot/.objs/ts_intf_parser.o &
  "$CC" -O2 -Iboot/treesitter_ocaml/common -Iboot/treesitter_ocaml/grammars/interface/src \
    -c boot/treesitter_ocaml/grammars/interface/src/scanner.c -o boot/.objs/ts_intf_scanner.o &
  "$CC" -O2 -Iboot/treesitter_ocamllex/src \
    -c boot/treesitter_ocamllex/src/parser.c -o boot/.objs/ts_lex_parser.o &
  "$CC" -O2 -Iboot/treesitter_ocamllex/src \
    -c boot/treesitter_ocamllex/src/scanner.c -o boot/.objs/ts_lex_scanner.o &
  "$CC" -O2 -ffunction-sections -fdata-sections -Iruntime \
    -c runtime/md5.c -o boot/.objs/md5.o &
  "$CC" -O2 -ffunction-sections -fdata-sections -Iruntime \
    -c runtime/blake2.c -o boot/.objs/blake2.o &
  wait
  "$CXX" -std=c++17 -O2 -fexceptions -Wl,--gc-sections \
    boot/.objs/value.o \
    boot/.objs/ts_lower.o \
    boot/.objs/eval.o \
    boot/.objs/prims.o \
    boot/.objs/marshal.o \
    boot/.objs/main.o \
    boot/.objs/ts_lib.o \
    boot/.objs/ts_ocaml_parser.o \
    boot/.objs/ts_ocaml_scanner.o \
    boot/.objs/ts_intf_parser.o \
    boot/.objs/ts_intf_scanner.o \
    boot/.objs/ts_lex_parser.o \
    boot/.objs/ts_lex_scanner.o \
    boot/.objs/md5.o \
    boot/.objs/blake2.o \
    -o boot/ocaml_boot
  rm -rf boot/.objs
fi

# Ensure build-system-generated sources and ocamlyacc lex/parser.ml are staged.
${MAKE:-make} stdlib/sys.ml utils/config.ml utils/domainstate.ml utils/domainstate.mli \
  lambda/runtimedef.ml bytecomp/opcodes.ml bytecomp/opnames.ml \
  parsing/camlinternalMenhirLib.ml parsing/camlinternalMenhirLib.mli \
  parsing/parser.ml parsing/parser.mli
./yacc/ocamlyacc -v lex/parser.mly
rm -f lex/parser.output

# Prepare stdlib module aliases in-place and restore originals after bootstrap.
for f in stdlib.mli stdlib.ml arrayLabels.mli listLabels.mli bytesLabels.mli stringLabels.mli moreLabels.mli stdLabels.mli; do
  if [ -f "stdlib/$f" ]; then
    if [ ! -f "stdlib/$f.orig" ]; then
      cp -f "stdlib/$f" "stdlib/$f.orig"
    fi
    (cd stdlib && awk -f ./expand_module_aliases.awk "$f.orig" > "$f")
  fi
done

printf '\000\n' > stdlib/runtime-launch-info
./runtime/ocamlrun -p > runtime/primitives
mkdir -p compilerlibs
BATCH_FILE="$(mktemp)"
trap 'rm -f "$BATCH_FILE"' EXIT

python3 - "$ROOTDIR" "$BATCH_FILE" << 'PYEOF'
import os, sys

root = sys.argv[1]
batch_path = sys.argv[2]
lines = []

def compute_waves(mods, depend_path, name_map=None):
    mod_set = set(mods)
    deps = {m: set() for m in mods}
    with open(depend_path) as f:
        content = f.read().replace("\\\n", " ")
    rev_map = {v: k for k, v in (name_map or {}).items()}
    for line in content.splitlines():
        if ":" not in line:
            continue
        lhs, rhs = line.split(":", 1)
        for t in lhs.split():
            if t.endswith(".cmi"):
                t_mod = rev_map.get(t[:-4], t[:-4])
                if t_mod in mod_set:
                    for d in rhs.split():
                        if d.endswith(".cmi"):
                            d_mod = rev_map.get(d[:-4], d[:-4])
                            if d_mod in mod_set and d_mod != t_mod:
                                deps[t_mod].add(d_mod)
    waves = []
    done = set()
    rem = list(mods)
    while rem:
        wave = [m for m in rem if deps[m].issubset(done)]
        assert wave, f"Cycle in {rem}"
        waves.append(wave)
        done.update(wave)
        rem = [m for m in rem if m not in done]
    return waves

# 1. Stdlib compilation in parallel waves.
lines.append(f"CD {root}/stdlib")
sflags = "ocamlc -nostdlib -I . -w -a"
if not os.path.exists(f"{root}/stdlib/camlinternalFormatBasics.cmi"):
    lines.append(f"{sflags} -nopervasives -c camlinternalFormatBasics.mli")
if not os.path.exists(f"{root}/stdlib/camlinternalFormatBasics.cmo"):
    lines.append(f"{sflags} -nopervasives -c camlinternalFormatBasics.ml")
if not os.path.exists(f"{root}/stdlib/stdlib.cmi"):
    lines.append(f"{sflags} -nopervasives -no-alias-deps -w -49 -c stdlib.mli")
if not os.path.exists(f"{root}/stdlib/stdlib.cmo"):
    lines.append(f"{sflags} -nopervasives -no-alias-deps -w -49 -c stdlib.ml")

stdlib_mods = []
with open(f"{root}/stdlib/StdlibModules") as f:
    in_list = False
    for line in f:
        s = line.strip().rstrip("\\").strip()
        if s.startswith("STDLIB_MODULE_BASENAMES"):
            in_list = True
            continue
        if in_list:
            if not s or s.startswith("STDLIB_"):
                break
            for tok in s.split():
                if tok not in ("camlinternalFormatBasics", "stdlib"):
                    stdlib_mods.append(tok)

def cap(m):
    return m[0].upper() + m[1:]

def stdlib_out_base(m):
    return m if m.startswith("camlinternal") else "stdlib__" + cap(m)

stdlib_name_map = {m: stdlib_out_base(m) for m in stdlib_mods}
for wave in compute_waves(stdlib_mods, f"{root}/stdlib/.depend", stdlib_name_map):
    need_wait = False
    for m in wave:
        out_base = stdlib_out_base(m)
        if not os.path.exists(f"{root}/stdlib/{out_base}.cmi"):
            extra = "" if m.startswith("camlinternal") else f"-open Stdlib -o {out_base}"
            lines.append(f"& {sflags} {extra} -c {m}.mli")
            need_wait = True
    if need_wait:
        lines.append("WAIT")

std_cmos = ["camlinternalFormatBasics.cmo", "stdlib.cmo"]
need_wait = False
for m in stdlib_mods:
    out_base = stdlib_out_base(m)
    if not os.path.exists(f"{root}/stdlib/{out_base}.cmo"):
        extra = "" if m.startswith("camlinternal") else f"-open Stdlib -o {out_base}"
        ml_extra = extra + (" -nolabels -no-alias-deps" if (m.endswith("Labels") or m == "float") else "")
        lines.append(f"& {sflags} {ml_extra} -c {m}.ml")
        need_wait = True
    std_cmos.append(f"{out_base}.cmo")
if need_wait:
    lines.append("WAIT")

if not os.path.exists(f"{root}/stdlib/std_exit.cmi"):
    lines.append(f"{sflags} -c std_exit.mli")
if not os.path.exists(f"{root}/stdlib/std_exit.cmo"):
    lines.append(f"{sflags} -c std_exit.ml")
if not os.path.exists(f"{root}/stdlib/stdlib.cma"):
    lines.append(f"{sflags} -a -o stdlib.cma {' '.join(std_cmos)}")
lines.append(f"CD {root}")

# 2. Compiler .mli -> .cmi and .ml -> .cmo in parallel waves.
incs = "ocamlc -nostdlib -I stdlib -I utils -I parsing -I typing -I bytecomp -I file_formats -I lambda -I driver -I lex -w -a"

utils_mods = [
    "utils/config", "utils/build_path_prefix_map", "utils/format_doc",
    "utils/utf8_lexeme", "utils/misc", "utils/identifiable", "utils/numbers",
    "utils/arg_helper", "utils/local_store", "utils/load_path", "utils/clflags",
    "utils/profile", "utils/terminfo", "utils/ccomp", "utils/warnings",
    "utils/consistbl", "utils/linkdeps", "utils/strongly_connected_components",
    "utils/targetint", "utils/int_replace_polymorphic_compare",
    "utils/domainstate", "utils/binutils", "utils/lazy_backtrack",
    "utils/diffing", "utils/diffing_with_keys", "utils/stable_matching",
    "utils/compression",
]
parsing_mods = [
    "parsing/location", "parsing/unit_info", "parsing/asttypes",
    "parsing/longident", "parsing/parsetree", "parsing/docstrings",
    "parsing/syntaxerr", "parsing/ast_helper", "parsing/ast_iterator",
    "parsing/builtin_attributes", "parsing/camlinternalMenhirLib",
    "parsing/parser", "parsing/lexer", "parsing/pprintast", "parsing/parse",
    "parsing/printast", "parsing/ast_mapper", "parsing/attr_helper",
    "parsing/ast_invariants", "parsing/depend",
]
typing_mods = [
    "typing/annot", "typing/value_rec_types", "typing/ident", "typing/path",
    "typing/primitive", "typing/type_immediacy", "typing/outcometree",
    "typing/shape", "typing/types", "typing/data_types", "typing/rawprinttyp",
    "typing/gprinttyp", "typing/btype", "typing/oprint", "typing/subst",
    "typing/predef", "typing/datarepr", "typing/typing_recovery",
    "file_formats/cmi_format", "typing/persistent_env", "typing/env",
    "typing/errortrace", "typing/typedtree", "typing/signature_group",
    "typing/printtyped", "typing/ctype", "typing/out_type", "typing/printtyp",
    "typing/errortrace_report", "typing/includeclass", "typing/mtype",
    "typing/envaux", "typing/includecore", "typing/tast_iterator",
    "typing/tast_mapper", "typing/stypes", "typing/shape_reduce",
    "file_formats/cmt_format", "typing/cmt2annot",
    "typing/typing_recovery_state", "typing/untypeast", "typing/includemod",
    "typing/signature_matching", "typing/includemod_errorprinter",
    "typing/typetexp", "typing/printpat", "typing/patterns", "typing/parmatch",
    "typing/typedecl_properties", "typing/typedecl_variance",
    "typing/typedecl_unboxed", "typing/typedecl_immediacy",
    "typing/typedecl_separability", "typing/typeopt", "typing/typedecl",
    "typing/value_rec_check", "typing/typecore", "typing/typeclass",
    "typing/typemod",
]
lambda_mods = [
    "lambda/debuginfo", "lambda/lambda", "lambda/printlambda", "lambda/switch",
    "lambda/matching", "lambda/value_rec_compiler", "lambda/translobj",
    "lambda/translattribute", "lambda/translprim", "lambda/translcore",
    "lambda/translclass", "lambda/translmod", "lambda/tmc", "lambda/simplif",
    "lambda/runtimedef",
]
comp_mods = [
    "file_formats/cmo_format", "bytecomp/meta", "bytecomp/opcodes",
    "bytecomp/bytesections", "bytecomp/dll", "bytecomp/symtable",
    "driver/pparse", "driver/compenv", "driver/main_args", "driver/compmisc",
    "driver/makedepend", "driver/compile_common",
]
bytecomp_mods = [
    "bytecomp/byterntm", "bytecomp/opnames", "bytecomp/instruct",
    "bytecomp/bytegen", "bytecomp/printinstr", "bytecomp/emitcode",
    "bytecomp/bytelink", "bytecomp/bytelibrarian", "bytecomp/bytepackager",
    "driver/errors", "driver/compile", "driver/maindriver", "driver/main",
]
lex_mods = [
    "lex/cset", "lex/syntax", "lex/parser", "lex/lexer", "lex/table",
    "lex/lexgen", "lex/exhaustiveness", "lex/compact", "lex/common",
    "lex/output", "lex/outputbis", "lex/main",
]

all_compiler_mods = (
    utils_mods + parsing_mods + typing_mods + lambda_mods + comp_mods + bytecomp_mods
)

lex_incs = "ocamlc -nostdlib -I stdlib -I lex -w -a"
for wave in compute_waves(all_compiler_mods, f"{root}/.depend"):
    need_wait = False
    for m in wave:
        mli = f"{m}.mli"
        ml = f"{m}.ml"
        cmi = f"{m}.cmi"
        if not os.path.exists(f"{root}/{cmi}"):
            if os.path.exists(f"{root}/{mli}") or m in ("parsing/camlinternalMenhirLib", "parsing/parser"):
                lines.append(f"& {incs} -c {mli}")
                need_wait = True
            elif os.path.exists(f"{root}/{ml}"):
                lines.append(f"& {incs} -c {ml}")
                need_wait = True
    if need_wait:
        lines.append("WAIT")

for m in lex_mods:
    mli = f"{m}.mli"
    cmi = f"{m}.cmi"
    if not os.path.exists(f"{root}/{cmi}") and (os.path.exists(f"{root}/{mli}") or m == "lex/parser"):
        lines.append(f"{lex_incs} -c {mli}")

# Now all .cmi files exist, so ALL .ml -> .cmo compilations are 100% independent!
common_cmos = []
for m in utils_mods + parsing_mods + typing_mods + lambda_mods + comp_mods:
    ml = f"{m}.ml"
    cmo = f"{m}.cmo"
    if os.path.exists(f"{root}/{ml}") or m in (
        "parsing/camlinternalMenhirLib", "parsing/parser", "parsing/lexer",
    ):
        if not os.path.exists(f"{root}/{cmo}"):
            lines.append(f"& {incs} -c {ml}")
        common_cmos.append(cmo)

bytecomp_cmos = []
for m in bytecomp_mods:
    ml = f"{m}.ml"
    cmo = f"{m}.cmo"
    if os.path.exists(f"{root}/{ml}") or m == "bytecomp/byterntm":
        if not os.path.exists(f"{root}/{cmo}"):
            lines.append(f"& {incs} -c {ml}")
        if m != "driver/main":
            bytecomp_cmos.append(cmo)

lex_cmos = []
for m in lex_mods:
    ml = f"{m}.ml"
    cmo = f"{m}.cmo"
    if os.path.exists(f"{root}/{ml}") or m in ("lex/parser", "lex/lexer"):
        if not os.path.exists(f"{root}/{cmo}"):
            lines.append(f"& {lex_incs} -c {ml}")
        lex_cmos.append(cmo)

lines.append("WAIT")
lines.append(f"{incs} -linkall -a -o compilerlibs/ocamlcommon.cma {' '.join(common_cmos)}")
lines.append(f"{incs} -a -o compilerlibs/ocamlbytecomp.cma {' '.join(bytecomp_cmos)}")
lines.append(f"CD {root}/stdlib")
lines.append(f"ocamlc -nostdlib -I . -use-prims ../runtime/primitives -compat-32 -g -linkall -o ../boot/ocamlc ../compilerlibs/ocamlcommon.cma ../compilerlibs/ocamlbytecomp.cma ../driver/main.cmo")
lines.append(f"ocamlc -nostdlib -I . -use-prims ../runtime/primitives -compat-32 -o ../boot/ocamllex {' '.join('../' + c for c in lex_cmos)}")
lines.append(f"CD {root}")

with open(batch_path, "w") as f:
    f.write("\n".join(lines) + "\n")
PYEOF

echo "  BOOT boot/ocamlc boot/ocamllex"
if [ ! -f lex/lexer.ml ]; then
  ./boot/ocaml_boot --ocaml_dir="$ROOTDIR" --mode=ocamllex -- -q -o lex/lexer.ml lex/lexer.mll
fi
./boot/ocaml_boot --ocaml_dir="$ROOTDIR" --mode=ocamlc --batch_cmds_file="$BATCH_FILE"

for f in stdlib.mli stdlib.ml arrayLabels.mli listLabels.mli bytesLabels.mli stringLabels.mli moreLabels.mli stdLabels.mli; do
  if [ -f "stdlib/$f.orig" ]; then
    mv -f "stdlib/$f.orig" "stdlib/$f"
  fi
done
rm -f stdlib/runtime-launch-info stdlib/*.cmi stdlib/*.cmo stdlib/*.cma \
      utils/*.cmi utils/*.cmo parsing/*.cmi parsing/*.cmo \
      typing/*.cmi typing/*.cmo file_formats/*.cmi file_formats/*.cmo \
      lambda/*.cmi lambda/*.cmo bytecomp/*.cmi bytecomp/*.cmo \
      driver/*.cmi driver/*.cmo lex/*.cmi lex/*.cmo \
      compilerlibs/*.cma
chmod +x boot/ocamlc boot/ocamllex
