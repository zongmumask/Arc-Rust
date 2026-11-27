//! OCG (optional content group) annotation layers, ported from ArcPDFKit's
//! `LayerManager` / `AnnotLayer` handle classes.
//!
//! Uses the ArcPDFium OCG C-extension API from `arc-pdfium-ext`; all calls
//! pass through the same process-wide serial boundary.

use super::{serial, EngineError, Result};
use arc_pdfium_ext as ext;
use std::ffi::CString;
use std::marker::PhantomData;
use std::ptr::NonNull;

/// A named annotation layer backed by an OCG dictionary.
#[derive(Clone, Debug, PartialEq)]
pub struct Layer {
    pub name: String,
    pub visible: bool,
    pub locked: bool,
    pub print: bool,
    pub export: bool,
    pub color: String,
    handle: NonNull<ext::fpdf_ocg__>,
}

/// Owns the layer list of a document; the borrowed document must outlive it.
pub struct LayerManager<'doc> {
    pub layers: Vec<Layer>,
    pub active: Option<usize>,
    _doc: PhantomData<&'doc ()>,
}

impl<'doc> LayerManager<'doc> {
    /// Load all OCG layers from the document catalog.
    pub fn load(document: &'doc super::Document) -> Self {
        serial::with_pdfium(|| {
            let count = unsafe { ext::FPDFOCG_GetCount(doc_handle(document)) }.max(0) as usize;
            let mut layers = Vec::with_capacity(count);
            for i in 0..count {
                let handle = unsafe { ext::FPDFOCG_Get(doc_handle(document), i as i32) };
                if let Some(handle) = NonNull::new(handle) {
                    layers.push(read_layer(handle, document));
                }
            }
            let active_handle = unsafe { ext::FPDFOCG_GetActiveLayer(doc_handle(document)) };
            let active = NonNull::new(active_handle).and_then(|h| {
                layers
                    .iter()
                    .position(|layer| layer.handle.as_ptr() == h.as_ptr())
            });
            Self {
                layers,
                active,
                _doc: PhantomData,
            }
        })
    }

    pub fn set_active(&mut self, document: &super::Document, index: usize) -> Result<()> {
        let handle = self
            .layers
            .get(index)
            .ok_or_else(|| EngineError::new("layer index out of range"))?
            .handle;
        let ok = serial::with_pdfium(|| unsafe {
            ext::FPDFOCG_SetActiveLayer(doc_handle(document), handle.as_ptr())
        });
        if ok == 0 {
            return Err(EngineError::new("FPDFOCG_SetActiveLayer failed"));
        }
        self.active = Some(index);
        Ok(())
    }

    pub fn clear_active(&mut self, document: &super::Document) -> Result<()> {
        let ok = serial::with_pdfium(|| unsafe {
            ext::FPDFOCG_SetActiveLayer(doc_handle(document), std::ptr::null_mut())
        });
        if ok == 0 {
            return Err(EngineError::new("FPDFOCG_SetActiveLayer failed"));
        }
        self.active = None;
        Ok(())
    }

    pub fn set_visible(
        &mut self,
        document: &super::Document,
        index: usize,
        visible: bool,
    ) -> Result<()> {
        let handle = self
            .layers
            .get(index)
            .ok_or_else(|| EngineError::new("layer index out of range"))?
            .handle;
        let ok = serial::with_pdfium(|| unsafe {
            ext::FPDFOCG_SetVisible(doc_handle(document), handle.as_ptr(), visible as i32)
        });
        if ok == 0 {
            return Err(EngineError::new("FPDFOCG_SetVisible failed"));
        }
        self.layers[index].visible = visible;
        Ok(())
    }

    pub fn rename(&mut self, index: usize, name: &str) -> Result<()> {
        let handle = self
            .layers
            .get(index)
            .ok_or_else(|| EngineError::new("layer index out of range"))?
            .handle;
        let wide = super::wide::to_pdfium(name);
        let ok =
            serial::with_pdfium(|| unsafe { ext::FPDFOCG_SetName(handle.as_ptr(), wide.as_ptr()) });
        if ok == 0 {
            return Err(EngineError::new("FPDFOCG_SetName failed"));
        }
        self.layers[index].name = name.to_string();
        Ok(())
    }

    pub fn add(&mut self, document: &super::Document, name: &str) -> Result<usize> {
        let wide = super::wide::to_pdfium(name);
        let handle = serial::with_pdfium(|| unsafe {
            ext::FPDFOCG_Create(doc_handle(document), wide.as_ptr())
        });
        let handle =
            NonNull::new(handle).ok_or_else(|| EngineError::new("FPDFOCG_Create failed"))?;
        let layer = serial::with_pdfium(|| read_layer(handle, document));
        self.layers.push(layer);
        Ok(self.layers.len() - 1)
    }

    pub fn remove(&mut self, document: &super::Document, index: usize) -> Result<()> {
        if index >= self.layers.len() {
            return Err(EngineError::new("layer index out of range"));
        }
        let handle = self.layers[index].handle;
        let ok = serial::with_pdfium(|| unsafe {
            ext::FPDFOCG_Delete(doc_handle(document), handle.as_ptr())
        });
        if ok == 0 {
            return Err(EngineError::new("FPDFOCG_Delete failed"));
        }
        self.layers.remove(index);
        if self.active == Some(index) {
            self.active = None;
        }
        Ok(())
    }

    /// Reorder layers by indices into the current `layers` vector.
    pub fn reorder(&mut self, document: &super::Document, order: &[usize]) -> Result<()> {
        if order.len() != self.layers.len() {
            return Err(EngineError::new("reorder must list every layer"));
        }
        let raw: Vec<i32> = order.iter().map(|&i| i as i32).collect();
        let ok = serial::with_pdfium(|| unsafe {
            ext::FPDFOCG_SetOrder(doc_handle(document), raw.as_ptr(), raw.len() as i32)
        });
        if ok == 0 {
            return Err(EngineError::new("FPDFOCG_SetOrder failed"));
        }
        let old = std::mem::take(&mut self.layers);
        self.layers = order.iter().map(|&i| old[i].clone()).collect();
        Ok(())
    }
}

fn read_layer(handle: NonNull<ext::fpdf_ocg__>, document: &super::Document) -> Layer {
    let len = unsafe { ext::FPDFOCG_GetName(handle.as_ptr(), std::ptr::null_mut(), 0) };
    let mut units = vec![0u16; len as usize / 2];
    if !units.is_empty() {
        unsafe {
            ext::FPDFOCG_GetName(handle.as_ptr(), units.as_mut_ptr(), len);
        }
    }
    while units.last() == Some(&0) {
        units.pop();
    }
    let name = String::from_utf16_lossy(&units);

    let color_len = unsafe { ext::FPDFOCG_GetColor(handle.as_ptr(), std::ptr::null_mut(), 0) };
    let mut color_bytes = vec![0u8; color_len as usize];
    if !color_bytes.is_empty() {
        unsafe {
            ext::FPDFOCG_GetColor(
                handle.as_ptr(),
                color_bytes.as_mut_ptr() as *mut std::os::raw::c_char,
                color_len,
            );
        }
    }
    while color_bytes.last() == Some(&0) {
        color_bytes.pop();
    }
    let color = String::from_utf8_lossy(&color_bytes).into_owned();

    Layer {
        name,
        visible: unsafe { ext::FPDFOCG_IsVisible(doc_handle(document), handle.as_ptr()) } != 0,
        locked: unsafe { ext::FPDFOCG_GetLocked(handle.as_ptr()) } != 0,
        print: unsafe { ext::FPDFOCG_GetPrintState(handle.as_ptr()) } != 0,
        export: unsafe { ext::FPDFOCG_GetExportState(handle.as_ptr()) } != 0,
        color,
        handle,
    }
}

/// Convenience: create a zero-initialized `FPDF_FORMFILLINFO` for hosts that
/// do not need custom callbacks (the struct is platform-specific and must be
/// allocated by the embedding layer).
#[allow(dead_code)]
pub fn zero_form_fill_info() -> arc_pdfium_sys::FPDF_FORMFILLINFO {
    // SAFETY: PDFium's C struct is a plain-old-data record; zeroed bytes are a
    // valid value for all pointer and integer fields.
    unsafe { std::mem::zeroed() }
}

/// Keep the `CString` import used for future color/name key conversion.
#[allow(dead_code)]
fn _cstring(s: &str) -> CString {
    CString::new(s).expect("nul-free string")
}

/// Cast the sys-owned document handle into the ext crate's identical C type.
fn doc_handle(document: &super::Document) -> ext::FPDF_DOCUMENT {
    document.handle.as_ptr() as ext::FPDF_DOCUMENT
}
