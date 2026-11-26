// Copyright 2026 The Quill Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ArcPDFium → ArcOfficeConvert 边界 ABI（内核侧，供 Office 转换库消费）：
//
//   FPDFExt_Page_GetCropBox          —— Stirling PageReader.cropBox（CPDF_Page
//                                      内部 CropBox/MediaBox 逻辑，公开 API 无）。
//   FPDFExt_Image_GetMediaBytes      —— 页面内嵌图像 → Word 可嵌入媒资字节：
//                                      DCTDecode 源原样 JPEG 透传；其它 Flate-解码
//                                      BGRA 重编 PNG（FlateModule 内链 zlib）。返回
//                                      malloc 缓冲，调用方 free() 释放。
//
// 纯 C extern "C"，随 Extensions 编译进 ArcPDFium 静态库。

#ifndef FPDF_OFFICE_EXT_H_
#define FPDF_OFFICE_EXT_H_

#include <stddef.h>

#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// 页 CropBox（MediaBox 兜底），PDF 用户空间。返回 FALSE 时用
// FPDF_GetPageWidth/Height 兜底。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_Page_GetCropBox(
    FPDF_PAGE page, float* llx, float* lly, float* urx, float* ury);

// |image| 的内嵌媒资：DCTDecode 原样 JPEG / 其它 PNG(RGBA) 重编（含按
// quarter_turns/flip 的方向转）。成功时 *out_bytes = malloc 缓冲（调用方 free），
// *out_len 字节数，*out_is_jpeg=1 表示 JPEG，*out_w/*out_h 像素尺寸。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFExt_Image_GetMediaBytes(
    FPDF_PAGEOBJECT image, int quarter_turns, FPDF_BOOL flip_h,
    FPDF_BOOL flip_v, unsigned char** out_bytes, size_t* out_len,
    FPDF_BOOL* out_is_jpeg, unsigned int* out_w, unsigned int* out_h);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FPDF_OFFICE_EXT_H_