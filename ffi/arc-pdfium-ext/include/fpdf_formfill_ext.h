#ifndef FPDF_FORMFILL_EXT_H_
#define FPDF_FORMFILL_EXT_H_

#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

void FPDF_CALLCONV FPDF_FFLDrawWithMatrix(FPDF_FORMHANDLE hHandle,
                                         FPDF_BITMAP bitmap,
                                         FPDF_PAGE page,
                                         const FS_MATRIX* matrix,
                                         const FS_RECTF* clipping,
                                         int flags);

// 单字段渲染：只画指定 widget。入参对齐 FPDFAnnot_RenderToBitmap（bitmap/page/annot/matrix）
// + 表单句柄。放大时重算 matrix 重渲染即可。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Widget_RenderToBitmap(FPDF_FORMHANDLE hHandle,
                           FPDF_BITMAP bitmap,
                           FPDF_PAGE page,
                           FPDF_ANNOTATION widgetAnnot,
                           const FS_MATRIX* matrix,
                           const FS_RECTF* clipping,
                           int flags);

#ifdef __cplusplus
}
#endif

#endif
