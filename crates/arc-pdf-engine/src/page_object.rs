//! Page-object handle wrapper, ported from ArcPDFKit `PageObject`.

use super::{serial, EngineError, Result};
use arc_pdf_core::{Rect, Vec2};
use arc_pdfium_sys as sys;
use std::marker::PhantomData;
use std::ptr::NonNull;

/// 2D affine matrix matching PDFium's `FS_MATRIX` / ArcPDFKit's `PDFMatrix`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PdfMatrix {
    pub a: f32,
    pub b: f32,
    pub c: f32,
    pub d: f32,
    pub e: f32,
    pub f: f32,
}

impl PdfMatrix {
    pub const IDENTITY: Self = Self {
        a: 1.0,
        b: 0.0,
        c: 0.0,
        d: 1.0,
        e: 0.0,
        f: 0.0,
    };

    fn from_fs(m: &sys::FS_MATRIX) -> Self {
        Self {
            a: m.a,
            b: m.b,
            c: m.c,
            d: m.d,
            e: m.e,
            f: m.f,
        }
    }

    fn to_fs(self) -> sys::FS_MATRIX {
        sys::FS_MATRIX {
            a: self.a,
            b: self.b,
            c: self.c,
            d: self.d,
            e: self.e,
            f: self.f,
        }
    }
}

/// A borrowed `FPDF_PAGEOBJECT`; the handle belongs to its page.
pub struct PageObject<'page> {
    pub(crate) handle: NonNull<sys::fpdf_pageobject_t__>,
    _page: PhantomData<&'page ()>,
}

impl<'page> PageObject<'page> {
    pub(crate) fn load(handle: NonNull<sys::fpdf_pageobject_t__>) -> Self {
        Self {
            handle,
            _page: PhantomData,
        }
    }

    pub fn object_type(&self) -> i32 {
        serial::with_pdfium(|| unsafe { sys::FPDFPageObj_GetType(self.handle.as_ptr()) })
    }

    pub fn bounds(&self) -> Option<Rect> {
        let mut left = 0.0f32;
        let mut bottom = 0.0f32;
        let mut right = 0.0f32;
        let mut top = 0.0f32;
        let ok = serial::with_pdfium(|| unsafe {
            sys::FPDFPageObj_GetBounds(
                self.handle.as_ptr(),
                &mut left,
                &mut bottom,
                &mut right,
                &mut top,
            )
        });
        if ok == 0 {
            None
        } else {
            Some(Rect::new(left, bottom, right - left, top - bottom))
        }
    }

    pub fn matrix(&self) -> PdfMatrix {
        let mut m = PdfMatrix::IDENTITY.to_fs();
        let ok = serial::with_pdfium(|| unsafe {
            sys::FPDFPageObj_GetMatrix(self.handle.as_ptr(), &mut m)
        });
        if ok == 0 {
            PdfMatrix::IDENTITY
        } else {
            PdfMatrix::from_fs(&m)
        }
    }

    pub fn set_matrix(&self, matrix: PdfMatrix) -> Result<()> {
        let fs = matrix.to_fs();
        let ok = serial::with_pdfium(|| unsafe {
            sys::FPDFPageObj_SetMatrix(self.handle.as_ptr(), &fs)
        });
        if ok == 0 {
            return Err(EngineError::new("FPDFPageObj_SetMatrix failed"));
        }
        Ok(())
    }

    pub fn fill_color(&self) -> Option<(u32, u32, u32, u32)> {
        let mut r = 0;
        let mut g = 0;
        let mut b = 0;
        let mut a = 0;
        let ok = serial::with_pdfium(|| unsafe {
            sys::FPDFPageObj_GetFillColor(self.handle.as_ptr(), &mut r, &mut g, &mut b, &mut a)
        });
        if ok == 0 {
            None
        } else {
            Some((r, g, b, a))
        }
    }

    pub fn stroke_color(&self) -> Option<(u32, u32, u32, u32)> {
        let mut r = 0;
        let mut g = 0;
        let mut b = 0;
        let mut a = 0;
        let ok = serial::with_pdfium(|| unsafe {
            sys::FPDFPageObj_GetStrokeColor(self.handle.as_ptr(), &mut r, &mut g, &mut b, &mut a)
        });
        if ok == 0 {
            None
        } else {
            Some((r, g, b, a))
        }
    }

    /// Point form of `e/f` (translation components) of the object matrix.
    pub fn position(&self) -> Vec2 {
        let m = self.matrix();
        Vec2::new(m.e, m.f)
    }
}
