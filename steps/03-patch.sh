#!/bin/bash -eux

PATCHES="$PWD/patches"
SOURCE="${PDFium_SOURCE_DIR:-pdfium}"
OS="${PDFium_TARGET_OS:?}"
TARGET_CPU="${PDFium_TARGET_CPU:?}"
TARGET_ENVIRONMENT="${PDFium_TARGET_ENVIRONMENT:-}"
ENABLE_V8=${PDFium_ENABLE_V8:-false}
BUILD_TYPE=${PDFium_BUILD_TYPE:-shared}

apply_patch() {
  local FILE="$1"
  local DIR="${2:-.}"
  patch --verbose -p1 -d "$DIR" -i "$FILE"
}

pushd "${SOURCE}"

[ "$BUILD_TYPE" == "shared" ] && [ "$OS" != "emscripten" ] && apply_patch "$PATCHES/shared_library.patch"
apply_patch "$PATCHES/public_headers.patch"

[ "$ENABLE_V8" == "true" ] && apply_patch "$PATCHES/v8/pdfium.patch"

case "$OS" in
  android)
    apply_patch "$PATCHES/android/build.patch" build
    ;;

  ios)
    apply_patch "$PATCHES/ios/pdfium.patch"
    [ "$ENABLE_V8" == "true" ] && apply_patch "$PATCHES/ios/v8.patch" v8
    ;;

  mac)
    apply_patch "$PATCHES/mac/build.patch" build
    ;;

  linux)
    [ "$ENABLE_V8" == "true" ] && apply_patch "$PATCHES/linux/v8.patch" v8
    apply_patch "$PATCHES/linux/build.patch" build
    ;;

  emscripten)
    apply_patch "$PATCHES/wasm/pdfium.patch"
    apply_patch "$PATCHES/wasm/build.patch" build
    if [ "$ENABLE_V8" == "true" ]; then
      apply_patch "$PATCHES/wasm/v8.patch" v8
    fi
    mkdir -p "build/config/wasm"
    cp "$PATCHES/wasm/config.gn" "build/config/wasm/BUILD.gn"
    # ==============================================================
    # 🌟 你的专属注入代码加在这里 🌟
    # 1. 把仓库根目录的 C++ 文件拷贝到当前目录（PDFium 源码根目录）
    # (注意：因为脚本顶部定义了 PATCHES="$PWD/patches"，所以 $PATCHES/.. 就是仓库根目录)
    WRAPPER_SRC="$PATCHES/../safe_pdf_wrapper.cpp"
    if [ ! -f "$WRAPPER_SRC" ]; then
      echo "[ERROR] safe_pdf_wrapper.cpp 未找到: $WRAPPER_SRC（请确认在 pdfium-binaries 根目录执行 build）"
      exit 1
    fi
    cp "$WRAPPER_SRC" safe_pdf_wrapper.cpp
    # 2. 在 pdfium 目标内为 emscripten 添加 sources（不能匹配第一个 sources = [，那是 pdfium_public_headers_impl 的 .h 列表，会触发 GN 语法错误）
    #    在 output_name = "pdfium" 后插入 if (target_os == "emscripten") { sources = [ "safe_pdf_wrapper.cpp" ] }
    sed -i '/output_name = "pdfium"/a\
  if (target_os == "emscripten") {\
    sources = [\
      "safe_pdf_wrapper.cpp"\
    ]\
  }' BUILD.gn
    # ==============================================================
    ;;

  win)
    apply_patch "$PATCHES/win/build.patch" build

    VERSION=${PDFium_VERSION:-0.0.0.0}
    YEAR=$(date +%Y)
    VERSION_CSV=${VERSION//./,}
    export YEAR VERSION VERSION_CSV
    envsubst < "$PATCHES/win/resources.rc" > "resources.rc"
    ;;
esac

case "$TARGET_ENVIRONMENT" in
  musl)
    apply_patch "$PATCHES/musl/pdfium.patch"
    apply_patch "$PATCHES/musl/build.patch" build
    mkdir -p "build/toolchain/linux/musl"
    cp "$PATCHES/musl/toolchain.gn" "build/toolchain/linux/musl/BUILD.gn"
    ;;
esac

case "$TARGET_CPU" in
  ppc64)
    apply_patch "$PATCHES/ppc64/pdfium.patch"
    apply_patch "$PATCHES/ppc64/build.patch" build
    ;;
esac

popd
