// 表单字段新建扩展：公共 fpdf_* API 无建 /AcroForm、字段 dict、/Parent 链路的入口，
// 故在 pdfium_ext 内直接操作 CPDF_Dictionary 拼结构（arc-pdfium-engineering Priority 3）。
#include <algorithm>
#include <vector>

#include "fpdf_form_ext.h"

#include "constants/form_flags.h"
#include "core/fpdfapi/font/cpdf_font.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_bafontmap.h"
#include "core/fpdfdoc/cpdf_formcontrol.h"
#include "core/fpdfdoc/cpdf_formfield.h"
#include "core/fpdfdoc/cpvt_line.h"
#include "core/fpdfdoc/cpvt_word.h"
#include "core/fxcrt/fx_string.h"
#include "core/fxcrt/widestring.h"
#include "fpdfsdk/cpdfsdk_annot.h"
#include "fpdfsdk/cpdfsdk_formfillenvironment.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/cpdfsdk_pageview.h"
#include "fpdfsdk/cpdfsdk_widget.h"
#include "fpdfsdk/pwl/cpwl_edit_impl.h"

namespace {

// 保底创建 catalog 的 /AcroForm：
//   1) 不存在的文档才新建（含最小 /DR>>Font Standard-Helvetica 资源 + /DA + /NeedAppearances），
//      已存在的复用原 DA/DR，避免覆盖既有表单默认样式。
//   2) /NeedAppearances true 是让 PDFium 自动构造 widget 外观（ResetAppearance）的关键，
//      新字段无需手写 /AP（cpdfsdk_pageview.cpp NewAnnot 判 NeedConstructAP）。
CPDF_Dictionary* GetOrCreateAcroForm(CPDF_Document* doc) {
  if (!doc) {
    return nullptr;
  }
  CPDF_Dictionary* root = doc->GetMutableRoot();
  if (!root) {
    return nullptr;
  }
  if (auto existing = root->GetMutableDictFor("AcroForm")) {
    // 已有 /AcroForm 的文档同样要保证 NeedAppearances=true：新建字段的 AP 生成
    // 依赖 ReloadPageView 后 NewAnnot 的 NeedConstructAP 分支（见下方注释），
    // 否则新建 widget 只挂空 AP 流，页面位图渲染为空白。
    existing->SetNewFor<CPDF_Boolean>("NeedAppearances", true);
    return existing.Get();
  }

  auto acro = doc->NewIndirect<CPDF_Dictionary>();
  root->SetNewFor<CPDF_Reference>("AcroForm", doc, acro->GetObjNum());

  // 最小字体资源：标准 14 字体 Helvetica（PDFium 内置，无需嵌入字库程序）。
  auto dr = acro->GetOrCreateDictFor("DR");
  auto font_dict = dr->GetOrCreateDictFor("Font");
  auto helv = doc->NewIndirect<CPDF_Dictionary>();
  helv->SetNewFor<CPDF_Name>("Type", "Font");
  helv->SetNewFor<CPDF_Name>("Subtype", "Type1");
  helv->SetNewFor<CPDF_Name>("BaseFont", "Helvetica");
  font_dict->SetNewFor<CPDF_Reference>("Helv", doc, helv->GetObjNum());

  // 默认外观字符串：/Helv 12 Tf 0 g（重置外观时字段继承 AcroForm 的 DA）。
  acro->SetNewFor<CPDF_String>("DA", "/Helv 12 Tf 0 g");
  acro->SetNewFor<CPDF_Boolean>("NeedAppearances", true);
  return acro.Get();
}

}  // namespace

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_AddTextField(FPDF_FORMHANDLE hHandle,
                       FPDF_PAGE page,
                       const FS_RECTF* rect,
                       FPDF_BYTESTRING fieldName) {
  if (!hHandle || !page || !rect) {
    return false;
  }
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }
  CPDF_Document* doc = pPage->GetDocument();
  CPDF_Dictionary* acro = GetOrCreateAcroForm(doc);
  if (!acro) {
    return false;
  }

  // ---- 1) 字段 dict（数据层）：/FT /T /Rect ----
  auto field = doc->NewIndirect<CPDF_Dictionary>();
  field->SetNewFor<CPDF_Name>("FT", "Tx");

  // 唯一 /T：用传入名，或用基于现有字段数的自增名（同 /T 会共享字段，故必须唯一）。
  ByteString tag;
  if (fieldName && *fieldName) {
    tag = ByteString(fieldName);
  } else if (auto existing = acro->GetArrayFor("Fields")) {
    tag = ByteString::Format("Field%d", static_cast<int>(existing->size()) + 1);
  } else {
    tag = "Field1";
  }
  field->SetNewFor<CPDF_String>("T", tag);

  field->SetRectFor("Rect",
                    CFX_FloatRect(rect->left, rect->bottom, rect->right, rect->top));
  field->SetNewFor<CPDF_Number>("Ff", 0);  // 可编辑（未置只读位）
  field->SetNewFor<CPDF_String>("DA", "/Helv 12 Tf 0 g");

  // ---- 2) 注册到 /AcroForm/Fields ----
  // Append 要求对象 IsInline（obj_num_==0）；indirect 对象须以 CPDF_Reference 追加
  // （`CPDF_Array::AppendInternal` 有 `CHECK(pObj->IsInline())`）。
  auto fields = acro->GetOrCreateArrayFor("Fields");
  fields->Append(pdfium::MakeRetain<CPDF_Reference>(doc, field->GetObjNum()));

  // ---- 3) widget 注解 dict（页面交互 + 外观层）：/Parent -> 字段 ----
  auto widget = doc->NewIndirect<CPDF_Dictionary>();
  widget->SetNewFor<CPDF_Name>("Type", "Annot");
  widget->SetNewFor<CPDF_Name>("Subtype", "Widget");
  widget->SetRectFor("Rect",
                     CFX_FloatRect(rect->left, rect->bottom, rect->right, rect->top));
  widget->SetNewFor<CPDF_Reference>("Parent", doc, field->GetObjNum());
  widget->SetNewFor<CPDF_Number>("F", 4);  // 打印位
  auto annots = pPage->GetOrCreateAnnotsArray();
  annots->Append(pdfium::MakeRetain<CPDF_Reference>(doc, widget->GetObjNum()));

  // ---- 4) 重建 pageview：让 FormFill 环境识别新 widget ----
  // GetOrCreatePageView 内部调 LoadFXAnnots -> NewAnnot；对 widget 判
  // NeedConstructAP 后调 ResetAppearance 自动造外观（无字段 /AP 也能画）。
  if (CPDFSDK_FormFillEnvironment* env =
          CPDFSDKFormFillEnvironmentFromFPDFFormHandle(hHandle)) {
    env->RemovePageView(pPage);
    env->GetOrCreatePageView(pPage);
  }
  return true;
}

FPDF_BOOL FPDF_CALLCONV FPDF_Form_SetTextFieldValue(FPDF_FORMHANDLE hHandle,
                                                    FPDF_PAGE page,
                                                    double x,
                                                    double y,
                                                    FPDF_WIDESTRING value) {
  if (!hHandle || !page || !value) {
    return false;
  }
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_FormFillEnvironment* env =
      CPDFSDKFormFillEnvironmentFromFPDFFormHandle(hHandle);
  if (!pPage || !env) {
    return false;
  }
  CPDFSDK_PageView* pv = env->GetPageView(pPage);
  if (!pv) {
    return false;
  }

  // UTF-16LE（FPDF_WIDESTRING，null 结尾）→ WideString。
  const unsigned short* p = value;
  size_t len = 0;
  while (p[len]) {
    len++;
  }
  pdfium::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(p),
                                    len * sizeof(unsigned short));
  WideString ws = WideString::FromUTF16LE(bytes);

  CFX_PointF pt(static_cast<float>(x), static_cast<float>(y));
  for (CPDFSDK_Annot* a : pv->GetAnnotList()) {
    if (a->GetAnnotSubtype() != CPDF_Annot::Subtype::WIDGET) {
      continue;
    }
    if (!a->GetRect().Contains(pt)) {
      continue;
    }
    // 直接写字段 /V 并重生 AP（不经 PWL 聚焦，链路最稳健）。
    auto* widget = static_cast<CPDFSDK_Widget*>(a);
    widget->SetValue(ws);
    widget->ResetAppearance(ws, CPDFSDK_Widget::kValueChanged);
    env->Invalidate(pPage, widget->GetRect().ToFxRect());
    return true;
  }
  return false;
}

FPDF_BOOL FPDF_CALLCONV FPDF_Form_MeasureFieldText(FPDF_FORMHANDLE form,
                                                   FPDF_PAGE page,
                                                   double x,
                                                   double y,
                                                   FPDF_WIDESTRING fullText,
                                                   int caretUTF16Len,
                                                   double* outCaretX,
                                                   double* outCaretBottomY,
                                                   double* outCaretTopY,
                                                   FPDF_BOOL* outAccurate) {
  *outCaretX = 0.0;
  *outCaretBottomY = 0.0;
  *outCaretTopY = 0.0;
  *outAccurate = 0;
  if (!form || !page || !fullText) {
    return false;
  }

  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_FormFillEnvironment* env =
      CPDFSDKFormFillEnvironmentFromFPDFFormHandle(form);
  if (!pPage || !env) {
    return false;
  }
  CPDFSDK_PageView* pv = env->GetPageView(pPage);
  if (!pv) {
    return false;
  }

  // 用 (x,y) 处命中字段定位 widget（不依赖易失效的聚焦态 focus_annot_）。
  CFX_PointF pt(static_cast<float>(x), static_cast<float>(y));
  CPDFSDK_Widget* widget = nullptr;
  for (CPDFSDK_Annot* a : pv->GetAnnotList()) {
    if (a->GetAnnotSubtype() != CPDF_Annot::Subtype::WIDGET) {
      continue;
    }
    if (!a->GetRect().Contains(pt)) {
      continue;
    }
    widget = static_cast<CPDFSDK_Widget*>(a);
    break;
  }
  if (!widget) {
    return false;
  }
  if (!pv->GetPDFDocument()) {
    return false;
  }

  // ---- 与 CPDFSDK_AppStream::SetAsTextField 同源排版（/Q 对齐 / 垂直 / 多行 /
  // 口令 / comb / 自动字号 / 滚动）----
  CPDF_BAFontMap font_map(pv->GetPDFDocument(),
                          widget->GetPDFAnnot()->GetMutableAnnotDict(),
                          ByteString("N"));
  auto edit = std::make_unique<CPWL_EditImpl>();
  edit->EnableRefresh(false);
  edit->SetFontMap(&font_map);
  CFX_FloatRect rcClient = widget->GetClientRect();
  edit->SetPlateRect(rcClient);
  edit->SetAlignmentH(widget->GetFormControl()->GetControlAlignment());

  const uint32_t flags = widget->GetFieldFlags();
  const bool bMultiLine = flags & pdfium::form_flags::kTextMultiline;
  if (bMultiLine) {
    edit->SetMultiLine(true);
    edit->SetAutoReturn(true);
  } else {
    edit->SetAlignmentV(1);  // 单行垂直居中（与 SetAsTextField 一致）
  }
  if (flags & pdfium::form_flags::kTextPassword) {
    edit->SetPasswordChar('*');
  }
  if (CPDF_FormField* field = widget->GetFormField()) {
    const int nMaxLen = field->GetMaxLen();
    if (nMaxLen > 0) {
      if (flags & pdfium::form_flags::kTextComb) {
        edit->SetCharArray(nMaxLen);
      } else {
        edit->SetLimitChar(nMaxLen);
      }
    }
  }
  float fs = widget->GetFontSize();
  if (fs > 0) {
    edit->SetFontSize(fs);
  } else {
    edit->SetAutoFontSize(true);
  }
  edit->Initialize();

  // UTF-16LE 全文 → WideString。
  size_t n = 0;
  const unsigned short* q = reinterpret_cast<const unsigned short*>(fullText);
  while (q[n]) {
    ++n;
  }
  pdfium::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(q),
                                    n * sizeof(unsigned short));

  // ---- caret：全文内容块左缘（已含 /Q 对齐偏移 + 滚动）+ 前缀真实宽度 ----
  // 单行下前缀在块内连续推进；多行 / 滚动 / 旋转 leave待验证（见文档）。
  edit->SetText(WideString::FromUTF16LE(bytes));
  edit->Paint();
  CFX_FloatRect fullC = edit->GetContentRect();

  double prefixWidth = 0.0;
  const int pn = std::min<int>(caretUTF16Len, static_cast<int>(n));
  if (pn > 0) {
    // 复用同一 edit 只排前缀取宽度（同字体/字号/对齐）。
    pdfium::span<const uint8_t> prefixBytes(
        reinterpret_cast<const uint8_t*>(q), pn * sizeof(unsigned short));
    edit->SetText(WideString::FromUTF16LE(prefixBytes));
    edit->Paint();
    prefixWidth = edit->GetContentRect().Width();
  }

  const double caretX = fullC.left + prefixWidth;
  const double botY = fullC.bottom;
  const double topY = fullC.top;
  CFX_FloatRect wrect = widget->GetRect();
  *outCaretX = static_cast<double>(wrect.left + caretX);
  *outCaretBottomY = static_cast<double>(wrect.bottom + botY);
  *outCaretTopY = static_cast<double>(wrect.bottom + topY);
  *outAccurate = 1;
  return true;
}

// ================= Prepare Form（表单准备）实现 =================

namespace {

WideString WideFromFPDF(FPDF_WIDESTRING s) {
  if (!s) {
    return WideString();
  }
  const unsigned short* p = s;
  size_t len = 0;
  while (p[len]) {
    len++;
  }
  return WideString::FromUTF16LE(pdfium::span(
      reinterpret_cast<const uint8_t*>(p), len * sizeof(unsigned short)));
}

WideString WideFromBytes(const ByteString& bs) {
  return WideString::FromLatin1(bs.AsStringView());
}

// /DA、/MK/CA、/S 等 ASCII 属性用。
ByteString BytesFromWide(const WideString& ws) {
  ByteString bs;
  for (wchar_t c : ws) {
    bs += static_cast<char>(c);
  }
  return bs;
}

// 两段式 UTF-16 输出：buffer 为 null 时仅回传所需字符数（不含结尾 null）。
// 注意：macOS wchar_t 为 4 字节（UTF-32），须转码 UTF-16LE（含代理对）。
bool WriteUTF16Out(const WideString& ws,
                   unsigned short* buffer,
                   size_t bufferLen,
                   size_t* outNeeded) {
  std::vector<unsigned short> out;
  out.reserve(ws.GetLength() * 2 + 1);
  for (wchar_t c : ws) {
    unsigned int u = static_cast<unsigned int>(c);
    if (u >= 0x10000) {
      u -= 0x10000;
      out.push_back(static_cast<unsigned short>(0xD800 + (u >> 10)));
      out.push_back(static_cast<unsigned short>(0xDC00 + (u & 0x3FF)));
    } else {
      out.push_back(static_cast<unsigned short>(u));
    }
  }
  const size_t need = out.size();
  if (outNeeded) {
    *outNeeded = need;
  }
  if (!buffer) {
    return true;
  }
  if (bufferLen < need + 1) {
    return false;
  }
  if (need) {
    memcpy(buffer, out.data(), need * sizeof(unsigned short));
  }
  buffer[need] = 0;
  return true;
}

CPDFSDK_Widget* HitWidgetAt(FPDF_FORMHANDLE hHandle,
                            CPDF_Page* pPage,
                            const CFX_PointF& pt) {
  if (!hHandle || !pPage) {
    return nullptr;
  }
  auto* env = CPDFSDKFormFillEnvironmentFromFPDFFormHandle(hHandle);
  if (!env) {
    return nullptr;
  }
  CPDFSDK_PageView* pv = env->GetPageView(pPage);
  if (!pv) {
    return nullptr;
  }
  for (CPDFSDK_Annot* a : pv->GetAnnotList()) {
    if (a->GetAnnotSubtype() != CPDF_Annot::Subtype::WIDGET) {
      continue;
    }
    if (!a->GetRect().Contains(pt)) {
      continue;
    }
    return static_cast<CPDFSDK_Widget*>(a);
  }
  return nullptr;
}

CPDF_Dictionary* FieldDictOfWidget(CPDFSDK_Widget* widget) {
  // Add* 建的结构里 widget /Parent 必指字段 dict。
  return widget->GetPDFAnnot()->GetMutableAnnotDict()->GetMutableDictFor("Parent");
}

// widget 的 on-state 名 = /AP/N 下非 "Off" 键（同 CPDF_FormControl::GetOnStateName）。
ByteString WidgetOnState(CPDF_Dictionary* widgetDict) {
  auto ap = widgetDict->GetDictFor("AP");
  if (!ap) {
    return ByteString();
  }
  auto n = ap->GetDictFor("N");
  if (!n) {
    return ByteString();
  }
  CPDF_DictionaryLocker locker(n);
  for (const auto& it : locker) {
    if (it.first != "Off") {
      return ByteString(it.first);
    }
  }
  return ByteString();
}

// 预置 /AP/N {Off, onState}（空流占位）：GetOnStateName/ResetAppearance 依赖键集。
void SeedAPOnStates(CPDF_Document* doc,
                    CPDF_Dictionary* widget,
                    const ByteString& onState) {
  auto ap = widget->GetOrCreateDictFor("AP");
  auto n = ap->GetOrCreateDictFor("N");
  auto off = doc->NewIndirect<CPDF_Stream>(pdfium::span<const uint8_t>());
  n->SetNewFor<CPDF_Reference>("Off", doc, off->GetObjNum());
  auto on = doc->NewIndirect<CPDF_Stream>(pdfium::span<const uint8_t>());
  ByteString onKey = onState.IsEmpty() ? ByteString("Yes") : onState;
  n->SetNewFor<CPDF_Reference>(onKey, doc, on->GetObjNum());
}

void ReloadPageView(FPDF_FORMHANDLE hHandle, CPDF_Page* pPage) {
  if (CPDFSDK_FormFillEnvironment* env =
          CPDFSDKFormFillEnvironmentFromFPDFFormHandle(hHandle)) {
    env->RemovePageView(pPage);
    env->GetOrCreatePageView(pPage);
  }
}

void InvalidateWidget(CPDFSDK_FormFillEnvironment* env, CPDFSDK_Widget* widget) {
  if (env) {
    env->Invalidate(widget->GetPDFPage(), widget->GetRect().ToFxRect());
  }
}

void ResetWidgetAppearance(CPDFSDK_Widget* widget,
                           CPDFSDK_Widget::ValueChanged changed) {
  widget->ResetAppearance(std::nullopt, changed);
  if (CPDFSDK_PageView* pv = widget->GetPageView()) {
    pv->GetFormFillEnv()->Invalidate(widget->GetPDFPage(),
                                     widget->GetRect().ToFxRect());
  }
}

// 建「字段 dict（注册进 /Fields）+ 页面 widget（/Parent 反指）」公共骨架。
// withKids 时字段带 /Kids（checkbox/radio 统一带，便于组管理）。
CPDF_Dictionary* CreateFieldAndWidget(CPDF_Document* doc,
                                      CPDF_Page* pPage,
                                      CPDF_Dictionary* acro,
                                      const FS_RECTF* rect,
                                      const char* ft,
                                      uint32_t ff,
                                      const ByteString& name,
                                      bool withKids,
                                      CPDF_Dictionary** outWidget) {
  CFX_FloatRect pdfRect(rect->left, rect->bottom, rect->right, rect->top);

  auto field = doc->NewIndirect<CPDF_Dictionary>();
  field->SetNewFor<CPDF_Name>("FT", ft);
  ByteString tag = name;
  if (tag.IsEmpty()) {
    auto existing = acro->GetArrayFor("Fields");
    tag = ByteString::Format("Field%d",
                             static_cast<int>(existing ? existing->size() : 0) + 1);
  }
  field->SetNewFor<CPDF_String>("T", tag);
  field->SetRectFor("Rect", pdfRect);
  field->SetNewFor<CPDF_Number>("Ff", static_cast<int>(ff));
  acro->GetOrCreateArrayFor("Fields")->Append(
      pdfium::MakeRetain<CPDF_Reference>(doc, field->GetObjNum()));

  auto widget = doc->NewIndirect<CPDF_Dictionary>();
  widget->SetNewFor<CPDF_Name>("Type", "Annot");
  widget->SetNewFor<CPDF_Name>("Subtype", "Widget");
  widget->SetRectFor("Rect", pdfRect);
  widget->SetNewFor<CPDF_Reference>("Parent", doc, field->GetObjNum());
  widget->SetNewFor<CPDF_Number>("F", 4);  // 打印位
  pPage->GetOrCreateAnnotsArray()->Append(
      pdfium::MakeRetain<CPDF_Reference>(doc, widget->GetObjNum()));
  if (withKids) {
    field->GetOrCreateArrayFor("Kids")->Append(
        pdfium::MakeRetain<CPDF_Reference>(doc, widget->GetObjNum()));
  }
  *outWidget = widget.Get();
  return field.Get();
}

CPDF_Dictionary* GetMutableAcroForm(CPDF_Document* doc) {
  return GetOrCreateAcroForm(doc);
}

// /AcroForm/Fields 顶层按 /T（Unicode）找字段 dict。
CPDF_Dictionary* FindFieldByName(CPDF_Document* doc,
                                 const WideString& name,
                                 CPDF_Dictionary* skip) {
  CPDF_Dictionary* root = doc->GetMutableRoot();
  if (!root) {
    return nullptr;
  }
  auto acro = root->GetMutableDictFor("AcroForm");
  if (!acro) {
    return nullptr;
  }
  auto fields = acro->GetMutableArrayFor("Fields");
  if (!fields) {
    return nullptr;
  }
  for (size_t i = 0; i < fields->size(); ++i) {
    CPDF_Dictionary* f = fields->GetMutableDictAt(i);
    if (!f || f == skip) {
      continue;
    }
    if (f->GetUnicodeTextFor("T") == name) {
      return f;
    }
  }
  return nullptr;
}

// radio 字段改名为「已存在 radio 组」：把命中 widget 并入该组。
bool RenameRadioField(CPDF_Document* doc,
                      CPDF_Dictionary* acro,
                      CPDFSDK_Widget* hit,
                      const WideString& newName) {
  CPDF_Dictionary* widgetDict = hit->GetPDFAnnot()->GetMutableAnnotDict();
  CPDF_Dictionary* fieldDict = FieldDictOfWidget(hit);
  if (!fieldDict) {
    return false;
  }
  CPDF_Dictionary* target = FindFieldByName(doc, newName, fieldDict);
  if (!target) {
    fieldDict->SetNewFor<CPDF_String>("T", newName.AsStringView());
    return true;
  }
  if (target->GetNameFor("FT") != "Btn" ||
      !(static_cast<uint32_t>(target->GetIntegerFor("Ff")) &
        pdfium::form_flags::kButtonRadio)) {
    return false;
  }
  // on-state 同组查重
  ByteString onNew = WidgetOnState(widgetDict);
  if (auto kids = target->GetMutableArrayFor("Kids")) {
    for (size_t i = 0; i < kids->size(); ++i) {
      CPDF_Dictionary* kid = kids->GetMutableDictAt(i);
      if (kid && !onNew.IsEmpty() && WidgetOnState(kid) == onNew) {
        return false;
      }
    }
  }
  // widget 挂到 target
  widgetDict->SetNewFor<CPDF_Reference>("Parent", doc, target->GetObjNum());
  target->GetOrCreateArrayFor("Kids")->Append(
      pdfium::MakeRetain<CPDF_Reference>(doc, widgetDict->GetObjNum()));
  // 源字段：移除该 widget；无剩余 widget 则从 /Fields 摘除
  size_t widgetNum = widgetDict->GetObjNum();
  bool removed = false;
  if (auto srcKids = fieldDict->GetMutableArrayFor("Kids")) {
    for (size_t i = 0; i < srcKids->size(); ++i) {
      auto obj = srcKids->GetMutableObjectAt(i);
      auto* ref = obj ? obj->AsReference() : nullptr;
      if (ref && ref->GetRefObjNum() == widgetNum) {
        srcKids->RemoveAt(i);
        removed = true;
        break;
      }
    }
    if (srcKids->size() == 0) {
      fieldDict->RemoveFor("Kids");
    }
  }
  if (!removed) {
    if (auto fields = acro->GetMutableArrayFor("Fields")) {
      for (size_t i = 0; i < fields->size(); ++i) {
        auto obj = fields->GetMutableObjectAt(i);
        auto* ref = obj ? obj->AsReference() : nullptr;
        if (ref && ref->GetRefObjNum() == fieldDict->GetObjNum()) {
          fields->RemoveAt(i);
          break;
        }
      }
    }
  }
  return true;
}

CPDF_FormField* FormFieldOf(CPDFSDK_Widget* widget) {
  return widget->GetFormControl() ? widget->GetFormControl()->GetField() : nullptr;
}

}  // namespace

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_AddCheckBox(FPDF_FORMHANDLE hHandle,
                      FPDF_PAGE page,
                      const FS_RECTF* rect,
                      FPDF_BYTESTRING fieldName,
                      FPDF_BYTESTRING onStateName) {
  if (!hHandle || !page || !rect) {
    return false;
  }
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }
  CPDF_Document* doc = pPage->GetDocument();
  CPDF_Dictionary* acro = GetOrCreateAcroForm(doc);
  if (!acro) {
    return false;
  }
  CPDF_Dictionary* widget = nullptr;
  CPDF_Dictionary* field = CreateFieldAndWidget(
      doc, pPage, acro, rect, "Btn", 0,
      ByteString(fieldName ? fieldName : ""), true, &widget);
  if (!field || !widget) {
    return false;
  }
  ByteString on = (onStateName && *onStateName) ? ByteString(onStateName)
                                                : ByteString("Yes");
  field->SetNewFor<CPDF_Name>("V", "Off");
  widget->SetNewFor<CPDF_Name>("AS", "Off");
  SeedAPOnStates(doc, widget, on);
  ReloadPageView(hHandle, pPage);
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_AddRadioButton(FPDF_FORMHANDLE hHandle,
                         FPDF_PAGE page,
                         const FS_RECTF* rect,
                         FPDF_BYTESTRING groupName,
                         FPDF_BYTESTRING choiceName) {
  if (!hHandle || !page || !rect || !choiceName || !*choiceName) {
    return false;
  }
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }
  CPDF_Document* doc = pPage->GetDocument();
  CPDF_Dictionary* acro = GetOrCreateAcroForm(doc);
  if (!acro) {
    return false;
  }
  CPDF_Dictionary* widget = nullptr;
  CPDF_Dictionary* field = CreateFieldAndWidget(
      doc, pPage, acro, rect, "Btn",
      pdfium::form_flags::kButtonNoToggleToOff | pdfium::form_flags::kButtonRadio,
      ByteString(groupName ? groupName : ""), true, &widget);
  if (!field || !widget) {
    return false;
  }
  field->SetNewFor<CPDF_Name>("V", "Off");
  widget->SetNewFor<CPDF_Name>("AS", "Off");
  SeedAPOnStates(doc, widget, ByteString(choiceName));
  ReloadPageView(hHandle, pPage);
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_AddListBox(FPDF_FORMHANDLE hHandle,
                     FPDF_PAGE page,
                     const FS_RECTF* rect,
                     FPDF_BYTESTRING fieldName) {
  if (!hHandle || !page || !rect) {
    return false;
  }
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }
  CPDF_Document* doc = pPage->GetDocument();
  CPDF_Dictionary* acro = GetOrCreateAcroForm(doc);
  if (!acro) {
    return false;
  }
  CPDF_Dictionary* widget = nullptr;
  CPDF_Dictionary* field = CreateFieldAndWidget(
      doc, pPage, acro, rect, "Ch", 0, ByteString(fieldName ? fieldName : ""),
      false, &widget);
  if (!field || !widget) {
    return false;
  }
  field->SetNewFor<CPDF_String>("DA", "/Helv 12 Tf 0 g");
  ReloadPageView(hHandle, pPage);
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_AddComboBox(FPDF_FORMHANDLE hHandle,
                      FPDF_PAGE page,
                      const FS_RECTF* rect,
                      FPDF_BYTESTRING fieldName) {
  if (!hHandle || !page || !rect) {
    return false;
  }
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }
  CPDF_Document* doc = pPage->GetDocument();
  CPDF_Dictionary* acro = GetOrCreateAcroForm(doc);
  if (!acro) {
    return false;
  }
  CPDF_Dictionary* widget = nullptr;
  CPDF_Dictionary* field = CreateFieldAndWidget(
      doc, pPage, acro, rect, "Ch", pdfium::form_flags::kChoiceCombo,
      ByteString(fieldName ? fieldName : ""), false, &widget);
  if (!field || !widget) {
    return false;
  }
  field->SetNewFor<CPDF_String>("DA", "/Helv 12 Tf 0 g");
  ReloadPageView(hHandle, pPage);
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_FieldNameExists(FPDF_DOCUMENT doc, FPDF_BYTESTRING name) {
  if (!doc || !name || !*name) {
    return false;
  }
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(doc);
  if (!pDoc) {
    return false;
  }
  return FindFieldByName(pDoc, WideFromBytes(ByteString(name)), nullptr) != nullptr;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_EnsureFontResource(FPDF_DOCUMENT doc,
                             FPDF_BYTESTRING baseFont,
                             FPDF_BYTESTRING tag) {
  if (!doc || !baseFont || !*baseFont || !tag || !*tag) {
    return false;
  }
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(doc);
  CPDF_Dictionary* acro = pDoc ? GetOrCreateAcroForm(pDoc) : nullptr;
  if (!acro) {
    return false;
  }
  auto fontDict = acro->GetOrCreateDictFor("DR")->GetOrCreateDictFor("Font");
  if (fontDict->KeyExist(tag)) {
    return true;
  }
  auto f = pDoc->NewIndirect<CPDF_Dictionary>();
  f->SetNewFor<CPDF_Name>("Type", "Font");
  f->SetNewFor<CPDF_Name>("Subtype", "Type1");
  f->SetNewFor<CPDF_Name>("BaseFont", baseFont);
  fontDict->SetNewFor<CPDF_Reference>(tag, pDoc, f->GetObjNum());
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetFieldPropertyInt(FPDF_FORMHANDLE hHandle,
                              FPDF_PAGE page,
                              double x,
                              double y,
                              FPDF_BYTESTRING key,
                              int value) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget) {
    return false;
  }
  CPDF_Dictionary* annotDict = widget->GetPDFAnnot()->GetMutableAnnotDict();
  const ByteString k(key ? key : "");
  if (k == "Ff" || k == "Q") {
    CPDF_Dictionary* fieldDict = FieldDictOfWidget(widget);
    if (!fieldDict) {
      return false;
    }
    fieldDict->SetNewFor<CPDF_Number>(k == "Ff" ? "Ff" : "Q", value);
    ResetWidgetAppearance(widget, CPDFSDK_Widget::kValueUnchanged);
    return true;
  }
  if (k == "F") {
    annotDict->SetNewFor<CPDF_Number>("F", value);
    InvalidateWidget(CPDFSDKFormFillEnvironmentFromFPDFFormHandle(hHandle),
                     widget);
    return true;
  }
  return false;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetFieldPropertyFloat(FPDF_FORMHANDLE hHandle,
                                FPDF_PAGE page,
                                double x,
                                double y,
                                FPDF_BYTESTRING key,
                                double value) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget) {
    return false;
  }
  const ByteString k(key ? key : "");
  if (k != "W") {
    return false;
  }
  CPDF_Dictionary* annotDict = widget->GetPDFAnnot()->GetMutableAnnotDict();
  if (value <= 0) {
    annotDict->RemoveFor("W");
  } else {
    annotDict->SetNewFor<CPDF_Number>("W", static_cast<float>(value));
  }
  ResetWidgetAppearance(widget, CPDFSDK_Widget::kValueUnchanged);
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetFieldPropertyStr(FPDF_FORMHANDLE hHandle,
                              FPDF_PAGE page,
                              double x,
                              double y,
                              FPDF_BYTESTRING key,
                              FPDF_WIDESTRING value) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget || !value) {
    return false;
  }
  CPDF_Document* doc = pPage->GetDocument();
  CPDF_Dictionary* annotDict = widget->GetPDFAnnot()->GetMutableAnnotDict();
  CPDF_Dictionary* fieldDict = FieldDictOfWidget(widget);
  const WideString ws = WideFromFPDF(value);
  const ByteString k(key ? key : "");
  if (k == "T") {
    if (!fieldDict) {
      return false;
    }
    bool isRadio =
        fieldDict->GetNameFor("FT") == "Btn" &&
        (static_cast<uint32_t>(fieldDict->GetIntegerFor("Ff")) &
         pdfium::form_flags::kButtonRadio);
    if (isRadio) {
      return RenameRadioField(doc, GetOrCreateAcroForm(doc), widget, ws);
    }
    fieldDict->SetNewFor<CPDF_String>("T", ws.AsStringView());
    return true;
  }
  if (k == "V" || k == "DV") {
    if (!fieldDict) {
      return false;
    }
    bool isBtn = fieldDict->GetNameFor("FT") == "Btn";
    if (isBtn) {
      fieldDict->SetNewFor<CPDF_Name>(k == "V" ? "V" : "DV", BytesFromWide(ws));
    } else {
      fieldDict->SetNewFor<CPDF_String>(k == "V" ? "V" : "DV",
                                        ws.AsStringView());
    }
    ResetWidgetAppearance(widget, CPDFSDK_Widget::kValueChanged);
    return true;
  }
  if (k == "DA") {
    if (!fieldDict) {
      return false;
    }
    fieldDict->SetNewFor<CPDF_String>("DA", BytesFromWide(ws));
    ResetWidgetAppearance(widget, CPDFSDK_Widget::kValueUnchanged);
    return true;
  }
  if (k == "S") {
    annotDict->SetNewFor<CPDF_Name>("S", BytesFromWide(ws));
    ResetWidgetAppearance(widget, CPDFSDK_Widget::kValueUnchanged);
    return true;
  }
  if (k == "MK_CA") {
    annotDict->GetOrCreateDictFor("MK")->SetNewFor<CPDF_Name>(
        "CA", BytesFromWide(ws));
    ResetWidgetAppearance(widget, CPDFSDK_Widget::kValueUnchanged);
    return true;
  }
  return false;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetFieldColor(FPDF_FORMHANDLE hHandle,
                        FPDF_PAGE page,
                        double x,
                        double y,
                        FPDF_BYTESTRING key,
                        float r,
                        float g,
                        float b) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget) {
    return false;
  }
  const ByteString k(key ? key : "");
  const char* mkKey = (k == "MK_BC") ? "BC" : (k == "MK_BG") ? "BG" : nullptr;
  if (!mkKey) {
    return false;
  }
  auto mk = widget->GetPDFAnnot()->GetMutableAnnotDict()->GetOrCreateDictFor("MK");
  if (r < 0) {  // 删除键（清除色）
    mk->RemoveFor(mkKey);
  } else {
    auto arr = mk->GetOrCreateArrayFor(mkKey);
    arr->Clear();
    arr->Append(pdfium::MakeRetain<CPDF_Number>(r));
    arr->Append(pdfium::MakeRetain<CPDF_Number>(g));
    arr->Append(pdfium::MakeRetain<CPDF_Number>(b));
  }
  ResetWidgetAppearance(widget, CPDFSDK_Widget::kValueUnchanged);
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetFieldDash(FPDF_FORMHANDLE hHandle,
                       FPDF_PAGE page,
                       double x,
                       double y,
                       const float* pattern,
                       size_t count) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget) {
    return false;
  }
  CPDF_Dictionary* annotDict = widget->GetPDFAnnot()->GetMutableAnnotDict();
  if (!pattern || count == 0) {
    annotDict->RemoveFor("D");
  } else {
    auto arr = annotDict->GetOrCreateArrayFor("D");
    arr->Clear();
    for (size_t i = 0; i < count; ++i) {
      arr->Append(pdfium::MakeRetain<CPDF_Number>(pattern[i]));
    }
  }
  ResetWidgetAppearance(widget, CPDFSDK_Widget::kValueUnchanged);
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetChoiceOption(FPDF_FORMHANDLE hHandle,
                          FPDF_PAGE page,
                          double x,
                          double y,
                          int index,
                          FPDF_WIDESTRING label,
                          FPDF_WIDESTRING exportValue) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget || !label) {
    return false;
  }
  CPDF_Dictionary* fieldDict = FieldDictOfWidget(widget);
  if (!fieldDict || fieldDict->GetNameFor("FT") != "Ch") {
    return false;
  }
  auto opt = fieldDict->GetOrCreateArrayFor("Opt");
  WideString wsLabel = WideFromFPDF(label);
  WideString wsExport = WideFromFPDF(exportValue);
  if (index < 0 || static_cast<size_t>(index) >= opt->size()) {
    if (wsExport.IsEmpty()) {
      opt->AppendNew<CPDF_String>(wsLabel.AsStringView());
    } else {  // [export, text]
      auto pair = pdfium::MakeRetain<CPDF_Array>();
      pair->AppendNew<CPDF_String>(wsExport.AsStringView());
      pair->AppendNew<CPDF_String>(wsLabel.AsStringView());
      opt->Append(std::move(pair));
    }
  } else {
    opt->RemoveAt(static_cast<size_t>(index));
    if (wsExport.IsEmpty()) {
      opt->InsertNewAt<CPDF_String>(static_cast<size_t>(index),
                                    wsLabel.AsStringView());
    } else {
      auto pair = pdfium::MakeRetain<CPDF_Array>();
      pair->AppendNew<CPDF_String>(wsExport.AsStringView());
      pair->AppendNew<CPDF_String>(wsLabel.AsStringView());
      opt->InsertAt(static_cast<size_t>(index), std::move(pair));
    }
  }
  ResetWidgetAppearance(widget, CPDFSDK_Widget::kValueChanged);
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_RemoveChoiceOption(FPDF_FORMHANDLE hHandle,
                             FPDF_PAGE page,
                             double x,
                             double y,
                             int index) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget) {
    return false;
  }
  CPDF_Dictionary* fieldDict = FieldDictOfWidget(widget);
  if (!fieldDict || fieldDict->GetNameFor("FT") != "Ch") {
    return false;
  }
  auto opt = fieldDict->GetMutableArrayFor("Opt");
  if (!opt || index < 0 || static_cast<size_t>(index) >= opt->size()) {
    return false;
  }
  opt->RemoveAt(static_cast<size_t>(index));
  if (opt->size() == 0) {
    fieldDict->RemoveFor("Opt");
  }
  ResetWidgetAppearance(widget, CPDFSDK_Widget::kValueChanged);
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_SetCheck(FPDF_FORMHANDLE hHandle,
                   FPDF_PAGE page,
                   double x,
                   double y,
                   FPDF_BOOL checked) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget) {
    return false;
  }
  widget->SetCheck(!!checked);  // CheckControl 处理 radio 组互斥 + /V 同步
  ResetWidgetAppearance(widget, CPDFSDK_Widget::kValueChanged);
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_GetFieldPropertyInt(FPDF_FORMHANDLE hHandle,
                              FPDF_PAGE page,
                              double x,
                              double y,
                              FPDF_BYTESTRING key,
                              int* outValue) {
  if (!outValue) {
    return false;
  }
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget) {
    return false;
  }
  const ByteString k(key ? key : "");
  if (k == "Ff" || k == "Q") {
    CPDF_Dictionary* fieldDict = FieldDictOfWidget(widget);
    if (!fieldDict) {
      return false;
    }
    *outValue = fieldDict->GetIntegerFor(k == "Ff" ? "Ff" : "Q");
    return true;
  }
  if (k == "F") {
    *outValue = widget->GetPDFAnnot()->GetMutableAnnotDict()->GetIntegerFor("F");
    return true;
  }
  return false;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_GetFieldPropertyFloat(FPDF_FORMHANDLE hHandle,
                                FPDF_PAGE page,
                                double x,
                                double y,
                                FPDF_BYTESTRING key,
                                double* outValue) {
  if (!outValue) {
    return false;
  }
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget) {
    return false;
  }
  const ByteString k(key ? key : "");
  if (k != "W") {
    return false;
  }
  *outValue = widget->GetPDFAnnot()->GetMutableAnnotDict()->GetFloatFor("W");
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_GetFieldPropertyStr(FPDF_FORMHANDLE hHandle,
                              FPDF_PAGE page,
                              double x,
                              double y,
                              FPDF_BYTESTRING key,
                              unsigned short* buffer,
                              size_t bufferLen,
                              size_t* outNeededUTF16) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget) {
    return false;
  }
  CPDF_Dictionary* annotDict = widget->GetPDFAnnot()->GetMutableAnnotDict();
  CPDF_Dictionary* fieldDict = FieldDictOfWidget(widget);
  const ByteString k(key ? key : "");
  WideString ws;
  if (k == "T" && fieldDict) {
    ws = fieldDict->GetUnicodeTextFor("T");
  } else if ((k == "V" || k == "DV") && fieldDict) {
    bool isBtn = fieldDict->GetNameFor("FT") == "Btn";
    ws = isBtn
             ? WideFromBytes(fieldDict->GetNameFor(k == "V" ? "V" : "DV"))
             : fieldDict->GetUnicodeTextFor(k == "V" ? "V" : "DV");
  } else if (k == "DA" && fieldDict) {
    // /DA 常由 /AcroForm 继承而不写进字段字典，GetStringFor 可能返回 nullptr，
    // 直接解引用会段错误；缺失时按空字符串处理（同 MK_CA 的 null 守卫）。
    const CPDF_Object* daObj = fieldDict->GetObjectFor("DA");
    ws = WideFromBytes(daObj ? daObj->GetString() : ByteString());
  } else if (k == "S") {
    ws = WideFromBytes(annotDict->GetNameFor("S"));
  } else if (k == "MK_CA") {
    ws = WideFromBytes(annotDict->GetDictFor("MK")
                           ? annotDict->GetDictFor("MK")->GetNameFor("CA")
                           : ByteString());
  } else {
    return false;
  }
  return WriteUTF16Out(ws, buffer, bufferLen, outNeededUTF16);
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_GetFieldColor(FPDF_FORMHANDLE hHandle,
                        FPDF_PAGE page,
                        double x,
                        double y,
                        FPDF_BYTESTRING key,
                        float* outR,
                        float* outG,
                        float* outB) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget) {
    return false;
  }
  const ByteString k(key ? key : "");
  const char* mkKey = (k == "MK_BC") ? "BC" : (k == "MK_BG") ? "BG" : nullptr;
  if (!mkKey) {
    return false;
  }
  auto mk = widget->GetPDFAnnot()->GetMutableAnnotDict()->GetDictFor("MK");
  auto arr = mk ? mk->GetArrayFor(mkKey) : nullptr;
  if (!arr || arr->size() < 3) {
    return false;
  }
  *outR = arr->GetFloatAt(0);
  *outG = arr->GetFloatAt(1);
  *outB = arr->GetFloatAt(2);
  return true;
}

FPDF_BOOL FPDF_CALLCONV
FPDF_Form_GetChoiceOptionCount(FPDF_FORMHANDLE hHandle,
                               FPDF_PAGE page,
                               double x,
                               double y,
                               int* outCount) {
  if (!outCount) {
    return false;
  }
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget) {
    return false;
  }
  CPDF_Dictionary* fieldDict = FieldDictOfWidget(widget);
  if (!fieldDict || fieldDict->GetNameFor("FT") != "Ch") {
    return false;
  }
  auto opt = fieldDict->GetArrayFor("Opt");
  *outCount = opt ? static_cast<int>(opt->size()) : 0;
  return true;
}

FPDF_BOOL FPDF_CALLCONV
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
                          size_t* outExportNeeded) {
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  CPDFSDK_Widget* widget =
      pPage ? HitWidgetAt(hHandle, pPage, CFX_PointF(
                                          static_cast<float>(x),
                                          static_cast<float>(y)))
            : nullptr;
  if (!widget) {
    return false;
  }
  CPDF_Dictionary* fieldDict = FieldDictOfWidget(widget);
  auto opt = fieldDict ? fieldDict->GetArrayFor("Opt") : nullptr;
  if (!opt || index < 0 || static_cast<size_t>(index) >= opt->size()) {
    return false;
  }
  const CPDF_Object* element = opt->GetObjectAt(static_cast<size_t>(index));
  if (!element) {
    return false;
  }
  if (const CPDF_Array* pair = element->AsArray()) {
    // [export, text]
    const CPDF_String* ex = pair->GetObjectAt(0)
                                ? pair->GetObjectAt(0)->AsString()
                                : nullptr;
    const CPDF_String* tx = pair->GetObjectAt(1)
                                ? pair->GetObjectAt(1)->AsString()
                                : nullptr;
    if (!WriteUTF16Out(tx ? tx->GetUnicodeText() : WideString(), labelBuffer,
                       labelBufferLen, outLabelNeeded)) {
      return false;
    }
    return WriteUTF16Out(ex ? ex->GetUnicodeText() : WideString(),
                         exportBuffer, exportBufferLen, outExportNeeded);
  }
  const CPDF_String* s = element->AsString();
  if (!s) {
    return false;
  }
  if (!WriteUTF16Out(s->GetUnicodeText(), labelBuffer, labelBufferLen,
                     outLabelNeeded)) {
    return false;
  }
  return WriteUTF16Out(s->GetUnicodeText(), exportBuffer, exportBufferLen,
                       outExportNeeded);
}