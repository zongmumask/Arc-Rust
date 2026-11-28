//! Per-page render state and tile keys.
//!
//! A `PageState` lives only while its page is in (or recently near) the
//! viewport. Its tile map is dropped with the page, which is the memory bound
//! for high-resolution tiles.

use super::image::RasterImage;
use crate::actor::RenderGuard;
use std::collections::HashMap;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct TileKey {
    pub page: u32,
    pub col: i32,
    pub row: i32,
}

#[derive(Default)]
pub struct PageState {
    pub preview: Option<RasterImage>,
    pub preview_guard: Option<RenderGuard>,
    pub tiles: HashMap<TileKey, RasterImage>,
    pub tile_guards: HashMap<TileKey, RenderGuard>,
    pub in_flight_preview: bool,
    pub pending_removal: bool,
}
