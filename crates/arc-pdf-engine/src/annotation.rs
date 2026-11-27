//! Annotation handle wrapper, ported from ArcPDFKit `Annotation`.
//!
//! A borrowed `FPDF_ANNOTATION` view tied to a `Page`. Snapshots are taken on
//! load (matching the Swift wrapper's cached properties); mutations write
//! through to PDFium immediately.

use super::{serial, wide, EngineError, Result};
use arc_pdf_core::Rect;
use arc_pdfium_sys as sys;
use std::marker::PhantomData;
use std::ptr::NonNull;

/// Annotation subtypes; raw values match `FPDF_ANNOTATION_SUBTYPE`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AnnotationSubtype {
    Unknown,
    Text,
    Link,
    FreeText,
    Line,
    Square,
    Circle,
    Polygon,
    PolyLine,
    Highlight,
    Underline,
    Squiggly,
    Strikeout,
    Stamp,
    Caret,
    Ink,
    Popup,
    FileAttachment,
    Sound,
    Movie,
    Widget,
    Screen,
    PrinterMark,
    TrapNet,
    Watermark,
    ThreeD,
    RichMedia,
    XfaWidget,
    Redact,
}

impl AnnotationSubtype {
    pub fn from_raw(raw: i32) -> Self {
        match raw {
            1 => Self::Text,
            2 => Self::Link,
            3 => Self::FreeText,
            4 => Self::Line,
            5 => Self::Square,
            6 => Self::Circle,
            7 => Self::Polygon,
            8 => Self::PolyLine,
            9 => Self::Highlight,
            10 => Self::Underline,
            11 => Self::Squiggly,
            12 => Self::Strikeout,
            13 => Self::Stamp,
            14 => Self::Caret,
            15 => Self::Ink,
            16 => Self::Popup,
            17 => Self::FileAttachment,
            18 => Self::Sound,
            19 => Self::Movie,
            20 => Self::Widget,
            21 => Self::Screen,
            22 => Self::PrinterMark,
            23 => Self::TrapNet,
            24 => Self::Watermark,
            25 => Self::ThreeD,
            26 => Self::RichMedia,
            27 => Self::XfaWidget,
            28 => Self::Redact,
            _ => Self::Unknown,
        }
    }

    pub fn as_raw(self) -> i32 {
        match self {
            Self::Unknown => 0,
            Self::Text => 1,
            Self::Link => 2,
            Self::FreeText => 3,
            Self::Line => 4,
            Self::Square => 5,
            Self::Circle => 6,
            Self::Polygon => 7,
            Self::PolyLine => 8,
            Self::Highlight => 9,
            Self::Underline => 10,
            Self::Squiggly => 11,
            Self::Strikeout => 12,
            Self::Stamp => 13,
            Self::Caret => 14,
            Self::Ink => 15,
            Self::Popup => 16,
            Self::FileAttachment => 17,
            Self::Sound => 18,
            Self::Movie => 19,
            Self::Widget => 20,
            Self::Screen => 21,
            Self::PrinterMark => 22,
            Self::TrapNet => 23,
            Self::Watermark => 24,
            Self::ThreeD => 25,
            Self::RichMedia => 26,
            Self::XfaWidget => 27,
            Self::Redact => 28,
        }
    }
}

/// A borrowed view of one annotation. The handle belongs to the page; it is
/// not closed here (PDFium closes annotations with the page).
pub struct Annotation<'page> {
    pub(crate) handle: NonNull<sys::fpdf_annotation_t__>,
    pub subtype: AnnotationSubtype,
    pub rect: Rect,
    _page: PhantomData<&'page ()>,
}

impl<'page> Annotation<'page> {
    pub(crate) fn load(handle: NonNull<sys::fpdf_annotation_t__>) -> Self {
        let subtype = serial::with_pdfium(|| unsafe {
            AnnotationSubtype::from_raw(sys::FPDFAnnot_GetSubtype(handle.as_ptr()))
        });
        let rect = serial::with_pdfium(|| unsafe {
            let mut r = sys::FS_RECTF {
                left: 0.0,
                top: 0.0,
                right: 0.0,
                bottom: 0.0,
            };
            if sys::FPDFAnnot_GetRect(handle.as_ptr(), &mut r) == 0 {
                return Rect::default();
            }
            Rect::new(r.left, r.bottom, r.right - r.left, r.top - r.bottom)
        });
        Self {
            handle,
            subtype,
            rect,
            _page: PhantomData,
        }
    }

    pub fn string_value(&self, key: &str) -> Result<String> {
        let key = std::ffi::CString::new(key).map_err(|_| EngineError::new("invalid annot key"))?;
        let len = serial::with_pdfium(|| unsafe {
            sys::FPDFAnnot_GetStringValue(
                self.handle.as_ptr(),
                key.as_ptr(),
                std::ptr::null_mut(),
                0,
            )
        });
        if len == 0 {
            return Ok(String::new());
        }
        let mut units = vec![0u16; len as usize];
        serial::with_pdfium(|| unsafe {
            sys::FPDFAnnot_GetStringValue(
                self.handle.as_ptr(),
                key.as_ptr(),
                units.as_mut_ptr(),
                len,
            );
        });
        while units.last() == Some(&0) {
            units.pop();
        }
        Ok(String::from_utf16_lossy(&units))
    }

    pub fn set_string_value(&self, key: &str, value: &str) -> Result<()> {
        let key = std::ffi::CString::new(key).map_err(|_| EngineError::new("invalid annot key"))?;
        let wide = wide::to_pdfium(value);
        let ok = serial::with_pdfium(|| unsafe {
            sys::FPDFAnnot_SetStringValue(self.handle.as_ptr(), key.as_ptr(), wide.as_ptr())
        });
        if ok == 0 {
            return Err(EngineError::new("FPDFAnnot_SetStringValue failed"));
        }
        Ok(())
    }

    pub fn set_rect(&mut self, rect: Rect) -> Result<()> {
        let fs = sys::FS_RECTF {
            left: rect.origin.x,
            bottom: rect.origin.y,
            right: rect.origin.x + rect.size.width,
            top: rect.origin.y + rect.size.height,
        };
        let ok =
            serial::with_pdfium(|| unsafe { sys::FPDFAnnot_SetRect(self.handle.as_ptr(), &fs) });
        if ok == 0 {
            return Err(EngineError::new("FPDFAnnot_SetRect failed"));
        }
        self.rect = rect;
        Ok(())
    }

    /// RGBA components (0..255) for a given annotation color type.
    pub fn color(&self, color_type: u32) -> Option<(u32, u32, u32, u32)> {
        let mut r = 0;
        let mut g = 0;
        let mut b = 0;
        let mut a = 0;
        let ok = serial::with_pdfium(|| unsafe {
            sys::FPDFAnnot_GetColor(
                self.handle.as_ptr(),
                color_type,
                &mut r,
                &mut g,
                &mut b,
                &mut a,
            )
        });
        if ok == 0 {
            None
        } else {
            Some((r, g, b, a))
        }
    }

    pub fn quad_count(&self) -> usize {
        serial::with_pdfium(|| unsafe {
            sys::FPDFAnnot_CountAttachmentPoints(self.handle.as_ptr())
        })
    }

    /// A quad as `[x1, y1, x2, y2, x3, y3, x4, y4]`.
    pub fn quad(&self, index: usize) -> Option<[f32; 8]> {
        let mut q = sys::FS_QUADPOINTSF {
            x1: 0.0,
            y1: 0.0,
            x2: 0.0,
            y2: 0.0,
            x3: 0.0,
            y3: 0.0,
            x4: 0.0,
            y4: 0.0,
        };
        let ok = serial::with_pdfium(|| unsafe {
            sys::FPDFAnnot_GetAttachmentPoints(self.handle.as_ptr(), index, &mut q)
        });
        if ok == 0 {
            None
        } else {
            Some([q.x1, q.y1, q.x2, q.y2, q.x3, q.y3, q.x4, q.y4])
        }
    }
}
