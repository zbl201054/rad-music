#!/usr/bin/env bash
# 发布包组装脚本（Windows / Linux / macOS 共用）
#
# 用法: package.sh <os> <slug> <pkg>
#   os   运行平台：Windows | Linux | macOS
#   slug 归档标识，如 win64 / linux-x86_64 / macos-arm64
#   pkg  归档格式：zip | tar.gz
#
# 约定：始终排除用户曲库与 cookie —— 只打包「可执行文件 + 运行时 DLL + web/ + plugins/ 的源码与清单 + Readme.md」
set -euo pipefail

OS="$1"
SLUG="$2"
PKG="$3"

# ---------------------------------------------------------------- 版本号
VER="${GITHUB_REF_NAME:-dev}"
VER="${VER#v}"
if [ "${GITHUB_REF_TYPE:-}" != "tag" ]; then
  VER="dev-$(printf '%s' "${GITHUB_SHA:-unknown}" | cut -c1-7)"
fi

NAME="rad-music-v${VER}-${SLUG}"
STAGE="dist/${NAME}"

rm -rf "$STAGE"
mkdir -p "$STAGE/web" "$STAGE/plugins/netease" "$STAGE/plugins/bilibili"

# ---------------------------------------------------------------- 可执行文件与运行时
if [ "$OS" = "Windows" ]; then
  cp api/build/radmusic_server.exe "$STAGE/"
  # MinGW-w64 运行时三件套（UCRT 由系统提供）
  cp /ucrt64/bin/libgcc_s_seh-1.dll \
     /ucrt64/bin/libstdc++-6.dll \
     /ucrt64/bin/libwinpthread-1.dll \
     "$STAGE/"
else
  cp api/build/radmusic_server "$STAGE/"
  chmod +x "$STAGE/radmusic_server"
fi

# ---------------------------------------------------------------- 前端与插件
cp -R web/. "$STAGE/web/"
cp Readme.md "$STAGE/"

# 插件只收源码与清单：绝不包含 cookie.txt / *.log / .venv 等运行时产物
for plugin in netease bilibili; do
  cp "plugins/${plugin}/main.py" \
     "plugins/${plugin}/plugin.json" \
     "plugins/${plugin}/README.md" \
     "$STAGE/plugins/${plugin}/"
done

# ---------------------------------------------------------------- 归档
mkdir -p dist
if [ "$PKG" = "zip" ]; then
  ( cd dist && zip -q -r "${NAME}.zip" "${NAME}" )
  ARTIFACT="dist/${NAME}.zip"
else
  tar -czf "dist/${NAME}.tar.gz" -C dist "${NAME}"
  ARTIFACT="dist/${NAME}.tar.gz"
fi

# 自检：归档内不得出现曲库、cookie 或日志
if unzip -l "$ARTIFACT" 2>/dev/null | grep -Eqi 'cookie|usec\.json|radmusic-data|\.log$'; then
  echo "::error::发布包中出现了禁止内容（cookie / 曲库 / 日志）"
  exit 1
elif tar -tzf "$ARTIFACT" 2>/dev/null | grep -Eqi 'cookie|usec\.json|radmusic-data|\.log$'; then
  echo "::error::发布包中出现了禁止内容（cookie / 曲库 / 日志）"
  exit 1
fi

rm -rf "$STAGE"
echo "已生成 ${ARTIFACT}"
