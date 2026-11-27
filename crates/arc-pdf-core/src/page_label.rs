//! Logical page labels, ported from ArcPDFKit `Models/PageLabel.swift`.
//!
//! Raw values match the `FPDFPAGELABEL_STYLE` C enum so ranges cross the FFI
//! boundary unchanged.

use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum PageLabelStyle {
    NoNumber = 0,
    Decimal = 1,
    UpperRoman = 2,
    LowerRoman = 3,
    UpperAlpha = 4,
    LowerAlpha = 5,
}

impl PageLabelStyle {
    /// Format a 1-based number exactly like PDFium's `GetLabelNumPortion`.
    pub fn format(self, number: i64) -> String {
        match self {
            Self::NoNumber => String::new(),
            Self::Decimal => number.to_string(),
            Self::UpperRoman => roman(number).to_uppercase(),
            Self::LowerRoman => roman(number),
            Self::UpperAlpha => letters(number).to_uppercase(),
            Self::LowerAlpha => letters(number),
        }
    }
}

fn roman(n: i64) -> String {
    const VALUES: [i64; 13] = [1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1];
    const SYMBOLS: [&str; 13] = [
        "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i",
    ];
    let mut n = n % 1_000_000;
    if n <= 0 {
        return String::new();
    }
    let mut out = String::new();
    for (value, symbol) in VALUES.into_iter().zip(SYMBOLS) {
        while n >= value {
            n -= value;
            out.push_str(symbol);
        }
    }
    out
}

fn letters(n: i64) -> String {
    if n == 0 {
        return String::new();
    }
    let zero_based = n - 1;
    let count = ((zero_based / 26 + 1) % 1000) as usize;
    let letter = (b'a' + (zero_based % 26) as u8) as char;
    std::iter::repeat_n(letter, count).collect()
}

/// A contiguous page-label range: pages `[first_index, next.first_index)`
/// share one numbering rule, with `start_number` numbering `first_index`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct PageLabelRange {
    pub first_index: usize,
    pub prefix: String,
    pub style: PageLabelStyle,
    pub start_number: i64,
}

impl PageLabelRange {
    pub fn new(
        first_index: usize,
        prefix: impl Into<String>,
        style: PageLabelStyle,
        start_number: i64,
    ) -> Self {
        Self {
            first_index,
            prefix: prefix.into(),
            style,
            start_number,
        }
    }

    /// `label = prefix + style(page_index - first_index + start_number)`.
    pub fn label(&self, index: usize) -> String {
        let n = index as i64 - self.first_index as i64 + self.start_number;
        format!("{}{}", self.prefix, self.style.format(n))
    }

    /// A plain decimal range starting at the physical page number equals no label.
    pub fn is_default(&self, index: usize) -> bool {
        self.style == PageLabelStyle::Decimal
            && self.first_index == index
            && self.start_number == index as i64 + 1
            && self.prefix.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn formats_match_pdfium_semantics() {
        assert_eq!(PageLabelStyle::NoNumber.format(42), "");
        assert_eq!(PageLabelStyle::Decimal.format(42), "42");
        assert_eq!(PageLabelStyle::LowerRoman.format(4), "iv");
        assert_eq!(PageLabelStyle::UpperRoman.format(1994), "MCMXCIV");
        assert_eq!(PageLabelStyle::LowerAlpha.format(1), "a");
        assert_eq!(PageLabelStyle::LowerAlpha.format(27), "aa");
        assert_eq!(PageLabelStyle::UpperAlpha.format(27), "AA");
    }

    #[test]
    fn range_label_and_default() {
        let range = PageLabelRange::new(3, "Ch.", PageLabelStyle::LowerRoman, 1);
        assert_eq!(range.label(3), "Ch.i");
        assert_eq!(range.label(5), "Ch.iii");
        let plain = PageLabelRange::new(0, "", PageLabelStyle::Decimal, 1);
        assert!(plain.is_default(0));
        assert!(!range.is_default(3));
    }
}
