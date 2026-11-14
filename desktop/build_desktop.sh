#!/usr/bin/env bash
# Rad Music 桌面端打包：产出一个双击即用的目录（不依赖浏览器）
#
#   dist/desktop/
#     radmusic_desktop.exe     原生窗口壳（Win32 + WebView2）
#     radmusic_server.exe      C++ 服务端（与原来完全一致）
#     WebView2Loader.dll       WebView2 加载器（系统一般已装运行时）
#     web/                     前端
#     plugins/                 音源插件（需要系统装 Python）
#     radmusic-data/           首次启动会自动建目录结构
#
# 用法： ./build_desktop.sh
#   RADMUSIC_VERSION  覆盖版本号（发布流水线传 tag；省略时用 Makefile 里的回落值）
#   DIST_DIR          输出目录（默认 dist/desktop；程序正在运行时换一个目录才能重建）
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DIST="${DIST_DIR:-$ROOT/dist/desktop}"
WEBVIEW2_INC="${WEBVIEW2_INC:-C:/AndroidSDK/webview2/build/native/include}"
WEBVIEW2_DLL="${WEBVIEW2_DLL:-C:/AndroidSDK/webview2/build/native/x64/WebView2Loader.dll}"

echo "== Rad Music 桌面端打包 =="

[[ -e "$WEBVIEW2_INC/WebView2.h" ]] || { echo "缺少 WebView2 头文件: $WEBVIEW2_INC"; exit 1; }

echo "-- 编译原生窗口壳"
cd "$HERE"
windres app.rc -O coff -o app.res
# -static：把 MinGW 运行时（libstdc++ / libgcc / libwinpthread）链进去，
# 发行包就不必再带三个 DLL，也不会因为用户 PATH 里没有 MSYS2 而静默启动失败。
g++ -std=c++17 -O2 -static -municode -mwindows main.cpp app.res -o radmusic_desktop.exe \
    -I"$WEBVIEW2_INC" \
    -lws2_32 -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -lgdi32 -luser32

echo "-- 编译服务端"
if [[ -n "${RADMUSIC_VERSION:-}" ]]; then
    (cd "$ROOT/api" && mingw32-make server VERSION="$RADMUSIC_VERSION" CXXFLAGS="-std=c++17 -Wall -Wextra -O2 -static" >/dev/null)
else
    (cd "$ROOT/api" && mingw32-make server CXXFLAGS="-std=c++17 -Wall -Wextra -O2 -static" >/dev/null)
fi

echo "-- 组装 dist"
rm -rf "$DIST"
mkdir -p "$DIST/web" "$DIST/radmusic-data"
cp radmusic_desktop.exe "$DIST/"
cp "$ROOT/api/build/radmusic_server.exe" "$DIST/"
[[ -e "$WEBVIEW2_DLL" ]] && cp "$WEBVIEW2_DLL" "$DIST/"
cp "$ROOT"/web/* "$DIST/web/"
if [[ -d "$ROOT/plugins" ]]; then
    mkdir -p "$DIST/plugins"
    # 只收源码与清单：cookie.txt / *.log / __pycache__ / selftest 这类运行时产物绝不进包
    for dir in "$ROOT"/plugins/*/; do
        name="$(basename "$dir")"
        [[ "$name" == *.log ]] && continue
        mkdir -p "$DIST/plugins/$name"
        for file in main.py plugin.json README.md preferences.json panel.html; do
            [[ -f "$dir/$file" ]] && cp "$dir/$file" "$DIST/plugins/$name/"
        done
    done
fi
cp "$ROOT/Readme.md" "$DIST/" 2>/dev/null || true

echo
echo "完成: $DIST"
du -sh "$DIST" 2>/dev/null | cut -f1
