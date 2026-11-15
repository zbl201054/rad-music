package com.radmusic.app;

import android.app.DownloadManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Environment;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Locale;

/**
 * 安卓端的更新流程（与桌面端 / 浏览器端共用同一套 Releases 约定）。
 *
 * 桌面端由 C++ 的 UpdateService 自己完成「检查 → 下载 → 替换文件 → 重启」；
 * 安卓不行：系统不允许应用静默替换自身，安装包必须交给系统安装器。
 * 所以这里的职责只有两件事：
 *   1. check()：直接查 GitHub / Gitee Releases（C++ 侧在 Android 上没有 curl 可用，
 *      网络请求只能由 Java 做），按频道挑出最新版本与本机 ABI 匹配的 APK；
 *   2. download()：把 APK 交给系统 DownloadManager（通知栏可见），下载完由
 *      installDownloaded() 拉起系统安装器。
 *
 * 前端（web/settings.js）通过 RadMusicNative.updateCheck / updateInstall 调用，
 * 结果以 JSON 回调回页面，字段与 GET /music/update 的降级状态对齐（见 api.cpp）。
 */
final class UpdateClient {
    /** 系统安装器要求的 MIME。 */
    static final String APK_MIME = "application/vnd.android.package-archive";
    /** 下载任务 id 存在这里，页面刷新后仍能「继续安装」。 */
    private static final String PREFS = "radmusic-update";
    private static final String KEY_DOWNLOAD_ID = "download_id";
    private static final String KEY_DOWNLOAD_NAME = "download_name";
    private static final int CONNECT_TIMEOUT_MS = 12000;
    private static final int READ_TIMEOUT_MS = 15000;
    private static final int MAX_RELEASES = 30;

    private UpdateClient() {}

    // ---------------------------------------------------------------- 检查更新

    /**
     * 查询发布列表并挑出该频道下最新的、带安卓安装包的那个版本。
     * 阻塞调用（网络），必须放在后台线程。
     *
     * @param repo    GitHub 仓库 owner/name
     * @param gitee   同上的 Gitee 镜像仓库（source=gitee 时使用）
     * @param channel release / dev / canary
     * @param source  github / gitee
     * @return 与 GET /music/update 同名的 JSON；失败时 ok=false 且 error 有原因
     */
    static JSONObject check(Context context, String repo, String gitee, String channel, String source) {
        JSONObject result = new JSONObject();
        final String normalizedSource = "gitee".equals(source) ? "gitee" : "github";
        final String normalizedChannel = normalizeChannel(channel);
        final String activeRepo = "gitee".equals(normalizedSource)
                ? (isBlank(gitee) ? repo : gitee)
                : repo;
        final String current = currentVersion();
        try {
            result.put("current", current);
            result.put("channel", normalizedChannel);
            result.put("channel_label", channelLabel(normalizedChannel));
            result.put("source", normalizedSource);
            result.put("repo", activeRepo == null ? "" : activeRepo);
            if (isBlank(activeRepo)) {
                result.put("ok", false);
                result.put("error", "还没有配置更新仓库（设置 → 版本 → 仓库配置）");
                return result;
            }
            String api = "gitee".equals(normalizedSource)
                    ? "https://gitee.com/api/v5/repos/" + activeRepo + "/releases?per_page=" + MAX_RELEASES
                    : "https://api.github.com/repos/" + activeRepo + "/releases?per_page=" + MAX_RELEASES;
            JSONArray releases = new JSONArray(fetch(api, normalizedSource));
            Release best = null;
            for (int i = 0; i < releases.length(); i++) {
                JSONObject item = releases.optJSONObject(i);
                if (item == null) continue;
                if (item.optBoolean("draft", false)) continue;
                String tag = item.optString("tag_name", item.optString("tag", ""));
                if (tag.isEmpty()) continue;
                if (!normalizedChannel.equals(classifyChannel(tag, item.optBoolean("prerelease", false)))) {
                    continue;
                }
                Release release = new Release();
                release.tag = tag;
                release.version = stripV(tag);
                release.notes = item.optString("body", "");
                release.name = item.optString("name", tag);
                release.publishedAt = item.optString("published_at", item.optString("created_at", ""));
                release.pageUrl = releasePageUrl(normalizedSource, activeRepo, tag);
                release.asset = pickAsset(item.optJSONArray("assets"));
                // 没有安卓安装包的版本不能作为更新目标（否则用户点下载会 404）
                if (release.asset == null) continue;
                if (best == null || compareVersionFull(release.version, best.version) > 0) best = release;
            }
            if (best == null) {
                result.put("ok", true);
                result.put("has_update", false);
                result.put("latest", "");
                result.put("tag", "");
                result.put("error", "");
                result.put("notes", "");
                result.put("page_url", "https://" + ("gitee".equals(normalizedSource) ? "gitee.com/" : "github.com/")
                        + activeRepo + "/releases");
                return result;
            }
            result.put("ok", true);
            result.put("latest", best.version);
            result.put("tag", best.tag);
            result.put("name", best.name);
            result.put("notes", best.notes);
            result.put("published_at", best.publishedAt);
            result.put("page_url", best.pageUrl);
            result.put("has_update", compareVersionFull(best.version, current) > 0);
            result.put("url", best.asset.url);
            result.put("asset_name", best.asset.name);
            result.put("size", best.asset.size);
            result.put("error", "");
            return result;
        } catch (Exception e) {
            try {
                result.put("ok", false);
                result.put("has_update", false);
                result.put("error", describe(e));
            } catch (Exception ignored) {
                // JSONObject.put 不会失败
            }
            return result;
        }
    }

    // ---------------------------------------------------------------- 下载 / 安装

    /** 把 APK 交给系统下载器；返回 DownloadManager 的下载 id。 */
    static long download(Context context, String url, String fileName, String version) {
        String name = isBlank(fileName) ? "radmusic-" + version + "-android-arm64.apk" : fileName;
        DownloadManager.Request request = new DownloadManager.Request(Uri.parse(url));
        request.setTitle(name);
        request.setDescription("Rad Music " + version + " 安装包");
        request.setMimeType(APK_MIME);
        request.setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED);
        request.setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, name);
        request.allowScanningByMediaScanner();
        DownloadManager manager = (DownloadManager) context.getSystemService(Context.DOWNLOAD_SERVICE);
        if (manager == null) throw new IllegalStateException("系统下载服务不可用");
        long id = manager.enqueue(request);
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putLong(KEY_DOWNLOAD_ID, id)
                .putString(KEY_DOWNLOAD_NAME, name)
                .apply();
        return id;
    }

    /** 已下载完的安装包 → 拉起系统安装器。返回是否成功发起安装。 */
    static boolean installDownloaded(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        long id = prefs.getLong(KEY_DOWNLOAD_ID, -1);
        if (id < 0) return false;
        DownloadManager manager = (DownloadManager) context.getSystemService(Context.DOWNLOAD_SERVICE);
        if (manager == null) return false;
        Uri uri = manager.getUriForDownloadedFile(id);
        if (uri == null) return false;   // 还没下载完 / 已被删除
        Intent intent = new Intent(Intent.ACTION_VIEW)
                .setDataAndType(uri, APK_MIME)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            context.startActivity(intent);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    /** 该下载是否已完成（用于页面上切换「下载中 / 安装」按钮）。 */
    static boolean downloadFinished(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        long id = prefs.getLong(KEY_DOWNLOAD_ID, -1);
        if (id < 0) return false;
        DownloadManager manager = (DownloadManager) context.getSystemService(Context.DOWNLOAD_SERVICE);
        if (manager == null) return false;
        DownloadManager.Query query = new DownloadManager.Query().setFilterById(id);
        try (android.database.Cursor cursor = manager.query(query)) {
            if (cursor == null || !cursor.moveToFirst()) return false;
            int status = cursor.getInt(cursor.getColumnIndexOrThrow(DownloadManager.COLUMN_STATUS));
            return status == DownloadManager.STATUS_SUCCESSFUL;
        } catch (Exception e) {
            return false;
        }
    }

    /** 是否允许「安装未知应用」（Android 8+ 必须为 true 才能拉起安装器）。 */
    static boolean canInstallPackages(Context context) {
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.O) return true;
        PackageManager pm = context.getPackageManager();
        return pm != null && pm.canRequestPackageInstalls();
    }

    /** 跳到「允许安装未知应用」的系统设置页。 */
    static boolean openInstallSettings(Context context) {
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.O) return false;
        try {
            Intent intent = new Intent(android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES)
                    .setData(Uri.parse("package:" + context.getPackageName()))
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    // ---------------------------------------------------------------- 版本号

    /** 当前安装的版本号（去掉构建脚本加的 -android 后缀，便于和服务端 / tag 比较）。 */
    static String currentVersion() {
        String name = BuildConfigVersion.NAME;
        if (name != null && name.endsWith("-android")) {
            name = name.substring(0, name.length() - "-android".length());
        }
        return name == null ? "" : name;
    }

    static int currentVersionCode() {
        return BuildConfigVersion.CODE;
    }

    // ---------------------------------------------------------------- 版本比较（对齐 update.cpp）

    /**
     * 连预发布后缀一起比较：0.12.0 > 0.12.0-canary.2；canary.10 > canary.9。
     * 与 C++ 的 compare_version_full 保持同一套语义，避免两端判断不一致。
     */
    static int compareVersionFull(String left, String right) {
        String a = left == null ? "" : left.trim();
        String b = right == null ? "" : right.trim();
        int[] mainA = parseMain(a);
        int[] mainB = parseMain(b);
        for (int i = 0; i < 3; i++) {
            if (mainA[i] != mainB[i]) return mainA[i] > mainB[i] ? 1 : -1;
        }
        String suffA = parseSuffix(a);
        String suffB = parseSuffix(b);
        if (suffA.isEmpty() && suffB.isEmpty()) return 0;
        if (suffA.isEmpty()) return 1;    // 正式版高于同号预发布
        if (suffB.isEmpty()) return -1;
        String[] partsA = suffA.split("[.-]");
        String[] partsB = suffB.split("[.-]");
        int count = Math.max(partsA.length, partsB.length);
        for (int i = 0; i < count; i++) {
            String x = i < partsA.length ? partsA[i] : "0";
            String y = i < partsB.length ? partsB[i] : "0";
            boolean numX = isNumeric(x);
            boolean numY = isNumeric(y);
            int cmp;
            if (numX && numY) {
                cmp = Long.compare(parseLong(x), parseLong(y));
            } else if (numX) {
                cmp = -1;   // 数字段低于字母段（semver）
            } else if (numY) {
                cmp = 1;
            } else {
                cmp = x.compareToIgnoreCase(y);
            }
            if (cmp != 0) return cmp > 0 ? 1 : -1;
        }
        return 0;
    }

    private static int[] parseMain(String version) {
        String text = stripV(version);
        int cut = text.length();
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c == '-' || c == '+') {
                cut = i;
                break;
            }
        }
        String[] segments = text.substring(0, cut).split("\\.");
        int[] out = new int[3];
        for (int i = 0; i < 3 && i < segments.length; i++) {
            String digits = segments[i].replaceAll("[^0-9]", "");
            out[i] = digits.isEmpty() ? 0 : (int) Math.min(parseLong(digits), Integer.MAX_VALUE);
        }
        return out;
    }

    private static String parseSuffix(String version) {
        String text = stripV(version);
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c == '-' || c == '+') return text.substring(i + 1);
        }
        return "";
    }

    private static String stripV(String version) {
        if (version == null) return "";
        String text = version.trim();
        if (!text.isEmpty() && (text.charAt(0) == 'v' || text.charAt(0) == 'V')) {
            return text.substring(1);
        }
        return text;
    }

    private static boolean isNumeric(String text) {
        if (text.isEmpty()) return false;
        for (int i = 0; i < text.length(); i++) {
            if (!Character.isDigit(text.charAt(i))) return false;
        }
        return true;
    }

    private static long parseLong(String text) {
        try {
            return Long.parseLong(text);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /** tag / prerelease 标记 → 频道 id（与 update.cpp 的 classify_update_channel 一致）。 */
    static String classifyChannel(String tag, boolean prerelease) {
        String text = tag == null ? "" : tag.toLowerCase(Locale.US);
        if (text.contains("-canary")) return "canary";
        if (text.contains("-dev") || text.contains("-alpha") || text.contains("-beta")
                || text.contains("-rc") || text.contains("-preview") || prerelease) {
            return "dev";
        }
        return "release";
    }

    static String normalizeChannel(String channel) {
        String text = channel == null ? "" : channel.trim();
        if ("dev".equals(text) || "canary".equals(text)) return text;
        return "release";
    }

    static String channelLabel(String channel) {
        if ("dev".equals(channel)) return "开发版";
        if ("canary".equals(channel)) return "金丝雀";
        return "正式版";
    }

    // ---------------------------------------------------------------- 内部数据结构

    private static final class Asset {
        String name = "";
        String url = "";
        long size = 0;
    }

    private static final class Release {
        String tag = "";
        String version = "";
        String name = "";
        String notes = "";
        String publishedAt = "";
        String pageUrl = "";
        Asset asset;
    }

    /** 从 releases[].assets 里挑安卓安装包：优先 android-arm64.apk，其次任意 .apk。 */
    private static Asset pickAsset(JSONArray assets) {
        if (assets == null) return null;
        Asset fuzzy = null;
        for (int i = 0; i < assets.length(); i++) {
            JSONObject item = assets.optJSONObject(i);
            if (item == null) continue;
            String name = item.optString("name", "");
            String url = item.optString("browser_download_url", item.optString("download_url", ""));
            if (name.isEmpty() || url.isEmpty()) continue;
            String lower = name.toLowerCase(Locale.US);
            if (!lower.endsWith(".apk")) continue;
            Asset asset = new Asset();
            asset.name = name;
            asset.url = url;
            asset.size = item.optLong("size", 0);
            if (lower.contains("android-arm64") || lower.contains("arm64")) return asset;
            if (fuzzy == null) fuzzy = asset;
        }
        return fuzzy;
    }

    private static String releasePageUrl(String source, String repo, String tag) {
        String base = "gitee".equals(source) ? "https://gitee.com/" : "https://github.com/";
        if (tag == null || tag.isEmpty()) return base + repo + "/releases";
        return base + repo + "/releases/tag/" + tag;
    }

    // ---------------------------------------------------------------- 网络

    private static String fetch(String url, String source) throws Exception {
        HttpURLConnection conn = (HttpURLConnection) new URL(url).openConnection();
        try {
            conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
            conn.setReadTimeout(READ_TIMEOUT_MS);
            conn.setRequestMethod("GET");
            conn.setRequestProperty("Accept", "gitee".equals(source)
                    ? "application/json" : "application/vnd.github+json");
            conn.setRequestProperty("User-Agent", "RadMusic-Android/" + currentVersion());
            int code = conn.getResponseCode();
            InputStream stream = code >= 200 && code < 300 ? conn.getInputStream() : conn.getErrorStream();
            String body = stream == null ? "" : readAll(stream);
            if (code < 200 || code >= 300) {
                throw new IllegalStateException("发布源返回 HTTP " + code
                        + (code == 403 || code == 429 ? "（接口限流，稍后再试或切换到另一个下载源）" : ""));
            }
            if (body.isEmpty()) throw new IllegalStateException("发布源返回了空响应");
            return body;
        } finally {
            conn.disconnect();
        }
    }

    private static String readAll(InputStream stream) throws Exception {
        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        byte[] chunk = new byte[16 * 1024];
        int read;
        try {
            while ((read = stream.read(chunk)) > 0) buffer.write(chunk, 0, read);
        } finally {
            stream.close();
        }
        return new String(buffer.toByteArray(), "UTF-8");
    }

    private static String describe(Exception e) {
        String message = e.getMessage();
        if (message == null || message.isEmpty()) message = e.getClass().getSimpleName();
        if (e instanceof java.net.UnknownHostException
                || e instanceof java.net.ConnectException
                || e instanceof java.net.SocketTimeoutException) {
            return "连不上发布源（" + message + "），可以换「下载源」再试";
        }
        return message;
    }

    private static boolean isBlank(String text) {
        return text == null || text.trim().isEmpty();
    }
}
