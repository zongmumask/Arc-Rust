//! GPUI window for viewing a PDF document.
//!
//! First migration slice: the three-pane workspace shell. Sidebar and
//! inspector are structural placeholders; the center pane hosts the reader.

use crate::style::action_button;

// Mirrors PDFWorkspaceSplitViewController split items (Swift baseline).
const PDF_SIDEBAR_MIN: Pixels = px(250.);
const PDF_SIDEBAR_MAX: Pixels = px(400.);
const PDF_INSPECTOR_MIN: Pixels = px(250.);
const PDF_INSPECTOR_MAX: Pixels = px(400.);
const PDF_PANE_WIDTH: Pixels = px(250.);
const PDF_CENTER_MIN: Pixels = px(400.);
use arc_pdf_view::PdfView;
use arc_pdf_viewer::PdfiumActor;
use gpui::prelude::*;
use gpui::*;
use std::path::PathBuf;

/// Which divider is currently being dragged.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PaneDrag {
    Sidebar,
    Inspector,
}

impl PaneDrag {
    fn id(self) -> &'static str {
        match self {
            Self::Sidebar => "pdf-sidebar-resize",
            Self::Inspector => "pdf-inspector-resize",
        }
    }
}

pub struct PdfWindow {
    path: PathBuf,
    sidebar_open: bool,
    inspector_open: bool,
    sidebar_width: Pixels,
    inspector_width: Pixels,
    sidebar_drag: Option<PaneDrag>,
    actor: PdfiumActor,
    reader: Entity<PdfView>,
    focus: FocusHandle,
}

impl PdfWindow {
    pub fn new(path: PathBuf, cx: &mut Context<Self>) -> Self {
        let actor = PdfiumActor::spawn(cx, &cx.background_executor());
        let reader = cx.new(|cx| PdfView::new(actor.clone(), cx));
        let actor_for_open = actor.clone();
        let open_path = path.clone();
        let reader_weak = reader.downgrade();
        cx.spawn(async move |_, cx| {
            if let Ok(Ok(info)) = actor_for_open.open(open_path).await {
                if let Some(reader) = reader_weak.upgrade() {
                    cx.update_entity(&reader, |view, cx| view.set_document(info.page_sizes, cx));
                }
            }
        })
        .detach();
        Self {
            path,
            sidebar_open: false,
            inspector_open: false,
            sidebar_width: PDF_PANE_WIDTH,
            inspector_width: PDF_PANE_WIDTH,
            sidebar_drag: None,
            actor,
            reader,
            focus: cx.focus_handle(),
        }
    }

    pub fn set_path(&mut self, path: PathBuf, cx: &mut Context<Self>) {
        self.path = path.clone();
        let actor = self.actor.clone();
        cx.spawn(async move |_cx, _| {
            actor.open(path).await.ok();
        })
        .detach();
        cx.notify();
    }

    fn toggle_sidebar(&mut self, _: &ClickEvent, _window: &mut Window, cx: &mut Context<Self>) {
        self.sidebar_open = !self.sidebar_open;
        cx.notify();
    }

    fn toggle_inspector(&mut self, _: &ClickEvent, _window: &mut Window, cx: &mut Context<Self>) {
        self.inspector_open = !self.inspector_open;
        cx.notify();
    }

    fn pane(&self, drag: PaneDrag, label: &str, width: Pixels, open: bool) -> impl IntoElement {
        let pane = div()
            .h_full()
            .w(width)
            .flex_shrink_0()
            .border_r_1()
            .border_color(rgb(0x33_333333))
            .bg(if drag == PaneDrag::Inspector {
                hsla(0.0, 0.0, 0.11, 0.96)
            } else {
                hsla(0.0, 0.0, 0.13, 0.96)
            })
            .child(
                div()
                    .p(px(12.))
                    .text_sm()
                    .text_color(rgb(0x99_999999))
                    .child(label.to_string()),
            );
        if open {
            pane.into_any_element()
        } else {
            div().w(px(0.)).h_full().into_any_element()
        }
    }

    fn resize_strip(&self, drag: PaneDrag, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .id(drag.id())
            .absolute()
            .top(px(0.))
            .bottom(px(0.))
            .left(if drag == PaneDrag::Sidebar {
                self.sidebar_width - px(7.)
            } else {
                px(0.)
            })
            .right(if drag == PaneDrag::Inspector {
                self.inspector_width - px(7.)
            } else {
                px(0.)
            })
            .w(px(15.))
            .cursor_col_resize()
            .on_mouse_down(
                MouseButton::Left,
                cx.listener(move |this, _, _window, cx| {
                    this.sidebar_drag = Some(drag);
                    cx.notify();
                }),
            )
    }

    fn overlay(&self, window: &mut Window, cx: &mut Context<Self>) -> Option<AnyElement> {
        let _drag = self.sidebar_drag?;
        let viewport = window.viewport_size();
        let max_center = (viewport.width - PDF_CENTER_MIN).max(px(0.));
        Some(
            div()
                .absolute()
                .inset(px(0.))
                .occlude()
                .cursor_col_resize()
                .on_mouse_move(cx.listener(move |this, event: &MouseMoveEvent, _, cx| {
                    let Some(drag) = this.sidebar_drag else {
                        return;
                    };
                    let width = match drag {
                        PaneDrag::Sidebar => event.position.x,
                        PaneDrag::Inspector => viewport.width - event.position.x,
                    };
                    let max = max_center.min(match drag {
                        PaneDrag::Sidebar => PDF_SIDEBAR_MAX,
                        PaneDrag::Inspector => PDF_INSPECTOR_MAX,
                    });
                    let min = match drag {
                        PaneDrag::Sidebar => PDF_SIDEBAR_MIN,
                        PaneDrag::Inspector => PDF_INSPECTOR_MIN,
                    };
                    let width = width.clamp(min, max.max(min));
                    match drag {
                        PaneDrag::Sidebar => this.sidebar_width = width,
                        PaneDrag::Inspector => this.inspector_width = width,
                    }
                    cx.notify();
                }))
                .on_mouse_up(
                    MouseButton::Left,
                    cx.listener(|this, _, _, cx| {
                        this.sidebar_drag = None;
                        cx.notify();
                    }),
                )
                .on_mouse_up_out(
                    MouseButton::Left,
                    cx.listener(|this, _, _, cx| {
                        this.sidebar_drag = None;
                        cx.notify();
                    }),
                )
                .into_any_element(),
        )
    }
}

impl Focusable for PdfWindow {
    fn focus_handle(&self, _cx: &App) -> FocusHandle {
        self.focus.clone()
    }
}

impl Render for PdfWindow {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let drag = self.sidebar_drag;
        let left = self.pane(PaneDrag::Sidebar, "Sidebar", self.sidebar_width, self.sidebar_open);
        let right = self.pane(
            PaneDrag::Inspector,
            "Inspector",
            self.inspector_width,
            self.inspector_open,
        );
        let path = self.path.clone();
        let reader = self.reader.clone();

        div()
            .size_full()
            .flex()
            .flex_col()
            .child(
                div()
                    .h(px(44.))
                    .flex()
                    .items_center()
                    .gap(px(8.))
                    .px(px(10.))
                    .border_b_1()
                    .border_color(rgb(0x33_333333))
                    .child(action_button("pdf-sidebar-toggle", "Sidebar").on_click(cx.listener(Self::toggle_sidebar)))
                    .child(action_button("pdf-inspector-toggle", "Inspector").on_click(cx.listener(Self::toggle_inspector)))
                    .child(
                        div()
                            .flex_1()
                            .overflow_hidden()
                            .text_ellipsis()
                            .child(path.display().to_string()),
                    ),
            )
            .child(
                div()
                    .id("pdf-workspace")
                    .relative()
                    .flex_1()
                    .flex()
                    .flex_row()
                    .overflow_hidden()
                    .child(left)
                    .child(
                        div()
                            .id("pdf-center")
                            .flex_1()
                            .h_full()
                            .min_w(PDF_CENTER_MIN)
                            .child(reader),
                    )
                    .child(right)
                    .child(if self.sidebar_open && drag != Some(PaneDrag::Inspector) {
                        self.resize_strip(PaneDrag::Sidebar, cx).into_any_element()
                    } else {
                        div().into_any_element()
                    })
                    .child(if self.inspector_open && drag != Some(PaneDrag::Sidebar) {
                        self.resize_strip(PaneDrag::Inspector, cx).into_any_element()
                    } else {
                        div().into_any_element()
                    })
                    .children(self.overlay(window, cx)),
            )
    }
}
