//! Watermark write/read/remove wrappers over ArcPDFium extensions.
//!
//! The pure watermark spec lives in `arc_pdf_core::WatermarkDefinition`;
//! this module only translates it into the C ABI and reads envelopes back.

use super::{serial, wide, EngineError, Result};
use arc_pdf_core::WatermarkDefinition;
use arc_pdfium_ext as ext;
use std::ffi::CString;
use std::os::raw::c_char;

/// C-side reconstructed watermark envelope.
#[derive(Clone, Debug, PartialEq)]
pub struct WatermarkInfo {
    pub guid: String,
    pub text: String,
    pub font_name: String,
    pub font_size: f32,
    pub rotation_deg: f32,
    pub opacity: f32,
    pub red: f32,
    pub green: f32,
    pub blue: f32,
    pub tiles_x: i32,
    pub tiles_y: i32,
    pub width_pt: f32,
    pub height_pt: f32,
    pub is_image: bool,
}

/// Add an editable text watermark to a page; returns the Form object number.
pub fn add_text_watermark(page: &super::Page<'_>, definition: &WatermarkDefinition) -> Result<i32> {
    let text = definition.text.as_deref().unwrap_or("");
    let guid = wide::to_pdfium(&definition.id);
    let wide_text = wide::to_pdfium(text);
    let font_name = definition
        .font
        .as_ref()
        .map(|f| f.postscript_name.as_str())
        .unwrap_or("Helvetica");
    let c_font = CString::new(font_name).map_err(|_| EngineError::new("invalid font name"))?;
    let color = definition.color.as_ref();
    let (tiles_x, tiles_y) = match definition.layout {
        arc_pdf_core::WatermarkLayout::Single => (1, 1),
        arc_pdf_core::WatermarkLayout::Grid => (3, 3),
    };
    let step = definition
        .font
        .as_ref()
        .map(|f| f.rel_size * 4.0)
        .unwrap_or(4.0) as f32;
    let obj = serial::with_pdfium(|| unsafe {
        ext::FPDFPage_AddWatermark(
            page_handle(page),
            doc_handle(page),
            guid.as_ptr(),
            wide_text.as_ptr(),
            c_font.as_ptr(),
            definition
                .font
                .as_ref()
                .map(|f| f.rel_size as f32)
                .unwrap_or(24.0),
            definition.rotation as f32,
            definition.opacity as f32,
            color.map(|c| c.red as f32).unwrap_or(0.5),
            color.map(|c| c.green as f32).unwrap_or(0.5),
            color.map(|c| c.blue as f32).unwrap_or(0.5),
            tiles_x,
            tiles_y,
            step,
            step,
            definition.anchor_x as f32,
            definition.anchor_y as f32,
            definition.offset_x as f32,
            definition.offset_y as f32,
            definition.on_top as i32,
            0,
            0,
        )
    });
    if obj <= 0 {
        return Err(EngineError::new("FPDFPage_AddWatermark failed"));
    }
    Ok(obj)
}

/// Read all watermark envelopes currently on a page.
pub fn watermarks(page: &super::Page<'_>) -> Vec<WatermarkInfo> {
    serial::with_pdfium(|| {
        let count = unsafe { ext::FPDFPage_GetWatermarkCount(page_handle(page)) }.max(0);
        (0..count)
            .filter_map(|i| {
                let mut raw: ext::FPDFExt_Watermark = unsafe { std::mem::zeroed() };
                let ok = unsafe { ext::FPDFPage_GetWatermarkAt(page_handle(page), i, &mut raw) };
                if ok == 0 {
                    return None;
                }
                let info = WatermarkInfo {
                    guid: c_string(raw.guid),
                    text: c_string(raw.text),
                    font_name: c_string(raw.font_name),
                    font_size: raw.font_size,
                    rotation_deg: raw.rotation_deg,
                    opacity: raw.opacity,
                    red: raw.red,
                    green: raw.green,
                    blue: raw.blue,
                    tiles_x: raw.tiles_x,
                    tiles_y: raw.tiles_y,
                    width_pt: raw.width_pt,
                    height_pt: raw.height_pt,
                    is_image: raw.watermark_type == 1,
                };
                unsafe { ext::FPDFPage_FreeWatermark(&mut raw) };
                Some(info)
            })
            .collect()
    })
}

pub fn remove_watermarks(page: &super::Page<'_>) -> Result<()> {
    let ok = serial::with_pdfium(|| unsafe { ext::FPDFPage_RemoveWatermark(page_handle(page)) });
    if ok == 0 {
        return Err(EngineError::new("FPDFPage_RemoveWatermark failed"));
    }
    Ok(())
}

fn c_string(ptr: *mut c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    unsafe { std::ffi::CStr::from_ptr(ptr) }
        .to_string_lossy()
        .into_owned()
}

fn page_handle(page: &super::Page<'_>) -> ext::FPDF_PAGE {
    page.handle.as_ptr() as ext::FPDF_PAGE
}

fn doc_handle(page: &super::Page<'_>) -> ext::FPDF_DOCUMENT {
    page.document_handle() as ext::FPDF_DOCUMENT
}
