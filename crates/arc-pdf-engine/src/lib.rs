//! PDFium engine: owns process-global init and raw handle lifetime.
//!
//! Phase 1-C is a single-threaded minimal engine used by the CLI. The future
//! GPUI app will wrap the same operations in a process-level actor.

use arc_pdfium_sys as sys;
use std::path::Path;
use std::sync::Once;

mod document;
mod error;
mod page;
mod text;
mod wide;

pub use document::Document;
pub use error::{EngineError, Result};
pub use page::Page;
pub use text::TextPage;

static INIT: Once = Once::new();

/// Process-global PDFium runtime. Create once before opening documents.
pub struct PdfiumRuntime;

impl PdfiumRuntime {
    pub fn init() -> Self {
        INIT.call_once(|| unsafe {
            sys::FPDF_InitLibrary();
        });
        Self
    }

    pub fn open_document(&self, path: &Path) -> Result<Document> {
        document::open(path)
    }
}

/// One match from `TextPage::find`, using PDF page coordinates.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TextMatch {
    pub start: usize,
    pub count: usize,
    pub rect: arc_pdf_core::Rect,
}

pub struct Raster {
    pub width: usize,
    pub height: usize,
    pub stride: std::os::raw::c_int,
    pub pixels: Vec<u8>,
}

struct BitmapGuard(sys::FPDF_BITMAP);
impl Drop for BitmapGuard {
    fn drop(&mut self) {
        unsafe { sys::FPDFBitmap_Destroy(self.0) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use arc_pdf_core::PageId;
    use std::path::PathBuf;

    fn fixture() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../artifacts/test-fixtures/smoke.pdf")
    }

    #[test]
    fn document_page_and_text_wrappers_work() {
        let runtime = PdfiumRuntime::init();
        let doc = runtime.open_document(&fixture()).expect("open fixture");
        assert!(doc.page_count() > 0);
        let page = doc.page(PageId(0)).expect("load page");
        let text = page.text_page().expect("load text page");
        assert!(text.char_count() > 0, "smoke.pdf should contain text");
        let first = text
            .text(0, text.char_count().min(32))
            .expect("extract text");
        assert!(!first.trim().is_empty(), "first chars should not be blank");
    }

    #[test]
    fn render_page_still_works_after_refactor() {
        let runtime = PdfiumRuntime::init();
        let doc = runtime.open_document(&fixture()).expect("open fixture");
        let raster = doc.render_page(PageId(0), 1.0).expect("render page");
        assert!(raster.width > 0 && raster.height > 0);
        assert_eq!(raster.pixels.len(), raster.width * raster.height * 4);
    }
}
