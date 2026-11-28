//! Lightweight leveled logging for the Arc PDF app.
//!
//! Mirrors ArcPDFKit's `Logger.swift`: debug/info/warn are compiled out in
//! release builds and carry file:line automatically; error is always emitted.
//! Messages go through the `log` facade and are installed by the app with a
//! stderr logger.

pub use log::{debug, error, info, warn};

/// Install a simple stderr logger. Safe to call more than once.
pub fn init() {
    #[cfg(debug_assertions)]
    let level = log::LevelFilter::Debug;
    #[cfg(not(debug_assertions))]
    let level = log::LevelFilter::Error;

    let _ = log::set_boxed_logger(Box::new(Logger));
    log::set_max_level(level);
}

struct Logger;

impl log::Log for Logger {
    fn enabled(&self, metadata: &log::Metadata<'_>) -> bool {
        metadata.level() <= log::max_level()
    }

    fn log(&self, record: &log::Record<'_>) {
        if !self.enabled(record.metadata()) {
            return;
        }
        let file = record
            .file()
            .and_then(|path| path.rsplit('/').next())
            .unwrap_or("unknown");
        let prefix = match record.level() {
            log::Level::Error => "❌",
            log::Level::Warn => "⚠️",
            log::Level::Info => "ℹ️",
            log::Level::Debug => "•",
            log::Level::Trace => "•",
        };
        eprintln!(
            "{} {}:{} {}",
            prefix,
            file,
            record.line().unwrap_or(0),
            record.args()
        );
    }

    fn flush(&self) {}
}
