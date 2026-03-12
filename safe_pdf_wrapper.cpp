#include "public/fpdfview.h"
#include "public/fpdf_edit.h"
#include "public/fpdf_transformpage.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

// 将英文/数字/符号水印转为 UTF-16LE（水印不包含中文，逐字节扩展即可）
static void ascii_to_utf16le(const char* src, uint16_t* dst, size_t dst_chars) {
  size_t i = 0;
  while (i + 1 < dst_chars && src[i]) {
    dst[i] = (uint16_t)(unsigned char)src[i];
    i++;
  }
  dst[i] = 0;
}

// 使用 fpdf_edit 在每一页打上 Helvetica 水印
static void add_watermark_to_document(FPDF_DOCUMENT doc, const char* watermark_text) {
  if (!doc || !watermark_text || !watermark_text[0])
    return;

  FPDF_FONT font = FPDFText_LoadStandardFont(doc, "Helvetica");
  if (!font)
    return;

  const int nPages = FPDF_GetPageCount(doc);
  const size_t wlen = strlen(watermark_text);
  uint16_t* wbuf = (uint16_t*)malloc((wlen + 1) * sizeof(uint16_t));
  if (!wbuf) {
    FPDFFont_Close(font);
    return;
  }
  ascii_to_utf16le(watermark_text, wbuf, wlen + 1);

  for (int i = 0; i < nPages; i++) {
    FPDF_PAGE page = FPDF_LoadPage(doc, i);
    if (!page)
      continue;

    float left, bottom, right, top;
    if (!FPDFPage_GetMediaBox(page, &left, &bottom, &right, &top)) {
      FPDF_ClosePage(page);
      continue;
    }
    float width = right - left;
    float height = top - bottom;

    FPDF_PAGEOBJECT textObj = FPDFPageObj_CreateTextObj(doc, font, 24.0f);
    if (!textObj) {
      FPDF_ClosePage(page);
      continue;
    }

    FPDFText_SetText(textObj, (FPDF_WIDESTRING)wbuf);

    // 页面中心（PDF 坐标系原点在左下角）
    double x = (double)(width / 2.0f - 60.0f);
    double y = (double)(height / 2.0f - 12.0f);
    FPDFPageObj_Transform(textObj, 1, 0, 0, 1, x, y);

    FPDFPageObj_SetFillColor(textObj, 180, 180, 180, 180);
    FPDFTextObj_SetTextRenderMode(textObj, FPDF_TEXTRENDERMODE_FILL);

    FPDFPage_InsertObject(page, textObj);
    FPDFPage_GenerateContent(page);
    FPDF_ClosePage(page);
  }

  free(wbuf);
  FPDFFont_Close(font);
}

// 内部用：返回当前要使用的水印字符串（仅英文+数字+符号）。返回空串表示不加水印。
static const char* get_watermark_text(void) {
  static const char* mock_watermark = "CONFIDENTIAL";  // 示例；改为 "" 即不加水印
  return mock_watermark;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
// data_buffer: PDF 二进制；size: 长度；password: 密码，无则传 null/0。
// 内部调用 get_watermark_text() 获取水印字符串并打水印。
FPDF_DOCUMENT FPDF_Custom_LoadMemDocument(uint8_t* data_buffer,
                                         int size,
                                         const char* password) {
  FPDF_DOCUMENT doc = FPDF_LoadMemDocument(data_buffer, size, password);
  if (!doc)
    return nullptr;

  const char* watermark_text = get_watermark_text();
  add_watermark_to_document(doc, watermark_text);
  return doc;
}

}
