# tree-sitter-ocaml

[![CI][ci]](https://github.com/tree-sitter/tree-sitter-ocaml/actions/workflows/ci.yml)
[![discord][discord]](https://discord.gg/w7nTvsVJhm)
[![matrix][matrix]](https://matrix.to/#/#tree-sitter-chat:matrix.org)
[![npm][npm]](https://www.npmjs.com/package/tree-sitter-ocaml)
[![crates][crates]](https://crates.io/crates/tree-sitter-ocaml)
[![pypi][pypi]](https://pypi.org/project/tree-sitter-ocaml/)

OCaml grammar for [tree-sitter][].

## Upstream Provenance (OCaml Bootstrap Vendoring)

- **Repository**: <https://github.com/tree-sitter/tree-sitter-ocaml>
- **Version**: `v0.25.0`
- **Commit**: `527d62ef0f24ce0d97fbedf004921d75d0a7e086`
- **License**: MIT (see `LICENSE`)
- **Modifications**:
  - None to the upstream grammar or scanner sources (`common/scanner.h`, `grammars/{ocaml,interface}/grammar.js`, `grammars/{ocaml,interface}/src/scanner.c`).
  - Only the `ocaml` (`.ml`) and `interface` (`.mli`) grammars and `common/scanner.h` needed for bootstrap are retained, along with their pre-generated C parser files (`grammars/{ocaml,interface}/src/parser.c` and `grammars/{ocaml,interface}/src/tree_sitter/{alloc,array,parser}.h`, generated from `grammar.js` via `tree-sitter generate --abi 15`) so that OCaml can be bootstrapped from source with a standard C/C++ compiler without requiring Node.js or the Rust `tree-sitter` CLI.

References

- [OCaml language reference](https://ocaml.org/manual/language.html)
- [OCaml language extensions](https://ocaml.org/manual/extn.html)
- [OCaml lexer](https://github.com/ocaml/ocaml/blob/trunk/parsing/lexer.mll)
- [OCaml parser](https://github.com/ocaml/ocaml/blob/trunk/parsing/parser.mly)

[ci]: https://img.shields.io/github/actions/workflow/status/tree-sitter/tree-sitter-ocaml/ci.yml?logo=github&label=CI
[discord]: https://img.shields.io/discord/1063097320771698699?logo=discord&label=discord
[matrix]: https://img.shields.io/matrix/tree-sitter-chat%3Amatrix.org?logo=matrix&label=matrix
[npm]: https://img.shields.io/npm/v/tree-sitter-ocaml?logo=npm
[crates]: https://img.shields.io/crates/v/tree-sitter-ocaml?logo=rust
[pypi]: https://img.shields.io/pypi/v/tree-sitter-ocaml?logo=pypi&logoColor=white&label=PyPI
[tree-sitter]: https://tree-sitter.github.io/tree-sitter/
