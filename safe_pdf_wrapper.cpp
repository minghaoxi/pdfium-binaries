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

#define LOG_ERROR(msg) \
  do { EM_ASM({ console.error(UTF8ToString($0)); }, (msg)); } while (0)
#define LOG_ERROR_PAGE(msg, page_idx) \
  do { EM_ASM({ console.error(UTF8ToString($0) + " page_index=" + $1); }, (msg), (page_idx)); } while (0)

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
  if (!font) {
    LOG_ERROR_PAGE("[safe_pdf_wrapper] FPDFText_LoadStandardFont(Helvetica) failed", page_index);
    return;
  }

  const size_t wlen = strlen(watermark_text);
  uint16_t* wbuf = (uint16_t*)malloc((wlen + 1) * sizeof(uint16_t));
  if (!wbuf) {
    LOG_ERROR_PAGE("[safe_pdf_wrapper] malloc watermark buffer failed", page_index);
    FPDFFont_Close(font);
    return;
  }
  ascii_to_utf16le(watermark_text, wbuf, wlen + 1);

  FPDF_PAGE page = FPDF_LoadPage(doc, page_index);
  if (!page) {
    LOG_ERROR_PAGE("[safe_pdf_wrapper] FPDF_LoadPage failed", page_index);
    free(wbuf);
    FPDFFont_Close(font);
    return;
  }

  float left, bottom, right, top;
  if (!FPDFPage_GetMediaBox(page, &left, &bottom, &right, &top)) {
    LOG_ERROR_PAGE("[safe_pdf_wrapper] FPDFPage_GetMediaBox failed", page_index);
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
  const double step_x = (double)font_size * 12.0;
  const double step_y = (double)font_size * 7.0;

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

// 探照灯：整页 bitmap 保存在 WASM 内，不暴露给 JS；仅通过 ApplySpotlight 输出“仅 spotlight 可见”的拷贝
static uint8_t* s_internal_page_buffer = nullptr;
static int s_internal_page_width = 0;
static int s_internal_page_height = 0;

#define FPDFBitmap_BGRA 4
#define FPDF_ANNOT 1

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
  if (!doc) {
    LOG_ERROR("[safe_pdf_wrapper] FPDF_LoadMemDocument failed");
    return nullptr;
  }
  s_last_doc = doc;
  memset(s_page_watermarked, 0, sizeof(s_page_watermarked));
  return doc;
}

EMSCRIPTEN_KEEPALIVE
// 翻页/加载页：JS 只调用此方法加载指定页，与 FPDF_LoadPage 行为一致。
// 内部在加载前对该页按需打水印（对 JS 透明）。
FPDF_PAGE FPDF_Custom_LoadPage(FPDF_DOCUMENT document, int page_index) {
  add_watermark_to_page_if_needed(document, page_index);
  FPDF_PAGE page = FPDF_LoadPage(document, page_index);
  if (!page)
    LOG_ERROR_PAGE("[safe_pdf_wrapper] FPDF_Custom_LoadPage: FPDF_LoadPage failed", page_index);
  return page;
}

// 在 WASM 内渲染整页到内部缓冲并保存，不把整页 bitmap 指针暴露给 JS。
// doc: 文档句柄；page_index: 页索引；scale: 缩放；out_width、out_height: 输出宽高（可为 null）。
// 返回 1 成功，0 失败。成功后 JS 仅通过 FPDF_Custom_ApplySpotlight(0, out_buf, ...) 获取“仅 spotlight”的拷贝。
EMSCRIPTEN_KEEPALIVE
int FPDF_Custom_RenderPageToInternalBuffer(FPDF_DOCUMENT doc,
                                            int page_index,
                                            float scale,
                                            int* out_width,
                                            int* out_height) {
  if (!doc || !out_width || !out_height)
    return 0;
  add_watermark_to_page_if_needed(doc, page_index);
  FPDF_PAGE page = FPDF_LoadPage(doc, page_index);
  if (!page) {
    LOG_ERROR_PAGE("[safe_pdf_wrapper] FPDF_Custom_RenderPageToInternalBuffer: FPDF_LoadPage failed", page_index);
    return 0;
  }
  const double docW = FPDF_GetPageWidth(page);
  const double docH = FPDF_GetPageHeight(page);
  const int w = (int)floor(docW * (double)scale);
  const int h = (int)floor(docH * (double)scale);
  if (w <= 0 || h <= 0) {
    FPDF_ClosePage(page);
    return 0;
  }
  if (s_internal_page_buffer) {
    free(s_internal_page_buffer);
    s_internal_page_buffer = nullptr;
  }
  const size_t buf_size = (size_t)w * (size_t)h * 4;
  s_internal_page_buffer = (uint8_t*)malloc(buf_size);
  if (!s_internal_page_buffer) {
    LOG_ERROR_PAGE("[safe_pdf_wrapper] FPDF_Custom_RenderPageToInternalBuffer: malloc failed", page_index);
    FPDF_ClosePage(page);
    return 0;
  }
  for (size_t i = 0; i < buf_size; i += 4) {
    s_internal_page_buffer[i] = 255;
    s_internal_page_buffer[i + 1] = 255;
    s_internal_page_buffer[i + 2] = 255;
    s_internal_page_buffer[i + 3] = 255;
  }
  FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA, s_internal_page_buffer, w * 4);
  if (!bitmap) {
    free(s_internal_page_buffer);
    s_internal_page_buffer = nullptr;
    FPDF_ClosePage(page);
    return 0;
  }
  FPDFBitmap_FillRect(bitmap, 0, 0, w, h, 0xffffffff);
  FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, 0, FPDF_ANNOT);
  FPDFBitmap_Destroy(bitmap);
  FPDF_ClosePage(page);
  s_internal_page_width = w;
  s_internal_page_height = h;
  *out_width = w;
  *out_height = h;
  return 1;
}

// 释放 WASM 内保存的整页缓冲（翻页或关闭时由 JS 调用）。
EMSCRIPTEN_KEEPALIVE
void FPDF_Custom_FreePageBuffer(void) {
  if (s_internal_page_buffer) {
    free(s_internal_page_buffer);
    s_internal_page_buffer = nullptr;
  }
  s_internal_page_width = 0;
  s_internal_page_height = 0;
}

// 探照灯模式：从源缓冲生成“仅保留圆心 (center_x, center_y)、半径 radius 内为真实 RGB”的 RGBA 拷贝到 output_buffer。
// 若 page_buffer 为 null，则使用 WASM 内保存的整页缓冲（s_internal_page_*），此时 width/height/stride 需与内部一致。
EMSCRIPTEN_KEEPALIVE
void FPDF_Custom_ApplySpotlight(uint8_t* page_buffer,
                                uint8_t* output_buffer,
                                int width,
                                int height,
                                int stride,
                                int center_x,
                                int center_y,
                                int radius) {
  if (!output_buffer || width <= 0 || height <= 0)
    return;
  const uint8_t* src = page_buffer;
  int src_stride = stride;
  if (!src) {
    src = s_internal_page_buffer;
    src_stride = s_internal_page_width * 4;
    if (!src || s_internal_page_width != width || s_internal_page_height != height)
      return;
  } else if (stride < width * 4) {
    return;
  }
  const int64_t r2 = (int64_t)radius * (int64_t)radius;
  const size_t out_stride = (size_t)width * 4;
  for (int y = 0; y < height; y++) {
    const int64_t dy = (int64_t)y - (int64_t)center_y;
    const int64_t dy2 = dy * dy;
    const uint8_t* src_row = src + (size_t)y * (size_t)src_stride;
    uint8_t* dst_row = output_buffer + (size_t)y * out_stride;
    for (int x = 0; x < width; x++) {
      const int64_t dx = (int64_t)x - (int64_t)center_x;
      const size_t src_off = (size_t)x * 4;
      const size_t dst_off = src_off;
      if (radius <= 0 || dx * dx + dy2 > r2) {
        dst_row[dst_off] = 0;
        dst_row[dst_off + 1] = 0;
        dst_row[dst_off + 2] = 0;
        dst_row[dst_off + 3] = 0;
      } else {
        dst_row[dst_off] = src_row[src_off + 2];
        dst_row[dst_off + 1] = src_row[src_off + 1];
        dst_row[dst_off + 2] = src_row[src_off];
        dst_row[dst_off + 3] = src_row[src_off + 3];
      }
    }
  }
}

}