//! Low-resolution whole-page preview requests.
//!
//! Previews are requested for every page intersecting the extended prefetch
//! band. The shared guard can invalidate a queued request before its PDFium
//! call starts; successful renders are stored as WebP bytes in the
//! document-scoped preview cache.

use super::image::RasterImage;
use super::page_state::PageState;
use crate::actor::{PdfiumActor, RenderGuard};
use crate::geometry::preview_scale;
use arc_pdf_core::PageId;
use gpui::{AppContext, Context, Window};

pub(super) fn request_preview<T>(
    index: usize,
    scale: f32,
    actor: PdfiumActor,
    window: &mut Window,
    cx: &mut Context<T>,
    pages: &mut std::collections::HashMap<usize, PageState>,
) where
    T: super::ReaderState + 'static,
{
    let state = pages.entry(index).or_default();
    if state.preview.is_some() || state.in_flight_preview {
        return;
    }
    state.in_flight_preview = true;
    let guard = RenderGuard::new(true);
    state.preview_guard = Some(guard.clone());
    let page = PageId(index as u32);
    cx.spawn_in(window, async move |this, cx| {
        let result = actor.render_page(page, scale, guard.clone()).await;
        let raster = match result {
            Ok(Ok(raster)) if guard.valid() => raster,
            _ => {
                if let Some(this) = this.upgrade() {
                    cx.update_entity(&this, |reader: &mut T, _| {
                        reader.pages_mut().get_mut(&(page.0 as usize)).map(|state| {
                            state.in_flight_preview = false;
                        });
                    });
                }
                return;
            }
        };
        let compressed = arc_pdf_render::encode_rgba(
            &raster.pixels,
            raster.width as u32,
            raster.height as u32,
        )
        .ok();
        let image = RasterImage::from_raster(&raster);
        if let Some(this) = this.upgrade() {
            cx.update_entity(&this, |reader: &mut T, _| {
                let state = reader.pages_mut().entry(page.0 as usize).or_default();
                state.in_flight_preview = false;
                state.preview = Some(image);
                if let Some(bytes) = compressed {
                    reader.preview_cache_mut().insert_compressed(page, bytes);
                }
            });
        }
    })
    .detach();
}

pub fn scale_for(width_pt: f32, height_pt: f32) -> f32 {
    preview_scale(arc_pdf_core::Size::new(width_pt, height_pt))
}
