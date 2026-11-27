//! Web-link detection wrapper, ported from ArcPDFKit `PageLink`.

use super::serial;
use arc_pdf_core::Rect;
use arc_pdfium_sys as sys;
use std::ptr::NonNull;

/// Owns an `FPDF_PAGELINK` created from a text page.
pub struct PageLink {
    handle: NonNull<sys::fpdf_pagelink_t__>,
}

impl PageLink {
    pub(crate) fn load_under(text_page: &super::TextPage<'_>) -> Result<Self, super::EngineError> {
        let handle = serial::with_pdfium(|| unsafe {
            sys::FPDFLink_LoadWebLinks(text_page.handle.as_ptr())
        });
        let handle = NonNull::new(handle)
            .ok_or_else(|| super::EngineError::new("FPDFLink_LoadWebLinks failed"))?;
        Ok(Self { handle })
    }

    pub fn count_web_links(&self) -> usize {
        serial::with_pdfium(|| unsafe { sys::FPDFLink_CountWebLinks(self.handle.as_ptr()) }).max(0)
            as usize
    }

    pub fn rect_count(&self, link_index: usize) -> usize {
        serial::with_pdfium(|| unsafe {
            sys::FPDFLink_CountRects(self.handle.as_ptr(), link_index as i32)
        })
        .max(0) as usize
    }

    pub fn rect(&self, link_index: usize, rect_index: usize) -> Option<Rect> {
        let mut left = 0.0f64;
        let mut top = 0.0f64;
        let mut right = 0.0f64;
        let mut bottom = 0.0f64;
        let ok = serial::with_pdfium(|| unsafe {
            sys::FPDFLink_GetRect(
                self.handle.as_ptr(),
                link_index as i32,
                rect_index as i32,
                &mut left,
                &mut top,
                &mut right,
                &mut bottom,
            )
        });
        if ok == 0 {
            return None;
        }
        Some(Rect::new(
            left as f32,
            bottom as f32,
            (right - left) as f32,
            (top - bottom) as f32,
        ))
    }

    pub fn url(&self, link_index: usize) -> Option<String> {
        let len = serial::with_pdfium(|| unsafe {
            sys::FPDFLink_GetURL(
                self.handle.as_ptr(),
                link_index as i32,
                std::ptr::null_mut(),
                0,
            )
        });
        if len <= 0 {
            return None;
        }
        let mut units = vec![0u16; len as usize];
        let ok = serial::with_pdfium(|| unsafe {
            sys::FPDFLink_GetURL(
                self.handle.as_ptr(),
                link_index as i32,
                units.as_mut_ptr(),
                len,
            )
        });
        if ok == 0 {
            return None;
        }
        while units.last() == Some(&0) {
            units.pop();
        }
        Some(String::from_utf16_lossy(&units))
    }
}

impl Drop for PageLink {
    fn drop(&mut self) {
        serial::with_pdfium(|| unsafe { sys::FPDFLink_CloseWebLinks(self.handle.as_ptr()) });
    }
}
