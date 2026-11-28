//! Pure geometry helpers mirroring `DocumentView` in the Swift app.
//!
//! Coordinates are in unscaled PDF points, top-left origin. Zoom is applied
//! only when painting; tile keys and visibility calculations stay stable
//! across zoom changes.

use arc_pdf_core::{Rect, Size};
use arc_pdf_render::Viewport;

/// Vertical gap between pages (Swift production `DocumentView.lineSpacing`).
pub const LINE_SPACING_PT: f32 = 4.0;
/// Horizontal page margin.
pub const PAGE_MARGIN_PT: f32 = 2.0;
/// Bottom toolbar clearance supplied by PDFViewController.
pub const BOTTOM_INSET_PT: f32 = 52.0;
/// Tile cell size in points (Swift PageView.fixedTileSize).
pub const TILE_SIZE_PT: f32 = 128.0;
/// Preview base scale and pixel cap (Swift Constants.previewScale).
pub const PREVIEW_BASE_SCALE: f32 = 2.0;
pub const PREVIEW_MAX_PIXELS: f32 = 1600.0;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PageFrame {
    pub index: usize,
    pub rect: Rect,
    pub size_pt: Size,
}

/// Compute the vertically-stacked, horizontally-centered page frames.
pub fn page_frame(page_sizes: &[Size]) -> Vec<PageFrame> {
    let content_width = page_sizes
        .iter()
        .map(|size| size.width + PAGE_MARGIN_PT * 2.0)
        .fold(0.0f32, f32::max);
    let mut y = 0.0;
    page_sizes
        .iter()
        .enumerate()
        .map(|(index, size)| {
            let frame = Rect::new((content_width - size.width) * 0.5, y, size.width, size.height);
            y += size.height + LINE_SPACING_PT;
            PageFrame {
                index,
                rect: frame,
                size_pt: *size,
            }
        })
        .collect()
}

/// Pages whose frames intersect the extended prefetch band.
pub fn visible_pages(page_frames: &[PageFrame], viewport: &Viewport) -> Vec<usize> {
    let prefetch = viewport.prefetch();
    page_frames
        .iter()
        .filter(|frame| prefetch.intersects(frame.rect))
        .map(|frame| frame.index)
        .collect()
}

/// Pages whose frames intersect the strict visible rect.
pub fn strictly_visible_pages(page_frames: &[PageFrame], viewport: &Viewport) -> Vec<usize> {
    page_frames
        .iter()
        .filter(|frame| viewport.visible.intersects(frame.rect))
        .map(|frame| frame.index)
        .collect()
}

/// Preview scale: base 2x, capped so neither dimension exceeds 1600 px.
pub fn preview_scale(page_size: Size) -> f32 {
    let raw_width = page_size.width * PREVIEW_BASE_SCALE;
    let raw_height = page_size.height * PREVIEW_BASE_SCALE;
    let downscale = (PREVIEW_MAX_PIXELS / raw_width.max(raw_height)).min(1.0);
    PREVIEW_BASE_SCALE * downscale
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pages_are_stacked_and_centered() {
        let frames = page_frame(&[Size::new(100.0, 200.0), Size::new(200.0, 100.0)]);
        assert_eq!(frames[0].rect.origin.x, 52.0);
        assert_eq!(frames[0].rect.origin.y, 0.0);
        assert_eq!(frames[1].rect.origin.x, 2.0);
        assert_eq!(frames[1].rect.origin.y, 204.0);
    }

    #[test]
    fn prefetch_includes_pages_outside_strict_view() {
        let frames = page_frame(&[Size::new(100.0, 100.0), Size::new(100.0, 100.0)]);
        let viewport = Viewport::new(Rect::new(0.0, 0.0, 100.0, 100.0));
        assert_eq!(visible_pages(&frames, &viewport).len(), 2);
        assert_eq!(strictly_visible_pages(&frames, &viewport).len(), 1);
    }

    #[test]
    fn preview_scale_caps_large_pages() {
        let scale = preview_scale(Size::new(2000.0, 1000.0));
        assert!((scale - 0.8).abs() < 1e-5);
        assert_eq!(preview_scale(Size::new(100.0, 100.0)), 2.0);
    }
}
