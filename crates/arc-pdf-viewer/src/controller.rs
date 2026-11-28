//! `PdfViewController`: the GPUI entity that hosts the reading pipeline.
//!
//! Responsibilities are split into sibling modules:
//! - `actor`: PDFium worker thread and guard
//! - `preview` / `tiles`: render request submission
//! - `lifecycle`: viewport sets, guards and delayed page destruction
//! - `paint` / `toolbar`: drawing and controls
//! - `page_state` / `image`: per-page state and GPUI image wrappers
//!
//! This file keeps only orchestration and GPUI `Render` wiring.

mod image;
mod lifecycle;
mod page_state;
mod paint;
mod preview;
mod tiles;
mod toolbar;

use self::page_state::PageState;
use crate::actor::PdfiumActor;
use crate::cache::PreviewCache;
use crate::geometry::{page_frame, BOTTOM_INSET_PT, PAGE_MARGIN_PT, PageFrame};
use arc_pdf_core::{Rect, Size as PdfSize};
use arc_pdf_render::Viewport;
use gpui::prelude::*;
use gpui::*;
use std::collections::HashMap;
use std::path::PathBuf;

const ZOOM_MIN: f32 = 0.25;
const ZOOM_MAX: f32 = 8.0;

/// Internal access used by the helper modules.
pub(super) trait ReaderState {
    fn pages_mut(&mut self) -> &mut HashMap<usize, PageState>;
    fn preview_cache_mut(&mut self) -> &mut PreviewCache;
}

pub struct PdfViewController {
    path: PathBuf,
    actor: PdfiumActor,
    page_sizes: Vec<PdfSize>,
    page_frames: Vec<PageFrame>,
    pages: HashMap<usize, PageState>,
    preview_cache: PreviewCache,
    viewport: Viewport,
    zoom: f32,
    scroll_handle: ScrollHandle,
    focus: FocusHandle,
    removal_task: Option<Task<()>>,
    loading: bool,
}

impl ReaderState for PdfViewController {
    fn pages_mut(&mut self) -> &mut HashMap<usize, PageState> {
        &mut self.pages
    }

    fn preview_cache_mut(&mut self) -> &mut PreviewCache {
        &mut self.preview_cache
    }
}

impl PdfViewController {
    pub fn new(path: PathBuf, actor: PdfiumActor, cx: &mut Context<Self>) -> Self {
        Self {
            path,
            actor,
            page_sizes: Vec::new(),
            page_frames: Vec::new(),
            pages: HashMap::new(),
            preview_cache: PreviewCache::new(),
            viewport: Viewport::new(Rect::new(0.0, 0.0, 0.0, 0.0)),
            zoom: 1.0,
            scroll_handle: ScrollHandle::new(),
            focus: cx.focus_handle(),
            removal_task: None,
            loading: false,
        }
    }

    pub fn zoom_in(&mut self, _: &ClickEvent, _window: &mut Window, cx: &mut Context<Self>) {
        self.set_zoom((self.zoom + 0.25).min(ZOOM_MAX), cx);
    }

    pub fn zoom_out(&mut self, _: &ClickEvent, _window: &mut Window, cx: &mut Context<Self>) {
        self.set_zoom((self.zoom - 0.25).max(ZOOM_MIN), cx);
    }

    fn set_zoom(&mut self, zoom: f32, cx: &mut Context<Self>) {
        if (zoom - self.zoom).abs() < 1e-4 {
            return;
        }
        self.zoom = zoom;
        self.rebuild_frames();
        for state in self.pages.values_mut() {
            state.tiles.clear();
            state.tile_guards.clear();
        }
        cx.notify();
    }

    fn rebuild_frames(&mut self) {
        self.page_frames = page_frame(&self.page_sizes);
    }

    fn load_document(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.loading || !self.page_sizes.is_empty() {
            return;
        }
        self.loading = true;
        let actor = self.actor.clone();
        let path = self.path.clone();
        cx.spawn_in(window, async move |this, _cx| {
            actor.open(path).await.expect("actor reply").expect("open");
            let info = actor.metadata().await.expect("actor reply").expect("metadata");
            if let Some(this) = this.upgrade() {
                _cx.update_entity(&this, |this, cx| {
                    this.page_sizes = info.page_sizes;
                    this.rebuild_frames();
                    cx.notify();
                });
            }
        })
        .detach();
    }

    fn current_viewport(&self) -> Option<Viewport> {
        let bounds = self.scroll_handle.bounds();
        let width = bounds.size.width.as_f32();
        let height = bounds.size.height.as_f32();
        if width <= 0.0 || height <= 0.0 {
            return None;
        }
        Some(Viewport::new(Rect::new(
            -bounds.origin.x.as_f32() / self.zoom,
            -bounds.origin.y.as_f32() / self.zoom,
            width / self.zoom,
            height / self.zoom,
        )))
    }

    fn update_viewport(&mut self, viewport: Viewport, window: &mut Window, cx: &mut Context<Self>) {
        self.viewport = viewport;
        let wanted = lifecycle::wanted_pages(&viewport, &self.page_frames);
        let wanted_set: std::collections::HashSet<usize> = wanted.iter().copied().collect();
        for index in &wanted {
            self.pages.entry(*index).or_default();
            let frame = self.page_frames[*index];
            let scale = preview::scale_for(frame.size_pt.width, frame.size_pt.height);
            preview::request_preview(
                *index,
                scale,
                self.actor.clone(),
                window,
                cx,
                &mut self.pages,
            );
        }
        for (index, state) in self.pages.iter_mut() {
            state.pending_removal = !wanted_set.contains(index);
        }
        lifecycle::update_guards(&viewport, &self.page_frames, &mut self.pages);
        tiles::request_tiles(
            &viewport,
            self.zoom,
            &self.page_frames,
            &self.actor,
            window,
            cx,
            &mut self.pages,
        );
        self.removal_task = Some(lifecycle::schedule_removal(
            viewport,
            self.page_frames.clone(),
            wanted_set,
            cx,
        ));
        cx.notify();
    }

    fn content_size(&self) -> gpui::Size<Pixels> {
        let width = self
            .page_frames
            .iter()
            .map(|frame| frame.rect.origin.x + frame.rect.size.width)
            .fold(0.0f32, f32::max)
            + PAGE_MARGIN_PT;
        let height = self
            .page_frames
            .iter()
            .map(|frame| frame.rect.origin.y + frame.rect.size.height)
            .fold(0.0f32, f32::max)
            + BOTTOM_INSET_PT;
        gpui::Size::new(px(width * self.zoom), px(height * self.zoom))
    }

    fn paint_state(&self) -> paint::PaintState {
        paint::PaintState {
            zoom: self.zoom,
            frames: self.page_frames.clone(),
            previews: self
                .pages
                .iter()
                .filter_map(|(index, state)| state.preview.clone().map(|image| (*index, image)))
                .collect(),
            tiles: self
                .pages
                .iter()
                .flat_map(|(_, state)| {
                    state
                        .tiles
                        .iter()
                        .map(|(key, image)| (*key, image.clone()))
                        .collect::<Vec<_>>()
                })
                .collect(),
        }
    }

    fn render_inner(&mut self, window: &mut Window, cx: &mut Context<Self>) -> AnyElement {
        self.load_document(window, cx);
        if let Some(viewport) = self.current_viewport() {
            self.update_viewport(viewport, window, cx);
        }
        let content_size = self.content_size();
        let state = self.paint_state();
        div()
            .id("pdf-viewport")
            .size_full()
            .flex()
            .flex_col()
            .child(toolbar::toolbar(
                self.zoom,
                Self::zoom_in,
                Self::zoom_out,
                cx,
            ))
            .child(
                div()
                    .id("pdf-scroll")
                    .flex_1()
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
                            move |bounds, _, window, _| paint::paint(&state, bounds, window),
                        )
                        .w(content_size.width)
                        .h(content_size.height),
                    ),
            )
            .into_any_element()
    }
}

impl Focusable for PdfViewController {
    fn focus_handle(&self, _cx: &App) -> FocusHandle {
        self.focus.clone()
    }
}

impl Render for PdfViewController {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.render_inner(window, cx)
    }
}
