//! PDFium actor, cache policy and geometry helpers for the GPUI reader.

mod actor;
mod controller;
mod cache;
mod geometry;

pub use actor::{DocumentInfo, PdfiumActor, RenderGuard};
pub use controller::PdfViewController;
pub use cache::{DecodeResult, PreviewCache};
pub use geometry::{page_frame, preview_scale, strictly_visible_pages, visible_pages, BOTTOM_INSET_PT, PAGE_MARGIN_PT, TILE_SIZE_PT, PageFrame};
