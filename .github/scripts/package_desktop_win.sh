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
mkdir -p "$STAGE/web"

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

# 插件只收源码与清单（与 web 版同一份清单，见 package.sh）
for plugin in netease bilibili recommend equalizer; do
    mkdir -p "$STAGE/plugins/${plugin}"
    for file in main.py plugin.json README.md preferences.json panel.html; do
        if [ -f "plugins/${plugin}/${file}" ]; then
            cp "plugins/${plugin}/${file}" "$STAGE/plugins/${plugin}/"
        fi
    done
done

cat > "$STAGE/使用说明.txt" <<'TXT'
Rad Music 桌面端

一、用安装包装（推荐）
   双击 rad-music-*-win64-desktop-setup.exe，一路下一步即可。
   默认装到 %LOCALAPPDATA%\Programs\RadMusic（按用户安装，不需要管理员权限），
   曲库 radmusic-data 就建在安装目录旁边；卸载时可以选择是否连曲库一起删。
   「设置 → 版本 → 检查更新」走的也是这个安装包：下载后静默安装并自动重启程序。

二、用免安装包
   解压到一个可写目录（别放 C:\Program Files 下，程序需要在自己旁边建 radmusic-data），
   双击 radmusic_desktop.exe 即可，界面在自带窗口里打开，不需要浏览器。
   免安装版可以手动下载上面那个安装包原地升级（安装时把目录选成它所在的位置）。

其它说明
   1. 曲库默认建在程序目录的 radmusic-data\，可在「设置 → 数据」里迁移。
   2. 在线音源需要系统里有 Python 3，并按 plugins\*\README.md 生成 cookie.txt。
   3. 首次运行若被 360 / 火绒拦截，把整个目录加入信任列表即可（程序未签名）。
   4. WebView2 运行时：Win10/11 一般已预装；若提示缺少，装一次
      https://go.microsoft.com/fwlink/p/?LinkId=2124703 即可。
   5. 退出程序时会一并关闭它拉起的插件进程（免得后台留着 python 占内存和端口）。
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

# ---------------------------------------------------------------- 4. 安装包（客户端更新用）
# 客户端「下载并安装」拿到的就是这个 setup.exe：静默跑它，整套程序（含外壳）一起换。
# 没装 Inno Setup 时只提示，不影响免安装 zip 的产出。
if bash desktop/build_installer.sh "$VER" "$STAGE"; then
    echo "安装包已生成"
else
    echo "::warning::没有生成安装包（Inno Setup 不可用），本次发布只有免安装 zip"
fi

rm -rf "$STAGE"
echo "已生成 dist/${NAME}.zip ($(du -h "dist/${NAME}.zip" | cut -f1))"
[[ -f "dist/rad-music-v${VER}-win64-desktop-setup.exe" ]] && \
    echo "已生成 dist/rad-music-v${VER}-win64-desktop-setup.exe"
exit 0
