//! GPUI view layer: `PdfView` (layout, scroll, visibility) and `PageView`
//! (per-page preview/tiles). Rendering orchestration lives in arc-pdf-viewer.

mod page_view;
mod pdf_view;

pub use page_view::PageView;
pub use pdf_view::PdfView;
