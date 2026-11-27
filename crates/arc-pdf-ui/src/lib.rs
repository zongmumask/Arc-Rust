//! Minimal GPUI views for the Arc PDF reader.

mod library;
#[cfg(target_os = "macos")]
mod native_sidebar;
mod pdf;
mod style;

pub use library::LibraryWindow;
pub use pdf::PdfWindow;
pub use style::{LIBRARY_DEFAULT_SIZE, LIBRARY_MIN_SIZE};
