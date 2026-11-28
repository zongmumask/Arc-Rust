//! Document-scoped, compressed preview cache with the Swift app's eviction
//! policy: WebP bytes, insertion timestamp, one oldest entry evicted when the
//! byte budget is exceeded. Reads decode off the UI thread.

use arc_pdf_core::PageId;
use arc_pdf_render::{decode_rgba, encode_rgba};
use std::collections::HashMap;
use std::time::Instant;

pub const PREVIEW_CACHE_BUDGET: usize = 1000 * 1024 * 1024;

#[derive(Clone, Debug)]
pub struct DecodeResult {
    pub width: u32,
    pub height: u32,
    pub pixels: Vec<u8>,
}

#[derive(Default)]
pub struct PreviewCache {
    entries: HashMap<PageId, (Vec<u8>, Instant)>,
    used: usize,
    budget: usize,
}

impl PreviewCache {
    pub fn new() -> Self {
        Self {
            entries: HashMap::new(),
            used: 0,
            budget: PREVIEW_CACHE_BUDGET,
        }
    }

    pub fn with_budget(budget: usize) -> Self {
        Self {
            entries: HashMap::new(),
            used: 0,
            budget,
        }
    }

    /// Compressed bytes. Encoding happens on the caller's (background) thread;
    /// the cache itself is owned by the UI entity and never crosses threads.
    pub fn insert_rgba(&mut self, page: PageId, pixels: &[u8], width: u32, height: u32) -> bool {
        let Ok(encoded) = encode_rgba(pixels, width, height) else {
            return false;
        };
        self.insert_compressed(page, encoded);
        true
    }

    pub fn insert_compressed(&mut self, page: PageId, bytes: Vec<u8>) {
        if let Some((old, _)) = self.entries.remove(&page) {
            self.used = self.used.saturating_sub(old.len());
        }
        self.used += bytes.len();
        self.entries.insert(page, (bytes, Instant::now()));
        if self.used > self.budget {
            self.evict_oldest();
        }
    }

    pub fn compressed(&self, page: PageId) -> Option<&[u8]> {
        self.entries.get(&page).map(|(bytes, _)| bytes.as_slice())
    }

    pub fn decode(page_bytes: &[u8]) -> Option<DecodeResult> {
        let (width, height, pixels) = decode_rgba(page_bytes).ok()?;
        Some(DecodeResult {
            width,
            height,
            pixels,
        })
    }

    pub fn remove(&mut self, page: PageId) {
        if let Some((old, _)) = self.entries.remove(&page) {
            self.used = self.used.saturating_sub(old.len());
        }
    }

    pub fn clear(&mut self) {
        self.entries.clear();
        self.used = 0;
    }

    pub fn used_bytes(&self) -> usize {
        self.used
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// Mirrors FPDFRenderCache.preview: one victim per insertion, oldest by
    /// insertion timestamp, no LRU touch on reads.
    fn evict_oldest(&mut self) {
        let oldest = self
            .entries
            .iter()
            .min_by_key(|(_, (_, timestamp))| *timestamp)
            .map(|(page, _)| *page);
        if let Some(page) = oldest {
            self.remove(page);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fake_webp(size: usize) -> Vec<u8> {
        vec![0x55; size]
    }

    #[test]
    fn evicts_one_oldest_entry_when_over_budget() {
        let mut cache = PreviewCache::with_budget(10);
        cache.insert_compressed(PageId(0), fake_webp(6));
        cache.insert_compressed(PageId(1), fake_webp(6));
        assert!(!cache.compressed(PageId(0)).is_some());
        assert!(cache.compressed(PageId(1)).is_some());
        assert_eq!(cache.len(), 1);
        assert_eq!(cache.used_bytes(), 6);
    }

    #[test]
    fn replacing_entry_adjusts_accounting() {
        let mut cache = PreviewCache::with_budget(20);
        cache.insert_compressed(PageId(0), fake_webp(8));
        cache.insert_compressed(PageId(0), fake_webp(4));
        assert_eq!(cache.used_bytes(), 4);
        assert_eq!(cache.len(), 1);
    }

    #[test]
    fn clear_drops_all_bytes() {
        let mut cache = PreviewCache::with_budget(20);
        cache.insert_compressed(PageId(0), fake_webp(8));
        cache.clear();
        assert!(cache.is_empty());
        assert_eq!(cache.used_bytes(), 0);
    }
}
