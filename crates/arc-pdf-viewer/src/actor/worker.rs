//! Dedicated-thread loop that owns the `arc_pdf_engine::Document`.
//!
//! `Document` is `!Send`; all PDFium calls happen on this single worker.
//! Requests are handled strictly in FIFO order, mirroring ArcPDFKit's
//! process-wide serial `pdfiumQueue`.

use super::guard::RenderGuard;
use super::message::Request;
use super::types::{ActorError, DocumentInfo};
use arc_pdf_core::PageId;
use arc_pdf_engine::Document;
use futures::StreamExt;
use std::path::PathBuf;

fn document_info(doc: &Document) -> DocumentInfo {
    let page_count = doc.page_count();
    let page_sizes = (0..page_count)
        .filter_map(|index| {
            doc.page_size(PageId(index as u32))
                .ok()
                .map(|(width, height)| arc_pdf_core::Size::new(width, height))
        })
        .collect();
    DocumentInfo {
        page_count,
        page_sizes,
    }
}

/// Processes one request against the actor's document.
fn handle(request: Request, doc: &mut Option<Document>, runtime: &arc_pdf_engine::PdfiumRuntime) {
    match request {
        Request::Open { path, reply } => {
            let result = runtime
                .open_document(&path)
                .map(|opened| {
                    let info = document_info(&opened);
                    *doc = Some(opened);
                    info
                })
                .map_err(|error| ActorError::Engine(error.to_string()));
            let _ = reply.send(result);
        }
        Request::Metadata(reply) => {
            let result = doc
                .as_ref()
                .ok_or_else(|| ActorError::Engine("no document open".into()))
                .map(document_info);
            let _ = reply.send(result);
        }
        Request::RenderPage {
            page,
            scale,
            guard,
            reply,
        } => {
            let result = doc
                .as_ref()
                .ok_or_else(|| ActorError::Engine("no document open".into()))
                .and_then(|document| render_page(document, page, scale, &guard));
            let _ = reply.send(result);
        }
        Request::RenderRegion {
            page,
            rect,
            scale,
            guard,
            reply,
        } => {
            let result = doc
                .as_ref()
                .ok_or_else(|| ActorError::Engine("no document open".into()))
                .and_then(|document| {
                    document
                        .render_region(page, rect, scale, &|| guard.valid())
                        .map_err(|error| ActorError::Engine(error.to_string()))
                });
            let _ = reply.send(result);
        }
    }
}

fn render_page(
    document: &Document,
    page: PageId,
    scale: f32,
    guard: &RenderGuard,
) -> Result<arc_pdf_engine::Raster, ActorError> {
    if !guard.valid() {
        return Err(ActorError::Engine("stale".into()));
    }
    let raster = document
        .render_page(page, scale)
        .map_err(|error| ActorError::Engine(error.to_string()))?;
    if !guard.valid() {
        return Err(ActorError::Engine("stale".into()));
    }
    Ok(raster)
}

/// Runs the actor loop until all senders are dropped.
pub(super) async fn run(
    mut receiver: futures::channel::mpsc::UnboundedReceiver<Request>,
    _first_path: PathBuf,
) {
    let runtime = arc_pdf_engine::PdfiumRuntime::init();
    let mut document: Option<Document> = None;
    while let Some(request) = receiver.next().await {
        handle(request, &mut document, &runtime);
    }
    drop(document);
    drop(runtime);
}
