#!/usr/bin/env bash
# 桌面端（Windows）发布包组装：在 MSYS2 UCRT64 环境里编译原生窗口壳与服务端，打成 zip
#
# 用法:
#   package_desktop_win.sh [版本号]        # 省略时取 GITHUB_REF_NAME / version.hpp 回落值
#
# 环境变量：
#   WEBVIEW2_INC  含 WebView2.h 的目录（默认 C:/AndroidSDK/webview2/build/native/include）
#   WEBVIEW2_DLL  WebView2Loader.dll 路径（默认 C:/AndroidSDK/webview2/build/native/x64/WebView2Loader.dll）
#
# 产物：dist/rad-music-v<版本>-win64-desktop.zip
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# ---------------------------------------------------------------- 版本号
VER="${1:-}"
if [[ -z "$VER" ]]; then
    if [[ "${GITHUB_REF_TYPE:-}" == "tag" && -n "${GITHUB_REF_NAME:-}" ]]; then
        VER="${GITHUB_REF_NAME#v}"
    else
        VER="$(sed -n 's/^#define RADMUSIC_VERSION "\(.*\)"/\1/p' api/include/radmusic/version.hpp | head -1)"
    fi
fi
[[ -n "$VER" ]] || { echo "无法确定版本号"; exit 1; }

NAME="rad-music-v${VER}-win64-desktop"
STAGE="dist/${NAME}"

echo "== Rad Music 桌面端（Windows）打包 =="
echo "   版本: $VER"

# ---------------------------------------------------------------- 1. 编译外壳 + 服务端
# 复用 desktop/build_desktop.sh（本地开发同一套命令），版本号通过 RADMUSIC_VERSION 传下去
RADMUSIC_VERSION="$VER" bash desktop/build_desktop.sh

SRC="${DIST_DIR:-$ROOT/dist/desktop}"
[[ -f "$SRC/radmusic_desktop.exe" ]] || { echo "缺少 radmusic_desktop.exe"; exit 1; }
[[ -f "$SRC/radmusic_server.exe" ]]  || { echo "缺少 radmusic_server.exe"; exit 1; }

# ---------------------------------------------------------------- 2. 组装发布目录
rm -rf "$STAGE"
mkdir -p "$STAGE/web" "$STAGE/plugins/netease" "$STAGE/plugins/bilibili"

cp "$SRC/radmusic_desktop.exe" "$STAGE/"
cp "$SRC/radmusic_server.exe"  "$STAGE/"
if [[ -f "$SRC/WebView2Loader.dll" ]]; then
    cp "$SRC/WebView2Loader.dll" "$STAGE/"
else
    echo "::error::缺少 WebView2Loader.dll（设置 WEBVIEW2_DLL）"
    exit 1
fi

cp -R web/. "$STAGE/web/"
cp Readme.md "$STAGE/"

# 插件只收源码与清单
for plugin in netease bilibili; do
    cp "plugins/${plugin}/main.py" \
       "plugins/${plugin}/plugin.json" \
       "plugins/${plugin}/README.md" \
       "$STAGE/plugins/${plugin}/"
done

cat > "$STAGE/使用说明.txt" <<'TXT'
Rad Music 桌面端（免安装）

1. 解压到一个可写目录（不要放在 C:\Program Files 下，程序需要在自己旁边建 radmusic-data）。
2. 双击 radmusic_desktop.exe 即可，界面在自带窗口里打开，不需要浏览器。
3. 曲库默认建在程序目录的 radmusic-data\，可在「设置 → 数据」里迁移。
4. 在线音源需要系统里有 Python 3，并按 plugins\*\README.md 生成 cookie.txt。
5. 首次运行若被 360 / 火绒拦截，把整个目录加入信任列表即可（程序未签名）。
6. WebView2 运行时：Win10/11 一般已预装；若提示缺少，装一次
   https://go.microsoft.com/fwlink/p/?LinkId=2124703 即可。

卸载：直接删掉这个目录（曲库一起删的话注意先备份）。
TXT

# ---------------------------------------------------------------- 3. 归档
PYTHON="$(command -v python3 || command -v python || true)"
rm -f "dist/${NAME}.zip"
if command -v zip >/dev/null 2>&1; then
    ( cd dist && zip -q -r "${NAME}.zip" "${NAME}" )
else
    # 本地没有 zip 时用 python 兜底（CI 一律走上面的 zip）
    "$PYTHON" -m zipfile -c "dist/${NAME}.zip" "$STAGE"
fi

# 自检：归档内不得出现曲库、cookie 或日志
if command -v unzip >/dev/null 2>&1; then
    LIST="$(unzip -l "dist/${NAME}.zip")"
else
    LIST="$("$PYTHON" -m zipfile -l "dist/${NAME}.zip")"
fi
if printf '%s' "$LIST" | grep -Eqi 'cookie|usec\.json|radmusic-data|\.log$|__pycache__|\.venv'; then
    echo "::error::发布包中出现了禁止内容（cookie / 曲库 / 日志）"
    exit 1
fi

rm -rf "$STAGE"
echo "已生成 dist/${NAME}.zip ($(du -h "dist/${NAME}.zip" | cut -f1))"
