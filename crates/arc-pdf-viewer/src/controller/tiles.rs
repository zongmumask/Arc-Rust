//! High-resolution tile requests for the strict visible rect.
//!
//! Tiles are 128pt page-space cells. Each request carries a guard that is
//! invalidated when the cell leaves the viewport; the engine re-checks it
//! immediately before touching PDFium.

use super::image::RasterImage;
use super::page_state::{PageState, TileKey};
use crate::actor::{PdfiumActor, RenderGuard};
use arc_pdf_core::{PageId, Rect};
use arc_pdf_render::{ScaleBucket, TileGrid, Viewport};
use gpui::{AppContext, Context, Window};
use std::collections::HashMap;

pub(super) fn request_tiles<T>(
    viewport: &Viewport,
    zoom: f32,
    page_frames: &[crate::geometry::PageFrame],
    actor: &PdfiumActor,
    window: &mut Window,
    cx: &mut Context<T>,
    pages: &mut HashMap<usize, PageState>,
) where
    T: super::ReaderState + 'static,
{
    let scale = ScaleBucket::from_scale(zoom).scale();
    let visible_pt = viewport.visible;
    let mut requests = Vec::new();
    for frame in page_frames {
        if !visible_pt.intersects(frame.rect) {
            continue;
        }
        let local = Rect::new(
            visible_pt.origin.x - frame.rect.origin.x,
            visible_pt.origin.y - frame.rect.origin.y,
            visible_pt.size.width,
            visible_pt.size.height,
        );
        let page_rect = Rect::new(0.0, 0.0, frame.size_pt.width, frame.size_pt.height);
        let local_viewport = Viewport::new(local);
        let state = pages.entry(frame.index).or_default();
        for (coord, rect) in TileGrid::visible_tiles(page_rect, &local_viewport) {
            let key = TileKey {
                page: frame.index as u32,
                col: coord.col,
                row: coord.row,
            };
            if state.tiles.contains_key(&key) || state.tile_guards.contains_key(&key) {
                continue;
            }
            let guard = RenderGuard::new(true);
            state.tile_guards.insert(key, guard.clone());
            requests.push((key, rect, guard));
        }
    }
    for (key, rect, guard) in requests {
        let actor = actor.clone();
        let future = cx.spawn(async move |this, cx| {
            let result = actor
                .render_region(PageId(key.page), rect, scale, guard.clone())
                .await;
            let image = match result {
                Ok(Ok(Some(raster))) if guard.valid() => Some(RasterImage::from_raster(&raster)),
                _ => None,
            };
            if let Some(this) = this.upgrade() {
                cx.update_entity(&this, |reader: &mut T, _| {
                    let state = reader.pages_mut().entry(key.page as usize).or_default();
                    state.tile_guards.remove(&key);
                    if let Some(image) = image {
                        state.tiles.insert(key, image);
                    }
                });
            }
        });
        window.spawn(&cx, async move |_| future.await).detach();
    }
}
