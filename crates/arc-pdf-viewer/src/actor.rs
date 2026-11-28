//! PDFium actor: client handle plus its internal message/worker modules.

mod guard;
mod message;
mod types;
mod worker;

pub use guard::RenderGuard;
pub use types::{ActorError, DocumentInfo};

use arc_pdf_core::{PageId, Rect};
use arc_pdf_engine::Raster;
use futures::channel::{mpsc, oneshot};
use gpui::{App, BackgroundExecutor};
use message::Request;
use std::path::{Path, PathBuf};

/// Cloneable client handle. Requests are delivered in order to one dedicated
/// thread that owns the PDFium document.
#[derive(Clone)]
pub struct PdfiumActor {
    sender: mpsc::UnboundedSender<Request>,
}

impl PdfiumActor {
    pub fn spawn(_cx: &App, executor: &BackgroundExecutor) -> Self {
        let (sender, receiver) = mpsc::unbounded::<Request>();
        executor
            .scheduler_executor()
            .spawn_dedicated(move |_| worker::run(receiver, PathBuf::new()))
            .detach();
        Self { sender }
    }

    pub fn open(
        &self,
        path: impl AsRef<Path>,
    ) -> oneshot::Receiver<Result<DocumentInfo, ActorError>> {
        let (reply, receiver) = oneshot::channel();
        let _ = self.sender.unbounded_send(Request::Open {
            path: path.as_ref().to_path_buf(),
            reply,
        });
        receiver
    }

    pub fn metadata(&self) -> oneshot::Receiver<Result<DocumentInfo, ActorError>> {
        let (reply, receiver) = oneshot::channel();
        let _ = self.sender.unbounded_send(Request::Metadata(reply));
        receiver
    }

    pub fn render_page(
        &self,
        page: PageId,
        scale: f32,
        guard: RenderGuard,
    ) -> oneshot::Receiver<Result<Raster, ActorError>> {
        let (reply, receiver) = oneshot::channel();
        let _ = self.sender.unbounded_send(Request::RenderPage {
            page,
            scale,
            guard,
            reply,
        });
        receiver
    }

    pub fn render_region(
        &self,
        page: PageId,
        rect: Rect,
        scale: f32,
        guard: RenderGuard,
    ) -> oneshot::Receiver<Result<Option<Raster>, ActorError>> {
        let (reply, receiver) = oneshot::channel();
        let _ = self.sender.unbounded_send(Request::RenderRegion {
            page,
            rect,
            scale,
            guard,
            reply,
        });
        receiver
    }
}
