//! Canvas painting for previews and cached high-res tiles.

use super::image::RasterImage;
use super::page_state::TileKey;
use crate::geometry::{PageFrame, TILE_SIZE_PT};
use gpui::{
    point, px, size, Bounds, Corners, Pixels, Window,
};

pub(super) struct PaintState {
    pub zoom: f32,
    pub frames: Vec<PageFrame>,
    pub previews: Vec<(usize, RasterImage)>,
    pub tiles: Vec<(TileKey, RasterImage)>,
}

pub(super) fn paint(state: &PaintState, bounds: Bounds<Pixels>, window: &mut Window) {
    let zoom = state.zoom;
    for (index, image) in &state.previews {
        let Some(frame) = state.frames.get(*index) else {
            continue;
        };
        let origin = point(
            bounds.origin.x + px(frame.rect.origin.x * zoom),
            bounds.origin.y + px(frame.rect.origin.y * zoom),
        );
        let size = size(px(frame.rect.size.width * zoom), px(frame.rect.size.height * zoom));
        let target = Bounds::new(origin, size);
        if target.intersects(&bounds) {
            let _ = window.paint_image(
                target,
                Bounds::new(origin, size),
                Corners::default(),
                image.0.clone(),
                0,
                false,
            );
        }
    }
    for (key, image) in &state.tiles {
        let Some(frame) = state.frames.get(key.page as usize) else {
            continue;
        };
        let tile_origin = point(
            frame.rect.origin.x + key.col as f32 * TILE_SIZE_PT,
            frame.rect.origin.y + key.row as f32 * TILE_SIZE_PT,
        );
        let origin = point(
            bounds.origin.x + px(tile_origin.x * zoom),
            bounds.origin.y + px(tile_origin.y * zoom),
        );
        let size = size(px(TILE_SIZE_PT * zoom), px(TILE_SIZE_PT * zoom));
        let target = Bounds::new(origin, size);
        if target.intersects(&bounds) {
            let _ = window.paint_image(
                target,
                Bounds::new(origin, size),
                Corners::default(),
                image.0.clone(),
                0,
                false,
            );
        }
    }
}
