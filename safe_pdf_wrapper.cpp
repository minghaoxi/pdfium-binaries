#include "public/fpdfview.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_transformpage.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <emscripten.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_PAGES_TO_TRACK 1024

// 将英文/数字/符号水印转为 UTF-16LE（水印不包含中文，逐字节扩展即可）
static void ascii_to_utf16le(const char* src, uint16_t* dst, size_t dst_chars) {
  size_t i = 0;
  while (i + 1 < dst_chars && src[i]) {
    dst[i] = (uint16_t)(unsigned char)src[i];
    i++;
  }
  dst[i] = 0;
}

// 对单页平铺水印：倾斜 angle_degrees 度，字体大小 font_size，充满整页
static void add_watermark_to_single_page(FPDF_DOCUMENT doc,
                                         int page_index,
                                         const char* watermark_text,
                                         float font_size,
                                         float angle_degrees) {
  if (!doc || !watermark_text || !watermark_text[0])
    return;
  if (font_size <= 0.f) font_size = 10.f;

  FPDF_FONT font = FPDFText_LoadStandardFont(doc, "Helvetica");
  if (!font)
    return;

  const size_t wlen = strlen(watermark_text);
  uint16_t* wbuf = (uint16_t*)malloc((wlen + 1) * sizeof(uint16_t));
  if (!wbuf) {
    FPDFFont_Close(font);
    return;
  }
  ascii_to_utf16le(watermark_text, wbuf, wlen + 1);

  FPDF_PAGE page = FPDF_LoadPage(doc, page_index);
  if (!page) {
    free(wbuf);
    FPDFFont_Close(font);
    return;
  }

  float left, bottom, right, top;
  if (!FPDFPage_GetMediaBox(page, &left, &bottom, &right, &top)) {
    FPDF_ClosePage(page);
    free(wbuf);
    FPDFFont_Close(font);
    return;
  }
  const double width = (double)(right - left);
  const double height = (double)(top - bottom);

  const double angle_rad = (double)angle_degrees * M_PI / 180.0;
  const double cos_a = cos(angle_rad);
  const double sin_a = sin(angle_rad);
  const double step_x = (double)font_size * 7.0;
  const double step_y = (double)font_size * 4.0;

  for (double y = bottom - height; y < top + height; y += step_y) {
    for (double x = left - width; x < right + width; x += step_x) {
      FPDF_PAGEOBJECT textObj = FPDFPageObj_CreateTextObj(doc, font, font_size);
      if (!textObj)
        continue;

      FPDFText_SetText(textObj, (FPDF_WIDESTRING)wbuf);
      FPDFPageObj_Transform(textObj, cos_a, sin_a, -sin_a, cos_a, x, y);
      FPDFPageObj_SetFillColor(textObj, 180, 180, 180, 180);
      FPDFTextObj_SetTextRenderMode(textObj, FPDF_TEXTRENDERMODE_FILL);

      FPDFPage_InsertObject(page, textObj);
    }
  }

  FPDFPage_GenerateContent(page);
  FPDF_ClosePage(page);
  free(wbuf);
  FPDFFont_Close(font);
}

// 内部用：返回当前要使用的水印字符串（仅英文+数字+符号）。返回空串表示不加水印。
static const char* get_watermark_text(void) {
  static const char* mock_watermark = "CONFIDENTIAL";  // 示例；改为 "" 即不加水印
  return mock_watermark;
}

// 记录当前文档下已打过水印的页，避免同一页重复打水印
static FPDF_DOCUMENT s_last_doc = nullptr;
static uint8_t s_page_watermarked[MAX_PAGES_TO_TRACK];

// 内部：若该页尚未打过水印则打上，否则跳过（对 JS 不可见）
static void add_watermark_to_page_if_needed(FPDF_DOCUMENT doc, int page_index) {
  if (!doc)
    return;
  if (doc != s_last_doc) {
    s_last_doc = doc;
    memset(s_page_watermarked, 0, sizeof(s_page_watermarked));
  }
  if (page_index < 0 || page_index >= MAX_PAGES_TO_TRACK)
    return;
  if (s_page_watermarked[page_index])
    return;

  const char* watermark_text = get_watermark_text();
  if (!watermark_text || !watermark_text[0])
    return;

  const float font_size = 10.f;
  const float angle_degrees = 45.f;
  add_watermark_to_single_page(doc, page_index, watermark_text, font_size, angle_degrees);
  s_page_watermarked[page_index] = 1;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
// data_buffer: PDF 二进制；size: 长度；password: 密码，无则传 null/0。
// 仅加载文档，不打水印。
FPDF_DOCUMENT FPDF_Custom_LoadMemDocument(uint8_t* data_buffer,
                                         int size,
                                         const char* password) {
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument(data_buffer, size, password);
  if (!doc)
    return nullptr;
  s_last_doc = doc;
  memset(s_page_watermarked, 0, sizeof(s_page_watermarked));
  return doc;
}

EMSCRIPTEN_KEEPALIVE
// 翻页/加载页：JS 只调用此方法加载指定页，与 FPDF_LoadPage 行为一致。
// 内部在加载前对该页按需打水印（对 JS 透明）。
FPDF_PAGE FPDF_Custom_LoadPage(FPDF_DOCUMENT document, int page_index) {
  add_watermark_to_page_if_needed(document, page_index);
  return FPDF_LoadPage(document, page_index);
}

}