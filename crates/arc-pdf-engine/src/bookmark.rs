//! Bookmark / destination navigation wrappers, ported from ArcPDFKit's
//! `Bookmark` and `Destination` handle classes.
//!
//! The FPDF handles are borrowed and only valid while the parent document is
//! alive. All calls run through the process-wide serial boundary.

use super::{serial, Document};
use arc_pdf_core::PageId;
use arc_pdfium_sys as sys;
use std::marker::PhantomData;
use std::ptr::NonNull;

/// Snapshot of one outline node, including its resolved title and children.
#[derive(Clone, Debug, PartialEq)]
pub struct Bookmark {
    pub title: String,
    pub dest: Option<Destination>,
    pub children: Vec<Bookmark>,
    handle: NonNull<sys::fpdf_bookmark_t__>,
}

impl Bookmark {
    /// Read the whole outline tree of a document.
    pub fn tree(document: &Document) -> Vec<Bookmark> {
        serial::with_pdfium(|| {
            let first = unsafe {
                sys::FPDFBookmark_GetFirstChild(document.handle.as_ptr(), std::ptr::null_mut())
            };
            read_siblings(document, first)
        })
    }

    fn from_handle(document: &Document, handle: NonNull<sys::fpdf_bookmark_t__>) -> Bookmark {
        let byte_len =
            unsafe { sys::FPDFBookmark_GetTitle(handle.as_ptr(), std::ptr::null_mut(), 0) };
        let mut units = vec![0u16; byte_len as usize / 2];
        if !units.is_empty() {
            unsafe {
                sys::FPDFBookmark_GetTitle(
                    handle.as_ptr(),
                    units.as_mut_ptr() as *mut std::os::raw::c_void,
                    byte_len,
                );
            }
        }
        while units.last() == Some(&0) {
            units.pop();
        }
        let title = String::from_utf16_lossy(&units);
        let dest = unsafe { sys::FPDFBookmark_GetDest(document.handle.as_ptr(), handle.as_ptr()) };
        let child =
            unsafe { sys::FPDFBookmark_GetFirstChild(document.handle.as_ptr(), handle.as_ptr()) };
        Bookmark {
            title,
            dest: NonNull::new(dest).map(|handle| Destination::from_handle(handle, document)),
            children: read_siblings(document, child),
            handle,
        }
    }
}

fn read_siblings(document: &Document, first: sys::FPDF_BOOKMARK) -> Vec<Bookmark> {
    let mut result = Vec::new();
    let mut current = first;
    while let Some(node) = NonNull::new(current) {
        result.push(Bookmark::from_handle(document, node));
        current =
            unsafe { sys::FPDFBookmark_GetNextSibling(document.handle.as_ptr(), node.as_ptr()) };
    }
    result
}

/// Snapshot of an `FPDF_DEST`: target page and optional PDF-space location.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Destination {
    page: Option<PageId>,
    x: Option<f32>,
    y: Option<f32>,
    handle: NonNull<sys::fpdf_dest_t__>,
    _doc: PhantomData<*const ()>,
}

impl Destination {
    fn from_handle(handle: NonNull<sys::fpdf_dest_t__>, document: &Document) -> Self {
        let page_index =
            unsafe { sys::FPDFDest_GetDestPageIndex(document.handle.as_ptr(), handle.as_ptr()) };
        let mut has_x = 0;
        let mut has_y = 0;
        let mut has_zoom = 0;
        let mut x: f32 = -1.0;
        let mut y: f32 = -1.0;
        let mut zoom: f32 = -1.0;
        unsafe {
            sys::FPDFDest_GetLocationInPage(
                handle.as_ptr(),
                &mut has_x,
                &mut has_y,
                &mut has_zoom,
                &mut x,
                &mut y,
                &mut zoom,
            );
        }
        Self {
            page: if page_index < 0 {
                None
            } else {
                Some(PageId(page_index as u32))
            },
            x: if has_x != 0 { Some(x) } else { None },
            y: if has_y != 0 { Some(y) } else { None },
            handle,
            _doc: PhantomData,
        }
    }

    pub fn page(&self) -> Option<PageId> {
        self.page
    }

    /// Destination point, with unspecified components reported as `-1.0`
    /// (matching ArcPDFKit's `getLocationInPage`).
    pub fn location(&self) -> (f32, f32) {
        (self.x.unwrap_or(-1.0), self.y.unwrap_or(-1.0))
    }
}
