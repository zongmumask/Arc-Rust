//! Request messages accepted by the PDFium worker thread.

use super::guard::RenderGuard;
use super::types::{ActorError, DocumentInfo};
use arc_pdf_core::{PageId, Rect};
use arc_pdf_engine::Raster;
use futures::channel::oneshot;
use std::path::PathBuf;

pub type Reply<T> = oneshot::Sender<Result<T, ActorError>>;

pub enum Request {
    Open {
        path: PathBuf,
        reply: Reply<DocumentInfo>,
    },
    Metadata(Reply<DocumentInfo>),
    RenderPage {
        page: PageId,
        scale: f32,
        guard: RenderGuard,
        reply: Reply<Raster>,
    },
    RenderRegion {
        page: PageId,
        rect: Rect,
        scale: f32,
        guard: RenderGuard,
        reply: Reply<Option<Raster>>,
    },
}
