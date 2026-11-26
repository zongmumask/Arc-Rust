#ifndef FPDF_FORM_EXT_H_
#define FPDF_FORM_EXT_H_

#include "fpdfview.h"

#ifdef __cplusplus
extern "C" {
#endif

// 在页面上新建一个文本域（TextField）。新建范围覆盖：
//   - catalog 的 /AcroForm（没有则新建，含最小 /DR 字体资源 + /DA + /NeedAppearances true）
//   - /Fields 数组注册字段 dict（/FT /T /Rect）
//   - 页面 /Annots 追加 widget 注解 dict（/Subtype Widget /Rect /Parent -> 字段）
// 最后重建 pageview 使 widget 被 FormFill 环境识别为可交互控件（NewAnnot+ResetAppearance）。
// rect 为 PDF 用户空间（y-up）矩形。成功返回 true。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_AddTextField(FPDF_FORMHANDLE hHandle,
                       FPDF_PAGE page,
                       const FS_RECTF* rect,
                       FPDF_BYTESTRING fieldName);

// 直接设置 (x,y) 处字段的当前值（UTF-16LE），并重生其外观（ResetAppearance）。
// 不经 PWL 聚焦，供表单输入桥整段提交用；成功返回 true。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetTextFieldValue(FPDF_FORMHANDLE hHandle,
                            FPDF_PAGE page,
                            double x,
                            double y,
                            FPDF_WIDESTRING value);

// 计算 (x,y) 处命中的文本域中，插入点在 caretUTF16Len 个 UTF-16 字符后时的 caret 几何。
//
// 与 CPDFSDK_AppStream::SetAsTextField 同源布局（同一 CPDF_BAFontMap + PlateRect(clientRect)
// + /Q 水平对齐 + 垂直对齐/多行/口令/comb/自动字号/滚动），复刻 CPWL_Edit::GetCaretInfo：
// caret 头 = caret 所在 word 的 (ptWord.x+fWidth, ptWord.y+fAscent)、脚 = (…, ptWord.y+fDescent)，
// 行末/空文本回退到 line 的 (ptLine.x, ptLine.y±fLineAscent/Descent)。word.ptWord 已 VTToEdit。
// 输出为 PDF 用户空间（y-up，无旋转字段）。prefix 为 UTF-16LE、null 结尾。未命中字段返回 FALSE。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_MeasureFieldText(FPDF_FORMHANDLE form,
                           FPDF_PAGE page,
                           double x,
                           double y,
                           FPDF_WIDESTRING fullText,
                           int caretUTF16Len,
                           double* outCaretX,
                           double* outCaretBottomY,
                           double* outCaretTopY,
                           FPDF_BOOL* outAccurate);

// ================= Prepare Form（表单准备）扩展 =================
// 约定：以下 API 须在持有 PDFium 独占访问的队列调用；rect/坐标均为 PDF 用户空间（y-up）。
// 属性读写以「page 内 (x,y) 命中的 widget」为定位（与 SetTextFieldValue 同法）。
// key 集合（字符串常量）：
//   字段级 "T"(名称) "Ff"(flags) "V"(值) "DV"(默认值) "DA"(外观串) "Q"(对齐 0/1/2)
//   widget 级 "F"(可见性 flags) "W"(线宽) "S"(线型 "S"/"D") "MK_BC"(边框色) "MK_BG"(填充色)
//            "MK_CA"(按钮样式码 c/l/n/u/s/h)
// Int 用 Set/GetFieldPropertyInt，Float 用 Float，字符串用 Str（UTF-16LE 输入）。

// 在页面上新建 Checkbox（/FT /Btn）。onStateName 为勾选态名（如 "Yes"）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_AddCheckBox(FPDF_FORMHANDLE hHandle,
                      FPDF_PAGE page,
                      const FS_RECTF* rect,
                      FPDF_BYTESTRING fieldName,
                      FPDF_BYTESTRING onStateName);

// 在页面上新建 RadioButton（/FT /Btn + Radio|NoToggleToOff），独立成组（groupName），
// 该按钮勾选态名为 choiceName。加入既有组用 SetFieldPropertyStr("T", 同名组名)。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_AddRadioButton(FPDF_FORMHANDLE hHandle,
                         FPDF_PAGE page,
                         const FS_RECTF* rect,
                         FPDF_BYTESTRING groupName,
                         FPDF_BYTESTRING choiceName);

// 在页面上新建 ListBox（/FT /Ch，无 Combo 位）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_AddListBox(FPDF_FORMHANDLE hHandle,
                     FPDF_PAGE page,
                     const FS_RECTF* rect,
                     FPDF_BYTESTRING fieldName);

// 在页面上新建 ComboBox（/FT /Ch + Combo 位）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_AddComboBox(FPDF_FORMHANDLE hHandle,
                      FPDF_PAGE page,
                      const FS_RECTF* rect,
                      FPDF_BYTESTRING fieldName);

// 全文档（/AcroForm/Fields）是否已有同名 /T 字段。用于创建前的唯一名生成。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_FieldNameExists(FPDF_DOCUMENT doc, FPDF_BYTESTRING name);

// 确保 /AcroForm/DR/Font 注册 tag -> Type1/BaseFont=baseFont（标准 14 字体直接可用）。
// App 侧改 /DA 字体前调用。成功返回 true。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_EnsureFontResource(FPDF_DOCUMENT doc,
                             FPDF_BYTESTRING baseFont,
                             FPDF_BYTESTRING tag);

// 写整型属性（Ff/Q/F）。成功返回 true。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetFieldPropertyInt(FPDF_FORMHANDLE hHandle,
                              FPDF_PAGE page,
                              double x,
                              double y,
                              FPDF_BYTESTRING key,
                              int value);

// 写浮点属性（W 线宽）。<=0 表示删除该键。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetFieldPropertyFloat(FPDF_FORMHANDLE hHandle,
                                FPDF_PAGE page,
                                double x,
                                double y,
                                FPDF_BYTESTRING key,
                                double value);

// 写字符串属性（T/V/DV/DA/S/MK_CA）。value 为 UTF-16LE。
// "T" 对 radio 字段特判：目标名已存在且为 radio 组时把命中 widget 并入该组（on-state 冲突则失败）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetFieldPropertyStr(FPDF_FORMHANDLE hHandle,
                              FPDF_PAGE page,
                              double x,
                              double y,
                              FPDF_BYTESTRING key,
                              FPDF_WIDESTRING value);

// 写 MK 颜色（key: "MK_BC" 边框 / "MK_BG" 填充）。r/g/b 为 0..1；a<0 表示删除该键。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetFieldColor(FPDF_FORMHANDLE hHandle,
                        FPDF_PAGE page,
                        double x,
                        double y,
                        FPDF_BYTESTRING key,
                        float r,
                        float g,
                        float b);

// 写虚线相位数组（/D）。count=0 删除。仅线型为 D 时有意义。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetFieldDash(FPDF_FORMHANDLE hHandle,
                       FPDF_PAGE page,
                       double x,
                       double y,
                       const float* pattern,
                       size_t count);

// 选项写入（List/Combo）：index=-1 追加；export 为空则导出值同 label。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetChoiceOption(FPDF_FORMHANDLE hHandle,
                          FPDF_PAGE page,
                          double x,
                          double y,
                          int index,
                          FPDF_WIDESTRING label,
                          FPDF_WIDESTRING exportValue);

// 删除选项（List/Combo）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_RemoveChoiceOption(FPDF_FORMHANDLE hHandle,
                             FPDF_PAGE page,
                             double x,
                             double y,
                             int index);

// 勾选/取消勾选（Checkbox 勾选；Radio 勾选时同组互斥由内核处理）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetCheck(FPDF_FORMHANDLE hHandle, FPDF_PAGE page, double x, double y, FPDF_BOOL checked);

// 读整型属性。成功返回 true。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_GetFieldPropertyInt(FPDF_FORMHANDLE hHandle,
                              FPDF_PAGE page,
                              double x,
                              double y,
                              FPDF_BYTESTRING key,
                              int* outValue);

// 读浮点属性。成功返回 true。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_GetFieldPropertyFloat(FPDF_FORMHANDLE hHandle,
                                FPDF_PAGE page,
                                double x,
                                double y,
                                FPDF_BYTESTRING key,
                                double* outValue);

// 读字符串属性（两段式：buffer 为 null 则回传所需 UTF-16 字符数，不含结尾 null）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_GetFieldPropertyStr(FPDF_FORMHANDLE hHandle,
                              FPDF_PAGE page,
                              double x,
                              double y,
                              FPDF_BYTESTRING key,
                              unsigned short* buffer,
                              size_t bufferLen,
                              size_t* outNeededUTF16);

// 读 MK 颜色（key: "MK_BC"/"MK_BG"）。未设置时返回 false。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_GetFieldColor(FPDF_FORMHANDLE hHandle,
                        FPDF_PAGE page,
                        double x,
                        double y,
                        FPDF_BYTESTRING key,
                        float* outR,
                        float* outG,
                        float* outB);

// 读选项数与选项内容（两段式同 GetFieldPropertyStr；exportNeeded 可为 null）。
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_GetChoiceOptionCount(FPDF_FORMHANDLE hHandle,
                               FPDF_PAGE page,
                               double x,
                               double y,
                               int* outCount);

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_Form_GetChoiceOption(FPDF_FORMHANDLE hHandle,
                          FPDF_PAGE page,
                          double x,
                          double y,
                          int index,
                          unsigned short* labelBuffer,
                          size_t labelBufferLen,
                          size_t* outLabelNeeded,
                          unsigned short* exportBuffer,
                          size_t exportBufferLen,
                          size_t* outExportNeeded);

#ifdef __cplusplus
}
#endif

#endif  // FPDF_FORM_EXT_H_