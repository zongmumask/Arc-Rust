#ifndef FPDF_ANNOT_EXT_H_
#define FPDF_ANNOT_EXT_H_

#include "fpdfview.h"
#include "fpdf_annot.h"
#include "fpdf_edit.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create a stamp annotation with image data
// Parameters:
//  - page: handle to a page
//  - imageWidth: width of the image in pixels
//  - imageHeight: height of the image in pixels
//  - rect_left: left coordinate of annotation rectangle
//  - rect_bottom: bottom coordinate of annotation rectangle
//  - rect_right: right coordinate of annotation rectangle
//  - rect_top: top coordinate of annotation rectangle
//  - rawData: RGBA image data (4 bytes per pixel, premultiplied alpha)
// Returns:
//  - The created annotation handle, or NULL on failure
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDF_CreateStampAnnotWithImageData(FPDF_PAGE page,
                                   int imageWidth,
                                   int imageHeight,
                                   float rect_left,
                                   float rect_bottom,
                                   float rect_right,
                                   float rect_top,
                                   const void* rawData);

// Safe variant of FPDF_CreateStampAnnotWithImageData.
// Parameters:
//  - rawData: RGBA image data (4 bytes per pixel, premultiplied alpha)
//  - rawDataSize: size of rawData in bytes (must be >= imageWidth*imageHeight*4)
// Returns:
//  - The created annotation handle, or NULL on failure (including when
//    rawDataSize is too small or imageWidth*imageHeight*4 would overflow).
// Notes:
//  - Prefer this over FPDF_CreateStampAnnotWithImageData, which has no way to
//    validate the buffer size.
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDF_CreateStampAnnotWithImageData2(FPDF_PAGE page,
                                    int imageWidth,
                                    int imageHeight,
                                    float rect_left,
                                    float rect_bottom,
                                    float rect_right,
                                    float rect_top,
                                    const void* rawData,
                                    unsigned long rawDataSize);

// Set annotation subtype (extended API)
// Parameters:
//  - annot: annotation handle
//  - subtype: annotation subtype (FPDF_ANNOTATION_SUBTYPE)
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Only updates the /Subtype in the annotation dictionary.
//    To reflect appearance changes, regenerate page content or refresh appearance after calling.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetSubtype(FPDF_ANNOTATION annot, FPDF_ANNOTATION_SUBTYPE subtype);

// Set annotation appearance stream
// Parameters:
//  - annot: annotation handle
//  - appearanceMode: appearance mode (normal, rollover, or down)
//  - value: appearance stream content as wide string
//  - blendMode: blend mode for the appearance
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Sets the appearance stream for the specified appearance mode.
//    The appearance stream defines how the annotation is rendered.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetAppearance(FPDF_ANNOTATION annot,
                        FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                        FPDF_WIDESTRING value,
                        FPDF_BYTESTRING blendMode);

// Render annotation to a bitmap
// Parameters:
//  - bitmap: handle to a device independent bitmap (as the output buffer)
//  - page: handle to the page containing the annotation
//  - annot: handle to the annotation to render
//  - matrix: transformation matrix for rendering (can be NULL for identity matrix)
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - The bitmap must be created with FPDFBitmap_Create or FPDFBitmap_CreateEx before calling.
//  - The matrix parameter allows for scaling, rotation, and translation of the annotation.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_RenderToBitmap(FPDF_BITMAP bitmap,
                         FPDF_PAGE page,
                         FPDF_ANNOTATION annot,
                         const FS_MATRIX* matrix);

// Get annotation appearance stream blend mode
// Parameters:
//  - annot: annotation handle
//  - appearanceMode: appearance mode (normal, rollover, or down)
//  - buffer: buffer to receive the blend mode string (can be NULL to query length)
//  - buflen: length of the buffer in bytes
// Returns:
//  - The length of the blend mode string in bytes (including null terminator)
//  - Returns 0 if blend mode is not found or on error
// Notes:
//  - Call with buffer=NULL to get required buffer size first
//  - The blend mode is retrieved from the ExtGState in the appearance stream's Resources
//  - Common blend modes: "Normal", "Multiply", "Screen", "Overlay", etc.
FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDFAnnot_GetAppearanceBlendMode(FPDF_ANNOTATION annot,
                                 FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                                 void* buffer,
                                 unsigned long buflen);

// Get annotation bounding box (BBox) from appearance stream
// Parameters:
//  - annot: annotation handle
//  - appearanceMode: appearance mode (normal, rollover, or down)
//  - rect: pointer to FS_RECTF to receive the bounding box
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - The BBox defines the coordinate space for the appearance stream
//  - If no BBox is found in the appearance stream, returns FALSE
//  - The returned rect is in the appearance stream's coordinate space
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetAppearanceBBox(FPDF_ANNOTATION annot,
                            FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                            FS_RECTF* rect);

// Get annotation color directly from dictionary (without AP stream check)
// Parameters:
//  - annot: annotation handle
//  - type: color type (FPDFANNOT_COLORTYPE_Color or FPDFANNOT_COLORTYPE_InteriorColor)
//  - R: pointer to receive red component (0-255)
//  - G: pointer to receive green component (0-255)
//  - B: pointer to receive blue component (0-255)
//  - A: pointer to receive alpha component (0-255)
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Unlike FPDFAnnot_GetColor, this method does NOT check for AP stream existence
//  - Always retrieves color from annotation dictionary (/C or /IC key)
//  - Returns default colors if color is not set in dictionary
//  - For Highlight annotations, default is yellow (255, 255, 0)
//  - For other annotations, default is black (0, 0, 0)
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetColorFromDict(FPDF_ANNOTATION annot,
                            FPDFANNOT_COLORTYPE type,
                            unsigned int* R,
                            unsigned int* G,
                            unsigned int* B,
                            unsigned int* A);

// Read any color-array key (e.g. /QLFillColor, /QLBorderColor, /C) from the
// annotation dictionary as 0..255 RGB. Returns FALSE if the key is absent or
// not a length>=3 color array. Used by FillSign vector-stamp undo restore to
// snapshot creation parameters at delete time.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetColorArr(FPDF_ANNOTATION annot,
                      const char* key,
                      unsigned int* R,
                      unsigned int* G,
                      unsigned int* B);

// TRUE if the top-level annotation dictionary contains the given key.
// Distinguishes an explicitly-set key (e.g. /C with a real border color) from an
// absent one: shapes with no border color (Apple omits /C, baking a transparent
// /CA 0 stroke into the AP) need to skip stroking, whereas FPDFAnnot_GetColorFromDict
// synthesizes opaque black for a missing /C and loses that distinction.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_KeyExists(FPDF_ANNOTATION annot,
                    const char* key);

// Set annotation color directly to dictionary (without AP stream check)
// Parameters:
//  - annot: annotation handle
//  - type: color type (FPDFANNOT_COLORTYPE_Color or FPDFANNOT_COLORTYPE_InteriorColor)
//  - R: red component (0-255)
//  - G: green component (0-255)
//  - B: blue component (0-255)
//  - A: alpha component (0-255)
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Unlike FPDFAnnot_SetColor, this method does NOT check for AP stream existence
//  - Always sets color to annotation dictionary (/C or /IC key)
//  - Sets opacity to /CA key
//  - Use this method when you want to update color even if AP stream exists
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetColorToDict(FPDF_ANNOTATION annot,
                          FPDFANNOT_COLORTYPE type,
                          unsigned int R,
                          unsigned int G,
                          unsigned int B,
                          unsigned int A);

// Update the AP (Appearance Stream) for a stamp annotation after move/resize.
// Updates the Form XObject's BBox to local coordinates [0, 0, width, height]
// and regenerates the stream content to position the embedded image correctly.
// Parameters:
//  - annot: handle to a stamp annotation
//  - rect_left: left coordinate of the new annotation rectangle
//  - rect_bottom: bottom coordinate of the new annotation rectangle
//  - rect_right: right coordinate of the new annotation rectangle
//  - rect_top: top coordinate of the new annotation rectangle
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Call this after FPDFAnnot_SetRect and image matrix update when moving/resizing a stamp.
//  - The BBox is set to local coordinates [0, 0, width, height] so the AP
//    remains valid regardless of the annotation's position on the page.
//  - matrix_* is the image object's transformation (page/user space). The rotation is
//    BAKED into the AP's cm (translated to local AP coords) because the image XObject
//    carries no /Matrix — without it, FPDFAnnot_RenderToBitmap / reopen lose the angle.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_UpdateStampAP(FPDF_ANNOTATION annot,
                        float rect_left,
                        float rect_bottom,
                        float rect_right,
                        float rect_top,
                        float matrix_a,
                        float matrix_b,
                        float matrix_c,
                        float matrix_d,
                        float matrix_e,
                        float matrix_f);

// Set a GoTo destination action on a link annotation.
// Parameters:
//  - annot: handle to a link annotation (must be FPDF_ANNOT_LINK subtype)
//  - page_index: zero-based page index to navigate to
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Sets the /A dictionary with /S /GoTo and /D [page_index /Fit]
//  - Overwrites any existing action on the annotation
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetGotoDestination(FPDF_ANNOTATION annot, int page_index);

// Function: FPDFAnnot_SetGotoDestinationXYZ
// Set a GoTo destination with specific X/Y position on the target page.
// Parameters:
//  - annot: handle to a link annotation (must be FPDF_ANNOT_LINK subtype)
//  - page_index: zero-based page index to navigate to
//  - x: X position on the target page in PDF coordinates (0 = left edge)
//  - y: Y position on the target page in PDF coordinates (0 = bottom edge)
//  - zoom: zoom factor (0 = retain current zoom)
// Returns:
//  - TRUE on success, FALSE on failure
// Notes:
//  - Sets the /A dictionary with /S /GoTo and /D [page_index /XYZ x y zoom]
//  - Overwrites any existing action on the annotation
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetGotoDestinationXYZ(FPDF_ANNOTATION annot,
                                 int page_index,
                                 float x,
                                 float y,
                                 float zoom);

// ============================================================================
// Fill & Sign 支持：FreeText 文本注解 & 文本图章
// （对齐 PDF Expert Fill & Sign 的文本/日期/签名/勾√叉×/图章/图片类型）
// ============================================================================

// 创建 FreeText 文本注解（对应"文本"、"日期"）。rect 为 PDF 用户空间(y-up)。
// 字体固定用 AcroForm/DR 里的标准 Helvetica（无 AcroForm 时自动建兜底，
// 与 FPDF_Form_AddTextField 同机制）。alignment: 0=左 1=中 2=右。
// 成功返回注释句柄，失败返回 NULL。样式改动后调 FPDFAnnot_GenerateFreeTextAP 重画。
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDF_CreateFreeTextAnnot(FPDF_PAGE page,
                         const FS_RECTF* rect,
                         FPDF_WIDESTRING text,
                         float font_size,
                         unsigned int colorR,
                         unsigned int colorG,
                         unsigned int colorB,
                         unsigned int colorA,
                         int alignment);

// 样式（Contents/DA/Q/C）已变后重生成 FreeText 外观。返回 TRUE 成功。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GenerateFreeTextAP(FPDF_ANNOTATION annot);

// 创建文本图章（对应"签名"、"勾√"、"叉×"、"APPROVED"等）。文本色经 /C，
// 背景/边框/字号/对齐存扩展键（应用私有前缀 /QL）：/QLFillColor(+Alpha) /
// QLBorderColor(+Width) /QLFontSize /QLTextAlignment。样式改动后调
// FPDFAnnot_GenerateTextStampAP 重画。
// textA/bgA 为透明度 0..255。has_background 为 TRUE 且 bgA>0 时画背景。
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDF_CreateTextStampAnnot(FPDF_PAGE page,
                          const FS_RECTF* rect,
                          FPDF_WIDESTRING text,
                          float font_size,
                          unsigned int textR,
                          unsigned int textG,
                          unsigned int textB,
                          unsigned int textA,
                          FPDF_BOOL has_background,
                          unsigned int bgR,
                          unsigned int bgG,
                          unsigned int bgB,
                          unsigned int bgA,
                          unsigned int borderR,
                          unsigned int borderG,
                          unsigned int borderB,
                          float border_width);

// 图章样式已变后重生成外观。返回 TRUE 成功。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GenerateTextStampAP(FPDF_ANNOTATION annot);

// 创建 PDF Expert 式矢量图章（圆角矩形底色 + Helvetica-Bold 粗体字，可带渐变底纹）。
// 与 PDF Expert /SBApproved 同机制：圆角矩形 path + 标准粗体字体，非同 Scratch 文本字形。
// 文字经 /C，底色 /QLFillColor、描边 /QLBorderColor(+Width)，圆角弧度 /QLCorner。
// corner_radius 0 退化为直角矩形。样式改动后调 FPDFAnnot_GenerateExpertStampAP 重画。
FPDF_EXPORT FPDF_ANNOTATION FPDF_CALLCONV
FPDF_CreateExpertStampAnnot(FPDF_PAGE page,
                            const FS_RECTF* rect,
                            FPDF_WIDESTRING text,
                            float font_size,
                            unsigned int textR,
                            unsigned int textG,
                            unsigned int textB,
                            unsigned int fillR,
                            unsigned int fillG,
                            unsigned int fillB,
                            unsigned int strokeR,
                            unsigned int strokeG,
                            unsigned int strokeB,
                            float corner_radius);

// PDF Expert 式图章样式已变后重生成外观。返回 TRUE 成功。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GenerateExpertStampAP(FPDF_ANNOTATION annot);

// 改字号：FreeText 重写 /DA 字号；图章写 /QLFontSize。不自动重画。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetFontSize(FPDF_ANNOTATION annot, float size);

// 改对齐：FreeText 写 /Q；图章写 /QLTextAlignment。q: 0=左 1=中 2=右。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetTextAlignment(FPDF_ANNOTATION annot, int q);

// 改背景色/透明度：FreeText 写 /C(+/CA)；图章写 /QLFillColor(+Alpha)。
// A=0 且为图章时清除背景。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetBackground(FPDF_ANNOTATION annot,
                        unsigned int R,
                        unsigned int G,
                        unsigned int B,
                        unsigned int A);

// 改图章边框：写 /QLBorderColor + /QLBorderWidth。width<=0 时清除边框。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetStampBorder(FPDF_ANNOTATION annot,
                         unsigned int R,
                         unsigned int G,
                         unsigned int B,
                         float width);

// 统一按 subtype 重生成外观（FreeText→GenerateFreeTextAP，Stamp→GenerateTextStampAP）。
// 供 FormatPanel 改完一组样式后只调这一个收口。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GenerateAP(FPDF_ANNOTATION annot);

// 把已嵌入文档的字体对象注册进 AcroForm 的 /DR/Font/<font_name>，
// 使 FreeText 的 /DA `/<font_name> <size> Tf` 能被 CPDF_GenerateAP::
// GenerateFreeTextAP 解析（否则查不到会回退 base14）。返回 TRUE 成功。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_RegisterFontToAcroFormDR(FPDF_DOCUMENT document,
                                   FPDF_FONT font,
                                   const char* font_name);

// 测 FreeText 批注文本的可编辑光标（caret）几何，与 CPDF_GenerateAP::
// GenerateFreeTextAP 用同一套 CPVT 排版，保证光标与最终 /AP 渲染对齐。
// text: 当前 /Contents 全文（UTF-16LE）；caretUTF16Len: caret 相对全文开头的
// UTF-16 前缀长度。输出 caretX/caretBottomY/caretTopY 为 annot 用户空间坐标
// （y-up，已含边框收缩与垂直居中）。返回 TRUE 且 outAccurate=TRUE 表示精确。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_MeasureFreeTextText(FPDF_ANNOTATION annot,
                              FPDF_WIDESTRING text,
                              int caretUTF16Len,
                              double* outCaretX,
                              double* outCaretBottomY,
                              double* outCaretTopY,
                              FPDF_BOOL* outAccurate);

// 取 FreeText 批注底层字典的对象号（objnum），用作文档级 AcroForm /DR/Font
// 唯一槽位 key：不同批注不共享同一个嵌入字体子集槽位，避免后写覆盖（见
// applyFillSignStyle 的 fillSignFontKey）。无有效对象号返回 0。
FPDF_EXPORT unsigned int FPDF_CALLCONV
FPDFAnnot_GetObjNum(FPDF_ANNOTATION annot);

// 线型批注端点样式（读 /LE 数组），取值与 PDFium FPDF_LINEENDING 一致。
typedef enum {
  FPDFExt_LineEnding_None = 0,
  FPDFExt_LineEnding_Square = 1,
  FPDFExt_LineEnding_Circle = 2,
  FPDFExt_LineEnding_Diamond = 3,
  FPDFExt_LineEnding_OpenArrow = 4,
  FPDFExt_LineEnding_ClosedArrow = 5,
} FPDFExt_LineEnding;

// 读取线型批注 /LE 起止端样式。start_style 为线段起点、end_style 为终点。
// 批注无 /LE 或非 Line 类型返回 FALSE（out 保持原值）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetLineEndingStyle(FPDF_ANNOTATION annot,
                             FPDFExt_LineEnding* start_style,
                             FPDFExt_LineEnding* end_style);

// 写线型批注标准端点键 /L（PDF 坐标，起止两点）。用于移动/缩放后更新标准几何，
// 使第三方/按 /L 渲染的路径（含缩略图）跟随新位置，而非仅靠 App 私有键 QLShapeLine。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetLine(FPDF_ANNOTATION annot, FS_POINTF start, FS_POINTF end);

// 边框样式枚举，取值对应批注字典 /BS /S（含缺省 Solid）。
typedef enum {
  FPDFExt_BorderStyle_Solid = 0,
  FPDFExt_BorderStyle_Dashed = 1,
  FPDFExt_BorderStyle_None = 2,
  FPDFExt_BorderStyle_Beveled = 3,
  FPDFExt_BorderStyle_Inset = 4,
  FPDFExt_BorderStyle_Underline = 5,
  FPDFExt_BorderStyle_Cloud = 6,
} FPDFExt_BorderStyle;

// 读取批注描边样式 /BS（第三方批注常把线宽/虚线/线帽放在 /BS，而非旧式 /Border）。
// 读取 /BS /S 样式、/BS /W 线宽、/BS /LC 线帽、/BS /LI 线接合；无 /BS 时回退 /Border[2] 作为线宽、
// 样式视为 Solid、帽/接合视为默认 Round。|dash_array| 非空时把 /BS /D 数组写入该缓冲，
// |dash_count| 入参为缓冲容量、返回实际个数（虚线时为 dash 长度序列，非虚线为 0）。
// |line_cap|/|line_join| 输出线帽/线接合（0=Butt/Miter, 1=Round, 2=ProjectingSquare/Bevel）。
// 成功返回 TRUE；|annot| 为空返回 FALSE（out 保持原值）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_GetBorderStyle(FPDF_ANNOTATION annot,
                         FPDFExt_BorderStyle* border_style,
                         float* line_width,
                         float* dash_array,
                         int* dash_count,
                         int* line_cap,
                         int* line_join);

// 写批注描边样式 /BS：/S 名称 + /D 虚线序列（实线时移除 /D），保留既有 /W。style_code
// 沿用 FPDFExt_BorderStyle（0 Solid, 1 Dashed, 2 None, ...）。供线型持久化——矢量化 layer
// 读 /BS /S + /D 还原虚线/点线。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetBorderStyle(FPDF_ANNOTATION annot,
                         int style_code,
                         const float* dash_array,
                         int dash_count);

// 以显式 BBox 尺寸构建外观流 /AP（BBox = [0, 0, bbox_width, bbox_height]，内容用
// 局部坐标）。与 FPDFAnnot_SetAppearance 区别：BBox 由调用方给定而非读批注当前 /Rect，
// 因此内容可归一化到批注自身 AABB 内（纯平移只改 /Rect，与外观流位置解耦、与 /Rect
// 写入时机无关），避免用页面绝对坐标 + 陈旧 /Rect 当 BBox 导致外观被裁/空白。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDFAnnot_SetAppearanceWithBBox(FPDF_ANNOTATION annot,
                                FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                                float bbox_width,
                                float bbox_height,
                                FPDF_WIDESTRING value,
                                FPDF_BYTESTRING blendMode);

// ──────── AP 外观流矢量几何提取（与 PDFium 渲染逐算子一致）────────
// 完全复用 PDFium 内容解释器（CPDF_Form / CPDF_StreamContentParser）解析批注
// /AP /N 外观流，把其中每个 PathObject 的描边/填充样式与贝塞尔路径一并压平返回，
// 供 App 侧 ShapeAnnotationLayer 重建矢量。样式与 path 全部取自 AP（渲染权威），
// 不读 /BS 等声明键——只管"AP 实际画了什么"。
//
// 路径点以 form 本地坐标给出（已叠加各 PathObject 的 CTM）。线宽等尺寸单位为
// form 本地用户空间，App 需按 BBox→Rect 的缩放映射到层本地坐标。

// 路径算子码：与内容流 m/l/c/h 一一对应。curveTo(c) 一次占 3 个控制/锚点。
#define FPDFExt_PathOp_Move 0
#define FPDFExt_PathOp_Line 1
#define FPDFExt_PathOp_Curve 2
#define FPDFExt_PathOp_Close 3

// 单个 PathObject 的扁平化结果。points 为 x,y 交错数组（float point_count 对）。
typedef struct FPDFExt_AppearancePath {
  int op_count;   // ops 长度
  const int* ops; // 每个元素一个 FPDFExt_PathOp
  int point_count;
  const float* points;

  int has_fill;   // 非 0 = 有填充
  int fill_type;  // 1=nonzero(winding), 2=evenodd
  int has_stroke; // 非 0 = 有描边

  float line_width;   // form 本地用户空间
  float miter_limit;
  int line_cap;       // 0 butt, 1 round, 2 square（CFX_GraphStateData::LineCap）
  int line_join;      // 0 miter, 1 round, 2 bevel（CFX_GraphStateData::LineJoin）

  int dash_count;     // 0 = 实线
  const float* dash;  // dash_count 个长度
  float dash_phase;

  int has_stroke_color;
  float stroke_r, stroke_g, stroke_b;  // 0..1
  int has_fill_color;
  float fill_r, fill_g, fill_b;        // 0..1
} FPDFExt_AppearancePath;

// 整条批注外观流的提取结果。
typedef struct FPDFExt_ShapeAppearance {
  float bbox_left, bbox_bottom, bbox_right, bbox_top;  // /AP /N 的 /BBox（form 本地）
  int path_count;
  const FPDFExt_AppearancePath* paths;
} FPDFExt_ShapeAppearance;

// 解析 /AP /N 并提取所有 PathObject。成功返回结果（须用 FPDFExt_FreeShapeAppearance
// 释放）；无 /AP /N、调解失败或非路径内容时返回 NULL（App 侧回退 key 生成）。
// |document| 为批注所属文档。
FPDF_EXPORT FPDFExt_ShapeAppearance* FPDF_CALLCONV
FPDFExt_ExtractShapeAppearance(FPDF_ANNOTATION annot, FPDF_DOCUMENT document);

// 释放 FPDFExt_ExtractShapeAppearance 返回的结果。
FPDF_EXPORT void FPDF_CALLCONV
FPDFExt_FreeShapeAppearance(FPDFExt_ShapeAppearance* appearance);

#ifdef __cplusplus
}
#endif

#endif  // FPDF_ANNOT_EXT_H_
