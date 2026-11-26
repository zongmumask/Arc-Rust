//! Build the consumer-side PDFium extensions migrated from Arc-PDFium.
//!
//! The C++ sources use PDFium internal headers, so they must be compiled
//! against the same pinned checkout that produced `libpdfium.a`. HarfBuzz
//! headers come from that checkout as well; the required HarfBuzz symbols are
//! already present in the PDFium archive (the config pins `harfbuzz.enabled =
//! false`, i.e. no separate HarfBuzz library).

use std::env;
use std::path::{Path, PathBuf};

fn repo_root(manifest_dir: &Path) -> PathBuf {
    manifest_dir.parent().unwrap().parent().unwrap().to_path_buf()
}

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let root = repo_root(&manifest_dir);
    let cpp_dir = manifest_dir.join("cpp");
    let include_dir = manifest_dir.join("include");
    let target = env::var("ARCPDF_PDFIUM_TARGET").unwrap_or_else(|_| "macos_arm64".into());
    let artifact_dir = root.join("artifacts/pdfium").join(&target);
    let pdfium_dir = root.join("build/pdfium");
    let freetype_inc = pdfium_dir.join("third_party/freetype/src/include");
    let harfbuzz_src = pdfium_dir.join("third_party/harfbuzz/src/src");

    println!("cargo:rerun-if-env-changed=ARCPDF_PDFIUM_TARGET");
    println!("cargo:rerun-if-changed={}", pdfium_dir.join(".git/HEAD").display());
    for path in [&cpp_dir, &include_dir, &artifact_dir.join("manifest.json")] {
        println!("cargo:rerun-if-changed={}", path.display());
    }

    for dir in [&pdfium_dir, &artifact_dir] {
        if !dir.exists() {
            panic!(
                "PDFium directory {} is missing. Run scripts/build-pdfium --target {} first",
                dir.display(),
                target
            );
        }
    }

    // Match the ArcPDFium podspec: gnu++20, system libc++, Apple frameworks.
    // Header order matters: FreeType must precede PDFium internals
    // (fpdf_font_ext.cpp) and extension headers take precedence over the
    // checked-in PDFium core headers with the same class names.
    let mut build = cc::Build::new();
    build
        .cpp(true)
        .std("c++20")
        .flag_if_supported("-fno-exceptions")
        .flag_if_supported("-fno-rtti")
        .include(&include_dir)
        .include(&pdfium_dir)
        .include(pdfium_dir.join("public"))
        .include(&freetype_inc)
        .include(&harfbuzz_src)
        .warnings(false);

    for entry in std::fs::read_dir(&cpp_dir).expect("read extension cpp dir") {
        let path = entry.expect("dir entry").path();
        if path.extension().is_some_and(|ext| ext == "cpp") {
            build.file(path);
        }
    }
    build.compile("arc-pdfium-ext");

    if cfg!(target_os = "macos") || cfg!(target_os = "ios") {
        println!("cargo:rustc-link-lib=framework=CoreText");
        println!("cargo:rustc-link-lib=framework=CoreFoundation");
        println!("cargo:rustc-link-lib=framework=CoreGraphics");
    }

    // Bindgen the pure-C extension headers against public PDFium headers only;
    // internal C++ headers stay confined to the C++ build above.
    let headers: Vec<PathBuf> = std::fs::read_dir(&include_dir)
        .expect("read extension include dir")
        .map(|e| e.expect("dir entry").path())
        .filter(|p| {
            p.file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|n| n.starts_with("fpdf") && n.ends_with("_ext.h"))
        })
        .collect();

    let mut builder = bindgen::Builder::default()
        .clang_arg(format!("-I{}", include_dir.display()))
        .clang_arg(format!("-I{}", artifact_dir.join("include").display()))
        .allowlist_function("FPDF.*")
        .allowlist_type("FPDF.*")
        .allowlist_var("FPDF.*")
        .layout_tests(false)
        .derive_debug(false)
        .generate_comments(false);
    for header in headers {
        builder = builder.header(header.to_string_lossy());
    }
    let bindings = builder.generate().expect("failed to generate extension bindings");
    bindings
        .write_to_file(PathBuf::from(env::var("OUT_DIR").unwrap()).join("bindings.rs"))
        .expect("failed to write extension bindings");
}
