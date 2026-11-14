package com.radmusic.app.plugin;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.HashMap;
import java.util.Map;

/**
 * 网易云音乐音源（Java 版，只做公开接口）。
 *
 * 与桌面端 Python 插件的差别：
 *   · 不实现登录 / 加密 API（weapi、eapi 需要 RSA 与 AES，且要带 Cookie），
 *     所以最高只到 128K「标准」音质，VIP / 无损曲目拿不到地址；
 *   · 其余 op 的字段与 plugins/netease/main.py 保持一致。
 */
final class NeteaseSource implements Source {

    private static final String API = "https://music.163.com";
    private static final String UA =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                    + "Chrome/120.0.0.0 Safari/537.36";

    private final int port;
    /** Cookie 落盘位置（app 私有目录；插件目录在 Android 上只读） */
    private final java.io.File cookieFile;
    private String cookie = "";
    private boolean cookieLoaded = false;

    NeteaseSource(int port, java.io.File cookieFile) {
        this.port = port;
        this.cookieFile = cookieFile;
    }

    private String cookie() {
        if (!cookieLoaded) {
            cookieLoaded = true;
            try {
                if (cookieFile != null && cookieFile.isFile()) {
                    byte[] data = new byte[(int) cookieFile.length()];
                    java.io.FileInputStream in = new java.io.FileInputStream(cookieFile);
                    int read = in.read(data);
                    in.close();
                    if (read > 0) cookie = new String(data, 0, read, "UTF-8").trim();
                }
            } catch (Exception e) {
                Http.log("读取网易云 Cookie 失败：" + e.getMessage());
            }
        }
        return cookie;
    }

    void saveCookie(String text) {
        cookie = text == null ? "" : text.trim();
        cookieLoaded = true;
        try {
            if (cookieFile == null) return;
            java.io.File parent = cookieFile.getParentFile();
            if (parent != null) parent.mkdirs();
            if (cookie.isEmpty()) {
                //noinspection ResultOfMethodCallIgnored
                cookieFile.delete();
                return;
            }
            java.io.FileOutputStream out = new java.io.FileOutputStream(cookieFile);
            out.write(cookie.getBytes("UTF-8"));
            out.close();
        } catch (Exception e) {
            Http.log("保存网易云 Cookie 失败：" + e.getMessage());
        }
    }

    @Override
    public String platform() {
        return "netease";
    }

    private Map<String, String> headers() {
        String stored = cookie();
        Map<String, String> h = Http.headers("User-Agent", UA, "Referer", "https://music.163.com/",
                "Accept", "application/json",
                "Cookie", stored.isEmpty() ? "os=pc; appver=2.9.7" : stored + "; os=pc");
        return h;
    }

    private JSONObject get(String path, Map<String, String> params) throws PluginError {
        StringBuilder url = new StringBuilder(API).append(path);
        if (params != null && !params.isEmpty()) {
            url.append(path.contains("?") ? "&" : "?");
            boolean first = true;
            for (Map.Entry<String, String> entry : params.entrySet()) {
                if (!first) url.append('&');
                first = false;
                url.append(BiliSource.encode(entry.getKey())).append('=').append(BiliSource.encode(entry.getValue()));
            }
        }
        try {
            return Http.getJson(url.toString(), headers());
        } catch (Http.HttpError e) {
            throw new PluginError(1011, "网易云请求失败：HTTP " + e.code);
        } catch (IOException e) {
            throw new PluginError(1011, "无法访问网易云：" + e.getMessage());
        }
    }

    @Override
    public Object request(String op, JSONObject params) throws PluginError {
        try {
        switch (op) {
            case "search": return search(params);
            case "get_song": return getSong(params);
            case "get_url": return getUrl(params);
            case "get_qualities": return getQualities(params);
            case "get_lyric": return getLyric(params);
            case "get_pic": return getPic(params);
            case "check_login": return checkLogin();
            case "login_cookie": return loginCookie(params);
            case "logout": return logout();
            case "settings": return settings();
            case "get_playlist": return getPlaylist(params);
            case "get_album": return getAlbum(params);
            case "search_playlist": return searchPlaylist(params);
            case "search_album": return searchAlbum(params);
            case "search_artist": return searchArtist(params);
            case "get_artist": return getArtist(params);
            case "search_mv": return searchMv(params);
            case "get_mv": return getMv(params);
            case "login_qr_start":
            case "login_qr_check":
            case "login_phone":
                throw new PluginError(1012, "Android 版插件未实现扫码 / 手机号登录，请在设置里粘贴 Cookie 登录");
            default: throw new PluginError(1012, "该插件不支持 " + op);
        }
        } catch (PluginError e) {
            throw e;
        } catch (JSONException e) {
            throw new PluginError(1011, "构造响应失败：" + e.getMessage());
        } catch (Exception e) {
            throw new PluginError(1011, "插件内部错误：" + e.getMessage());
        }
    }

    // ------------------------------------------------------------ 搜索

    private JSONObject search(JSONObject params) throws PluginError, JSONException {
        String keyword = params.optString("keyword", params.optString("q", "")).trim();
        if (keyword.isEmpty()) throw new PluginError(1007, "搜索关键字不能为空");
        int page = Math.max(1, params.optInt("page", 1));
        int pageSize = Math.min(50, Math.max(1, params.optInt("page_size", 20)));

        Map<String, String> query = new HashMap<>();
        query.put("s", keyword);
        query.put("type", "1");
        query.put("limit", String.valueOf(pageSize));
        query.put("offset", String.valueOf((page - 1) * pageSize));
        JSONObject payload = get("/api/search/get/web", query);

        JSONObject result = payload.optJSONObject("result");
        JSONArray songs = result == null ? null : result.optJSONArray("songs");
        JSONArray items = new JSONArray();
        if (songs != null) {
            for (int i = 0; i < songs.length(); i++) {
                JSONObject song = songs.optJSONObject(i);
                if (song != null) items.put(itemOf(song));
            }
        }
        JSONObject out = new JSONObject();
        int total = result == null ? items.length() : result.optInt("songCount", items.length());
        try {
            out.put("items", items);
            out.put("keyword", keyword);
            out.put("page", page);
            out.put("page_size", pageSize);
            out.put("total", total);
            out.put("has_more", page * pageSize < total);
        } catch (Exception ignored) {
            // 字段写入失败不影响返回
        }
        return out;
    }

    private JSONObject itemOf(JSONObject song) {
        JSONObject item = new JSONObject();
        try {
            String id = song.optString("id", "");
            String name = song.optString("name", "");
            String artist = artistOf(song);
            JSONObject album = song.optJSONObject("album");
            String albumName = album == null ? "" : album.optString("name", "");
            String pic = album == null ? "" : album.optString("picUrl", "");
            int duration = Math.round(song.optLong("duration", 0) / 1000f);

            item.put("platform", "netease");
            item.put("id", id);
            item.put("item_id", id);
            item.put("sha", BiliSource.sha256Hex("netease:" + id));
            item.put("name", name);
            item.put("title", name);
            JSONArray artists = new JSONArray();
            if (!artist.isEmpty()) artists.put(artist);
            item.put("artists", artists);
            item.put("artist", artist);
            item.put("author", artist);
            item.put("album", albumName);
            item.put("duration", duration);
            item.put("duration_seconds", duration);
            item.put("pic", pic);
            item.put("cover", pic);
            item.put("available", true);
            item.put("reason", "");
            item.put("quality", qualityJson("standard"));
            JSONArray qualities = new JSONArray();
            qualities.put(qualityJson("standard"));
            item.put("qualities", qualities);
        } catch (Exception ignored) {
            // 单个条目失败不影响整体
        }
        return item;
    }

    private static String artistOf(JSONObject song) {
        JSONArray artists = song.optJSONArray("artists");
        if (artists == null) artists = song.optJSONArray("ar");
        if (artists == null || artists.length() == 0) return "";
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < artists.length(); i++) {
            JSONObject entry = artists.optJSONObject(i);
            if (entry == null) continue;
            if (sb.length() > 0) sb.append(" / ");
            sb.append(entry.optString("name", ""));
        }
        return sb.toString();
    }

    // ------------------------------------------------------------ 详情

    private JSONObject detail(String id) throws PluginError, JSONException {
        Map<String, String> query = new HashMap<>();
        query.put("ids", "[" + id + "]");
        JSONObject payload = get("/api/song/detail", query);
        JSONArray songs = payload.optJSONArray("songs");
        if (songs == null || songs.length() == 0) throw new PluginError(1004, "歌曲不存在: " + id);
        JSONObject song = songs.optJSONObject(0);
        if (song == null) throw new PluginError(1004, "歌曲不存在: " + id);
        return song;
    }

    private JSONObject getSong(JSONObject params) throws PluginError, JSONException {
        String id = idOf(params);
        JSONObject song = detail(id);
        String name = song.optString("name", "");
        String artist = artistOf(song);
        JSONObject album = song.optJSONObject("al");
        String albumName = album == null ? "" : album.optString("name", "");
        String pic = album == null ? "" : album.optString("picUrl", "");
        int duration = Math.round(song.optLong("dt", 0) / 1000f);

        JSONObject out = new JSONObject();
        try {
            out.put("platform", "netease");
            out.put("version", "1.0.0");
            out.put("location", "");
            out.put("sha", BiliSource.sha256Hex("netease:" + id));
            out.put("name", name);
            out.put("title", name);
            out.put("author", artist);
            out.put("pic", pic);
            out.put("secret", 0);
            out.put("duration", duration);
            out.put("duration_seconds", duration);
            out.put("album", albumName);
            out.put("item_id", id);
            out.put("quality", qualityJson("standard"));
            out.put("available", true);
            out.put("reason", "");
        } catch (Exception ignored) {
            // 忽略
        }
        return out;
    }

    // ------------------------------------------------------------ 播放地址

    private JSONObject getUrl(JSONObject params) throws PluginError, JSONException {
        String id = idOf(params);
        // 官方外链会 302 到 CDN；先把最终地址解出来，再由插件代理（避免 Referer / 防盗链问题）
        String outer = API + "/song/media/outer/url?id=" + BiliSource.encode(id) + ".mp3";
        String resolved = resolve(outer);
        if (resolved == null || resolved.isEmpty()) {
            throw new PluginError(1021, "网易云没有返回可用的播放地址（可能是 VIP / 版权受限曲目）");
        }
        String proxied = "http://127.0.0.1:" + port + "/media?u=" + BiliSource.base64Url(resolved) + "&k=audio";
        JSONObject out = new JSONObject();
        try {
            out.put("url", proxied);
            out.put("location", proxied);
            out.put("quality", qualityJson("standard"));
            out.put("quality_id", "standard");
            out.put("mime", "audio/mpeg");
            out.put("codec", "mp3");
            out.put("audio_only", true);
            out.put("note", "Java 版插件未接入登录，仅标准音质");
        } catch (Exception ignored) {
            // 忽略
        }
        return out;
    }

    /** 跟随一次重定向拿最终地址；不下载内容 */
    private String resolve(String url) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setInstanceFollowRedirects(false);
            conn.setConnectTimeout(Http.TIMEOUT);
            conn.setReadTimeout(Http.TIMEOUT);
            conn.setRequestMethod("GET");
            for (Map.Entry<String, String> entry : headers().entrySet()) {
                conn.setRequestProperty(entry.getKey(), entry.getValue());
            }
            final int code = conn.getResponseCode();
            if (code >= 300 && code < 400) {
                String location = conn.getHeaderField("Location");
                if (location != null && location.startsWith("http")) return location;
            }
            if (code == 200) return url;
            return "";
        } catch (IOException e) {
            return "";
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    private JSONObject getQualities(JSONObject params) throws PluginError, JSONException {
        JSONObject out = new JSONObject();
        JSONArray all = new JSONArray();
        all.put(qualityJson("standard"));
        try {
            out.put("id", idOf(params));
            out.put("platform", "netease");
            out.put("qualities", all);
            out.put("available_qualities", all);
            out.put("available", true);
            out.put("max_quality", "standard");
        } catch (Exception ignored) {
            // 忽略
        }
        return out;
    }

    // ------------------------------------------------------------ 歌词

    private JSONObject getLyric(JSONObject params) throws PluginError, JSONException {
        String id = idOf(params);
        Map<String, String> query = new HashMap<>();
        query.put("id", id);
        query.put("lv", "1");
        query.put("kv", "1");
        query.put("tv", "-1");
        JSONObject payload = get("/api/song/lyric", query);

        JSONObject lrc = payload.optJSONObject("lrc");
        String lyricText = lrc == null ? "" : lrc.optString("lyric", "");
        if (lyricText.trim().isEmpty()) throw new PluginError(1015, "该歌曲没有歌词");

        JSONArray lines = new JSONArray();
        Map<Long, String> translated = new HashMap<>();
        JSONObject tlyric = payload.optJSONObject("tlyric");
        if (tlyric != null) {
            for (String[] pair : parseLrc(tlyric.optString("lyric", ""))) {
                try {
                    translated.put(Long.parseLong(pair[0]), pair[1]);
                } catch (NumberFormatException ignored) {
                    // 忽略坏行
                }
            }
        }
        for (String[] pair : parseLrc(lyricText)) {
            try {
                long time = Long.parseLong(pair[0]);
                JSONObject line = new JSONObject();
                line.put("time_ms", time);
                line.put("text", pair[1]);
                String tr = translated.get(time);
                if (tr != null && !tr.isEmpty()) line.put("tr", tr);
                lines.put(line);
            } catch (Exception ignored) {
                // 忽略坏行
            }
        }
        if (lines.length() == 0) throw new PluginError(1015, "歌词内容为空");

        JSONObject out = new JSONObject();
        try {
            out.put("source", "netease");
            out.put("format", "lrc");
            out.put("timed", true);
            out.put("offset_ms", 0);
            out.put("lines", lines);
            out.put("lrc", lyricText);
        } catch (Exception ignored) {
            // 忽略
        }
        return out;
    }

    /** 解析 LRC 的 [mm:ss.xxx] 行 → {毫秒, 文本} */
    private static java.util.List<String[]> parseLrc(String text) {
        java.util.List<String[]> out = new java.util.ArrayList<>();
        if (text == null || text.isEmpty()) return out;
        for (String raw : text.split("\n")) {
            String line = raw.trim();
            if (!line.startsWith("[")) continue;
            int close = line.indexOf(']');
            if (close <= 0) continue;
            String stamp = line.substring(1, close);
            String content = line.substring(close + 1).trim();
            String[] parts = stamp.split(":");
            if (parts.length < 2) continue;
            try {
                long minutes = Long.parseLong(parts[0].trim());
                double seconds = Double.parseDouble(parts[1].trim());
                out.add(new String[]{String.valueOf((long) (minutes * 60000 + seconds * 1000)), content});
            } catch (NumberFormatException ignored) {
                // 跳过元信息行（[ti:] / [ar:] 等）
            }
        }
        return out;
    }

    private JSONObject getPic(JSONObject params) throws PluginError, JSONException {
        JSONObject song = detail(idOf(params));
        JSONObject album = song.optJSONObject("al");
        String pic = album == null ? "" : album.optString("picUrl", "");
        JSONObject out = new JSONObject();
        try {
            out.put("platform", "netease");
            out.put("id", idOf(params));
            out.put("pic", pic);
            out.put("url", pic);
        } catch (Exception ignored) {
            // 忽略
        }
        return out;
    }

    private JSONObject checkLogin() throws JSONException {
        JSONObject out = new JSONObject();
        out.put("platform", "netease");
        boolean loggedIn = false;
        try {
            JSONObject data = get("/api/nuser/account/get", null);
            JSONObject profile = data.optJSONObject("profile");
            loggedIn = profile != null && profile.optLong("userId", 0) > 0;
            out.put("logged_in", loggedIn);
            if (loggedIn) {
                out.put("nickname", profile.optString("nickname", ""));
                out.put("user_id", String.valueOf(profile.optLong("userId", 0)));
                out.put("avatar", profile.optString("avatarUrl", ""));
                out.put("vip", profile.optInt("vipType", 0) != 0);
                out.put("vip_label", profile.optInt("vipType", 0) != 0 ? "黑胶 VIP" : "普通用户");
            } else {
                out.put("vip", false);
                out.put("vip_label", "未登录（标准音质可直接使用）");
            }
        } catch (PluginError e) {
            out.put("logged_in", false);
            out.put("vip", false);
            out.put("vip_label", "未登录");
            out.put("note", e.getMessage());
        }
        out.put("cookie_configured", !cookie().isEmpty());
        return out;
    }

    private JSONObject loginCookie(JSONObject params) throws JSONException, PluginError {
        String text = params.optString("cookie", params.optString("music_u", "")).trim();
        if (text.isEmpty()) throw new PluginError(1007, "Cookie 不能为空（至少需要 MUSIC_U）");
        if (!text.contains("=")) text = "MUSIC_U=" + text;
        if (!text.contains("MUSIC_U=")) throw new PluginError(1007, "Cookie 里没有 MUSIC_U，无法登录");
        final String previous = cookie();
        saveCookie(text);
        JSONObject state = checkLogin();
        if (!state.optBoolean("logged_in", false)) {
            saveCookie(previous);
            throw new PluginError(1003, "Cookie 无效或已过期（account/get 返回未登录）");
        }
        Http.log("网易云登录成功：" + state.optString("nickname", ""));
        return state;
    }

    private JSONObject logout() throws JSONException {
        saveCookie("");
        JSONObject out = new JSONObject();
        out.put("platform", "netease");
        out.put("logged_in", false);
        out.put("cookie_configured", false);
        return out;
    }

    private JSONObject settings() throws JSONException {
        JSONObject out = new JSONObject();
        out.put("platform", "netease");
        out.put("plugin_id", "netease.source");
        out.put("version", "1.3.0-java");
        out.put("endpoint", "http://127.0.0.1:" + port);
        out.put("login", checkLogin());
        out.put("values", new JSONObject());
        out.put("schema", new JSONArray());
        out.put("note", "Android 版插件：登录请粘贴 MUSIC_U Cookie；不含扫码 / 手机号登录");
        return out;
    }

    // ------------------------------------------------------------ 搜索（cloudsearch）

    /** 网易云统一搜索：type 1 单曲 / 10 专辑 / 1000 歌单 / 100 歌手 / 1004 MV */
    private JSONObject cloudSearch(String keyword, int type, int page, int pageSize) throws PluginError {
        Map<String, String> form = new HashMap<>();
        form.put("s", keyword);
        form.put("type", String.valueOf(type));
        form.put("limit", String.valueOf(pageSize));
        form.put("offset", String.valueOf(Math.max(0, (page - 1) * pageSize)));
        try {
            return Http.postJson(API + "/api/cloudsearch/pc", form, headers());
        } catch (Http.HttpError e) {
            throw new PluginError(1011, "网易云搜索失败：HTTP " + e.code);
        } catch (IOException e) {
            throw new PluginError(1011, "无法访问网易云：" + e.getMessage());
        }
    }

    private static int pageOf(JSONObject params) {
        return Math.max(1, params.optInt("page", 1));
    }

    private static int pageSizeOf(JSONObject params, int fallback) {
        return Math.min(50, Math.max(1, params.optInt("page_size", fallback)));
    }

    private static String keywordOf(JSONObject params) throws PluginError {
        String keyword = params.optString("keyword", params.optString("q", "")).trim();
        if (keyword.isEmpty()) throw new PluginError(1007, "搜索关键字不能为空");
        return keyword;
    }

    private JSONObject searchPlaylist(JSONObject params) throws PluginError, JSONException {
        String keyword = keywordOf(params);
        int page = pageOf(params);
        int size = pageSizeOf(params, 20);
        JSONObject payload = cloudSearch(keyword, 1000, page, size);
        JSONObject result = payload.optJSONObject("result");
        JSONArray raw = result == null ? null : result.optJSONArray("playlists");
        JSONArray items = new JSONArray();
        if (raw != null) {
            for (int i = 0; i < raw.length(); i++) {
                JSONObject entry = raw.optJSONObject(i);
                if (entry == null) continue;
                String id = entry.optString("id", "");
                if (id.isEmpty()) continue;
                JSONObject creator = entry.optJSONObject("creator");
                JSONObject item = new JSONObject();
                item.put("platform", "netease");
                item.put("kind", "playlist");
                item.put("id", id);
                item.put("item_id", id);
                item.put("name", entry.optString("name", ""));
                item.put("creator", creator == null ? "" : creator.optString("nickname", ""));
                item.put("creator_id", creator == null ? "" : String.valueOf(creator.optLong("userId", 0)));
                item.put("pic", entry.optString("coverImgUrl", ""));
                item.put("track_count", entry.optInt("trackCount", 0));
                item.put("play_count", entry.optLong("playCount", 0));
                items.put(item);
            }
        }
        JSONObject out = new JSONObject();
        out.put("items", items);
        out.put("keyword", keyword);
        out.put("page", page);
        out.put("page_size", size);
        out.put("total", result == null ? items.length() : result.optInt("playlistCount", items.length()));
        out.put("has_more", result != null && result.optBoolean("hasMore", false));
        out.put("type", "playlist");
        return out;
    }

    private JSONObject searchAlbum(JSONObject params) throws PluginError, JSONException {
        String keyword = keywordOf(params);
        int page = pageOf(params);
        int size = pageSizeOf(params, 20);
        JSONObject payload = cloudSearch(keyword, 10, page, size);
        JSONObject result = payload.optJSONObject("result");
        JSONArray raw = result == null ? null : result.optJSONArray("albums");
        JSONArray items = new JSONArray();
        if (raw != null) {
            for (int i = 0; i < raw.length(); i++) {
                JSONObject entry = raw.optJSONObject(i);
                if (entry == null) continue;
                String id = entry.optString("id", "");
                if (id.isEmpty()) continue;
                JSONObject item = new JSONObject();
                item.put("platform", "netease");
                item.put("kind", "album");
                item.put("id", id);
                item.put("item_id", id);
                item.put("name", entry.optString("name", ""));
                item.put("artist", artistOf(entry));
                item.put("pic", entry.optString("picUrl", ""));
                int size2 = entry.optInt("size", 0);
                item.put("size", size2);
                item.put("track_count", size2);
                long publish = entry.optLong("publishTime", 0);
                item.put("publish_time", publish);
                item.put("year", publish > 0 ? new java.text.SimpleDateFormat("yyyy")
                        .format(new java.util.Date(publish)) : "0");
                items.put(item);
            }
        }
        JSONObject out = new JSONObject();
        out.put("items", items);
        out.put("keyword", keyword);
        out.put("page", page);
        out.put("page_size", size);
        out.put("total", result == null ? items.length() : result.optInt("albumCount", items.length()));
        out.put("has_more", result != null && result.optBoolean("hasMore", false));
        out.put("type", "album");
        return out;
    }

    private JSONObject searchArtist(JSONObject params) throws PluginError, JSONException {
        String keyword = keywordOf(params);
        int page = pageOf(params);
        int size = pageSizeOf(params, 20);
        JSONObject payload = cloudSearch(keyword, 100, page, size);
        JSONObject result = payload.optJSONObject("result");
        JSONArray raw = result == null ? null : result.optJSONArray("artists");
        JSONArray artists = new JSONArray();
        if (raw != null) {
            for (int i = 0; i < raw.length(); i++) {
                JSONObject entry = raw.optJSONObject(i);
                if (entry == null) continue;
                String id = entry.optString("id", "");
                if (id.isEmpty()) continue;
                JSONObject artist = new JSONObject();
                artist.put("id", id);
                artist.put("name", entry.optString("name", ""));
                String pic = entry.optString("picUrl", entry.optString("img1v1Url", ""));
                artist.put("avatar", pic);
                artist.put("pic", pic);
                artist.put("alias", entry.optJSONArray("alias") == null ? new JSONArray()
                        : entry.optJSONArray("alias"));
                artist.put("platform", "netease");
                artist.put("fans", 0);
                artists.put(artist);
            }
        }
        JSONObject out = new JSONObject();
        out.put("artists", artists);
        out.put("keyword", keyword);
        out.put("page", page);
        out.put("page_size", size);
        out.put("total", result == null ? artists.length() : result.optInt("artistCount", artists.length()));
        return out;
    }

    private JSONObject searchMv(JSONObject params) throws PluginError, JSONException {
        String keyword = keywordOf(params);
        int page = pageOf(params);
        int size = pageSizeOf(params, 20);
        JSONObject payload = cloudSearch(keyword, 1004, page, size);
        JSONObject result = payload.optJSONObject("result");
        JSONArray raw = result == null ? null : result.optJSONArray("mvs");
        JSONArray mvs = new JSONArray();
        if (raw != null) {
            for (int i = 0; i < raw.length(); i++) {
                JSONObject entry = raw.optJSONObject(i);
                if (entry == null) continue;
                String id = entry.optString("id", "");
                if (id.isEmpty()) continue;
                JSONObject mv = new JSONObject();
                mv.put("platform", "netease");
                mv.put("id", id);
                mv.put("item_id", id);
                mv.put("sha", BiliSource.sha256Hex("netease:mv:" + id));
                mv.put("title", entry.optString("name", ""));
                mv.put("name", entry.optString("name", ""));
                mv.put("artist", entry.optString("artistName", artistOf(entry)));
                mv.put("author", entry.optString("artistName", ""));
                mv.put("cover", entry.optString("cover", ""));
                mv.put("pic", entry.optString("cover", ""));
                mv.put("duration", entry.optLong("duration", 0) / 1000);
                mv.put("duration_seconds", entry.optLong("duration", 0) / 1000);
                mv.put("play_count", entry.optLong("playCount", 0));
                mvs.put(mv);
            }
        }
        JSONObject out = new JSONObject();
        out.put("mvs", mvs);
        out.put("items", mvs);
        out.put("total", result == null ? mvs.length() : result.optInt("mvCount", mvs.length()));
        out.put("keyword", keyword);
        out.put("page", page);
        out.put("page_size", size);
        return out;
    }

    // ------------------------------------------------------------ 详情

    private JSONObject trackOf(JSONObject song, String platform) throws JSONException {
        JSONObject track = new JSONObject();
        String id = song.optString("id", "");
        String name = song.optString("name", "");
        JSONObject album = song.optJSONObject("al");
        if (album == null) album = song.optJSONObject("album");
        track.put("platform", platform);
        track.put("item_id", id);
        track.put("sha", BiliSource.sha256Hex("netease:" + id));
        track.put("name", name);
        track.put("author", artistOf(song));
        track.put("album", album == null ? "" : album.optString("name", ""));
        track.put("pic", album == null ? "" : album.optString("picUrl", ""));
        long duration = song.optLong("dt", song.optLong("duration", 0));
        track.put("duration", Math.round(duration / 1000f));
        track.put("group_key", "");
        return track;
    }

    private JSONObject getPlaylist(JSONObject params) throws PluginError, JSONException {
        String id = idOf(params);
        Map<String, String> query = new HashMap<>();
        query.put("id", id);
        query.put("n", "1000");
        JSONObject data = get("/api/v6/playlist/detail", query);
        JSONObject playlist = data.optJSONObject("playlist");
        if (playlist == null) throw new PluginError(1004, "歌单不存在: " + id);
        JSONArray rawTracks = playlist.optJSONArray("tracks");
        JSONArray tracks = new JSONArray();
        if (rawTracks != null) {
            for (int i = 0; i < rawTracks.length(); i++) {
                JSONObject song = rawTracks.optJSONObject(i);
                if (song != null) tracks.put(trackOf(song, "netease"));
            }
        }
        JSONObject creator = playlist.optJSONObject("creator");
        JSONObject out = new JSONObject();
        out.put("platform", "netease");
        out.put("id", id);
        out.put("name", playlist.optString("name", ""));
        out.put("creator", creator == null ? "" : creator.optString("nickname", ""));
        out.put("pic", playlist.optString("coverImgUrl", ""));
        out.put("total", playlist.optInt("trackCount", tracks.length()));
        out.put("tracks", tracks);
        return out;
    }

    private JSONObject getAlbum(JSONObject params) throws PluginError, JSONException {
        String id = idOf(params);
        JSONObject data = get("/api/v1/album/" + id, null);
        JSONObject album = data.optJSONObject("album");
        if (album == null) throw new PluginError(1004, "专辑不存在: " + id);
        JSONArray rawTracks = data.optJSONArray("songs");
        JSONArray tracks = new JSONArray();
        if (rawTracks != null) {
            for (int i = 0; i < rawTracks.length(); i++) {
                JSONObject song = rawTracks.optJSONObject(i);
                if (song == null) continue;
                // /api/v1/album 的单曲字段是扁平结构（artists / album），补一层适配
                JSONObject track = new JSONObject();
                String sid = song.optString("id", "");
                track.put("platform", "netease");
                track.put("item_id", sid);
                track.put("sha", BiliSource.sha256Hex("netease:" + sid));
                track.put("name", song.optString("name", ""));
                track.put("author", artistOf(song));
                track.put("album", album.optString("name", ""));
                track.put("pic", album.optString("picUrl", ""));
                long duration = song.optLong("dt", song.optLong("duration", 0));
                track.put("duration", Math.round(duration / 1000f));
                track.put("group_key", "");
                tracks.put(track);
            }
        }
        JSONObject out = new JSONObject();
        out.put("platform", "netease");
        out.put("id", id);
        out.put("name", album.optString("name", ""));
        out.put("artist", artistOf(album));
        out.put("pic", album.optString("picUrl", ""));
        out.put("publish_time", album.optLong("publishTime", 0));
        out.put("total", tracks.length());
        out.put("tracks", tracks);
        return out;
    }

    private JSONObject getArtist(JSONObject params) throws PluginError, JSONException {
        String id = idOf(params);
        JSONObject top = get("/api/artist/top/song", single("id", id));
        JSONArray songs = top.optJSONArray("songs");
        JSONArray hot = new JSONArray();
        String name = "";
        if (songs != null) {
            for (int i = 0; i < songs.length(); i++) {
                JSONObject song = songs.optJSONObject(i);
                if (song == null) continue;
                JSONArray ar = song.optJSONArray("ar");
                if (name.isEmpty() && ar != null && ar.length() > 0) {
                    JSONObject first = ar.optJSONObject(0);
                    if (first != null) name = first.optString("name", "");
                }
                JSONObject item = new JSONObject();
                String sid = song.optString("id", "");
                item.put("platform", "netease");
                item.put("item_id", sid);
                item.put("id", sid);
                item.put("sha", BiliSource.sha256Hex("netease:" + sid));
                item.put("name", song.optString("name", ""));
                item.put("author", artistOf(song));
                JSONObject al = song.optJSONObject("al");
                item.put("album", al == null ? "" : al.optString("name", ""));
                long duration = song.optLong("dt", 0);
                item.put("duration", Math.round(duration / 1000f));
                item.put("duration_seconds", Math.round(duration / 1000f));
                item.put("pic", al == null ? "" : al.optString("picUrl", ""));
                item.put("artist_id", id);
                item.put("available", true);
                item.put("reason", "");
                item.put("quality", qualityJson("standard"));
                JSONArray qualities = new JSONArray();
                qualities.put(qualityJson("standard"));
                item.put("qualities", qualities);
                hot.put(item);
            }
        }
        // 资料：用列表页拿到首个歌手名 + 头像（公开接口没有单独的资料接口）
        JSONObject out = new JSONObject();
        out.put("platform", "netease");
        out.put("source_id", "netease:" + id);
        out.put("id", id);
        out.put("name", name);
        out.put("avatar", "");
        out.put("pic", "");
        out.put("alias", new JSONArray());
        out.put("brief_desc", "");
        out.put("album_count", 0);
        out.put("music_count", hot.length());
        out.put("mv_count", 0);
        out.put("hot_songs", hot);
        return out;
    }

    private JSONObject getMv(JSONObject params) throws PluginError, JSONException {
        String id = idOf(params);
        JSONObject data = get("/api/mv/detail", single("id", id));
        JSONObject mv = data.optJSONObject("data");
        if (mv == null) throw new PluginError(1004, "MV 不存在: " + id);
        // 播放地址需要登录态；未登录时只有封面和元信息
        String url = "";
        try {
            JSONObject play = get("/api/song/enhance/player/url/v1",
                    Http.headers("id", id, "level", "standard", "encodeType", "flac"));
            JSONArray list = play.optJSONArray("data");
            if (list != null && list.length() > 0) {
                JSONObject first = list.optJSONObject(0);
                if (first != null) url = first.optString("url", "");
            }
        } catch (PluginError ignored) {
            // 拿不到地址不影响返回元信息
        }
        if (!url.isEmpty()) {
            url = "http://127.0.0.1:" + port + "/media?u=" + BiliSource.base64Url(url) + "&k=video";
        } else {
            throw new PluginError(1021, "网易云 MV 播放地址需要登录（请在设置里粘贴 MUSIC_U Cookie）");
        }
        JSONObject out = new JSONObject();
        out.put("platform", "netease");
        out.put("id", id);
        out.put("item_id", id);
        out.put("sha", BiliSource.sha256Hex("netease:mv:" + id));
        out.put("title", mv.optString("name", ""));
        out.put("artist", mv.optString("artistName", ""));
        out.put("album", "");
        out.put("pic", mv.optString("cover", ""));
        out.put("cover", mv.optString("cover", ""));
        out.put("url", url);
        out.put("location", url);
        out.put("duration", mv.optLong("duration", 0) / 1000);
        out.put("duration_seconds", mv.optLong("duration", 0) / 1000);
        return out;
    }

    private static Map<String, String> single(String key, String value) {
        Map<String, String> query = new HashMap<>();
        query.put(key, value);
        return query;
    }

    // ------------------------------------------------------------ 工具

    private static String idOf(JSONObject params) throws PluginError {
        String id = params.optString("id", "");
        if (id.isEmpty()) id = params.optString("item_id", "");
        if (id.isEmpty()) throw new PluginError(1007, "缺少曲目 id");
        return id;
    }

    private static JSONObject qualityJson(String id) throws JSONException {
        JSONObject spec = new JSONObject();
        try {
            spec.put("id", id).put("quality_id", id).put("label", "标准").put("level", "low")
                .put("codec", "mp3").put("lossless", false).put("bitrate", 128)
                .put("sample_rate", 44100).put("channels", 2);
        } catch (Exception ignored) {
            // 忽略
        }
        return spec;
    }
}
