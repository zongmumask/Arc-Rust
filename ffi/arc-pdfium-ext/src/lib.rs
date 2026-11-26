//! Consumer-side PDFium extensions migrated from Arc-PDFium.
//!
//! The C++ implementations in `cpp/` are compiled against the pinned PDFium
//! checkout by `build.rs`; this crate exposes their C ABI as raw bindings.
//! Symbol names and signatures are kept identical to the validated
//! Arc-macOS extension set (169 entry points).

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

// Keep the PDFium archive dependency explicit: consumers linking only this
// crate still need `libpdfium.a` for the internal symbols used by the C++
// extensions (and for the public C API types in the bindings).
use arc_pdfium_sys as _;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pdfium_archive_is_linked() {
        // Calling the public API forces a link against libpdfium.a and
        // verifies the ext crate's archive dependency is live.
        unsafe { arc_pdfium_sys::FPDF_InitLibrary() };
        unsafe { arc_pdfium_sys::FPDF_DestroyLibrary() };
    }

    /// A few representative symbols from different ABI layers must remain
    /// linkable: public-only, internal-helper, and matrix-aware formfill.
    #[test]
    fn extension_symbols_are_linkable() {
        let symbols: [*const (); 3] = [
            FPDFAnnot_GetLineEndingStyle as *const (),
            FPDFExt_GetBookmarkCount as *const (),
            FPDF_FFLDrawWithMatrix as *const (),
        ];
        assert!(symbols.iter().all(|p| !p.is_null()));
    }
}
