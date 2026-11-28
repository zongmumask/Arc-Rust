# PDFWindow / PDFView 迁移设计（阅读优先）

> 状态：A–E 初步实施中；只覆盖阅读路径，批注/表单暂不迁移。
> 目标：在 Rust + GPUI 中复刻 ArcPDF-macOS 的“预览 + 瓦片 + 缓存 + 可见性二次判定”行为语义，不照搬 AppKit 类型。

## 1. Swift 基线（源码事实）

- `Quill/Controllers/PDFWindowController.swift:13,68`：窗口根为三栏 split。
- `Quill/Controllers/PDFWorkspaceSplitViewController.swift:65-91,177-188`：左/中/右三栏，左默认折叠、250–400pt；右默认折叠、250–400pt；中栏可伸缩；保存名 `WindowContentSplitView`。
- `Quill/Controllers/PDFViewController.swift:51-77`：持有 `Document` 与 `PDFView`。
- `Quill/PDFKit/View/PDFView.swift:13,145-177,191-196,330-333`：`NSScrollView` 子类，magnification 0.1–32，clip bounds 变化驱动页面可见性更新。
- `Quill/PDFKit/View/DocumentView.swift:47-49,255-329`：预加载带 `visibleRect` 上下各扩展 `0.8 × height`；页面 View 延迟 0.15s 销毁并重查扩展区。
- `Quill/PDFKit/View/PageView.swift:203-278,846-946`：低清整页 preview + 高清 128pt 逻辑瓦片；preview 用扩展区判定；瓦片入队前、串行队列内 `renderRect` 的二次守卫、完成后均检查严格可见区。
- `Quill/Extensions/Document+.swift:38-51`、`ArcPDFKit/Classes/Render/FPDFRenderCache.swift:11,47-116`：preview cache per document、WebP 压缩、1GB 上限、超限时淘汰一个最早插入项；tile cache per PageView、原始位图、无上限、随页面销毁释放。
- `ArcPDFKit/Classes/PDFium/Page.swift:253,338-347`：`shouldRenderBlock` 在 entry 与真正 FPDF 调用前各检查一次。

## 2. Rust 模块布局

- `crates/arc-pdf-ui/src/pdf.rs`：窗口级三栏骨架与可拖拽分隔条（sidebar/inspector 先留白）。
- `crates/arc-pdf-view/src/`：明确的 GPUI 视图类型：
  - `pdf_view.rs`：`PdfView`（对应 Swift `PDFView + DocumentView`），滚动容器、页面几何、viewport、可见页生命周期。
  - `page_view.rs`：`PageView`（对应 Swift `PageView`），单页 preview、tiles、guards 与绘制状态。
- `crates/arc-pdf-viewer/src/`：阅读管线，按职责拆分：
  - `actor.rs` + `actor/{guard,message,types,worker}.rs`：专用线程 PDFium actor、请求消息、可见性守卫。
  - `controller.rs` + `controller/{preview,tiles,lifecycle,paint,toolbar,page_state,image}.rs`：保留给过渡/复用；当前窗口挂载 `PdfView`，`PageView` 为独立 entity。
  - `cache.rs`：文档级 WebP preview cache 与 FIFO-by-timestamp 淘汰。
  - `geometry.rs`：页面 frame、严格可见/扩展预取集合、preview scale（2×，1600px 上限）。
- `crates/arc-pdf-engine/src/page.rs` / `document.rs`：`render_region` 在真正 PDFium 调用前重复检查 guard。

## 3. 运行语义

1. 打开文档：actor 在专用线程 `FPDF_LoadMemDocument`，读取页数/页面尺寸回 UI。
2. 页面进入扩展预取带：请求整页 preview（2× 上限 1600px）；cache miss 时 actor 渲染，编码 WebP 存 per-document cache；页面/瓦片离开时翻转 guard，串行队列执行前即放弃过期渲染。
3. 页面进入严格可见区：按 128pt 网格请求瓦片；每块瓦片有独立 guard；完成后仍可见才写 per-page tile cache。
4. 页面离开扩展带 150ms 后销毁 PageState 与其 tiles；preview 保留在 cache。
5. 缩放改变页面绘制尺寸，按 bucket（1/4 octave）量化 tile scale，并清空旧 scale 的 tiles。

## 4. 当前简化/后续

- 单页连续滚动；双页模式、阅读位置恢复、密码、表单/批注尚未迁移。
- 页面可见性通过 GPUI `ScrollHandle::bounds` + render 驱动，未使用自定义 Element/virtual list。
- preview cache 当前以 page id 为 key，per document 实例；后续可引入 `DocumentId` 复用全局 cache。
- 失效协议（dirty rect → 预渲染 → 一次性重绘）留给批注/表单阶段。
