# Tree-sitter C Runtime Library

## Upstream Provenance (OCaml Bootstrap Vendoring)

- **Repository**: <https://github.com/tree-sitter/tree-sitter> (`lib/` directory)
- **Version**: `v0.25.10`
- **Commit**: `da6fe9beb4f7f67beb75914ca8e0d48ae48d6406`
- **License**: MIT
- **Modifications**: None. Only the `lib/include` and `lib/src` C runtime files from the upstream repository are included (unmodified) so that `boot/ocaml_boot` can be built with a standard C/C++ compiler without external dependencies.
