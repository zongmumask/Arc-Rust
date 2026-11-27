//! UTF-16LE conversions for PDFium wide strings.
//!
//! PDFium expects UTF-16LE buffers with an explicit byte length; Swift's
//! `FPDFWideString` helper did the same conversion before handing data to FPDF.

pub(crate) fn to_pdfium(text: &str) -> Vec<u16> {
    let mut units: Vec<u16> = text.encode_utf16().collect();
    units.push(0);
    units
}

#[allow(dead_code)]
pub(crate) unsafe fn from_pdfium(data: &[u8]) -> String {
    let mut units = data
        .chunks_exact(2)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
        .collect::<Vec<_>>();
    while units.last() == Some(&0) {
        units.pop();
    }
    String::from_utf16_lossy(&units)
}
