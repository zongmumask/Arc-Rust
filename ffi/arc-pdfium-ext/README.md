# arc-pdfium-ext

Consumer-side PDFium extensions migrated from `Arc-PDFium/Extensions` (the set
validated in Arc-macOS): 22 C++ sources, 23 headers, 169 exported C entry points.
Sources are copied verbatim from the validated revision; do not edit them
without also updating the upstream Arc-PDFium extension set.

## Build contract

- `build.rs` compiles all `cpp/*.cpp` with `cc` (`-std=c++20`, no exceptions or
  RTTI) against the pinned checkout in `build/pdfium`, using the same include
  layout as the old podspec: extension headers, PDFium root + `public`,
  FreeType, and HarfBuzz headers. HarfBuzz symbols are already contained in
  `libpdfium.a` from `arc-pdfium-sys`, so no separate HarfBuzz library is
  linked.
- Bindings for the pure-C `_ext.h` headers are generated with bindgen against
  the public headers in `artifacts/pdfium/<target>/include`.
- `ARCPDF_PDFIUM_TARGET` selects the artifact/checkout (default
  `macos_arm64`). Run `scripts/build-pdfium --target <target>` first: the
  checkout is gitignored and must match the revision pinned in
  `pdfium/pdfium-revision.toml` (currently `bc58207`) and the prebuilt archive.
- Apple builds link CoreText/CoreFoundation/CoreGraphics for
  `fpdf_font_ext.cpp`'s CoreText path.

## Known caveats

- `FPDFExt_TextPage_GetEmbeddedFont` is declared in `fpdf_page_extract_ext.h`
  but had no implementation in the legacy extension set; no symbol exists.
- `fpdf_font_subset.cpp` requires HarfBuzz headers from the checkout. On a
  fresh clone the checkout must be synced before building (as with the core
  library itself).
- Internal C++ headers and `#define private public` in two helper sources tie
  B/C-layer extensions to the pinned PDFium revision; revalidate all symbols
  when upgrading the revision.
