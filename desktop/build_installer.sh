#!/usr/bin/env bash
# 打 Windows 安装包（客户端「安装包更新」用的那个 *-setup.exe）
#
# 用法：
#   bash desktop/build_installer.sh <版本号> <已组装好的发布目录>
#   例：bash desktop/build_installer.sh 0.15.0 dist/rad-music-v0.15.0-win64-desktop
#
# 环境变量：
#   ISCC         ISCC.exe 路径（省略时按常见安装位置 / PATH 查找）
#   RADMUSIC_OUT 输出目录（默认仓库 dist/）
#
# 产物：<输出目录>/rad-music-v<版本>-win64-desktop-setup.exe
#
# 依赖 Inno Setup 6（CI 里由 workflow 装；本机可用 winget/choco 装一次）。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="${1:-}"
STAGE="${2:-}"

[[ -n "$VER" ]] || { echo "用法: build_installer.sh <版本号> <发布目录>"; exit 1; }
[[ -n "$STAGE" ]] || { echo "用法: build_installer.sh <版本号> <发布目录>"; exit 1; }
[[ -d "$STAGE" ]] || { echo "发布目录不存在: $STAGE"; exit 1; }
[[ -f "$STAGE/radmusic_desktop.exe" ]] || { echo "发布目录里没有 radmusic_desktop.exe: $STAGE"; exit 1; }
[[ -f "$ROOT/desktop/installer/radmusic.iss" ]] || { echo "缺少 desktop/installer/radmusic.iss"; exit 1; }
# Inno Setup 6 本体不带中文语言文件，这份是随仓库走的（取自 issrc 的 Unofficial/ChineseSimplified.isl）
[[ -f "$ROOT/desktop/installer/ChineseSimplified.isl" ]] || {
    echo "缺少中文语言文件 desktop/installer/ChineseSimplified.isl（安装向导的第二语言）"
    exit 1
}

OUT="${RADMUSIC_OUT:-$ROOT/dist}"
mkdir -p "$OUT"

# ---------------------------------------------------------------- 找 ISCC
if [[ -n "${ISCC:-}" && -f "$ISCC" ]]; then
    :
else
    ISCC=""
    for candidate in \
        "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
        "/c/Program Files/Inno Setup 6/ISCC.exe" \
        "/c/Users/$USERNAME/AppData/Local/Programs/InnoSetup6/ISCC.exe" \
        "/c/Program Files (x86)/Inno Setup 5/ISCC.exe"
    do
        [[ -f "$candidate" ]] && { ISCC="$candidate"; break; }
    done
    # 按用户安装（/CURRENTUSER）也可能落在带空格的默认目录里
    if [[ -z "$ISCC" && -n "${LOCALAPPDATA:-}" ]]; then
        for candidate in "$LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe" \
                         "$LOCALAPPDATA/Programs/InnoSetup6/ISCC.exe"; do
            [[ -f "$candidate" ]] && { ISCC="$candidate"; break; }
        done
    fi
    if [[ -z "$ISCC" ]] && command -v iscc >/dev/null 2>&1; then
        ISCC="$(command -v iscc)"
    fi
fi
if [[ -z "${ISCC:-}" || ! -f "$ISCC" ]]; then
    echo "找不到 ISCC.exe（Inno Setup 6）。"
    echo "  CI：workflow 会用 choco 装；本机：winget install JRSoftware.InnoSetup"
    echo "  或者设置环境变量 ISCC 指向 ISCC.exe。"
    exit 2
fi

# ---------------------------------------------------------------- 路径转 Windows 形式
# ISCC 是原生 Windows 程序，拿不懂 /g/... 这种 MSYS 路径
win_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s' "$1"
    fi
}

export RADMUSIC_VERSION="$VER"
export RADMUSIC_STAGE="$(win_path "$(cd "$STAGE" && pwd)")"
export RADMUSIC_OUT="$(win_path "$(cd "$OUT" && pwd)")"

echo "== 打安装包 =="
echo "   版本: $VER"
echo "   源目录: $RADMUSIC_STAGE"
echo "   输出: $RADMUSIC_OUT"
echo "   ISCC: $ISCC"

# 不加 /Q：编译日志里有「压缩了哪些文件」，发布出问题时能直接看出漏了什么
"$ISCC" "$(win_path "$ROOT/desktop/installer/radmusic.iss")"

TARGET="$OUT/rad-music-v${VER}-win64-desktop-setup.exe"
if [[ ! -f "$TARGET" ]]; then
    echo "::error::安装包没有生成：$TARGET"
    exit 1
fi

# 去掉「低完整性（Low）」标签再交付。
#   Inno Setup 的安装程序启动时要把自己解到 %TEMP%\is-XXXX.tmp。如果这个 exe 带着
#   Mandatory Label\Low，Windows 会把进程也降到 Low，而 %TEMP% 是 Medium ——
#   于是立刻报「Setup was unable to create the directory …\is-XXXX.tmp，
#   Error 5: 拒绝访问」，exit 4、几百毫秒就死、连日志都不留。
#   某些受保护 / 沙箱化的目录（本机工作区 G:\Rad music 就是）会给里面新建的文件打上
#   Low 标签，于是「同一个安装包，在仓库里双击装不上、复制到桌面就能装」。
#   只在确实带 Low 时才动它：CI 上产出的文件没有标签，这条不生效。
#   注意 icacls 的开关要用 // 开头，否则 MSYS/git-bash 会把 /setintegritylevel 当成路径改写。
if command -v icacls >/dev/null 2>&1; then
    TARGET_WIN="$(win_path "$TARGET")"
    if icacls "$TARGET_WIN" 2>/dev/null | grep -qi "low mandatory"; then
        icacls "$TARGET_WIN" //setintegritylevel Medium >/dev/null 2>&1 || true
        echo "   已去掉低完整性标签（否则安装程序建不了 %TEMP% 目录）"
    fi
fi

echo "已生成 $TARGET ($(du -h "$TARGET" | cut -f1))"
