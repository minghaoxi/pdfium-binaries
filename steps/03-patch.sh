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
    
    # 1. 确保所有 C/C++ 文件都拷贝到了 fpdfsdk 目录下
    # 这里包含了你的包装器和国密库（假设你已经准备好了 sm2/3/4 的 .c 文件）
    cp "$PATCHES/../safe_pdf_wrapper.cpp" fpdfsdk/
    # 如果有国密文件，一并拷贝
    # cp "$PATCHES/../sm2.c" fpdfsdk/
    # cp "$PATCHES/../sm3.c" fpdfsdk/
    # cp "$PATCHES/../sm4.c" fpdfsdk/

    # 2. 精准注入：找到 fpdf_view.cpp 这行（它是 PDFium SDK 的核心文件，绝对存在）
    # 在它后面插入我们的自定义文件
    # 注意：一定要操作 fpdfsdk/BUILD.gn 而不是根目录的 BUILD.gn
    sed -i '/"fpdf_view.cpp",/a \    "safe_pdf_wrapper.cpp",' fpdfsdk/BUILD.gn
    
    # 如果你要加国密文件，可以连着写：
    # sed -i '/"safe_pdf_wrapper.cpp",/a \    "sm2.c",\n    "sm3.c",\n    "sm4.c",' fpdfsdk/BUILD.gn
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
