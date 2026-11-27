//! Rust equivalent of ArcPDFKit's `Page` handle wrapper.

use super::BitmapGuard;
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
        let handle =
            unsafe { sys::FPDF_LoadPage(document.handle.as_ptr(), id.0 as std::os::raw::c_int) };
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

    /// Render the whole page into a fresh RGBA raster at `scale`.
    pub fn render_page(&self, scale: f32) -> Result<Raster> {
        let (width_pt, height_pt) = self._doc.page_size(self.id)?;
        let width = (width_pt * scale).ceil() as usize;
        let height = (height_pt * scale).ceil() as usize;
        if width == 0 || height == 0 {
            return Err(EngineError::new("empty render target"));
        }

        let mut pixels = vec![0u8; width * height * 4];
        let stride = (width * 4) as std::os::raw::c_int;
        let bitmap = unsafe {
            sys::FPDFBitmap_CreateEx(
                width as std::os::raw::c_int,
                height as std::os::raw::c_int,
                sys::FPDFBitmap_BGRA as std::os::raw::c_int,
                pixels.as_mut_ptr() as *mut std::os::raw::c_void,
                stride,
            )
        };
        if bitmap.is_null() {
            return Err(EngineError::new("FPDFBitmap_CreateEx failed"));
        }
        let _bitmap_guard = BitmapGuard(bitmap);

        unsafe {
            sys::FPDFBitmap_FillRect(
                bitmap,
                0,
                0,
                width as std::os::raw::c_int,
                height as std::os::raw::c_int,
                0xFFFFFFFF,
            );
        }
        let matrix = sys::FS_MATRIX {
            a: scale,
            b: 0.0,
            c: 0.0,
            d: scale,
            e: 0.0,
            f: 0.0,
        };
        let clip = sys::FS_RECTF {
            left: 0.0,
            top: 0.0,
            right: width as f32,
            bottom: height as f32,
        };
        unsafe {
            sys::FPDF_RenderPageBitmapWithMatrix(
                bitmap,
                self.handle.as_ptr(),
                &matrix,
                &clip,
                sys::FPDF_REVERSE_BYTE_ORDER as std::os::raw::c_int,
            );
        }
        for chunk in pixels.as_chunks_mut::<4>().0 {
            chunk.swap(0, 2);
        }
        Ok(Raster {
            width,
            height,
            stride,
            pixels,
        })
    }

    /// Lazily create a text page for this page, mirroring `Page.textPage`.
    pub fn text_page(&self) -> Result<TextPage<'_>> {
        TextPage::load(self)
    }
}

impl Drop for Page<'_> {
    fn drop(&mut self) {
        unsafe { sys::FPDF_ClosePage(self.handle.as_ptr()) }
    }
}
