//! Shared cancellation flag used by preview and tile render requests.
//!
//! The UI flips the flag when a page/tile leaves its visibility band; the PDFium
//! engine evaluates it immediately before calling FPDF, which is the critical
//! guard for stale work queued behind slow serial renders.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

#[derive(Clone, Debug, Default)]
pub struct RenderGuard(Arc<AtomicBool>);

impl RenderGuard {
    pub fn new(valid: bool) -> Self {
        Self(Arc::new(AtomicBool::new(valid)))
    }

    pub fn set(&self, valid: bool) {
        self.0.store(valid, Ordering::SeqCst);
    }

    pub fn valid(&self) -> bool {
        self.0.load(Ordering::SeqCst)
    }
}
