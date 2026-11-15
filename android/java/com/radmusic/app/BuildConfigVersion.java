package com.radmusic.app;

/**
 * 版本信息占位。
 *
 * 本工程不依赖 Gradle（用手写脚本调用 aapt2/javac/d8 打包），因此没有自动生成的
 * BuildConfig。构建脚本会在编译前把下面的 NAME / CODE 替换成实际版本号。
 *
 * CODE 是单调递增的 versionCode（0.14.1 → 140100 这类），系统安装器据此判断
 * 「这是升级」；恒为 1 时虽然还能同版本覆盖安装，但无法表达升降级，也无法阻止
 * 装上更旧的包。注入规则见 android/build_apk.sh 的 version_code()。
 */
final class BuildConfigVersion {
    static final String NAME = "0.15.1-android";
    static final int CODE = 1501090;

    private BuildConfigVersion() {}
}
