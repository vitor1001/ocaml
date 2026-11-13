# Tree-sitter C Runtime Library (`lib/`)

## Upstream Provenance (OCaml Bootstrap Vendoring)

- **Repository**: <https://github.com/tree-sitter/tree-sitter> (`lib/` directory)
- **Version**: `v0.25.10`
- **Commit**: `da6fe9beb4f7f67beb75914ca8e0d48ae48d6406`
- **License**: MIT
- **Modifications**: None. The upstream C runtime sources (`lib/src/*.{c,h}`) and public C header (`lib/include/tree_sitter/api.h`) are vendored unmodified so that `boot/ocaml_boot` can be compiled with a standard C/C++ compiler without requiring a system-wide `libtree-sitter` installation or Rust toolchain.

## Subdirectories

* [`src`](./src) - C source code for the Tree-sitter library
* [`include`](./include) - C headers for the Tree-sitter library
