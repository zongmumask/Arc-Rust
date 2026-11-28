use arc_pdf_ui::{LibraryWindow, PdfWindow, LIBRARY_DEFAULT_SIZE, LIBRARY_MIN_SIZE};
use gpui::*;
use std::path::PathBuf;

fn main() {
    arc_pdf_log::init();
    let app = gpui_platform::application();
    let initial_pdf = std::env::args().nth(1).map(PathBuf::from).filter(|path| {
        path.extension()
            .is_some_and(|ext| ext.eq_ignore_ascii_case("pdf"))
    });

    // Arc-macOS AppDelegate: the Library is the launch window unless the app
    // was opened with a file. quiet-md main.rs: open the workspace window
    // from `app.run` with configured titlebar/min-size options.
    app.run(move |cx: &mut App| {
        if let Some(path) = initial_pdf.clone() {
            open_pdf_window(path, cx);
        } else {
            open_library(cx);
        }
    });
}

fn open_library(cx: &mut App) {
    let bounds = WindowBounds::centered(None, LIBRARY_DEFAULT_SIZE, cx);
    let options = WindowOptions {
        window_bounds: Some(bounds),
        window_min_size: Some(LIBRARY_MIN_SIZE),
        // quiet-md theme::window_background(): Blurred lets the native glass
        // material show through the transparent GPUI surface.
        window_background: if cfg!(target_os = "macos") {
            WindowBackgroundAppearance::Blurred
        } else {
            WindowBackgroundAppearance::Opaque
        },
        titlebar: Some(TitlebarOptions {
            // LibraryWindowController hides the native title; the visible
            // title is the toolbar's dynamic source label.
            title: Default::default(),
            appears_transparent: true,
            traffic_light_position: Some(point(px(20.), px(14.))),
        }),
        ..Default::default()
    };
    cx.open_window(options, |window, cx| {
        cx.new(|cx| LibraryWindow::new(window, cx))
    })
    .unwrap();
}

fn open_pdf_window(path: PathBuf, cx: &mut App) {
    cx.open_window(WindowOptions::default(), |_window, cx| {
        cx.new(|cx| PdfWindow::new(path.clone(), cx))
    })
    .unwrap();
}
