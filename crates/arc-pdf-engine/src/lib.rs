//! PDFium engine: owns process-global init and raw handle lifetime.
//!
//! Phase 1-C is a single-threaded minimal engine used by the CLI. The future
//! GPUI app will wrap the same operations in a process-level actor.

use arc_pdfium_sys as sys;
use std::path::Path;
use std::sync::Once;

mod annotation;
mod bookmark;
mod document;
mod error;
mod form;
mod layer;
mod ocr;
mod page;
mod page_link;
mod page_object;
mod serial;
mod text;
mod watermark;
mod wide;

pub use annotation::{Annotation, AnnotationSubtype};
pub use bookmark::{Bookmark, Destination};
pub use document::Document;
pub use error::{EngineError, Result};
pub use form::FormHandler;
pub use layer::{Layer, LayerManager};
pub use ocr::{OcrRecognizer, OcrText, OcrTextBox, OcrTextSourceKind};
pub use page::Page;
pub use page_link::PageLink;
pub use page_object::{PageObject, PdfMatrix};
pub use text::TextPage;
pub use watermark::{add_text_watermark, remove_watermarks, watermarks, WatermarkInfo};

static INIT: Once = Once::new();

/// Process-global PDFium runtime. Create once before opening documents.
pub struct PdfiumRuntime;

impl PdfiumRuntime {
    pub fn init() -> Self {
        INIT.call_once(|| {
            serial::with_pdfium(|| unsafe {
                sys::FPDF_InitLibrary();
            });
        });
        Self
    }

    pub fn open_document(&self, path: &Path) -> Result<Document> {
        serial::with_pdfium(|| document::open(path))
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
        serial::with_pdfium(|| unsafe { sys::FPDFBitmap_Destroy(self.0) })
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
    fn navigation_wrappers_work() {
        let runtime = PdfiumRuntime::init();
        let doc = runtime.open_document(&fixture()).expect("open fixture");
        // The smoke fixture has no outline; reading an empty tree must be safe.
        let bookmarks = doc.bookmarks();
        assert!(bookmarks.is_empty() || !bookmarks[0].title.is_empty());
        let page = doc.page(PageId(0)).expect("load page");
        let text = page.text_page().expect("load text page");
        let links = text.page_link().expect("load web links");
        let _ = links.count_web_links();
    }

    #[test]
    fn annotation_and_object_readers_work() {
        let runtime = PdfiumRuntime::init();
        let doc = runtime.open_document(&fixture()).expect("open fixture");
        let page = doc.page(PageId(0)).expect("load page");
        let annotations = page.annotations();
        for annot in &annotations {
            let _ = annot.string_value("Contents");
            let _ = annot.color(0);
            let _ = annot.quad_count();
        }
        let objects = page.objects();
        for object in &objects {
            let _ = object.object_type();
            let _ = object.bounds();
            let _ = object.matrix();
            let _ = object.fill_color();
        }
    }

    #[test]
    fn form_and_layer_wrappers_work() {
        let runtime = PdfiumRuntime::init();
        let doc = runtime.open_document(&fixture()).expect("open fixture");
        let layers = doc.layers();
        assert!(layers.layers.is_empty() || !layers.layers[0].name.is_empty());
        let _ = layers.active;
    }

    #[test]
    fn render_region_respects_guard_and_clip() {
        let runtime = PdfiumRuntime::init();
        let doc = runtime.open_document(&fixture()).expect("open fixture");
        let (width_pt, height_pt) = doc.page_size(PageId(0)).expect("page size");
        let region = arc_pdf_core::Rect::new(0.0, 0.0, width_pt / 2.0, height_pt / 2.0);
        let full = doc.render_page(PageId(0), 1.0).expect("full page");
        let tile = doc
            .render_region(PageId(0), region, 1.0, &|| true)
            .expect("region render")
            .expect("not skipped");
        assert!(tile.width < full.width);
        assert!(tile.height < full.height);
        assert_eq!(tile.pixels.len(), tile.width * tile.height * 4);
        let skipped = doc
            .render_region(PageId(0), region, 1.0, &|| false)
            .expect("guarded render");
        assert!(skipped.is_none());
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
