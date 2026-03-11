#include "public/fpdfview.h" 
#include "public/fpdf_edit.h" // 如果你用到了编辑功能（比如打水印）也要加前缀

#include <stdint.h>
#include <emscripten.h>

extern "C" {
    EMSCRIPTEN_KEEPALIVE
    // ⚠️ 只要名字以 FPDF 开头，打包脚本就会自动认出它并导出给 JS！
    FPDF_DOCUMENT FPDF_Custom_LoadMemDocument(uint8_t* data_buffer, int size, const char* password) {
        
        // 🚨 你的内存解密/预处理逻辑 🚨
        // for(int i = 0; i < size; i++) { data_buffer[i] ^= 0x5A; }
        
        return FPDF_LoadMemDocument(data_buffer, size, password);
    }
}