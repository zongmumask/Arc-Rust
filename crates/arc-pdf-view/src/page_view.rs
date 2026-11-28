//! `PageView`: one visible page's preview and high-resolution tiles.
//!
//! Mirrors ArcPDF-macOS `PageView`: a low-res full-page preview appears first,
//! then 128pt tiles for the strict visible region are rendered on the PDFium
//! actor. The tile map is dropped with this entity when the page leaves the
//! prefetch band, which bounds tile memory to nearby pages.

use arc_pdf_core::{PageId, Rect, Size as PdfSize};
use arc_pdf_engine::Raster;
use arc_pdf_log::warn;
use arc_pdf_render::{ScaleBucket, TileGrid, Viewport};
use arc_pdf_viewer::{PdfiumActor, RenderGuard, TILE_SIZE_PT};
use gpui::{AppContext, Context, RenderImage, Window};
use std::collections::HashMap;
use std::sync::Arc;

#[derive(Clone, Debug)]
pub struct RasterImage(pub Arc<RenderImage>);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct TileKey {
    pub col: i32,
    pub row: i32,
}

#[derive(Clone, Debug)]
pub struct TileImage {
    pub image: Arc<RenderImage>,
    pub rect: Rect,
}

fn image_from_raster(raster: &Raster) -> Arc<RenderImage> {
    let row_bytes = raster.width * 4;
    let mut bytes = vec![0u8; raster.width * raster.height * 4];
    for y in 0..raster.height {
        let src = y * raster.stride as usize;
        let dst = y * row_bytes;
        let count = (raster.stride as usize).min(row_bytes);
        bytes[dst..dst + count].copy_from_slice(&raster.pixels[src..src + count]);
    }
    let image = image::RgbaImage::from_raw(raster.width as u32, raster.height as u32, bytes)
        .expect("raster buffer size");
    Arc::new(RenderImage::new(vec![image::Frame::new(image)]))
}

pub struct PageView {
    index: usize,
    size_pt: PdfSize,
    preview: Option<RasterImage>,
    preview_guard: Option<RenderGuard>,
    tiles: HashMap<TileKey, TileImage>,
    tile_guards: HashMap<TileKey, RenderGuard>,
    in_flight_preview: bool,
}

impl PageView {
    pub fn new(index: usize, size_pt: PdfSize, cx: &mut Context<Self>) -> Self {
        let _ = cx;
        Self {
            index,
            size_pt,
            preview: None,
            preview_guard: None,
            tiles: HashMap::new(),
            tile_guards: HashMap::new(),
            in_flight_preview: false,
        }
    }

    pub fn index(&self) -> usize {
        self.index
    }

    pub fn size_pt(&self) -> PdfSize {
        self.size_pt
    }

    pub fn preview(&self) -> Option<&RasterImage> {
        self.preview.as_ref()
    }

    pub fn tiles(&self) -> impl Iterator<Item = (&TileKey, &TileImage)> {
        self.tiles.iter()
    }

    /// Request the low-res whole-page preview if it is not cached or in flight.
    /// `scale` comes from `preview_scale` (2x, capped at 1600 px per side).
    pub fn request_preview(&mut self, scale: f32, actor: PdfiumActor, cx: &mut Context<Self>) {
        if self.preview.is_some() || self.in_flight_preview {
            return;
        }
        self.in_flight_preview = true;
        let guard = RenderGuard::new(true);
        self.preview_guard = Some(guard.clone());
        let page = PageId(self.index as u32);
        cx.spawn(async move |this, cx| {
            let result = actor.render_page(page, scale, guard.clone()).await;
            let image = match result {
                Ok(Ok(raster)) if guard.valid() => Some(RasterImage(image_from_raster(&raster))),
                Ok(Ok(_)) => None,
                Ok(Err(error)) => {
                    warn!("preview failed page {}: {}", page.0 + 1, error);
                    None
                }
                Err(_) => {
                    warn!("preview actor closed page {}", page.0 + 1);
                    None
                }
            };
            if let Some(this) = this.upgrade() {
                cx.update_entity(&this, |page: &mut Self, _| {
                    page.in_flight_preview = false;
                    if let Some(image) = image {
                        page.preview = Some(image);
                    }
                });
            }
        })
        .detach();
    }

    /// Request high-res tiles intersecting the strict visible region.
    /// `local_visible` is in this page's coordinate space (points).
    pub fn request_tiles(
        &mut self,
        local_visible: Viewport,
        zoom: f32,
        actor: &PdfiumActor,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        // Match Swift: target scale includes backing ratio, and bucketizing
        // prevents zoom jitter from invalidating the cache on every frame.
        let scale = ScaleBucket::from_scale(zoom).scale();
        let page_rect = Rect::new(0.0, 0.0, self.size_pt.width, self.size_pt.height);
        let mut requests = Vec::new();
        for (coord, rect) in TileGrid::visible_tiles(page_rect, &local_visible) {
            let key = TileKey { col: coord.col, row: coord.row };
            if self.tiles.contains_key(&key) || self.tile_guards.contains_key(&key) {
                continue;
            }
            let guard = RenderGuard::new(true);
            self.tile_guards.insert(key, guard.clone());
            requests.push((key, rect, guard));
        }
        let page_id = PageId(self.index as u32);
        for (key, rect, guard) in requests {
            let actor = actor.clone();
            let future = cx.spawn(async move |this, cx| {
                let result = actor
                    .render_region(page_id, rect, scale, guard.clone())
                    .await;
                let image = match result {
                    Ok(Ok(Some(raster))) if guard.valid() => Some(TileImage {
                        image: image_from_raster(&raster),
                        rect,
                    }),
                    Ok(Ok(Some(_))) | Ok(Ok(None)) | Ok(Err(_)) | Err(_) => None,
                };
                if let Some(this) = this.upgrade() {
                    cx.update_entity(&this, |page: &mut Self, _| {
                        page.tile_guards.remove(&key);
                        if let Some(image) = image {
                            page.tiles.insert(key, image);
                        }
                    });
                }
            });
            window.spawn(&cx, async move |_| future.await).detach();
        }
    }

    /// Invalidate queued work when regions leave their visibility band.
    pub fn update_visibility(&mut self, in_prefetch: bool, local_visible: Option<Viewport>) {
        if let Some(guard) = &self.preview_guard {
            guard.set(in_prefetch);
        }
        for (key, guard) in &self.tile_guards {
            let cell = Rect::new(
                key.col as f32 * TILE_SIZE_PT,
                key.row as f32 * TILE_SIZE_PT,
                TILE_SIZE_PT,
                TILE_SIZE_PT,
            );
            guard.set(local_visible.is_some_and(|viewport| viewport.visible.intersects(cell)));
        }
    }

    pub fn drop_guards(&mut self) {
        self.preview_guard = None;
        self.tile_guards.clear();
    }
}
