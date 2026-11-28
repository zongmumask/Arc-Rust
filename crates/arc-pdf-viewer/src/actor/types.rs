//! Shared result types for the PDFium worker.

use arc_pdf_core::Size;

#[derive(Debug, Clone)]
pub enum ActorError {
    Closed,
    Engine(String),
}

impl std::fmt::Display for ActorError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Closed => write!(f, "pdfium actor closed"),
            Self::Engine(message) => write!(f, "{message}"),
        }
    }
}

#[derive(Debug, Clone)]
pub struct DocumentInfo {
    pub page_count: usize,
    pub page_sizes: Vec<Size>,
}
