#!/usr/bin/env bash
# 安卓端发布包组装（CI 用）：调 android/build_apk.sh，再把 APK 放到 dist/ 顶层
#
# 产物：dist/rad-music-v<版本>-android-arm64.apk
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

VER="${1:-}"
if [[ -z "$VER" ]]; then
    if [[ "${GITHUB_REF_TYPE:-}" == "tag" && -n "${GITHUB_REF_NAME:-}" ]]; then
        VER="${GITHUB_REF_NAME#v}"
    elif [[ -n "${RADMUSIC_VERSION:-}" ]]; then
        VER="$RADMUSIC_VERSION"
    else
        VER="$(sed -n 's/^#define RADMUSIC_VERSION "\(.*\)"/\1/p' api/include/radmusic/version.hpp | head -1)"
    fi
fi
[[ -n "$VER" ]] || { echo "无法确定版本号"; exit 1; }

export RADMUSIC_VERSION="$VER"
bash android/build_apk.sh

SRC="dist/android/radmusic-${VER}-arm64.apk"
[[ -f "$SRC" ]] || { echo "缺少 APK: $SRC"; exit 1; }
cp "$SRC" "dist/rad-music-v${VER}-android-arm64.apk"
echo "已生成 dist/rad-music-v${VER}-android-arm64.apk"
