//! Viewport-driven lifecycle: visible/prefetch sets, guards and 150ms removal.

use super::page_state::PageState;
use crate::geometry::{visible_pages, PageFrame};
use arc_pdf_core::Rect;
use arc_pdf_render::Viewport;
use gpui::{AppContext, Context, Task};
use std::collections::{HashMap, HashSet};
use std::time::Duration;

const REMOVE_DELAY: Duration = Duration::from_millis(150);

/// Update the shared guards for every in-flight page/tile render.
pub(super) fn update_guards(
    viewport: &Viewport,
    page_frames: &[PageFrame],
    pages: &mut HashMap<usize, PageState>,
) {
    for (index, state) in pages.iter() {
        let in_band = page_frames
            .get(*index)
            .is_some_and(|frame| viewport.prefetch().intersects(frame.rect));
        if let Some(guard) = &state.preview_guard {
            guard.set(in_band);
        }
        let local_visible = page_frames.get(*index).map(|frame| Rect::new(
            viewport.visible.origin.x - frame.rect.origin.x,
            viewport.visible.origin.y - frame.rect.origin.y,
            viewport.visible.size.width,
            viewport.visible.size.height,
        ));
        for (key, guard) in &state.tile_guards {
            let cell = Rect::new(
                key.col as f32 * crate::geometry::TILE_SIZE_PT,
                key.row as f32 * crate::geometry::TILE_SIZE_PT,
                crate::geometry::TILE_SIZE_PT,
                crate::geometry::TILE_SIZE_PT,
            );
            guard.set(local_visible.is_some_and(|rect| rect.intersects(cell)));
        }
    }
}

/// Compute page indices intersecting the extended prefetch band.
pub(super) fn wanted_pages(viewport: &Viewport, frames: &[PageFrame]) -> Vec<usize> {
    visible_pages(frames, viewport)
}

/// Schedule a debounced page removal. Recomputes visibility after the delay so
/// quick scroll reversals keep the existing page (and its tile cache).
pub(super) fn schedule_removal<T>(
    viewport: Viewport,
    page_frames: Vec<PageFrame>,
    wanted: HashSet<usize>,
    cx: &mut Context<T>,
) -> Task<()>
where
    T: super::ReaderState + 'static,
{
    let timer = cx.background_executor().timer(REMOVE_DELAY);
    cx.spawn(async move |this, cx| {
        timer.await;
        if let Some(this) = this.upgrade() {
            cx.update_entity(&this, |reader: &mut T, _| {
                reader.pages_mut().retain(|index, state| {
                    let in_band = page_frames
                        .get(*index)
                        .is_some_and(|frame| viewport.prefetch().intersects(frame.rect));
                    if !in_band {
                        state.preview_guard = None;
                        state.tile_guards.clear();
                    }
                    in_band || wanted.contains(index)
                });
            });
        }
    })
}
