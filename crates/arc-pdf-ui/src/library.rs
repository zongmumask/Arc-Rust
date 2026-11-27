//! GPUI port of Arc-macOS `LibraryWindowController`.
//!
//! Startup rule is the same as `AppDelegate.applicationDidFinishLaunching`:
//! the library is shown unless the app was launched with a PDF file.

use gpui::prelude::*;
use gpui::*;
use std::path::PathBuf;

use crate::pdf::PdfWindow;
use crate::style::{
    action_button, small_shadow, BOTTOM_BAR_HEIGHT, BROWSER_MIN, CARD_GAP, CARD_HEIGHT, CARD_WIDTH,
    CONTENT_MARGIN, PLACEHOLDER_SVG, SIDEBAR_MAX, SIDEBAR_MIN, SIDEBAR_WIDTH, TOOLBAR_HEIGHT,
};

#[derive(Clone, Copy, PartialEq, Eq)]
enum LibrarySource {
    Recent,
    Starred,
}

impl LibrarySource {
    fn title(self) -> &'static str {
        match self {
            Self::Recent => "Recently Opened",
            Self::Starred => "Starred",
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ViewStyle {
    Gallery,
    List,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SortOption {
    Name,
    CreatedDate,
    ModifiedDate,
    Size,
    Kind,
}

impl SortOption {
    const ALL: [Self; 5] = [
        Self::Name,
        Self::CreatedDate,
        Self::ModifiedDate,
        Self::Size,
        Self::Kind,
    ];

    fn title(self) -> &'static str {
        match self {
            Self::Name => "Name",
            Self::CreatedDate => "Created Date",
            Self::ModifiedDate => "Modified Date",
            Self::Size => "Size",
            Self::Kind => "Kind",
        }
    }
}

#[derive(Clone)]
struct RecentEntry {
    path: PathBuf,
    starred: bool,
    opened_order: usize,
}

pub struct LibraryWindow {
    appearance: WindowAppearance,
    focus: FocusHandle,
    source: LibrarySource,
    style: ViewStyle,
    sort: SortOption,
    sort_menu_open: bool,
    search: String,
    sidebar_open: bool,
    sidebar_width: Pixels,
    entries: Vec<RecentEntry>,
    next_order: usize,
    sidebar_drag: bool,
    #[cfg(target_os = "macos")]
    glass: crate::native_sidebar::NativeSidebarGlass,
}

impl LibraryWindow {
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        let appearance = window.appearance();
        window.observe_window_appearance(|_window, _cx| {}).detach();

        Self {
            appearance,
            focus: cx.focus_handle(),
            source: LibrarySource::Recent,
            style: ViewStyle::Gallery,
            sort: SortOption::ModifiedDate,
            sort_menu_open: false,
            search: String::new(),
            sidebar_open: true,
            sidebar_width: SIDEBAR_WIDTH,
            entries: Vec::new(),
            next_order: 0,
            sidebar_drag: false,
            #[cfg(target_os = "macos")]
            glass: crate::native_sidebar::NativeSidebarGlass::default(),
        }
    }

    /// Add a document to the library, matching RecentDocumentsStore's
    /// "most recent first, deduplicated" semantics.
    pub fn add_document(&mut self, path: PathBuf) {
        self.entries.retain(|entry| entry.path != path);
        self.entries.insert(
            0,
            RecentEntry {
                path,
                starred: false,
                opened_order: self.next_order,
            },
        );
        self.next_order += 1;
    }

    fn select_source(&mut self, source: LibrarySource, cx: &mut Context<Self>) {
        self.source = source;
        cx.notify();
    }

    fn toggle_sidebar(&mut self, _: &ClickEvent, _: &mut Window, cx: &mut Context<Self>) {
        self.sidebar_open = !self.sidebar_open;
        cx.notify();
    }

    fn toggle_style(&mut self, _: &ClickEvent, _: &mut Window, cx: &mut Context<Self>) {
        self.style = match self.style {
            ViewStyle::Gallery => ViewStyle::List,
            ViewStyle::List => ViewStyle::Gallery,
        };
        cx.notify();
    }

    fn toggle_sort_menu(&mut self, _: &ClickEvent, _: &mut Window, cx: &mut Context<Self>) {
        self.sort_menu_open = !self.sort_menu_open;
        cx.notify();
    }

    fn open_pdf(&mut self, _: &ClickEvent, window: &mut Window, cx: &mut Context<Self>) {
        let receiver = cx.prompt_for_paths(PathPromptOptions {
            files: true,
            directories: false,
            multiple: false,
            prompt: Some("Choose a File".into()),
            extensions: Some(vec!["pdf".into()]),
        });
        cx.spawn_in(window, async move |_, cx| {
            if let Ok(Ok(Some(paths))) = receiver.await {
                if let Some(path) = paths.into_iter().next() {
                    cx.update(|_, app| {
                        app.open_window(WindowOptions::default(), |_window, cx| {
                            cx.new(|cx| PdfWindow::new(path.clone(), cx))
                        })
                        .ok();
                    })?;
                }
            }
            Ok::<(), gpui::private::anyhow::Error>(())
        })
        .detach();
    }

    fn is_dark(&self) -> bool {
        matches!(
            self.appearance,
            WindowAppearance::Dark | WindowAppearance::VibrantDark
        )
    }

    fn visible_entries(&self) -> Vec<RecentEntry> {
        let mut entries: Vec<_> = self
            .entries
            .iter()
            .filter(|entry| {
                let wanted = match self.source {
                    LibrarySource::Recent => true,
                    LibrarySource::Starred => entry.starred,
                };
                let matches = self.search.is_empty()
                    || entry
                        .path
                        .file_name()
                        .and_then(|name| name.to_str())
                        .is_some_and(|name| {
                            name.to_lowercase().contains(&self.search.to_lowercase())
                        });
                wanted && matches
            })
            .cloned()
            .collect();

        match self.sort {
            SortOption::Name | SortOption::Kind => {
                entries.sort_by(|a, b| a.path.file_name().cmp(&b.path.file_name()))
            }
            SortOption::CreatedDate | SortOption::ModifiedDate => {
                entries.sort_by_key(|a| std::cmp::Reverse(a.opened_order))
            }
            SortOption::Size => entries.sort_by(|a, b| {
                std::fs::metadata(&b.path)
                    .map(|m| m.len())
                    .unwrap_or(0)
                    .cmp(&std::fs::metadata(&a.path).map(|m| m.len()).unwrap_or(0))
            }),
        }
        entries
    }

    fn render_sidebar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let dark = self.is_dark();
        // The native NSGlassEffectView sits behind GPUI on macOS 26+; a
        // translucent fill keeps the sidebar distinct on older systems.
        #[cfg(target_os = "macos")]
        let sidebar_bg = if crate::native_sidebar::NativeSidebarGlass::supported() {
            hsla(0.0, 0.0, 0.0, 0.0)
        } else {
            hsla(0.0, 0.0, 0.0, if dark { 0.22 } else { 0.12 })
        };
        #[cfg(not(target_os = "macos"))]
        let sidebar_bg = if dark { rgb(0x232325) } else { rgb(0xECECEE) };
        let fg = if dark { rgb(0xE7E7E7) } else { rgb(0x1D1D1F) };
        let muted = if dark { rgb(0x98989D) } else { rgb(0x6E6E73) };
        let accent = if dark { rgb(0x0A84FF) } else { rgb(0x007AFF) };
        let selected_bg = if dark {
            hsla(0.60, 0.95, 0.55, 0.15)
        } else {
            hsla(0.60, 0.95, 0.50, 0.15)
        };
        let starred_count = self.entries.iter().filter(|entry| entry.starred).count();

        let mut rows = Vec::new();
        for (source, label, count, id) in [
            (
                LibrarySource::Recent,
                "Recent",
                self.entries.len(),
                "library-source-recent",
            ),
            (
                LibrarySource::Starred,
                "Starred",
                starred_count,
                "library-source-starred",
            ),
        ] {
            let selected = self.source == source;
            rows.push(
                div()
                    .id(id)
                    .h(px(28.))
                    .px(px(10.))
                    .flex()
                    .items_center()
                    .gap(px(8.))
                    .rounded(px(6.))
                    .cursor_pointer()
                    .text_size(px(13.))
                    .when(selected, |d| {
                        d.bg(selected_bg)
                            .text_color(accent)
                            .font_weight(FontWeight::SEMIBOLD)
                    })
                    .when(!selected, |d| d.text_color(fg))
                    .child(
                        div().w(px(16.)).flex_none().text_center().child(
                            if source == LibrarySource::Recent {
                                "◷"
                            } else {
                                "★"
                            }
                            .to_string(),
                        ),
                    )
                    .child(div().flex_1().child(label.to_string()))
                    .when(count > 0, |d| {
                        d.child(
                            div()
                                .text_size(px(12.))
                                .text_color(if selected { accent } else { muted })
                                .child(count.to_string()),
                        )
                    })
                    .on_click(cx.listener(move |this, _, _, cx| this.select_source(source, cx)))
                    .into_any_element(),
            );
        }

        div()
            .w(self.sidebar_width)
            .min_w(SIDEBAR_MIN)
            .max_w(SIDEBAR_MAX)
            .h_full()
            .flex_none()
            .bg(sidebar_bg)
            .text_color(fg)
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(4.))
                    .pt(px(52.))
                    .px(px(10.))
                    .child(
                        div()
                            .id("library-sidebar-toggle")
                            .h(px(26.))
                            .flex()
                            .items_center()
                            .px(px(4.))
                            .rounded(px(6.))
                            .cursor_pointer()
                            .text_size(px(12.))
                            .text_color(muted)
                            .hover(|d| d.bg(hsla(0.0, 0.0, 0.0, 0.06)))
                            .child("‹  Hide Sidebar")
                            .on_click(cx.listener(Self::toggle_sidebar)),
                    )
                    .children(rows),
            )
    }

    fn render_toolbar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let dark = self.is_dark();
        let fg = if dark { rgb(0xE7E7E7) } else { rgb(0x1D1D1F) };
        let toolbar_bg = if dark { rgb(0x242426) } else { rgb(0xF2F2F2) };
        let border = if dark { rgb(0x3B3B3D) } else { rgb(0xD6D6D8) };

        div()
            .h(TOOLBAR_HEIGHT)
            .flex_none()
            .flex()
            .items_center()
            .gap(px(10.))
            .px(px(14.))
            .bg(toolbar_bg)
            .border_b_1()
            .border_color(border)
            .text_color(fg)
            .child(
                div()
                    .text_size(px(15.))
                    .font_weight(FontWeight::SEMIBOLD)
                    .child(self.source.title().to_string()),
            )
            .child(div().flex_1())
            .child(
                div()
                    .relative()
                    .child(
                        action_button("library-sort", "Sort ▾")
                            .border_color(border)
                            .on_click(cx.listener(Self::toggle_sort_menu)),
                    )
                    .when(self.sort_menu_open, |d| {
                        d.child(
                            div()
                                .absolute()
                                .top(px(32.))
                                .right(px(0.))
                                .w(px(160.))
                                .flex()
                                .flex_col()
                                .p(px(4.))
                                .rounded(px(6.))
                                .bg(if dark { rgb(0x2C2C2E) } else { rgb(0xFFFFFF) })
                                .border_1()
                                .border_color(border)
                                .shadow(small_shadow())
                                .children(SortOption::ALL.into_iter().map(|option| {
                                    div()
                                        .id(ElementId::Name(
                                            format!("library-sort-{:?}", option).into(),
                                        ))
                                        .h(px(26.))
                                        .flex()
                                        .items_center()
                                        .px(px(8.))
                                        .rounded(px(4.))
                                        .cursor_pointer()
                                        .child(
                                            if self.sort == option { "✓ " } else { "   " }
                                                .to_string(),
                                        )
                                        .child(option.title().to_string())
                                        .on_click(cx.listener(move |this, _, _, cx| {
                                            this.sort = option;
                                            this.sort_menu_open = false;
                                            cx.notify();
                                        }))
                                        .into_any_element()
                                })),
                        )
                    }),
            )
            .child(
                action_button(
                    "library-view-style",
                    if self.style == ViewStyle::Gallery {
                        "☷"
                    } else {
                        "☰"
                    },
                )
                .border_color(border)
                .on_click(cx.listener(Self::toggle_style)),
            )
            .child(
                div()
                    .id("library-search")
                    .w(px(180.))
                    .h(px(26.))
                    .flex()
                    .items_center()
                    .px(px(8.))
                    .rounded(px(6.))
                    .border_1()
                    .border_color(border)
                    .text_size(px(12.))
                    .child(if self.search.is_empty() {
                        "Search Documents".to_string()
                    } else {
                        self.search.clone()
                    }),
            )
    }

    fn render_card(&self, entry: &RecentEntry, cx: &mut Context<Self>) -> impl IntoElement {
        let dark = self.is_dark();
        let fg = if dark { rgb(0xE7E7E7) } else { rgb(0x1D1D1F) };
        let muted = if dark { rgb(0x98989D) } else { rgb(0x6E6E73) };
        let surface = if dark { rgb(0x2C2C2E) } else { rgb(0xFFFFFF) };
        let border = if dark { rgb(0x3B3B3D) } else { rgb(0xD6D6D8) };
        let accent = if dark { rgb(0x0A84FF) } else { rgb(0x007AFF) };
        let name = entry
            .path
            .file_name()
            .and_then(|n| n.to_str())
            .unwrap_or("Untitled")
            .to_string();
        let path = entry.path.clone();
        let size = std::fs::metadata(&entry.path)
            .map(|m| format!("{} KB", m.len() / 1024))
            .unwrap_or_else(|_| "Unknown Size".to_string());

        div()
            .w(CARD_WIDTH)
            .h(CARD_HEIGHT)
            .flex_none()
            .rounded(px(5.))
            .bg(surface)
            .border_1()
            .border_color(border)
            .shadow(small_shadow())
            .child(
                div()
                    .h(px(210.))
                    .mx(px(16.))
                    .mt(px(10.))
                    .rounded(px(5.))
                    .bg(if dark { rgb(0x1C1C1E) } else { rgb(0xF2F2F2) })
                    .flex()
                    .items_center()
                    .justify_center()
                    .child(svg().data(PLACEHOLDER_SVG).size(px(72.)).text_color(muted)),
            )
            .child(
                div()
                    .mt(px(5.))
                    .mx(px(16.))
                    .text_size(px(14.))
                    .text_color(fg)
                    .truncate()
                    .child(name),
            )
            .child(
                div()
                    .mt(px(8.))
                    .mx(px(16.))
                    .flex()
                    .items_center()
                    .gap(px(5.))
                    .text_size(px(11.))
                    .text_color(muted)
                    .child("1/1")
                    .child(div().w(px(1.)).h(px(12.)).bg(accent).opacity(0.5))
                    .child(size)
                    .child(div().flex_1())
                    .child(
                        div()
                            .id(ElementId::Name(format!("star-{}", path.display()).into()))
                            .w(px(24.))
                            .h(px(24.))
                            .flex()
                            .items_center()
                            .justify_center()
                            .cursor_pointer()
                            .text_color(if entry.starred { accent } else { muted })
                            .child(if entry.starred { "★" } else { "☆" }.to_string())
                            .on_click(cx.listener(move |this, _, _, cx| {
                                let path = path.clone();
                                if let Some(entry) =
                                    this.entries.iter_mut().find(|e| e.path == path)
                                {
                                    entry.starred = !entry.starred;
                                    cx.notify();
                                }
                            })),
                    ),
            )
    }

    fn render_browser(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let dark = self.is_dark();
        let bg = if dark { rgb(0x1C1C1E) } else { rgb(0xF7F7F7) };
        let fg = if dark { rgb(0xE7E7E7) } else { rgb(0x1D1D1F) };
        let muted = if dark { rgb(0x98989D) } else { rgb(0x6E6E73) };
        let border = if dark { rgb(0x3B3B3D) } else { rgb(0xD6D6D8) };
        let entries = self.visible_entries();

        let content = if entries.is_empty() {
            let message = if !self.search.is_empty() {
                format!("No results for \"{}\"", self.search)
            } else if self.source == LibrarySource::Starred {
                "No starred files".to_string()
            } else {
                "No recently opened files".to_string()
            };
            div()
                .flex_1()
                .flex()
                .items_center()
                .justify_center()
                .child(div().text_size(px(20.)).text_color(fg).child(message))
                .into_any_element()
        } else if self.style == ViewStyle::Gallery {
            div()
                .flex_1()
                .p(CONTENT_MARGIN)
                .flex()
                .flex_wrap()
                .gap(CARD_GAP)
                .children(entries.iter().map(|entry| self.render_card(entry, cx)))
                .into_any_element()
        } else {
            div()
                .flex_1()
                .p(px(26.))
                .flex()
                .flex_col()
                .gap(px(2.))
                .children(entries.iter().map(|entry| {
                    let name = entry
                        .path
                        .file_name()
                        .and_then(|n| n.to_str())
                        .unwrap_or("Untitled")
                        .to_string();
                    div()
                        .h(px(56.))
                        .flex()
                        .items_center()
                        .gap(px(8.))
                        .px(px(6.))
                        .rounded(px(4.))
                        .text_size(px(13.))
                        .text_color(fg)
                        .hover(|d| d.bg(if dark { rgb(0x2C2C2E) } else { rgb(0xE5E5EA) }))
                        .child(svg().data(PLACEHOLDER_SVG).size(px(40.)).text_color(muted))
                        .when(entry.starred, |d| d.child("★"))
                        .child(div().flex_1().child(name))
                        .child(
                            div()
                                .w(px(80.))
                                .text_right()
                                .text_size(px(12.))
                                .text_color(muted)
                                .child("1/1"),
                        )
                        .into_any_element()
                }))
                .into_any_element()
        };

        div()
            .flex_1()
            .min_w(BROWSER_MIN)
            .h_full()
            .flex()
            .flex_col()
            .bg(bg)
            .text_color(fg)
            .child(content)
            .child(
                div()
                    .h(BOTTOM_BAR_HEIGHT)
                    .flex_none()
                    .flex()
                    .items_center()
                    .justify_end()
                    .gap(px(12.))
                    .px(px(24.))
                    .border_t_1()
                    .border_color(border)
                    .child(action_button("library-tools", "Tools").border_color(border))
                    .child(
                        action_button("library-open-file", "Open File")
                            .border_color(border)
                            .on_click(cx.listener(Self::open_pdf)),
                    ),
            )
    }
}

impl Focusable for LibraryWindow {
    fn focus_handle(&self, _cx: &App) -> FocusHandle {
        self.focus.clone()
    }
}

impl Render for LibraryWindow {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.appearance = window.appearance();
        let dark = self.is_dark();
        #[cfg(target_os = "macos")]
        self.glass.sync(
            if self.sidebar_open {
                f32::from(self.sidebar_width)
            } else {
                0.
            },
            window,
            dark,
        );

        // quiet-md app.rs: the sidebar spans the full window height (including
        // the title bar); the top bar belongs only to the right pane, with a
        // left inset that clears the traffic lights when the sidebar is hidden.
        let right_pane = div()
            .relative()
            .flex_1()
            .min_w_0()
            .h_full()
            .flex()
            .flex_col()
            .child(
                div()
                    .h(TOOLBAR_HEIGHT)
                    .flex_none()
                    .pl(if self.sidebar_open { px(18.) } else { px(92.) })
                    .child(self.render_toolbar(cx)),
            )
            .child(self.render_browser(cx));

        let mut root = div()
            .id("library-workspace")
            .track_focus(&self.focus)
            .key_context("Library")
            .relative()
            .size_full()
            .flex()
            .flex_row()
            // Transparent so the native glass beneath the GPUI surface is visible.
            .bg(hsla(0.0, 0.0, 0.0, 0.0))
            .when(self.sidebar_open, |d| d.child(self.render_sidebar(cx)))
            .child(right_pane);

        if self.sidebar_open {
            root = root.child(
                div()
                    .id("library-sidebar-resize")
                    .absolute()
                    .top(px(0.))
                    .bottom(px(0.))
                    .left(self.sidebar_width - px(6.))
                    .w(px(15.))
                    .cursor(CursorStyle::ResizeLeftRight)
                    .child(
                        div()
                            .absolute()
                            .top(px(0.))
                            .bottom(px(0.))
                            .left(px(6.))
                            .w(px(1.))
                            .bg(if dark { rgb(0x3B3B3D) } else { rgb(0xD6D6D8) })
                            .when(self.sidebar_drag, |line| {
                                line.bg(if dark { rgb(0x0A84FF) } else { rgb(0x007AFF) })
                            }),
                    )
                    .on_mouse_down(
                        MouseButton::Left,
                        cx.listener(|this, _, _, cx| {
                            this.sidebar_drag = true;
                            cx.notify();
                        }),
                    ),
            );
        }

        if self.sidebar_drag {
            root = root.child(
                deferred(
                    div()
                        .id("library-sidebar-resize-overlay")
                        .absolute()
                        .inset_0()
                        .occlude()
                        .cursor(CursorStyle::ResizeLeftRight)
                        .on_mouse_move(cx.listener(|this, ev: &MouseMoveEvent, window, cx| {
                            if ev.pressed_button != Some(MouseButton::Left) {
                                this.sidebar_drag = false;
                                cx.notify();
                                return;
                            }
                            let x = ev.position.x;
                            let max = SIDEBAR_MAX
                                .min(px(f32::from(window.viewport_size().width)) - BROWSER_MIN);
                            this.sidebar_width = x.clamp(SIDEBAR_MIN, max.max(SIDEBAR_MIN));
                            cx.notify();
                        }))
                        .on_mouse_up(
                            MouseButton::Left,
                            cx.listener(|this, _, _, cx| {
                                this.sidebar_drag = false;
                                cx.notify();
                            }),
                        )
                        .on_mouse_up_out(
                            MouseButton::Left,
                            cx.listener(|this, _, _, cx| {
                                this.sidebar_drag = false;
                                cx.notify();
                            }),
                        ),
                )
                .with_priority(4),
            );
        }
        root
    }
}
