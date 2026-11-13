# tree-sitter-ocamllex

[![CI][ci]](https://github.com/314eter/tree-sitter-ocamllex/actions/workflows/ci.yml)
[![npm][npm]](https://www.npmjs.com/package/tree-sitter-ocamllex)
[![crates][crates]](https://crates.io/crates/tree-sitter-ocamllex)
[![pypi][pypi]](https://pypi.org/project/tree-sitter-ocamllex/)

OCamllex grammar for [tree-sitter][].

## Upstream Provenance (OCaml Bootstrap Vendoring)

- **Repository**: <https://github.com/314eter/tree-sitter-ocamllex>
- **Version**: `v0.25.0`
- **Commit**: `64d543e98b4092e2efe5d0c93d78ff4db9cbff39`
- **License**: MIT (see `LICENSE`)
- **Modifications**:
  - None to the upstream grammar or scanner sources (`grammar.js`, `src/scanner.c`).
  - Only the files needed to build the C parser are retained, plus the pre-generated C parser files (`src/parser.c` and `src/tree_sitter/{alloc,array,parser}.h`, generated from `grammar.js` via `tree-sitter generate --abi 15`) so that OCaml can be bootstrapped from source with a standard C/C++ compiler without requiring Node.js or the Rust `tree-sitter` CLI.

### References

- [OCamllex Manual](https://ocaml.org/manual/lexyacc.html)
- [OCamllex parser](https://github.com/ocaml/ocaml/blob/trunk/lex/parser.mly)

[ci]: https://img.shields.io/github/actions/workflow/status/314eter/tree-sitter-ocamllex/ci.yml?logo=github&label=CI
[npm]: https://img.shields.io/npm/v/tree-sitter-ocamllex?logo=npm
[crates]: https://img.shields.io/crates/v/tree-sitter-ocamllex?logo=rust
[pypi]: https://img.shields.io/pypi/v/tree-sitter-ocamllex?logo=pypi&logoColor=white&label=PyPI
[tree-sitter]: https://tree-sitter.github.io/tree-sitter/
