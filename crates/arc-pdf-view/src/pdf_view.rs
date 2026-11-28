//! `PdfView`: page geometry, scroll viewport, visible-page lifecycle and paint.
//!
//! Mirrors Swift `PDFView + DocumentView` responsibilities. Each page in the
//! prefetch band owns a `PageView` entity; the entity (and its tiles) dies
//! after 150ms outside the band.

use crate::page_view::PageView;
use arc_pdf_core::{Rect, Size as PdfSize};
use arc_pdf_render::Viewport;
use arc_pdf_viewer::{
    page_frame, preview_scale, visible_pages, BOTTOM_INSET_PT, PAGE_MARGIN_PT, PageFrame,
    PdfiumActor,
};
use gpui::{
    canvas, div, point, px, size, App, AppContext, Bounds, ClickEvent, Context,
    Corners, Entity, FocusHandle, Focusable, InteractiveElement, IntoElement, ParentElement, Render,
    ScrollEvent, ScrollHandle, StatefulInteractiveElement, Styled, Task, Window,
};
use std::collections::{HashMap, HashSet};
use std::time::Duration;

const ZOOM_MIN: f32 = 0.25;
const ZOOM_MAX: f32 = 8.0;
const REMOVE_DELAY: Duration = Duration::from_millis(150);

pub struct PdfView {
    actor: PdfiumActor,
    page_sizes: Vec<PdfSize>,
    frames: Vec<PageFrame>,
    pages: HashMap<usize, Entity<PageView>>,
    viewport: Viewport,
    zoom: f32,
    fit_width: bool,
    scroll_handle: ScrollHandle,
    focus: FocusHandle,
    removal_task: Option<Task<()>>,
}

impl PdfView {
    pub fn new(actor: PdfiumActor, cx: &mut Context<Self>) -> Self {
        Self {
            actor,
            page_sizes: Vec::new(),
            frames: Vec::new(),
            pages: HashMap::new(),
            viewport: Viewport::new(Rect::new(0.0, 0.0, 0.0, 0.0)),
            zoom: 1.0,
            fit_width: true,
            scroll_handle: ScrollHandle::new(),
            focus: cx.focus_handle(),
            removal_task: None,
        }
    }

    pub fn set_document(&mut self, sizes: Vec<PdfSize>, cx: &mut Context<Self>) {
        self.page_sizes = sizes;
        self.rebuild_frames();
        cx.notify();
    }

    /// Fit the widest page to the viewport width (Swift `updateFitWidthMagnification`).
    pub fn set_viewport_width(&mut self, width_pt: f32, cx: &mut Context<Self>) {
        if !self.fit_width || width_pt <= 0.0 || self.frames.is_empty() {
            return;
        }
        let content_width = self
            .frames
            .iter()
            .map(|frame| frame.rect.origin.x + frame.rect.size.width + PAGE_MARGIN_PT)
            .fold(0.0f32, f32::max);
        if content_width <= 0.0 {
            return;
        }
        let target = width_pt / content_width;
        if (target - self.zoom).abs() > 0.0001 {
            self.zoom = target.clamp(ZOOM_MIN, ZOOM_MAX);
            cx.notify();
        }
    }

    pub fn zoom_in(&mut self, _: &ClickEvent, _window: &mut Window, cx: &mut Context<Self>) {
        self.fit_width = false;
        self.set_zoom((self.zoom + 0.25).min(ZOOM_MAX), cx);
    }

    pub fn zoom_out(&mut self, _: &ClickEvent, _window: &mut Window, cx: &mut Context<Self>) {
        self.fit_width = false;
        self.set_zoom((self.zoom - 0.25).max(ZOOM_MIN), cx);
    }

    fn set_zoom(&mut self, zoom: f32, cx: &mut Context<Self>) {
        if (zoom - self.zoom).abs() < 1e-4 {
            return;
        }
        self.zoom = zoom;
        self.rebuild_frames();
        cx.notify();
    }

    fn rebuild_frames(&mut self) {
        self.frames = page_frame(&self.page_sizes);
    }

    fn current_viewport(&self) -> Option<Viewport> {
        let bounds = self.scroll_handle.bounds();
        let width = bounds.size.width.as_f32();
        let height = bounds.size.height.as_f32();
        if width <= 0.0 || height <= 0.0 {
            return None;
        }
        let offset = self.scroll_handle.offset();
        let viewport = Viewport::new(Rect::new(
            -offset.x.as_f32() / self.zoom,
            -offset.y.as_f32() / self.zoom,
            width / self.zoom,
            height / self.zoom,
        ));
        Some(viewport)
    }

    fn local_viewport(&self, viewport: &Viewport, frame: &PageFrame) -> Viewport {
        Viewport::new(Rect::new(
            viewport.visible.origin.x - frame.rect.origin.x,
            viewport.visible.origin.y - frame.rect.origin.y,
            viewport.visible.size.width,
            viewport.visible.size.height,
        ))
    }

    pub fn update_viewport(
        &mut self,
        viewport: Viewport,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.viewport = viewport;
        let wanted: Vec<usize> = visible_pages(&self.frames, &viewport);
        let wanted_set: HashSet<usize> = wanted.iter().copied().collect();

        for index in wanted {
            let frame = self.frames[index];
            let entity = match self.pages.get(&index) {
                Some(entity) => entity.clone(),
                None => {
                    let entity = cx.new(|cx| PageView::new(index, frame.size_pt, cx));
                    self.pages.insert(index, entity.clone());
                    entity
                }
            };
            let scale = preview_scale(frame.size_pt);
            let actor = self.actor.clone();
            entity.update(cx, |page, cx| page.request_preview(scale, actor, cx));
            let local = self.local_viewport(&viewport, &frame);
            let zoom = self.zoom;
            let actor = self.actor.clone();
            cx.with_window(cx.entity_id(), |window, app| {
                entity.update(app, |page, page_cx| {
                    page.request_tiles(local, zoom, &actor, window, page_cx);
                });
            });
        }

        for (index, entity) in &self.pages {
            let in_band = self
                .frames
                .get(*index)
                .is_some_and(|frame| viewport.prefetch().intersects(frame.rect));
            let local = self
                .frames
                .get(*index)
                .map(|frame| self.local_viewport(&viewport, frame));
            entity.update(cx, |page, _| page.update_visibility(in_band, local));
        }

        self.removal_task = Some(self.schedule_removal(wanted_set, cx));
        cx.notify();
    }

    fn schedule_removal(&mut self, wanted: HashSet<usize>, cx: &mut Context<Self>) -> Task<()> {
        let timer = cx.background_executor().timer(REMOVE_DELAY);
        let frames = self.frames.clone();
        let viewport = self.viewport;
        cx.spawn(async move |this, cx| {
            timer.await;
            if let Some(this) = this.upgrade() {
                let stale: Vec<Entity<PageView>> = cx.update_entity(&this, |view: &mut Self, _| {
                    let mut stale = Vec::new();
                    view.pages.retain(|index, entity| {
                        let in_band = frames
                            .get(*index)
                            .is_some_and(|frame| viewport.prefetch().intersects(frame.rect));
                        if !in_band {
                            stale.push(entity.clone());
                        }
                        in_band || wanted.contains(index)
                    });
                    stale
                });
                for entity in stale {
                    entity.update(cx, |page, _| page.drop_guards());
                }
            }
        })
    }

    fn content_size(&self) -> gpui::Size<gpui::Pixels> {
        let width = self
            .frames
            .iter()
            .map(|frame| frame.rect.origin.x + frame.rect.size.width)
            .fold(0.0f32, f32::max)
            + PAGE_MARGIN_PT;
        let height = self
            .frames
            .iter()
            .map(|frame| frame.rect.origin.y + frame.rect.size.height)
            .fold(0.0f32, f32::max)
            + BOTTOM_INSET_PT;
        gpui::Size::new(px(width * self.zoom), px(height * self.zoom))
    }

    fn paint_state(&self, cx: &App) -> PaintState {
        let zoom = self.zoom;
        let mut previews = Vec::new();
        let mut tiles = Vec::new();
        for (index, entity) in &self.pages {
            let page = entity.read(cx);
            if let Some(preview) = page.preview() {
                if let Some(frame) = self.frames.get(*index) {
                    previews.push((page_bounds(frame, zoom), preview.0.clone()));
                }
            }
            for (_, tile) in page.tiles() {
                if let Some(frame) = self.frames.get(*index) {
                    tiles.push((tile_bounds(frame, tile, zoom), tile.image.clone()));
                }
            }
        }
        PaintState { previews, tiles }
    }

    fn render_inner(&mut self, window: &mut Window, cx: &mut Context<Self>) -> gpui::AnyElement {
        // Use the real window width here; ScrollHandle::bounds() is only set
        // after the first layout and reports a zero/placeholder width earlier.
        let window_width = window.viewport_size().width.as_f32();
        if window_width > 0.0 {
            self.set_viewport_width(window_width, cx);
        }
        if let Some(viewport) = self.current_viewport() {
            self.update_viewport(viewport, window, cx);
        }
        let content_size = self.content_size();
        let state = self.paint_state(cx);
        div()
            .id("pdf-view")
            .size_full()
            .overflow_y_scroll()
            .track_scroll(&self.scroll_handle)
            .on_scroll(cx.listener(|this: &mut Self, _: &ScrollEvent, window, cx| {
                if let Some(viewport) = this.current_viewport() {
                    this.update_viewport(viewport, window, cx);
                }
            }))
            .child(
                canvas(
                    move |_, _, _| (),
                    move |bounds, _, window, _| {
                        // Paint the low-res full-page preview first, then the
                        // high-res tiles on top (Swift PageView layer order).
                        for (target, image) in &state.previews {
                            let target = *target + bounds.origin;
                            if target.intersects(&bounds) {
                                let _ = window.paint_image(
                                    target,
                                    target,
                                    Corners::default(),
                                    image.clone(),
                                    0,
                                    false,
                                );
                            }
                        }
                        for (target, image) in &state.tiles {
                            let target = *target + bounds.origin;
                            if target.intersects(&bounds) {
                                let _ = window.paint_image(
                                    target,
                                    target,
                                    Corners::default(),
                                    image.clone(),
                                    0,
                                    false,
                                );
                            }
                        }
                    },
                )
                .w(content_size.width)
                .h(content_size.height),
            )
            .into_any_element()
    }
}

struct PaintState {
    previews: Vec<(Bounds<gpui::Pixels>, std::sync::Arc<gpui::RenderImage>)>,
    tiles: Vec<(Bounds<gpui::Pixels>, std::sync::Arc<gpui::RenderImage>)>,
}

fn page_bounds(frame: &PageFrame, zoom: f32) -> Bounds<gpui::Pixels> {
    Bounds::new(
        point(px(frame.rect.origin.x * zoom), px(frame.rect.origin.y * zoom)),
        size(
            px(frame.rect.size.width * zoom),
            px(frame.rect.size.height * zoom),
        ),
    )
}

fn tile_bounds(frame: &PageFrame, tile: &crate::page_view::TileImage, zoom: f32) -> Bounds<gpui::Pixels> {
    let origin = point(
        px((frame.rect.origin.x + tile.rect.origin.x) * zoom),
        px((frame.rect.origin.y + tile.rect.origin.y) * zoom),
    );
    Bounds::new(
        origin,
        size(
            px(tile.rect.size.width * zoom),
            px(tile.rect.size.height * zoom),
        ),
    )
}

impl Focusable for PdfView {
    fn focus_handle(&self, _cx: &App) -> FocusHandle {
        self.focus.clone()
    }
}

impl Render for PdfView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.render_inner(window, cx)
    }
}
