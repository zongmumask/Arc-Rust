//! Process-wide serial boundary for every PDFium call.
//!
//! ArcPDFKit routes all FPDF access through one global `pdfiumQueue` because
//! PDFium keeps process-global, unsynchronised state (notably the font
//! mapper). Rust wrappers are `!Send`, but different documents could still be
//! used from different threads, so calls are funnelled through one lock.
//!
//! The lock is reentrant: wrapper methods compose (`Document::render_page`
//! loads a page and queries page size, `TextPage::find` asks for rects), so a
//! plain `Mutex` would deadlock. Nested calls on the same thread reuse the
//! lock; other threads wait until the outermost call finishes.

use std::cell::Cell;
use std::sync::{Mutex, MutexGuard, OnceLock};

static PDFIUM_LOCK: OnceLock<Mutex<()>> = OnceLock::new();

thread_local! {
    static LOCK_DEPTH: Cell<usize> = const { Cell::new(0) };
}

struct PdfiumGuard {
    _held: Option<MutexGuard<'static, ()>>,
}

impl PdfiumGuard {
    fn enter() -> Self {
        if LOCK_DEPTH.with(|depth| depth.get()) > 0 {
            LOCK_DEPTH.with(|depth| depth.set(depth.get() + 1));
            return Self { _held: None };
        }

        let mutex = PDFIUM_LOCK.get_or_init(|| Mutex::new(()));
        let guard = mutex
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        LOCK_DEPTH.with(|depth| depth.set(1));
        Self { _held: Some(guard) }
    }
}

impl Drop for PdfiumGuard {
    fn drop(&mut self) {
        LOCK_DEPTH.with(|depth| {
            let next = depth.get().saturating_sub(1);
            depth.set(next);
        });
    }
}

/// Run a closure while holding the process-wide PDFium boundary.
pub(crate) fn with_pdfium<R>(f: impl FnOnce() -> R) -> R {
    let _guard = PdfiumGuard::enter();
    f()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::Arc;
    use std::thread;

    #[test]
    fn nested_calls_share_one_boundary() {
        let active = Arc::new(AtomicUsize::new(0));
        let peak = Arc::new(AtomicUsize::new(0));

        with_pdfium(|| {
            with_pdfium(|| {
                let now = active.fetch_add(1, Ordering::SeqCst) + 1;
                peak.fetch_max(now, Ordering::SeqCst);
                active.fetch_sub(1, Ordering::SeqCst);
            });
            assert_eq!(peak.load(Ordering::SeqCst), 1);
        });
    }

    #[test]
    fn other_threads_wait_for_the_boundary() {
        let started = Arc::new(std::sync::Barrier::new(2));
        let counter = Arc::new(AtomicUsize::new(0));
        let other = {
            let started = started.clone();
            let counter = counter.clone();
            thread::spawn(move || {
                started.wait();
                with_pdfium(|| {
                    counter.fetch_add(1, Ordering::SeqCst);
                });
            })
        };

        with_pdfium(|| {
            started.wait();
            assert_eq!(counter.load(Ordering::SeqCst), 0);
            thread::yield_now();
        });
        other.join().expect("worker thread");
        assert_eq!(counter.load(Ordering::SeqCst), 1);
    }
}
