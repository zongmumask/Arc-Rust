//! Rust equivalent of ArcPDFKit's `Document` handle wrapper.

use super::{serial, Bookmark, EngineError, FormHandler, LayerManager, Page, Result};
use arc_pdf_core::{ContentGeneration, PageId};
use arc_pdfium_sys as sys;
use std::path::Path;
use std::ptr::NonNull;

/// Owns an `FPDF_DOCUMENT` and the in-memory bytes backing it.
///
/// All FPDF calls in this crate must happen on one thread/serial executor,
/// mirroring ArcPDFKit's process-wide `pdfiumQueue`. The types are `!Send`
/// because the raw handles themselves are not thread-safe.
pub struct Document {
    pub(crate) handle: NonNull<sys::fpdf_document_t__>,
    _bytes: Vec<u8>,
    generation: ContentGeneration,
    _not_send: std::marker::PhantomData<*const ()>,
}

pub(crate) fn open(path: &Path) -> Result<Document> {
    let bytes =
        std::fs::read(path).map_err(|e| EngineError::new(format!("read PDF failed: {e}")))?;
    let handle = serial::with_pdfium(|| unsafe {
        sys::FPDF_LoadMemDocument(
            bytes.as_ptr() as *const std::os::raw::c_void,
            bytes.len() as std::os::raw::c_int,
            std::ptr::null(),
        )
    });
    let handle =
        NonNull::new(handle).ok_or_else(|| EngineError::new("FPDF_LoadMemDocument failed"))?;
    Ok(Document {
        handle,
        _bytes: bytes,
        generation: ContentGeneration::default(),
        _not_send: std::marker::PhantomData,
    })
}

impl Document {
    pub fn generation(&self) -> ContentGeneration {
        self.generation
    }

    pub fn page_count(&self) -> usize {
        serial::with_pdfium(|| unsafe { sys::FPDF_GetPageCount(self.handle.as_ptr()) as usize })
    }

    pub fn page_size(&self, page: PageId) -> Result<(f32, f32)> {
        let mut width = 0.0f64;
        let mut height = 0.0f64;
        let ok = serial::with_pdfium(|| unsafe {
            sys::FPDF_GetPageSizeByIndex(
                self.handle.as_ptr(),
                page.0 as std::os::raw::c_int,
                &mut width,
                &mut height,
            )
        });
        if ok == 0 {
            return Err(EngineError::new(format!(
                "page size failed for page {}",
                page.0
            )));
        }
        Ok((width as f32, height as f32))
    }

    /// Load a page handle; the returned `Page` borrows this document.
    pub fn page(&self, id: PageId) -> Result<Page<'_>> {
        Page::load(self, id)
    }

    pub fn render_page(&self, page: PageId, scale: f32) -> Result<super::Raster> {
        self.page(page)?.render_page(scale)
    }

    /// Render a page-space rectangle. `guard` is evaluated twice inside the
    /// engine, the second time immediately before FPDF_RenderPageBitmapWithMatrix.
    pub fn render_region(
        &self,
        page: PageId,
        rect: arc_pdf_core::Rect,
        scale: f32,
        guard: &dyn Fn() -> bool,
    ) -> Result<Option<super::Raster>> {
        self.page(page)?.render_region(
            rect.origin.x,
            rect.origin.y,
            rect.size.width,
            rect.size.height,
            scale,
            guard,
        )
    }

    /// Form interaction facade. Hosts that own a form environment can attach
    /// one handle; `FormHandler` itself never creates or destroys it.
    pub fn form_handler(
        &self,
        handle: std::ptr::NonNull<sys::fpdf_form_handle_t__>,
    ) -> FormHandler<'_> {
        FormHandler::from_handle(handle)
    }

    /// Load annotation layers (OCGs) from this document.
    pub fn layers(&self) -> LayerManager<'_> {
        LayerManager::load(self)
    }

    /// Read the PDF outline as an owned tree of bookmark snapshots.
    pub fn bookmarks(&self) -> Vec<Bookmark> {
        Bookmark::tree(self)
    }
}

impl Drop for Document {
    fn drop(&mut self) {
        serial::with_pdfium(|| unsafe { sys::FPDF_CloseDocument(self.handle.as_ptr()) })
    }
}
