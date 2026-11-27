//! OCR data model and recognition seam, ported from ArcPDFKit `OCR/*`.
//!
//! The recognizer itself is platform-specific (Vision on macOS); this module
//! owns the pure data types and the `TextSource` behaviour built from OCR
//! boxes so scanned-page search can share the same kernel paths.

use arc_pdf_core::Rect;

/// One recognized word, in PDF page user space.
#[derive(Clone, Debug, PartialEq)]
pub struct OcrTextBox {
    pub text: String,
    pub rect: Rect,
    pub font_size: f32,
}

/// Where an OCR character came from; used by downstream text models.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OcrTextSourceKind {
    Ocr,
    Pdfium,
}

/// An OCR-derived text source: chars are the concatenated boxes with single
/// spaces between them, and each char's rect is its share of a box's width.
#[derive(Clone, Debug)]
pub struct OcrText {
    boxes: Vec<OcrTextBox>,
    chars: Vec<char>,
    offsets: Vec<usize>,
    last_rects: Vec<Rect>,
}

impl OcrText {
    pub fn new(boxes: Vec<OcrTextBox>) -> Self {
        let mut chars = Vec::new();
        let mut offsets = Vec::with_capacity(boxes.len());
        for (i, b) in boxes.iter().enumerate() {
            if i > 0 {
                chars.push(' ');
            }
            offsets.push(chars.len());
            chars.extend(b.text.chars());
        }
        Self {
            boxes,
            chars,
            offsets,
            last_rects: Vec::new(),
        }
    }

    pub fn char_count(&self) -> usize {
        self.chars.len()
    }

    pub fn char_at(&self, index: usize) -> Option<char> {
        self.chars.get(index).copied()
    }

    pub fn font_size_at(&self, index: usize) -> f32 {
        self.box_for(index).map(|b| b.font_size).unwrap_or(0.0)
    }

    fn box_for(&self, index: usize) -> Option<&OcrTextBox> {
        self.offsets
            .iter()
            .zip(self.boxes.iter())
            .find(|(start, b)| index >= **start && index < **start + b.text.chars().count())
            .map(|(_, b)| b)
    }

    pub fn rect_at(&self, index: usize) -> Option<Rect> {
        let (i, (_, b)) = self
            .offsets
            .iter()
            .zip(self.boxes.iter())
            .enumerate()
            .find(|(_, (start, b))| index >= **start && index < **start + b.text.chars().count())?;
        let count = b.text.chars().count();
        if count == 0 {
            return None;
        }
        let char_width = b.rect.size.width / count as f32;
        Some(Rect::new(
            b.rect.origin.x + (index - self.offsets[i]) as f32 * char_width,
            b.rect.origin.y,
            char_width,
            b.rect.size.height,
        ))
    }

    /// Rects for a character range, mirroring OCRTextSource.countRects.
    pub fn rects(&mut self, start: usize, count: usize) -> usize {
        self.last_rects.clear();
        if start >= self.char_count() {
            return 0;
        }
        let end = (start + count).min(self.char_count());
        for i in start..end {
            if let Some(r) = self.rect_at(i) {
                self.last_rects.push(r);
            }
        }
        self.last_rects.len()
    }

    pub fn last_rects(&self) -> &[Rect] {
        &self.last_rects
    }

    /// Full text with one space between words.
    pub fn full_text(&self) -> String {
        self.chars.iter().collect()
    }
}

/// Host-injected recognizer. Implementations bridge to Vision/macOS or any
/// other OCR backend; no platform framework enters this crate.
pub trait OcrRecognizer {
    fn recognize(&self, image_rgba: &[u8], width: usize, height: usize) -> Vec<OcrTextBox>;
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> OcrText {
        OcrText::new(vec![
            OcrTextBox {
                text: "Hi".into(),
                rect: Rect::new(10.0, 20.0, 20.0, 10.0),
                font_size: 10.0,
            },
            OcrTextBox {
                text: "there".into(),
                rect: Rect::new(40.0, 20.0, 50.0, 10.0),
                font_size: 12.0,
            },
        ])
    }

    #[test]
    fn ocr_text_maps_chars_and_rects() {
        let mut text = sample();
        assert_eq!(text.full_text(), "Hi there");
        assert_eq!(text.char_count(), 8);
        assert_eq!(text.char_at(2), Some(' '));
        assert_eq!(text.font_size_at(0), 10.0);
        assert_eq!(text.font_size_at(3), 12.0);
        let r = text.rect_at(1).expect("rect");
        assert!((r.origin.x - 20.0).abs() < 0.01);
        assert_eq!(text.rects(0, 2), 2);
        assert_eq!(text.last_rects().len(), 2);
    }
}
