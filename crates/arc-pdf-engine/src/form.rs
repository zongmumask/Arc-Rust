//! Form interaction facade, ported from ArcPDFKit `FormHandler`.
//!
//! All coordinates are PDF user space (y-up); platform event modifiers are
//! supplied by the embedding app as `FWL_EVENTFLAG_*` values. The FormFillInfo
//! callbacks remain an app-side seam and are intentionally not ported.

use super::{serial, Page};
use arc_pdfium_sys as sys;
use std::marker::PhantomData;
use std::ptr::NonNull;

/// Borrowed `FPDF_FORMHANDLE`. The handle belongs to the document.
pub struct FormHandler<'doc> {
    pub(crate) handle: NonNull<sys::fpdf_form_handle_t__>,
    _doc: PhantomData<&'doc ()>,
}

impl<'doc> FormHandler<'doc> {
    pub(crate) fn from_handle(handle: NonNull<sys::fpdf_form_handle_t__>) -> Self {
        Self {
            handle,
            _doc: PhantomData,
        }
    }

    /// -1 means no form field at the point; otherwise an `FPDF_FORMFIELD_*` type.
    pub fn field_type_at(&self, point: (f64, f64), page: &Page<'_>) -> i32 {
        serial::with_pdfium(|| unsafe {
            sys::FPDFPage_HasFormFieldAtPoint(
                self.handle.as_ptr(),
                page.handle.as_ptr(),
                point.0,
                point.1,
            )
        })
    }

    pub fn mouse_move(&self, point: (f64, f64), page: &Page<'_>, modifier: i32) -> bool {
        serial::with_pdfium(|| unsafe {
            sys::FORM_OnMouseMove(
                self.handle.as_ptr(),
                page.handle.as_ptr(),
                modifier,
                point.0,
                point.1,
            )
        }) != 0
    }

    pub fn focus(&self, point: (f64, f64), page: &Page<'_>, modifier: i32) -> bool {
        serial::with_pdfium(|| unsafe {
            sys::FORM_OnFocus(
                self.handle.as_ptr(),
                page.handle.as_ptr(),
                modifier,
                point.0,
                point.1,
            )
        }) != 0
    }

    pub fn left_down(&self, point: (f64, f64), page: &Page<'_>, modifier: i32) -> bool {
        serial::with_pdfium(|| unsafe {
            sys::FORM_OnLButtonDown(
                self.handle.as_ptr(),
                page.handle.as_ptr(),
                modifier,
                point.0,
                point.1,
            )
        }) != 0
    }

    pub fn left_up(&self, point: (f64, f64), page: &Page<'_>, modifier: i32) -> bool {
        serial::with_pdfium(|| unsafe {
            sys::FORM_OnLButtonUp(
                self.handle.as_ptr(),
                page.handle.as_ptr(),
                modifier,
                point.0,
                point.1,
            )
        }) != 0
    }

    pub fn double_click(&self, point: (f64, f64), page: &Page<'_>, modifier: i32) -> bool {
        serial::with_pdfium(|| unsafe {
            sys::FORM_OnLButtonDoubleClick(
                self.handle.as_ptr(),
                page.handle.as_ptr(),
                modifier,
                point.0,
                point.1,
            )
        }) != 0
    }

    pub fn key_down(&self, key_code: i32, modifier: i32, page: &Page<'_>) -> bool {
        serial::with_pdfium(|| unsafe {
            sys::FORM_OnKeyDown(
                self.handle.as_ptr(),
                page.handle.as_ptr(),
                key_code,
                modifier,
            )
        }) != 0
    }

    pub fn key_up(&self, key_code: i32, modifier: i32, page: &Page<'_>) -> bool {
        serial::with_pdfium(|| unsafe {
            sys::FORM_OnKeyUp(
                self.handle.as_ptr(),
                page.handle.as_ptr(),
                key_code,
                modifier,
            )
        }) != 0
    }

    pub fn character(&self, ch: i32, modifier: i32, page: &Page<'_>) -> bool {
        serial::with_pdfium(|| unsafe {
            sys::FORM_OnChar(self.handle.as_ptr(), page.handle.as_ptr(), ch, modifier)
        }) != 0
    }

    /// Flush uncommitted field edits before switching tools / closing a page.
    pub fn reset_focus(&self) {
        serial::with_pdfium(|| unsafe { sys::FORM_ForceToKillFocus(self.handle.as_ptr()) });
    }

    /// Current value of the form field at `point` (UTF-16 decoded).
    pub fn field_value_at(&self, point: (f64, f64), page: &Page<'_>) -> Option<String> {
        let mut p = sys::FS_POINTF {
            x: point.0 as f32,
            y: point.1 as f32,
        };
        let annot = serial::with_pdfium(|| unsafe {
            sys::FPDFAnnot_GetFormFieldAtPoint(self.handle.as_ptr(), page.handle.as_ptr(), &mut p)
        });
        let annot = NonNull::new(annot)?;
        let len = serial::with_pdfium(|| unsafe {
            sys::FPDFAnnot_GetFormFieldValue(
                self.handle.as_ptr(),
                annot.as_ptr(),
                std::ptr::null_mut(),
                0,
            )
        });
        let mut units = vec![0u16; len as usize / 2 + 1];
        serial::with_pdfium(|| unsafe {
            sys::FPDFAnnot_GetFormFieldValue(
                self.handle.as_ptr(),
                annot.as_ptr(),
                units.as_mut_ptr(),
                len,
            );
        });
        while units.last() == Some(&0) {
            units.pop();
        }
        Some(String::from_utf16_lossy(&units))
    }
}
