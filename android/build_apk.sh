#!/usr/bin/env bash
# Rad Music Android 打包脚本（不依赖 Gradle，Windows / Linux / macOS 通用）
#
# 做四件事：
#   1. 用 NDK 的 clang 把 api/ 交叉编译成 arm64-v8a 的可执行文件（伪装成 lib*.so 才能放进 jniLibs）
#   2. 把 web/ 打进 assets
#   3. aapt2 编资源 → javac → d8 → 组装 APK
#   4. zipalign + apksigner 签名
#
# 用法：
#   ./build_apk.sh              # 完整构建
#   ./build_apk.sh --skip-native # 只重新打包 Java/资源（原生库没改时快很多）
#
# 依赖：JDK 21、Android SDK（platform-tools / platforms;android-34 / build-tools;34.0.0 / ndk;26.1）
#   可用环境变量覆盖：
#     ANDROID_SDK_ROOT / ANDROID_HOME    SDK 根目录
#     JAVA_HOME_DIR / JAVA_HOME          JDK 根目录
#     ANDROID_NDK                        NDK 目录
#     ANDROID_BUILD_TOOLS                build-tools 目录
#     ANDROID_KEYSTORE_B64               调试签名库的 base64（CI 用，避免把 keystore 提交进仓库）
#     R8_JAR                             独立 d8 用的 r8.jar 路径
#
# 产物：dist/android/radmusic-<版本>-arm64.apk
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# ---------------------------------------------------------------- 宿主识别
UNAME_S="$(uname -s)"
case "$UNAME_S" in
    MINGW*|MSYS*|CYGWIN*) HOST_OS=windows; HOST_TAG=windows-x86_64 ;;
    Darwin)               HOST_OS=macos;   HOST_TAG=darwin-x86_64 ;;
    *)                    HOST_OS=linux;   HOST_TAG=linux-x86_64 ;;
esac
if [[ "$HOST_OS" == windows ]]; then EXE=".exe"; else EXE=""; fi

# ---------------------------------------------------------------- 工具链定位
if [[ -n "${ANDROID_SDK_ROOT:-}" ]]; then
    SDK="$ANDROID_SDK_ROOT"
elif [[ -n "${ANDROID_HOME:-}" ]]; then
    SDK="$ANDROID_HOME"
elif [[ "$HOST_OS" == windows ]]; then
    SDK="C:/AndroidSDK"
elif [[ "$HOST_OS" == macos ]]; then
    SDK="$HOME/Library/Android/sdk"
else
    SDK="$HOME/Android/Sdk"
fi

JDK="${JAVA_HOME_DIR:-${JAVA_HOME:-$SDK/jdk21}}"
[[ -d "$JDK" ]] || { echo "找不到 JDK: $JDK（用 JAVA_HOME_DIR 指定）"; exit 1; }

# NDK / build-tools / platform：优先用约定版本，缺失时退到目录里最新的一个
pick_latest() {
    local base="$1"
    [[ -d "$base" ]] || return 1
    ls -1 "$base" 2>/dev/null | sort -V | tail -1
}
NDK="${ANDROID_NDK:-$SDK/ndk/26.1.10909125}"
if [[ ! -d "$NDK" ]]; then
    NDK="$SDK/ndk/$(pick_latest "$SDK/ndk")"
fi
BT="${ANDROID_BUILD_TOOLS:-$SDK/build-tools/34.0.0}"
if [[ ! -d "$BT" ]]; then
    BT="$SDK/build-tools/$(pick_latest "$SDK/build-tools")"
fi
PLATFORM_JAR="$SDK/platforms/android-34/android.jar"
if [[ ! -f "$PLATFORM_JAR" ]]; then
    PLATFORM_JAR="$SDK/platforms/$(pick_latest "$SDK/platforms")/android.jar"
fi

OUT="$HERE/build"
ASSETS="$HERE/assets"
JNI="$HERE/jniLibs/arm64-v8a"
APK_DIR="$ROOT/dist/android"
MIN_SDK=24

# keytool / apksigner 需要 java 在 PATH 里
export JAVA_HOME="$(cd "$JDK" && pwd -W 2>/dev/null || echo "$JDK")"
export PATH="$JDK/bin:$PATH"

PYTHON="$(command -v python3 || command -v python || true)"
[[ -n "$PYTHON" ]] || { echo "找不到 python3 / python（组装 APK 需要）"; exit 1; }

SKIP_NATIVE=0
[[ "${1:-}" == "--skip-native" ]] && SKIP_NATIVE=1

# 版本号以源码里的回落值为准（与桌面端 binary 内嵌的一致）；CI 可用环境变量覆盖
VERSION="${RADMUSIC_VERSION:-}"
if [[ -z "$VERSION" ]]; then
    VERSION="$(sed -n 's/^#define RADMUSIC_VERSION "\(.*\)"/\1/p' "$ROOT/api/include/radmusic/version.hpp" | head -1)"
fi
VERSION="${VERSION:-0.0.0}"
APP_VERSION="$VERSION-android"

echo "== Rad Music Android 构建 =="
echo "   宿主     : $HOST_OS ($HOST_TAG)"
echo "   SDK      : $SDK"
echo "   NDK      : $NDK"
echo "   版本     : $APP_VERSION"

for tool in "$JDK/bin/javac$EXE" "$BT/aapt2$EXE" "$BT/zipalign$EXE" "$PLATFORM_JAR"; do
    [[ -e "$tool" ]] || { echo "缺少构建工具: $tool"; exit 1; }
done
APSIGNER="$BT/apksigner"
[[ "$HOST_OS" == windows ]] && APSIGNER="$BT/apksigner.bat"
[[ -e "$APSIGNER" ]] || { echo "缺少构建工具: $APSIGNER"; exit 1; }

mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/gen" "$JNI" "$ASSETS/web" "$APK_DIR"

# ---------------------------------------------------------------- 1. 原生服务端
if [[ $SKIP_NATIVE -eq 0 ]]; then
    echo "-- 交叉编译 radmusic_server (arm64-v8a)"
    CLANG="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android${MIN_SDK}-clang++"
    [[ -e "$CLANG" ]] || CLANG="$CLANG.cmd"
    [[ -e "$CLANG" ]] || CLANG="$CLANG.exe"
    [[ -e "$CLANG" ]] || { echo "找不到 NDK clang: $CLANG"; exit 1; }

    # 路径里有空格（项目目录 "Rad music"），必须用数组传递，不能靠字符串展开
    SRCS=()
    while IFS= read -r f; do SRCS+=("$f"); done < <(ls -1 "$ROOT"/api/src/*.cpp)
    "$CLANG" -std=c++17 -O2 -fPIE -pie -static-libstdc++ \
        -DRADMUSIC_VERSION="\"$VERSION\"" \
        -I"$ROOT/api/include" \
        "${SRCS[@]}" "$ROOT/api/tools/radmusic_server.cpp" \
        -o "$JNI/libradmusic_server.so" \
        -Wl,-z,max-page-size=16384
    echo "   产物: $(du -h "$JNI/libradmusic_server.so" | cut -f1)"
fi

# ---------------------------------------------------------------- 2. 前端资源
echo "-- 同步 web/ 到 assets"
WEB_VERSION="$(cd "$ROOT/web" && cat *.js *.css *.html | md5sum | cut -c1-12)"
rm -rf "$ASSETS/web"
mkdir -p "$ASSETS/web"
cp "$ROOT"/web/* "$ASSETS/web/"
printf '%s' "$WEB_VERSION" > "$ASSETS/web_version.txt"
echo "   前端版本标记: $WEB_VERSION"

# ---------------------------------------------------------------- 3. Java 源码
echo "-- 注入版本号"
sed -i "s|static final String NAME = \".*\";|static final String NAME = \"$APP_VERSION\";|" \
    "$HERE/java/com/radmusic/app/BuildConfigVersion.java"

# ---------------------------------------------------------------- 4. 编译资源
echo "-- aapt2 编译资源"
rm -rf "$OUT/compiled" "$OUT/gen" "$OUT/classes" "$OUT/dex"
mkdir -p "$OUT/compiled" "$OUT/gen" "$OUT/classes" "$OUT/dex"
"$BT/aapt2$EXE" compile --dir "$HERE/res" -o "$OUT/compiled/res.zip"

# 版本号写进清单：用 aapt2 link 的 --version-* 覆盖
# manifest 里的 versionName 优先级高于 --version-name，这里先把版本号写进一份临时清单
sed "s|android:versionName=\"[^\"]*\"|android:versionName=\"$VERSION\"|" \
    "$HERE/AndroidManifest.xml" > "$OUT/AndroidManifest.xml"

"$BT/aapt2$EXE" link \
    -o "$OUT/base.apk" \
    -I "$PLATFORM_JAR" \
    --manifest "$OUT/AndroidManifest.xml" \
    -A "$ASSETS" \
    --java "$OUT/gen" \
    --min-sdk-version $MIN_SDK \
    --target-sdk-version 34 \
    --version-name "$VERSION" \
    "$OUT/compiled/res.zip"

# ---------------------------------------------------------------- 5. javac / d8
echo "-- javac"
JAVA_SRCS=()
while IFS= read -r f; do JAVA_SRCS+=("$f"); done < <(find "$HERE/java" "$OUT/gen" -name '*.java')
"$JDK/bin/javac$EXE" -encoding UTF-8 -source 8 -target 8 -nowarn \
    -bootclasspath "$PLATFORM_JAR" \
    -d "$OUT/classes" \
    "${JAVA_SRCS[@]}"

echo "-- d8 (dex)"
CLASSES=()
while IFS= read -r f; do CLASSES+=("$f"); done < <(find "$OUT/classes" -name '*.class')
# build-tools 34 自带的 d8(8.2.2) 在 JDK 21 上解析匿名类会抛 NPE（R8 内部 bug），
# 这里改用独立的最新 R8 jar（同一个 d8 工具，只是版本新一点）。
R8_JAR="${R8_JAR:-$SDK/r8.jar}"
if [[ ! -f "$R8_JAR" ]]; then
    echo "   下载 R8（约 20 MB）…"
    curl -L --noproxy '*' --retry 3 --retry-delay 2 -o "$R8_JAR" \
        "https://maven.aliyun.com/repository/google/com/android/tools/r8/9.5.29/r8-9.5.29.jar"
fi
"$JDK/bin/java$EXE" -cp "$R8_JAR" com.android.tools.r8.D8 \
    --lib "$PLATFORM_JAR" --min-api $MIN_SDK --output "$OUT/dex" "${CLASSES[@]}"

# ---------------------------------------------------------------- 6. 组装 APK
echo "-- 组装 APK"
cp "$OUT/base.apk" "$OUT/unsigned.apk"
# 把 dex 与原生可执行文件塞进 APK：
#   classes.dex 正常压缩；
#   lib/arm64-v8a/*.so 用 STORED（不压缩）并给 0755，配合 extractNativeLibs="true"
#   让系统在安装时解压到 nativeLibraryDir 并保留可执行位。
"$PYTHON" - "$OUT/unsigned.apk" "$OUT/dex/classes.dex" "$JNI/libradmusic_server.so" <<'PYEOF'
import sys, zipfile, time
apk, dex, so = sys.argv[1], sys.argv[2], sys.argv[3]
with zipfile.ZipFile(apk, "a", zipfile.ZIP_DEFLATED) as z:
    z.write(dex, "classes.dex")
    info = zipfile.ZipInfo("lib/arm64-v8a/libradmusic_server.so", time.localtime()[:6])
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o755 << 16
    with open(so, "rb") as handle:
        z.writestr(info, handle.read())
print("   classes.dex 与原生服务端已写入")
PYEOF

"$BT/zipalign$EXE" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

# ---------------------------------------------------------------- 7. 签名
KEYSTORE="$HERE/keystore/radmusic-debug.keystore"
# CI 里不提交 keystore：用 secret 传 base64，解码后复用同一把调试签名，
# 这样每次发版签出来的包签名一致，用户可以覆盖安装升级。
if [[ ! -f "$KEYSTORE" && -n "${ANDROID_KEYSTORE_B64:-}" ]]; then
    echo "-- 从 ANDROID_KEYSTORE_B64 还原签名库"
    mkdir -p "$(dirname "$KEYSTORE")"
    printf '%s' "$ANDROID_KEYSTORE_B64" | base64 -d > "$KEYSTORE"
fi
if [[ ! -f "$KEYSTORE" ]]; then
    echo "-- 生成调试签名（首次）"
    mkdir -p "$(dirname "$KEYSTORE")"
    "$JDK/bin/keytool$EXE" -genkeypair -v \
        -keystore "$KEYSTORE" -storepass radmusic -keypass radmusic \
        -alias radmusic -keyalg RSA -keysize 2048 -validity 10950 \
        -dname "CN=Rad Music, OU=Android, O=Rad Music, L=CN, S=CN, C=CN" >/dev/null 2>&1
fi

APK="$APK_DIR/radmusic-${VERSION}-arm64.apk"
"$APSIGNER" sign \
    --ks "$KEYSTORE" --ks-pass pass:radmusic --key-pass pass:radmusic \
    --min-sdk-version $MIN_SDK \
    --out "$APK" "$OUT/aligned.apk"

rm -f "$OUT/aligned.apk" "$OUT/unsigned.apk"
echo
echo "构建完成: $APK  ($(du -h "$APK" | cut -f1))"
