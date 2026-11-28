//! Toolbar and zoom controls above the PDF canvas.

use gpui::*;

pub(super) const TOOLBAR_HEIGHT: Pixels = px(36.);

pub(super) fn toolbar<T>(
    zoom: f32,
    zoom_in: impl Fn(&mut T, &ClickEvent, &mut Window, &mut Context<T>) + 'static,
    zoom_out: impl Fn(&mut T, &ClickEvent, &mut Window, &mut Context<T>) + 'static,
    cx: &mut Context<T>,
) -> impl IntoElement
where
    T: 'static,
{
    div()
        .h(TOOLBAR_HEIGHT)
        .flex()
        .items_center()
        .gap(px(6.))
        .px(px(8.))
        .border_b_1()
        .border_color(rgb(0x33_333333))
        .child(
            div()
                .id("pdf-zoom-in")
                .px(px(8.))
                .py(px(3.))
                .rounded(px(4.))
                .border_1()
                .border_color(rgb(0x55_555555))
                .cursor_pointer()
                .child("+")
                .on_click(cx.listener(zoom_in)),
        )
        .child(
            div()
                .id("pdf-zoom-out")
                .px(px(8.))
                .py(px(3.))
                .rounded(px(4.))
                .border_1()
                .border_color(rgb(0x55_555555))
                .cursor_pointer()
                .child("-")
                .on_click(cx.listener(zoom_out)),
        )
        .child(format!("{:.0}%", zoom * 100.0))
}
