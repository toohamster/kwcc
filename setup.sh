#!/usr/bin/env bash
# setup.sh — 下载所有依赖源码到 ./deps
set -euo pipefail

DEPS_DIR="$(cd "$(dirname "$0")" && pwd)/deps"
mkdir -p "$DEPS_DIR/sokol" "$DEPS_DIR/nanovg" "$DEPS_DIR/microui" "$DEPS_DIR/mquickjs"
mkdir -p "$(dirname "$0")/assets"

download() {
  url="$1"
  dest="$2"
  echo "=> $dest"
  curl -fSL --retry 3 "$url" -o "$dest"
}

# --- Sokol (窗口与 GFX) ---
echo "[1/5] Downloading sokol..."
SOKOL_BASE="https://raw.githubusercontent.com/floooh/sokol/master"
download "$SOKOL_BASE/sokol_app.h"  "$DEPS_DIR/sokol/sokol_app.h"
download "$SOKOL_BASE/sokol_gfx.h"  "$DEPS_DIR/sokol/sokol_gfx.h"
download "$SOKOL_BASE/sokol_glue.h" "$DEPS_DIR/sokol/sokol_glue.h"
download "$SOKOL_BASE/sokol_log.h"  "$DEPS_DIR/sokol/sokol_log.h"

# --- NanoVG (向量绘图) ---
echo "[2/5] Downloading nanovg..."
NANOVG_BASE="https://raw.githubusercontent.com/memononen/NanoVG/master/src"
download "$NANOVG_BASE/nanovg.h"        "$DEPS_DIR/nanovg/nanovg.h"
download "$NANOVG_BASE/nanovg.c"        "$DEPS_DIR/nanovg/nanovg.c"
download "$NANOVG_BASE/nanovg_gl.h"     "$DEPS_DIR/nanovg/nanovg_gl.h"
download "$NANOVG_BASE/fontstash.h"     "$DEPS_DIR/nanovg/fontstash.h"
download "$NANOVG_BASE/stb_image.h"     "$DEPS_DIR/nanovg/stb_image.h"
download "$NANOVG_BASE/stb_truetype.h"  "$DEPS_DIR/nanovg/stb_truetype.h"

# --- microui (IMGUI 布局) ---
echo "[3/5] Downloading microui..."
MICROUI_BASE="https://raw.githubusercontent.com/rxi/microui/master/src"
download "$MICROUI_BASE/microui.h"  "$DEPS_DIR/microui/microui.h"
download "$MICROUI_BASE/microui.c"  "$DEPS_DIR/microui/microui.c"

# --- mquickjs (轻量脚本引擎) ---
echo "[4/5] Downloading mquickjs..."
MQJS_URL="https://github.com/bellard/mquickjs/archive/refs/heads/master.tar.gz"
MQJS_TMP=$(mktemp -d)
curl -fSL "$MQJS_URL" -o "$MQJS_TMP/mquickjs.tar.gz"
tar -xzf "$MQJS_TMP/mquickjs.tar.gz" -C "$MQJS_TMP"
FOUND_DIR=$(find "$MQJS_TMP" -maxdepth 1 -type d -name 'mquickjs*' | head -1)
if [ -n "$FOUND_DIR" ]; then
  # Core interpreter (linked into final binary)
  cp "$FOUND_DIR/mquickjs.c"       "$FOUND_DIR/mquickjs.h" \
     "$FOUND_DIR/mquickjs_priv.h"  "$FOUND_DIR/mquickjs_opcode.h" \
     "$FOUND_DIR/cutils.c"         "$FOUND_DIR/cutils.h" \
     "$FOUND_DIR/dtoa.c"           "$FOUND_DIR/dtoa.h" \
     "$FOUND_DIR/libm.c"           "$FOUND_DIR/libm.h" \
     "$FOUND_DIR/list.h" \
     "$FOUND_DIR/softfp_template.h" "$FOUND_DIR/softfp_template_icvt.h" \
     "$DEPS_DIR/mquickjs/"
  # Build tool (used at build time only, NOT linked)
  cp "$FOUND_DIR/mquickjs_build.c" "$FOUND_DIR/mquickjs_build.h" \
     "$FOUND_DIR/mqjs_stdlib.c" \
     "$DEPS_DIR/mquickjs/"
fi
rm -rf "$MQJS_TMP"

# --- 字体 (Roboto) ---
echo "[5/5] Downloading font..."
FONT_URL="https://fonts.gstatic.com/s/roboto/v30/KFOmCnqEu92Fr1Me5Q.ttf"
download "$FONT_URL" "$(dirname "$0")/assets/Roboto-Regular.ttf"

echo ""
echo "Done! All dependencies downloaded to ./deps"
