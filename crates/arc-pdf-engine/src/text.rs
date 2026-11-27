//! Rust equivalent of ArcPDFKit's `TextPage` handle wrapper.

use super::wide;
use super::Page;
use super::{EngineError, Result};
use arc_pdf_core::Rect;
use arc_pdfium_sys as sys;
use std::ptr::NonNull;

/// Owning `FPDF_TEXTPAGE` handle, tied to the borrowed `Page`.
pub struct TextPage<'page> {
    handle: NonNull<sys::fpdf_textpage_t__>,
    _page: &'page Page<'page>,
}

impl<'page> TextPage<'page> {
    pub(crate) fn load(page: &'page Page<'page>) -> Result<Self> {
        let handle = unsafe { sys::FPDFText_LoadPage(page.handle.as_ptr()) };
        let handle =
            NonNull::new(handle).ok_or_else(|| EngineError::new("FPDFText_LoadPage failed"))?;
        Ok(Self {
            handle,
            _page: page,
        })
    }

    pub fn char_count(&self) -> usize {
        let count = unsafe { sys::FPDFText_CountChars(self.handle.as_ptr()) };
        count.max(0) as usize
    }

    pub fn unicode_at(&self, index: usize) -> Option<char> {
        let raw = unsafe { sys::FPDFText_GetUnicode(self.handle.as_ptr(), index as i32) };
        char::from_u32(raw)
    }

    pub fn text(&self, start: usize, count: usize) -> Result<String> {
        let mut units = vec![0u16; count];
        let written = unsafe {
            sys::FPDFText_GetText(
                self.handle.as_ptr(),
                start as i32,
                count as i32,
                units.as_mut_ptr(),
            )
        };
        if written == 0 {
            return Ok(String::new());
        }
        // FPDFText_GetText writes UTF-16 code units plus a NUL terminator.
        let len = (written as usize).saturating_sub(1).min(units.len());
        Ok(String::from_utf16_lossy(&units[..len]))
    }

    pub fn char_box(&self, index: usize) -> Option<Rect> {
        let mut left = 0.0f64;
        let mut top = 0.0f64;
        let mut right = 0.0f64;
        let mut bottom = 0.0f64;
        let ok = unsafe {
            sys::FPDFText_GetCharBox(
                self.handle.as_ptr(),
                index as i32,
                &mut left,
                &mut right,
                &mut top,
                &mut bottom,
            )
        };
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

    pub fn rect_count(&self, start: usize, count: usize) -> usize {
        let n =
            unsafe { sys::FPDFText_CountRects(self.handle.as_ptr(), start as i32, count as i32) };
        n.max(0) as usize
    }

    pub fn rect_at(&self, index: usize) -> Option<Rect> {
        let mut left = 0.0f64;
        let mut top = 0.0f64;
        let mut right = 0.0f64;
        let mut bottom = 0.0f64;
        let ok = unsafe {
            sys::FPDFText_GetRect(
                self.handle.as_ptr(),
                index as i32,
                &mut left,
                &mut top,
                &mut right,
                &mut bottom,
            )
        };
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

    pub fn find(&self, needle: &str) -> Vec<super::TextMatch> {
        let pattern = wide::to_pdfium(needle);
        let handle = unsafe {
            sys::FPDFText_FindStart(
                self.handle.as_ptr(),
                pattern.as_ptr(),
                sys::FPDF_MATCHCASE as std::os::raw::c_ulong,
                0,
            )
        };
        if handle.is_null() {
            return Vec::new();
        }
        let mut matches = Vec::new();
        while unsafe { sys::FPDFText_FindNext(handle) } != 0 {
            let start = unsafe { sys::FPDFText_GetSchResultIndex(handle) }.max(0) as usize;
            let count = unsafe { sys::FPDFText_GetSchCount(handle) }.max(0) as usize;
            let mut rect = Rect::default();
            for i in 0..self.rect_count(start, count) {
                if let Some(r) = self.rect_at(i) {
                    if i == 0 {
                        rect = r;
                    } else {
                        // Rect::union is not in core yet; extend by min/max below.
                        let min_x = rect.origin.x.min(r.origin.x);
                        let min_y = rect.origin.y.min(r.origin.y);
                        let max_x =
                            (rect.origin.x + rect.size.width).max(r.origin.x + r.size.width);
                        let max_y =
                            (rect.origin.y + rect.size.height).max(r.origin.y + r.size.height);
                        rect = Rect::new(min_x, min_y, max_x - min_x, max_y - min_y);
                    }
                }
            }
            matches.push(super::TextMatch { start, count, rect });
        }
        unsafe { sys::FPDFText_FindClose(handle) };
        matches
    }
}

impl Drop for TextPage<'_> {
    fn drop(&mut self) {
        unsafe { sys::FPDFText_ClosePage(self.handle.as_ptr()) }
    }
}
