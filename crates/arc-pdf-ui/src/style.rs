//! Shared GPUI styling and layout constants for the Arc PDF windows.

use gpui::*;

pub(super) const PLACEHOLDER_SVG: &[u8] = include_bytes!("../../../assets/svg/placeholder.svg");

/// Arc-macOS LibraryWindowController-aligned layout constants.
pub const LIBRARY_DEFAULT_SIZE: Size<Pixels> = Size {
    width: px(1200.),
    height: px(700.),
};
pub const LIBRARY_MIN_SIZE: Size<Pixels> = Size {
    width: px(1000.),
    height: px(600.),
};

// Mirrors LibrarySplitViewController.LayoutConstants and
// DocumentBrowserController.Layout (Arc-macOS).
pub(super) const SIDEBAR_MIN: Pixels = px(180.);
pub(super) const SIDEBAR_MAX: Pixels = px(400.);
pub(super) const SIDEBAR_WIDTH: Pixels = px(220.);
pub(super) const BROWSER_MIN: Pixels = px(400.);
pub(super) const CONTENT_MARGIN: Pixels = px(36.);
pub(super) const CARD_WIDTH: Pixels = px(180.);
pub(super) const CARD_HEIGHT: Pixels = px(270.);
pub(super) const CARD_GAP: Pixels = px(20.);
pub(super) const BOTTOM_BAR_HEIGHT: Pixels = px(44.);
pub(super) const TOOLBAR_HEIGHT: Pixels = px(48.);

pub(super) fn action_button(id: &'static str, label: &str) -> Stateful<Div> {
    div()
        .id(id)
        .px(px(10.))
        .py(px(6.))
        .rounded(px(6.))
        .border_1()
        .border_color(rgb(0x0088_8888))
        .cursor_pointer()
        .child(label.to_string())
}

pub(super) fn small_shadow() -> Vec<BoxShadow> {
    vec![BoxShadow {
        color: hsla(0.0, 0.0, 0.0, 0.15),
        offset: point(px(0.), px(0.)),
        blur_radius: px(5.),
        spread_radius: px(0.),
        inset: false,
    }]
}
