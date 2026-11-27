//! GPUI window for viewing a PDF document.

use arc_pdf_core::PageId;
use arc_pdf_engine::{PdfiumRuntime, Raster};
use gpui::prelude::*;
use gpui::*;
use std::path::PathBuf;
use std::sync::Arc;

use crate::style::action_button;

pub struct PdfWindow {
    path: PathBuf,
    raster: Option<Arc<RenderImage>>,
    image_size: Size<Pixels>,
    scale: f32,
    scroll_handle: ScrollHandle,
    focus: FocusHandle,
}

impl PdfWindow {
    pub fn new(path: PathBuf, cx: &mut Context<Self>) -> Self {
        Self {
            path,
            raster: None,
            image_size: Size {
                width: px(0.),
                height: px(0.),
            },
            scale: 2.0,
            scroll_handle: ScrollHandle::new(),
            focus: cx.focus_handle(),
        }
    }

    pub fn set_path(&mut self, path: PathBuf, cx: &mut Context<Self>) {
        self.path = path;
        self.raster = None;
        cx.notify();
    }

    fn render_current(&mut self, cx: &mut Context<Self>) {
        let runtime = PdfiumRuntime::init();
        let Ok(document) = runtime.open_document(&self.path) else {
            return;
        };
        if document.page_count() == 0 {
            return;
        }
        let Ok(raster) = document.render_page(PageId(0), self.scale) else {
            return;
        };
        let image = render_image_from_raster(&raster);
        self.image_size = Size {
            width: px(raster.width as f32),
            height: px(raster.height as f32),
        };
        self.raster = Some(image);
        cx.notify();
    }

    fn zoom_in(&mut self, _: &ClickEvent, _window: &mut Window, cx: &mut Context<Self>) {
        self.scale = (self.scale + 0.25).min(8.0);
        self.render_current(cx);
    }

    fn zoom_out(&mut self, _: &ClickEvent, _window: &mut Window, cx: &mut Context<Self>) {
        self.scale = (self.scale - 0.25).max(0.5);
        self.render_current(cx);
    }
}

impl Focusable for PdfWindow {
    fn focus_handle(&self, _cx: &App) -> FocusHandle {
        self.focus.clone()
    }
}

impl Render for PdfWindow {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        if self.raster.is_none() {
            self.render_current(cx);
        }

        let image = self.raster.clone();
        let image_size = self.image_size;
        div()
            .size_full()
            .flex()
            .flex_col()
            .child(
                div()
                    .flex()
                    .gap(px(8.))
                    .p(px(8.))
                    .child(action_button("zoom-in", "+").on_click(cx.listener(Self::zoom_in)))
                    .child(action_button("zoom-out", "-").on_click(cx.listener(Self::zoom_out)))
                    .child(div().child(format!("{} · {}x", self.path.display(), self.scale))),
            )
            .child(
                div()
                    .id("pdf-scroll")
                    .flex_1()
                    .overflow_y_scroll()
                    .track_scroll(&self.scroll_handle)
                    .child(if let Some(image) = image {
                        canvas(
                            move |_, _, _| image.clone(),
                            move |bounds, image, window, _| {
                                window
                                    .paint_image(
                                        bounds,
                                        Bounds::new(bounds.origin, image_size),
                                        Corners::default(),
                                        image,
                                        0,
                                        false,
                                    )
                                    .ok();
                            },
                        )
                        .w(image_size.width)
                        .h(image_size.height)
                        .into_any_element()
                    } else {
                        div().child("Rendering…").into_any_element()
                    }),
            )
    }
}

fn render_image_from_raster(raster: &Raster) -> Arc<RenderImage> {
    let row_bytes = raster.width * 4;
    let mut bytes = vec![0u8; raster.width * raster.height * 4];
    for y in 0..raster.height {
        let src_start = y * raster.stride as usize;
        let dst_start = y * row_bytes;
        let copy = (raster.stride as usize).min(row_bytes);
        bytes[dst_start..dst_start + copy]
            .copy_from_slice(&raster.pixels[src_start..src_start + copy]);
    }
    let image = image::RgbaImage::from_raw(raster.width as u32, raster.height as u32, bytes)
        .expect("raster buffer size");
    Arc::new(RenderImage::new(vec![image::Frame::new(image)]))
}
