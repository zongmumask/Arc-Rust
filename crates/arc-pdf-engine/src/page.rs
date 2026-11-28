//! Rust equivalent of ArcPDFKit's `Page` handle wrapper.

use super::{serial, BitmapGuard};
use super::{Document, TextPage};
use super::{EngineError, Raster, Result};
use arc_pdf_core::PageId;
use arc_pdfium_sys as sys;
use std::ptr::NonNull;

/// A loaded `FPDF_PAGE`. The borrow of `Document` keeps the owner alive.
pub struct Page<'doc> {
    pub(crate) handle: NonNull<sys::fpdf_page_t__>,
    pub(crate) id: PageId,
    _doc: &'doc Document,
}

impl<'doc> Page<'doc> {
    pub(crate) fn load(document: &'doc Document, id: PageId) -> Result<Self> {
        let handle = serial::with_pdfium(|| unsafe {
            sys::FPDF_LoadPage(document.handle.as_ptr(), id.0 as std::os::raw::c_int)
        });
        let handle = NonNull::new(handle)
            .ok_or_else(|| EngineError::new(format!("FPDF_LoadPage failed for page {}", id.0)))?;
        Ok(Self {
            handle,
            id,
            _doc: document,
        })
    }

    pub fn id(&self) -> PageId {
        self.id
    }

    pub(crate) fn document_handle(&self) -> sys::FPDF_DOCUMENT {
        self._doc.handle.as_ptr()
    }

    /// Render the whole page into a fresh RGBA raster at `scale`.
    pub fn render_page(&self, scale: f32) -> Result<Raster> {
        let (width_pt, height_pt) = self._doc.page_size(self.id)?;
        self.render_region(0.0, 0.0, width_pt, height_pt, scale, &|| true)?
            .ok_or_else(|| EngineError::new("render guard rejected whole-page render"))
    }

    /// Render one page-space rectangle into a fresh RGBA raster.
    ///
    /// `guard` is checked immediately before the FPDF render call, while the
    /// serial PDFium boundary is held. Fast scrolling can enqueue many tiles
    /// that are already outside the viewport by the time they reach the head
    /// of the queue; the guard lets callers drop that work before it reaches
    /// PDFium. Returning `false` produces `Ok(None)`.
    pub fn render_region(
        &self,
        x_pt: f32,
        y_pt: f32,
        width_pt: f32,
        height_pt: f32,
        scale: f32,
        guard: &dyn Fn() -> bool,
    ) -> Result<Option<Raster>> {
        if !guard() {
            return Ok(None);
        }
        let width = (width_pt * scale).ceil() as usize;
        let height = (height_pt * scale).ceil() as usize;
        if width == 0 || height == 0 {
            return Err(EngineError::new("empty render target"));
        }

        let mut pixels = vec![0u8; width * height * 4];
        let stride = (width * 4) as std::os::raw::c_int;
        let bitmap = serial::with_pdfium(|| unsafe {
            sys::FPDFBitmap_CreateEx(
                width as std::os::raw::c_int,
                height as std::os::raw::c_int,
                sys::FPDFBitmap_BGRA as std::os::raw::c_int,
                pixels.as_mut_ptr() as *mut std::os::raw::c_void,
                stride,
            )
        });
        if bitmap.is_null() {
            return Err(EngineError::new("FPDFBitmap_CreateEx failed"));
        }
        let _bitmap_guard = BitmapGuard(bitmap);

        serial::with_pdfium(|| unsafe {
            sys::FPDFBitmap_FillRect(
                bitmap,
                0,
                0,
                width as std::os::raw::c_int,
                height as std::os::raw::c_int,
                0xFFFFFFFF,
            );
        });
        let matrix = sys::FS_MATRIX {
            a: scale,
            b: 0.0,
            c: 0.0,
            d: scale,
            e: -x_pt * scale,
            f: -y_pt * scale,
        };
        let clip = sys::FS_RECTF {
            left: 0.0,
            top: 0.0,
            right: width as f32,
            bottom: height as f32,
        };
        if !guard() {
            return Ok(None);
        }
        serial::with_pdfium(|| unsafe {
            sys::FPDF_RenderPageBitmapWithMatrix(
                bitmap,
                self.handle.as_ptr(),
                &matrix,
                &clip,
                sys::FPDF_REVERSE_BYTE_ORDER as std::os::raw::c_int,
            );
        });
        for chunk in pixels.as_chunks_mut::<4>().0 {
            chunk.swap(0, 2);
        }
        Ok(Some(Raster {
            width,
            height,
            stride,
            pixels,
        }))
    }

    /// Read annotations as owned snapshots (matching ArcPDFKit's cached view).
    pub fn annotations(&self) -> Vec<super::Annotation<'_>> {
        super::serial::with_pdfium(|| {
            let count =
                unsafe { sys::FPDFPage_GetAnnotCount(self.handle.as_ptr()) }.max(0) as usize;
            (0..count)
                .filter_map(|index| {
                    let handle =
                        unsafe { sys::FPDFPage_GetAnnot(self.handle.as_ptr(), index as i32) };
                    std::ptr::NonNull::new(handle).map(super::Annotation::load)
                })
                .collect()
        })
    }

    /// Borrowed views of all page objects.
    pub fn objects(&self) -> Vec<super::PageObject<'_>> {
        super::serial::with_pdfium(|| {
            let count = unsafe { sys::FPDFPage_CountObjects(self.handle.as_ptr()) }.max(0) as usize;
            (0..count)
                .filter_map(|index| {
                    let handle =
                        unsafe { sys::FPDFPage_GetObject(self.handle.as_ptr(), index as i32) };
                    std::ptr::NonNull::new(handle).map(super::PageObject::load)
                })
                .collect()
        })
    }

    /// Lazily create a text page for this page, mirroring `Page.textPage`.
    pub fn text_page(&self) -> Result<TextPage<'_>> {
        TextPage::load(self)
    }
}

impl Drop for Page<'_> {
    fn drop(&mut self) {
        serial::with_pdfium(|| unsafe { sys::FPDF_ClosePage(self.handle.as_ptr()) })
    }
}
