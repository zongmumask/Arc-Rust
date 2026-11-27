//! Native Liquid Glass material for the Library sidebar (macOS 26+).
//!
//! Mirrors quiet-md `src/native_header.rs`: the `NSGlassEffectView` sits
//! *beneath* GPUI's transparent surface so GPUI keeps drawing the text and
//! handling events. The view's frame follows the sidebar width (plus a small
//! underlap that hides the rim under the browser pane).

use std::cell::RefCell;

use gpui::Window;
use objc2::msg_send;
use objc2::runtime::AnyObject;
use objc2_foundation::{NSPoint, NSRect, NSSize};
use raw_window_handle::{HasWindowHandle, RawWindowHandle};

/// How far the glass runs under the browser pane, past the sidebar's edge.
const UNDERLAP: f64 = 12.;

#[derive(Clone, Copy, PartialEq)]
struct Frame {
    x: f64,
    y: f64,
    width: f64,
    height: f64,
}

struct GlassView {
    view: *mut AnyObject,
    frame: Frame,
    hidden: bool,
    dark: Option<bool>,
}

/// Owns the native glass view; dropped with the LibraryWindow entity.
#[derive(Default)]
pub struct NativeSidebarGlass {
    slot: RefCell<Option<GlassView>>,
}

impl NativeSidebarGlass {
    pub fn supported() -> bool {
        objc2::runtime::AnyClass::get("NSGlassEffectView").is_some()
    }

    /// Fit the glass to the sidebar. `width == 0` hides it (sidebar collapsed).
    /// Call this during `Render`, before GPUI presents the frame.
    pub fn sync(&self, width: f32, window: &Window, dark: bool) {
        if !Self::supported() {
            return;
        }
        let Ok(handle) = HasWindowHandle::window_handle(window) else {
            return;
        };
        let RawWindowHandle::AppKit(handle) = handle.as_raw() else {
            return;
        };
        unsafe {
            let gpui_view = handle.ns_view.as_ptr() as *mut AnyObject;
            let parent: *mut AnyObject = msg_send![gpui_view, superview];
            if parent.is_null() {
                return;
            }
            let base: NSRect = msg_send![gpui_view, frame];
            let frame = Frame {
                x: base.origin.x,
                y: base.origin.y,
                width: if width > 0. {
                    width as f64 + UNDERLAP
                } else {
                    0.
                },
                height: base.size.height,
            };
            let mut slot = self.slot.borrow_mut();
            if slot.is_none() && width > 0. {
                let cls = objc2::class!(NSGlassEffectView);
                let view: *mut AnyObject = msg_send![cls, alloc];
                let rect = NSRect::new(
                    NSPoint::new(frame.x, frame.y),
                    NSSize::new(frame.width, frame.height),
                );
                let view: *mut AnyObject = msg_send![view, initWithFrame:rect];
                let _: () = msg_send![view, setCornerRadius:0f64];
                let _: () =
                    msg_send![parent, addSubview:view positioned:-1isize relativeTo:gpui_view];
                window.present_next_frame_in_transaction();
                *slot = Some(GlassView {
                    view,
                    frame,
                    hidden: false,
                    dark: None,
                });
            }
            let Some(glass) = slot.as_mut() else {
                return;
            };
            if glass.frame != frame {
                let rect = NSRect::new(
                    NSPoint::new(frame.x, frame.y),
                    NSSize::new(frame.width, frame.height),
                );
                let _: () = msg_send![glass.view, setFrame:rect];
                glass.frame = frame;
                window.present_next_frame_in_transaction();
            }
            if glass.dark != Some(dark) {
                let name = if dark {
                    c"NSAppearanceNameDarkAqua"
                } else {
                    c"NSAppearanceNameAqua"
                };
                let name: *mut AnyObject =
                    msg_send![objc2::class!(NSString), stringWithUTF8String:name.as_ptr()];
                let appearance: *mut AnyObject =
                    msg_send![objc2::class!(NSAppearance), appearanceNamed:name];
                let _: () = msg_send![glass.view, setAppearance:appearance];
                glass.dark = Some(dark);
                window.present_next_frame_in_transaction();
            }
            let hidden = width <= 0.;
            if hidden != glass.hidden {
                let _: () = msg_send![glass.view, setHidden:hidden];
                glass.hidden = hidden;
                window.present_next_frame_in_transaction();
            }
        }
    }
}

impl Drop for GlassView {
    fn drop(&mut self) {
        unsafe {
            let _: () = msg_send![self.view, removeFromSuperview];
            let _: () = msg_send![self.view, release];
        }
    }
}
