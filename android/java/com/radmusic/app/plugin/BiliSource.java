package com.radmusic.app.plugin;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.UnsupportedEncodingException;
import java.net.URLEncoder;
import java.nio.charset.Charset;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * 哔哩哔哩音源（Java 版）。
 *
 * 为什么重写而不是带 Python 插件：Android 上没有 Python 运行时，
 * 而音源插件是宿主通过 HTTP JSON-RPC 调用的独立进程 —— 这里把「独立进程」换成
 * app 内的一个线程，协议完全不变，所以 C++ 服务端一行都不用改。
 *
 * 覆盖宿主实际用到的 op：search / get_song / get_url / get_qualities / get_lyric / get_pic。
 * 接口与 plugins/bilibili/main.py 保持一致（同样的字段名与错误码）。
 */
final class BiliSource implements Source {

    private static final String API = "https://api.bilibili.com";
    private static final String REFERER = "https://www.bilibili.com/";
    private static final String UA =
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                    + "Chrome/120.0.0.0 Safari/537.36";

    /** wbi 重排表（bilibili 前端写死的 64 位索引） */
    private static final int[] MIXIN_TAB = {
            46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35, 27, 43, 5, 49,
            33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13, 37, 48, 7, 16, 24, 55, 40, 61,
            26, 17, 0, 1, 60, 51, 30, 4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36,
            20, 34, 44, 52,
    };

    /** DASH 音轨 id → 宿主音质 id */
    private static final Map<Integer, String> AUDIO_QUALITY = new HashMap<>();
    static {
        AUDIO_QUALITY.put(30251, "hires");
        AUDIO_QUALITY.put(30250, "dolby");
        AUDIO_QUALITY.put(30280, "a192");
        AUDIO_QUALITY.put(30232, "a132");
        AUDIO_QUALITY.put(30216, "a64");
    }

    private String mixinKey = "";
    private long mixinStamp = 0L;
    private String buvid = "";
    private final int port;
    /** Cookie 落盘位置（Android 上插件目录只读，所以放在 app 私有目录） */
    private final java.io.File cookieFile;
    private String cookie = "";
    private boolean cookieLoaded = false;

    BiliSource(int port, java.io.File cookieFile) {
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
                Http.log("读取 bilibili Cookie 失败：" + e.getMessage());
            }
        }
        return cookie;
    }

    private void saveCookie(String text) {
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
            Http.log("保存 bilibili Cookie 失败：" + e.getMessage());
        }
    }

    @Override
    public String platform() {
        return "bilibili";
    }

    // ------------------------------------------------------------ 请求基础设施

    private Map<String, String> headers() {
        Map<String, String> h = Http.headers("User-Agent", UA, "Referer", REFERER, "Accept", "application/json");
        String stored = cookie();
        if (!stored.isEmpty()) {
            h.put("Cookie", buvid.isEmpty() ? stored : stored + "; " + buvid);
        } else if (!buvid.isEmpty()) {
            h.put("Cookie", buvid);
        }
        return h;
    }

    private void ensureBuvid() {
        if (!buvid.isEmpty()) return;
        try {
            JSONObject data = api("/x/frontend/finger/spi", null, false);
            String b3 = data.optString("b_3", "");
            String b4 = data.optString("b_4", "");
            if (!b3.isEmpty()) buvid = "buvid3=" + b3 + (b4.isEmpty() ? "" : "; buvid4=" + b4);
        } catch (Exception e) {
            Http.log("获取 buvid 失败：" + e.getMessage());
        }
    }

    /** 调 bilibili API，返回 data 字段；code != 0 抛 PluginError */
    private JSONObject api(String path, Map<String, String> params, boolean sign) throws PluginError {
        StringBuilder url = new StringBuilder(API).append(path);
        if (params != null && !params.isEmpty()) {
            String query = sign ? wbiSign(params) : plainQuery(params);
            url.append(path.contains("?") ? "&" : "?").append(query);
        }
        try {
            JSONObject payload = Http.getJson(url.toString(), headers());
            int code = payload.optInt("code", 0);
            if (code == -403 && sign) {
                mixinKey = "";   // 签名失效，刷新一次再试
                String query = wbiSign(params);
                StringBuilder retry = new StringBuilder(API).append(path);
                retry.append(path.contains("?") ? "&" : "?").append(query);
                payload = Http.getJson(retry.toString(), headers());
                code = payload.optInt("code", 0);
            }
            if (code != 0) throw new PluginError(mapCode(code), businessMessage(code));
            JSONObject data = payload.optJSONObject("data");
            return data != null ? data : new JSONObject();
        } catch (Http.HttpError e) {
            if (e.code == 412) throw new PluginError(1014, "被 bilibili 风控拦截（412），请稍后重试");
            if (e.code == 404) throw new PluginError(1004, "资源不存在（404）");
            throw new PluginError(1011, "请求失败：HTTP " + e.code);
        } catch (PluginError e) {
            throw e;
        } catch (Exception e) {
            throw new PluginError(1011, "无法访问 bilibili：" + e.getMessage());
        }
    }

    private static int mapCode(int code) {
        if (code == -101) return 1014;
        if (code == -352 || code == -799 || code == -509) return 1014;
        if (code == -404 || code == 62002) return 1004;
        if (code == 87007 || code == 87008) return 1021;
        return 1011;
    }

    private static String businessMessage(int code) {
        switch (code) {
            case -101: return "该内容需要登录 bilibili";
            case -352: return "触发 bilibili 风控（-352），请稍后重试";
            case -799: return "请求过于频繁（-799）";
            case -404: return "视频不存在或已失效";
            case 62002: return "稿件不可见";
            case 87007:
            case 87008: return "该清晰度需要大会员";
            case -10403: return "该视频存在地区限制";
            default: return "bilibili 返回错误码 " + code;
        }
    }

    // ------------------------------------------------------------ wbi 签名

    private String loadMixinKey() throws PluginError {
        if (!mixinKey.isEmpty() && System.currentTimeMillis() - mixinStamp < 900_000L) return mixinKey;
        JSONObject data = api("/x/web-interface/nav", null, false);
        JSONObject images = data.optJSONObject("wbi_img");
        String img = stem(images == null ? "" : images.optString("img_url", ""));
        String sub = stem(images == null ? "" : images.optString("sub_url", ""));
        if (img.isEmpty() || sub.isEmpty()) throw new PluginError(1011, "bilibili 未返回签名密钥（wbi_img 为空）");
        String origin = img + sub;
        StringBuilder mixin = new StringBuilder();
        for (int index : MIXIN_TAB) {
            if (index < origin.length()) mixin.append(origin.charAt(index));
        }
        mixinKey = mixin.length() > 32 ? mixin.substring(0, 32) : mixin.toString();
        mixinStamp = System.currentTimeMillis();
        return mixinKey;
    }

    private static String stem(String url) {
        if (url == null || url.isEmpty()) return "";
        String tail = url.substring(url.lastIndexOf('/') + 1);
        int dot = tail.indexOf('.');
        return dot >= 0 ? tail.substring(0, dot) : tail;
    }

    private String wbiSign(Map<String, String> params) throws PluginError {
        TreeMap<String, String> sorted = new TreeMap<>(params);
        sorted.put("wts", String.valueOf(System.currentTimeMillis() / 1000));
        String query = queryOf(sorted);
        // bilibili 的签名要求去掉这几个字符
        StringBuilder clean = new StringBuilder(query.length());
        for (int i = 0; i < query.length(); i++) {
            char ch = query.charAt(i);
            if (ch != '!' && ch != '\'' && ch != '(' && ch != ')' && ch != '*') clean.append(ch);
        }
        String text = clean.toString();
        return text + "&w_rid=" + md5(text + loadMixinKey());
    }

    private static String plainQuery(Map<String, String> params) {
        return queryOf(new TreeMap<>(params));
    }

    private static String queryOf(TreeMap<String, String> params) {
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, String> entry : params.entrySet()) {
            if (sb.length() > 0) sb.append('&');
            sb.append(encode(entry.getKey())).append('=').append(encode(entry.getValue()));
        }
        return sb.toString();
    }

    // ------------------------------------------------------------ 视频信息

    /** item_id 可能是 bvid，也可能是 "BV1xx_p3"（分 P） */
    private JSONObject video(String rawId) throws PluginError, JSONException {
        String bvid = "";
        int page = 1;
        if (rawId != null) {
            String text = rawId.trim();
            int underscore = text.lastIndexOf('_');
            if (underscore > 0) {
                try {
                    page = Math.max(1, Integer.parseInt(text.substring(underscore + 1)));
                    text = text.substring(0, underscore);
                } catch (NumberFormatException ignored) {
                    page = 1;
                }
            }
            bvid = text;
        }
        if (bvid.isEmpty()) throw new PluginError(1007, "缺少曲目 id");

        Map<String, String> params = new HashMap<>();
        params.put("bvid", bvid);
        JSONObject data = api("/x/web-interface/view", params, false);
        if (!data.has("bvid")) throw new PluginError(1004, "视频不存在: " + rawId);

        JSONArray pages = data.optJSONArray("pages");
        int count = pages == null ? 0 : pages.length();
        int index = count > 0 ? Math.min(Math.max(page, 1), count) - 1 : 0;
        JSONObject part = count > 0 ? pages.optJSONObject(index) : null;

        long cid = part != null ? part.optLong("cid", 0) : data.optLong("cid", 0);
        if (cid <= 0) throw new PluginError(1004, "视频缺少 cid，无法取播放地址");

        JSONObject owner = data.optJSONObject("owner");
        String title = stripTags(data.optString("title", ""));
        String partTitle = part != null ? stripTags(part.optString("part", "")) : "";
        String display = title;
        if (count > 1 && !partTitle.isEmpty() && !title.contains(partTitle)) {
            display = title + " - P" + (index + 1) + " " + partTitle;
        }

        JSONObject video = new JSONObject();
        video.put("bvid", data.optString("bvid"));
        video.put("aid", data.optLong("aid", 0));
        video.put("cid", cid);
        video.put("item_id", itemIdOf(data.optString("bvid"), index + 1));
        video.put("page", index + 1);
        video.put("page_count", count);
        video.put("title", title);
        video.put("display_title", display);
        video.put("author", owner == null ? "" : stripTags(owner.optString("name", "")));
        video.put("mid", owner == null ? 0 : owner.optLong("mid", 0));
        video.put("pic", normalizeCover(data.optString("pic", "")));
        video.put("duration", part != null ? part.optLong("duration", data.optLong("duration", 0))
                                           : data.optLong("duration", 0));
        video.put("pubdate", data.optLong("pubdate", 0));
        video.put("tname", data.optString("tname_v2", data.optString("tname", "")));
        int state = data.optInt("state", 0);
        video.put("state", state);
        video.put("available", state == 0);
        video.put("reason", state == 0 ? "" : "稿件当前不可见（state=" + state + "）");
        return video;
    }

    private static String itemIdOf(String bvid, int page) {
        return page > 1 ? bvid + "_p" + page : bvid;
    }

    // ------------------------------------------------------------ 音轨

    private JSONObject playurl(JSONObject video) throws PluginError {
        Map<String, String> params = new HashMap<>();
        params.put("bvid", video.optString("bvid"));
        params.put("cid", String.valueOf(video.optLong("cid")));
        params.put("fnval", "4048");
        params.put("fnver", "0");
        params.put("fourk", "1");
        ensureBuvid();
        try {
            return api("/x/player/wbi/playurl", params, true);
        } catch (PluginError e) {
            if (e.code != 1011 && e.code != 1014) throw e;
            return api("/x/player/playurl", params, false);
        }
    }

    /** DASH 音轨（含单独挂在 dash.flac / dash.dolby 下的 Hi-Res 与杜比） */
    private List<JSONObject> dashAudios(JSONObject video) throws PluginError {
        JSONObject data = playurl(video);
        JSONObject dash = data.optJSONObject("dash");
        List<JSONObject> entries = new ArrayList<>();
        if (dash == null) return entries;

        JSONArray audio = dash.optJSONArray("audio");
        if (audio != null) {
            for (int i = 0; i < audio.length(); i++) {
                JSONObject item = audio.optJSONObject(i);
                if (item != null) entries.add(item);
            }
        }
        // Hi-Res(30251) 在 dash.flac.audio，杜比(30250) 在 dash.dolby.audio
        for (String key : new String[]{"flac", "dolby"}) {
            JSONObject group = dash.optJSONObject(key);
            if (group == null) continue;
            Object inner = group.opt("audio");
            if (inner instanceof JSONObject) {
                entries.add((JSONObject) inner);
            } else if (inner instanceof JSONArray) {
                JSONArray list = (JSONArray) inner;
                for (int i = 0; i < list.length(); i++) {
                    JSONObject item = list.optJSONObject(i);
                    if (item != null) entries.add(item);
                }
            }
        }
        return entries;
    }

    private static String audioUrl(JSONObject entry) {
        String url = entry.optString("baseUrl", entry.optString("base_url", ""));
        if (!url.isEmpty()) return url;
        JSONArray backups = entry.optJSONArray("backupUrl");
        if (backups == null) backups = entry.optJSONArray("backup_url");
        if (backups != null && backups.length() > 0) return backups.optString(0, "");
        return "";
    }

    private static String qualityOf(JSONObject entry) {
        int id = entry.optInt("id", 0);
        String known = AUDIO_QUALITY.get(id);
        if (known != null) return known;
        long bandwidth = entry.optLong("bandwidth", 0);
        if (bandwidth >= 180_000) return "a192";
        if (bandwidth >= 90_000) return "a132";
        return "a64";
    }

    // ------------------------------------------------------------ 媒体代理

    /**
     * bilibili 的音频 CDN 校验 Referer，WebView 直连会 403，
     * 所以播放地址统一指向插件自己的 /media 端点，由插件带 Referer 转发。
     */
    private String proxyUrl(String upstream) {
        String encoded = base64Url(upstream);
        return "http://127.0.0.1:" + port + "/media?u=" + encoded + "&k=audio";
    }

    // ------------------------------------------------------------ op 实现

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
            case "get_fav_folders": return getFavFolders(params);
            case "search_artist": return searchArtist(params);
            case "get_artist": return getArtist(params);
            case "get_artist_mvs": return getArtistMvs(params);
            case "search_mv": return searchMv(params);
            case "get_mv": return getMv(params);
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

    private JSONObject search(JSONObject params) throws PluginError, JSONException {
        String keyword = params.optString("keyword", params.optString("q", "")).trim();
        if (keyword.isEmpty()) throw new PluginError(1007, "搜索关键字不能为空");
        int page = Math.max(1, params.optInt("page", 1));
        int pageSize = Math.min(50, Math.max(1, params.optInt("page_size", 20)));

        ensureBuvid();
        Map<String, String> query = new HashMap<>();
        query.put("search_type", "video");
        query.put("keyword", keyword);
        query.put("page", String.valueOf(page));
        query.put("page_size", String.valueOf(pageSize));
        JSONObject data = api("/x/web-interface/wbi/search/type", query, true);

        JSONArray raw = data.optJSONArray("result");
        JSONArray items = new JSONArray();
        if (raw != null) {
            for (int i = 0; i < raw.length(); i++) {
                JSONObject entry = raw.optJSONObject(i);
                if (entry == null) continue;
                String bvid = entry.optString("bvid", "").trim();
                if (!bvid.startsWith("BV")) continue;   // 课堂 / 专栏等非视频结果
                items.put(searchItem(entry, bvid));
            }
        }
        JSONObject result = new JSONObject();
        result.put("items", items);
        result.put("keyword", keyword);
        result.put("page", page);
        result.put("page_size", pageSize);
        result.put("total", data.optInt("numResults", items.length()));
        result.put("has_more", page < data.optInt("numPages", 0));
        return result;
    }

    private JSONObject searchItem(JSONObject raw, String bvid) throws JSONException {
        JSONObject item = new JSONObject();
        String title = stripTags(raw.optString("title", ""));
        String author = stripTags(raw.optString("author", ""));
        String pic = normalizeCover(raw.optString("pic", ""));
        int duration = parseDuration(raw.opt("duration"));
        item.put("platform", "bilibili");
        item.put("id", bvid);
        item.put("item_id", bvid);
        item.put("sha", sha256Hex("bilibili:" + bvid));
        item.put("name", title);
        item.put("title", title);
        JSONArray artists = new JSONArray();
        if (!author.isEmpty()) artists.put(author);
        item.put("artists", artists);
        item.put("artist", author);
        item.put("author", author);
        item.put("album", "");
        item.put("duration", duration);
        item.put("duration_seconds", duration);
        item.put("pic", pic);
        item.put("cover", pic);
        item.put("available", true);
        item.put("reason", "");
        item.put("artist_id", raw.optLong("mid", 0));
        item.put("aid", raw.optLong("aid", 0));
        item.put("quality", qualityJson("a192", null));
        JSONArray qualities = new JSONArray();
        qualities.put(qualityJson("a192", null));
        qualities.put(qualityJson("a132", null));
        qualities.put(qualityJson("a64", null));
        item.put("qualities", qualities);
        return item;
    }

    private JSONObject getSong(JSONObject params) throws PluginError, JSONException {
        return songBase(video(idOf(params)));
    }

    private static String idOf(JSONObject params) {
        String id = params.optString("id", "");
        if (id.isEmpty()) id = params.optString("item_id", "");
        return id;
    }

    private JSONObject songBase(JSONObject video) throws PluginError, JSONException {
        List<JSONObject> audios = new ArrayList<>();
        try {
            audios = dashAudios(video);
        } catch (PluginError e) {
            if (e.code == 1004 || e.code == 1021) throw e;
        }
        JSONObject best = qualityJson(audios.isEmpty() ? "a64" : qualityOf(audios.get(0)), null);

        JSONObject out = new JSONObject();
        out.put("platform", "bilibili");
        out.put("version", "1.0.0");
        out.put("location", "");
        out.put("sha", sha256Hex("bilibili:" + video.optString("item_id")));
        out.put("name", video.optString("display_title"));
        out.put("title", video.optString("display_title"));
        out.put("author", video.optString("author"));
        out.put("pic", video.optString("pic"));
        out.put("secret", 0);
        out.put("duration", video.optLong("duration"));
        out.put("duration_seconds", video.optLong("duration"));
        out.put("track_no", video.optInt("page", 1));
        out.put("quality", best);
        out.put("album", video.optString("tname"));
        out.put("item_id", video.optString("item_id"));
        out.put("bvid", video.optString("bvid"));
        out.put("aid", video.optLong("aid"));
        out.put("cid", video.optLong("cid"));
        out.put("page", video.optInt("page", 1));
        out.put("page_count", video.optInt("page_count", 1));
        out.put("pubdate", video.optLong("pubdate"));
        out.put("available", video.optBoolean("available", true));
        out.put("reason", video.optString("reason", ""));
        return out;
    }

    private JSONObject getUrl(JSONObject params) throws PluginError, JSONException {
        JSONObject video = video(idOf(params));
        if (!video.optBoolean("available", true)) {
            throw new PluginError(1004, video.optString("reason", "视频不可用"));
        }
        String requested = params.optString("quality", params.optString("quality_id", ""));

        List<JSONObject> audios = new ArrayList<>();
        try {
            audios = dashAudios(video);
        } catch (PluginError e) {
            if (e.code == 1004 || e.code == 1021) throw e;
        }
        if (!audios.isEmpty()) {
            JSONObject audio = pickAudio(audios, requested);
            if (audio == null) throw new PluginError(1021, "该视频没有可用音轨");
            String qualityId = qualityOf(audio);
            String upstream = audioUrl(audio);
            if (upstream.isEmpty()) throw new PluginError(1021, "该视频没有可用的音轨地址");
            JSONObject out = new JSONObject();
            out.put("url", proxyUrl(upstream));
            out.put("location", proxyUrl(upstream));
            out.put("quality", qualityJson(qualityId, audio));
            out.put("quality_id", qualityId);
            out.put("mime", "audio/mp4");
            out.put("codec", audio.optString("codecs", ""));
            out.put("bandwidth", audio.optLong("bandwidth", 0));
            out.put("audio_only", true);
            out.put("duration", video.optLong("duration"));
            out.put("duration_seconds", video.optLong("duration"));
            out.put("bvid", video.optString("bvid"));
            out.put("cid", video.optLong("cid"));
            out.put("page", video.optInt("page", 1));
            return out;
        }

        // 老视频只有混流 MP4：退回 durl，<audio> 同样能出声
        JSONObject data = api("/x/player/playurl", playurlParams(video, 1), false);
        JSONArray durl = data.optJSONArray("durl");
        if (durl == null || durl.length() == 0) throw new PluginError(1021, "该视频没有可用的音轨地址");
        JSONObject first = durl.optJSONObject(0);
        String upstream = first == null ? "" : first.optString("url", "");
        if (upstream.isEmpty() && first != null) {
            JSONArray backups = first.optJSONArray("backup_url");
            if (backups != null && backups.length() > 0) upstream = backups.optString(0, "");
        }
        if (upstream.isEmpty()) throw new PluginError(1021, "该视频没有可用的音轨地址");

        JSONObject out = new JSONObject();
        out.put("url", proxyUrl(upstream));
        out.put("location", proxyUrl(upstream));
        out.put("quality", qualityJson("a64", null));
        out.put("quality_id", "a64");
        out.put("audio_only", false);
        out.put("note", "该视频只有混流 MP4，已退回整段音视频");
        out.put("duration", video.optLong("duration"));
        out.put("duration_seconds", video.optLong("duration"));
        out.put("bvid", video.optString("bvid"));
        out.put("cid", video.optLong("cid"));
        return out;
    }

    private static Map<String, String> playurlParams(JSONObject video, int fnval) {
        Map<String, String> params = new HashMap<>();
        params.put("bvid", video.optString("bvid"));
        params.put("cid", String.valueOf(video.optLong("cid")));
        params.put("fnval", String.valueOf(fnval));
        params.put("fnver", "0");
        params.put("fourk", "1");
        return params;
    }

    /** 按请求音质挑音轨；请求为空或不可用时退回最高可用 */
    private static JSONObject pickAudio(List<JSONObject> audios, String requested) {
        if (audios.isEmpty()) return null;
        String wanted = normalizeQuality(requested);
        if (wanted != null) {
            for (JSONObject audio : audios) {
                if (wanted.equals(qualityOf(audio))) return audio;
            }
        }
        return audios.get(0);
    }

    private static String normalizeQuality(String value) {
        String text = value == null ? "" : value.trim().toLowerCase();
        if (text.isEmpty()) return null;
        if (text.contains("hires") || text.contains("hi-res")) return "hires";
        if (text.contains("dolby")) return "dolby";
        if (text.equals("a192") || text.contains("192")) return "a192";
        if (text.equals("a132") || text.contains("132")) return "a132";
        if (text.equals("a64") || text.contains("64")) return "a64";
        return null;
    }

    private JSONObject getQualities(JSONObject params) throws PluginError, JSONException {
        JSONObject video = video(idOf(params));
        List<JSONObject> audios = new ArrayList<>();
        try {
            audios = dashAudios(video);
        } catch (PluginError e) {
            if (e.code == 1004) throw e;
        }
        JSONArray available = new JSONArray();
        List<String> seen = new ArrayList<>();
        for (JSONObject audio : audios) {
            String id = qualityOf(audio);
            if (seen.contains(id)) continue;
            seen.add(id);
            available.put(qualityJson(id, audio));
        }
        JSONArray all = new JSONArray();
        for (String id : new String[]{"hires", "dolby", "a192", "a132", "a64"}) {
            all.put(qualityJson(id, null));
        }
        JSONObject out = new JSONObject();
        out.put("id", video.optString("item_id"));
        out.put("platform", "bilibili");
        out.put("qualities", all);
        out.put("available_qualities", available);
        out.put("available", video.optBoolean("available", true) && available.length() > 0);
        out.put("max_quality", available.length() > 0 ? available.optJSONObject(0).optString("quality_id") : "");
        out.put("bvid", video.optString("bvid"));
        out.put("cid", video.optLong("cid"));
        return out;
    }

    private JSONObject getLyric(JSONObject params) throws PluginError, JSONException {
        JSONObject video = video(idOf(params));
        Map<String, String> query = new HashMap<>();
        query.put("bvid", video.optString("bvid"));
        query.put("cid", String.valueOf(video.optLong("cid")));
        JSONObject data;
        try {
            data = api("/x/player/wbi/v2", query, true);
        } catch (PluginError e) {
            data = api("/x/player/v2", query, false);
        }
        JSONObject subtitle = data.optJSONObject("subtitle");
        JSONArray subtitles = subtitle == null ? null : subtitle.optJSONArray("subtitles");
        if (subtitles == null || subtitles.length() == 0) throw new PluginError(1015, "该视频没有 CC 字幕");

        JSONObject chosen = null;
        for (String wanted : new String[]{"zh-CN", "zh-Hans", "ai-zh", "zh-Hant"}) {
            for (int i = 0; i < subtitles.length(); i++) {
                JSONObject entry = subtitles.optJSONObject(i);
                if (entry != null && wanted.equals(entry.optString("lan", ""))
                        && !entry.optString("subtitle_url", "").isEmpty()) {
                    chosen = entry;
                    break;
                }
            }
            if (chosen != null) break;
        }
        if (chosen == null) {
            for (int i = 0; i < subtitles.length(); i++) {
                JSONObject entry = subtitles.optJSONObject(i);
                if (entry != null && !entry.optString("subtitle_url", "").isEmpty()) {
                    chosen = entry;
                    break;
                }
            }
        }
        if (chosen == null) throw new PluginError(1015, "该视频没有可用字幕");

        String url = normalizeCover(chosen.optString("subtitle_url", ""));
        JSONObject body;
        try {
            body = Http.getJson(url, headers());
        } catch (Exception e) {
            throw new PluginError(1015, "字幕下载失败：" + e.getMessage());
        }
        JSONArray rows = body.optJSONArray("body");
        JSONArray lines = new JSONArray();
        if (rows != null) {
            for (int i = 0; i < rows.length(); i++) {
                JSONObject row = rows.optJSONObject(i);
                if (row == null) continue;
                String text = stripTags(row.optString("content", ""));
                if (text.isEmpty()) continue;
                JSONObject line = new JSONObject();
                line.put("time_ms", (long) (row.optDouble("from", 0) * 1000));
                line.put("text", text);
                lines.put(line);
            }
        }
        if (lines.length() == 0) throw new PluginError(1015, "字幕内容为空");

        JSONObject out = new JSONObject();
        out.put("source", "bilibili");
        out.put("format", "lrc");
        out.put("timed", true);
        out.put("offset_ms", 0);
        out.put("lan", chosen.optString("lan", ""));
        out.put("lan_doc", chosen.optString("lan_doc", ""));
        out.put("lines", lines);
        return out;
    }

    private JSONObject getPic(JSONObject params) throws PluginError, JSONException {
        JSONObject video = video(idOf(params));
        JSONObject out = new JSONObject();
        out.put("platform", "bilibili");
        out.put("id", video.optString("item_id"));
        out.put("pic", video.optString("pic"));
        out.put("url", video.optString("pic"));
        return out;
    }

    private JSONObject checkLogin() throws JSONException {
        return accountState();
    }

    /** /x/web-interface/nav：登录态、昵称、头像、会员状态 */
    private JSONObject accountState() throws JSONException {
        JSONObject out = new JSONObject();
        out.put("platform", "bilibili");
        try {
            JSONObject data = api("/x/web-interface/nav", null, false);
            boolean loggedIn = data.optInt("isLogin", 0) == 1;
            int vipType = data.optInt("vipType", 0);
            out.put("logged_in", loggedIn);
            out.put("uname", data.optString("uname", ""));
            out.put("mid", data.optString("mid", ""));
            out.put("face", normalizeCover(data.optString("face", "")));
            out.put("vip", vipType != 0);
            out.put("vip_label", vipType == 1 ? "大会员" : (vipType == 2 ? "年度大会员" : "普通用户"));
            out.put("cookie_configured", !cookie().isEmpty());
        } catch (PluginError e) {
            // 未登录时 nav 也可能报错，按未登录处理
            out.put("logged_in", false);
            out.put("vip", false);
            out.put("vip_label", "未登录");
            out.put("note", e.getMessage());
        }
        return out;
    }

    private JSONObject loginCookie(JSONObject params) throws JSONException, PluginError {
        String text = params.optString("cookie", params.optString("sessdata", "")).trim();
        if (text.isEmpty()) throw new PluginError(1007, "Cookie 不能为空（至少需要 SESSDATA）");
        if (!text.contains("=")) text = "SESSDATA=" + text;
        if (!text.contains("SESSDATA=")) throw new PluginError(1007, "Cookie 里没有 SESSDATA，无法登录");
        final String previous = cookie();
        saveCookie(text);
        JSONObject state;
        try {
            state = accountState();
        } catch (Exception e) {
            saveCookie(previous);
            throw new PluginError(1003, "Cookie 无效或已过期：" + e.getMessage());
        }
        if (!state.optBoolean("logged_in", false)) {
            saveCookie(previous);
            throw new PluginError(1003, "Cookie 无效或已过期（nav 返回未登录）");
        }
        Http.log("bilibili 登录成功：" + state.optString("uname", ""));
        return state;
    }

    private JSONObject logout() throws JSONException {
        saveCookie("");
        mixinKey = "";
        buvid = "";
        JSONObject out = new JSONObject();
        out.put("platform", "bilibili");
        out.put("logged_in", false);
        out.put("cookie_configured", false);
        return out;
    }

    private JSONObject settings() throws JSONException {
        JSONObject out = new JSONObject();
        out.put("platform", "bilibili");
        out.put("plugin_id", "bilibili.source");
        out.put("version", "1.2.0-java");
        out.put("endpoint", "http://127.0.0.1:" + port);
        out.put("login", accountState());
        out.put("values", new JSONObject());
        out.put("schema", new JSONArray());
        out.put("note", "Android 版插件：登录态通过粘贴 Cookie 导入，其余配置项固定");
        return out;
    }

    // ------------------------------------------------------------ 收藏夹 / 投稿 / 合集

    private JSONObject favList(String mediaId, int limit) throws JSONException, PluginError {
        JSONObject out = new JSONObject();
        try {
            Map<String, String> query = new HashMap<>();
            query.put("media_id", mediaId);
            query.put("ps", String.valueOf(limit));
            query.put("pn", "1");
            query.put("platform", "web");
            JSONObject data = api("/x/v3/fav/resource/list", query, false);
            JSONObject info = data.optJSONObject("info");
            JSONArray medias = data.optJSONArray("medias");
            JSONArray tracks = new JSONArray();
            if (medias != null) {
                for (int i = 0; i < medias.length(); i++) {
                    JSONObject media = medias.optJSONObject(i);
                    if (media == null) continue;
                    String bvid = media.optString("bvid", "");
                    if (!bvid.startsWith("BV")) continue;
                    if (media.optInt("type", 2) != 2) continue;
                    JSONObject track = new JSONObject();
                    track.put("platform", "bilibili");
                    track.put("item_id", bvid);
                    track.put("sha", sha256Hex("bilibili:" + bvid));
                    track.put("name", stripTags(media.optString("title", "")));
                    JSONObject upper = media.optJSONObject("upper");
                    track.put("author", stripTags(upper == null ? "" : upper.optString("name", "")));
                    track.put("duration", media.optInt("duration", 0));
                    track.put("group_key", "");
                    tracks.put(track);
                }
            }
            out.put("platform", "bilibili");
            out.put("id", "fav:" + mediaId);
            out.put("name", stripTags(info == null ? "" : info.optString("title", "收藏夹 " + mediaId)));
            out.put("total", info == null ? tracks.length() : info.optInt("media_count", tracks.length()));
            out.put("tracks", tracks);
        } catch (PluginError e) {
            if (e.code == 1014) throw new PluginError(1021, "收藏夹需要登录 bilibili（请在设置里粘贴 Cookie）");
            throw e;
        }
        return out;
    }

    private JSONObject upVideos(String mid, int limit) throws JSONException, PluginError {
        Map<String, String> query = new HashMap<>();
        query.put("mid", mid);
        query.put("ps", String.valueOf(Math.min(50, limit)));
        query.put("pn", "1");
        query.put("order", "pubdate");
        JSONObject data;
        try {
            data = api("/x/space/wbi/arc/search", query, true);
        } catch (PluginError e) {
            if (e.code != 1014) throw e;
            // wbi 版被风控时换非签名版再试一次
            data = api("/x/space/arc/search", query, false);
        }
        JSONObject list = data.optJSONObject("list");
        JSONArray vlist = list == null ? null : list.optJSONArray("vlist");
        JSONArray tracks = new JSONArray();
        if (vlist != null) {
            for (int i = 0; i < vlist.length(); i++) {
                JSONObject entry = vlist.optJSONObject(i);
                if (entry == null) continue;
                String bvid = entry.optString("bvid", "");
                if (!bvid.startsWith("BV")) continue;
                JSONObject track = new JSONObject();
                track.put("platform", "bilibili");
                track.put("item_id", bvid);
                track.put("sha", sha256Hex("bilibili:" + bvid));
                track.put("name", stripTags(entry.optString("title", "")));
                track.put("author", stripTags(entry.optString("author", "")));
                track.put("pic", normalizeCover(entry.optString("pic", "")));
                track.put("duration", parseDuration(entry.opt("length")));
                track.put("group_key", "");
                tracks.put(track);
            }
        }
        JSONObject out = new JSONObject();
        out.put("platform", "bilibili");
        out.put("id", "up:" + mid);
        out.put("name", "UP 主 " + mid + " 的投稿");
        out.put("total", tracks.length());
        out.put("tracks", tracks);
        return out;
    }

    private JSONObject seasonList(String mid, String seasonId, int limit) throws JSONException, PluginError {
        Map<String, String> query = new HashMap<>();
        query.put("mid", mid);
        query.put("season_id", seasonId);
        query.put("page_num", "1");
        query.put("page_size", String.valueOf(limit));
        JSONObject data = api("/x/polymer/web-space/seasons_archives_list", query, false);
        JSONArray archives = data.optJSONArray("archives");
        JSONObject meta = data.optJSONObject("meta");
        String owner = stripTags(meta == null ? "" : meta.optString("name", ""));
        JSONArray tracks = new JSONArray();
        if (archives != null) {
            for (int i = 0; i < archives.length(); i++) {
                JSONObject entry = archives.optJSONObject(i);
                if (entry == null) continue;
                String bvid = entry.optString("bvid", "");
                if (!bvid.startsWith("BV")) continue;
                JSONObject track = new JSONObject();
                track.put("platform", "bilibili");
                track.put("item_id", bvid);
                track.put("sha", sha256Hex("bilibili:" + bvid));
                track.put("name", stripTags(entry.optString("title", "")));
                track.put("author", owner);
                track.put("duration", entry.optInt("duration", 0));
                track.put("group_key", "");
                tracks.put(track);
            }
        }
        JSONObject out = new JSONObject();
        out.put("platform", "bilibili");
        out.put("id", "season:" + mid + ":" + seasonId);
        out.put("name", stripTags(meta == null ? "" : meta.optString("title", "合集 " + seasonId)));
        out.put("total", tracks.length());
        out.put("tracks", tracks);
        return out;
    }

    private JSONObject getPlaylist(JSONObject params) throws JSONException, PluginError {
        String raw = params.optString("id", params.optString("item_id", "")).trim();
        int limit = Math.min(100, Math.max(1, params.optInt("limit", 50)));
        if (raw.isEmpty()) throw new PluginError(1007, "缺少歌单标识（fav:<收藏夹id> / up:<mid> / season:<mid>:<season_id>）");
        String lowered = raw.toLowerCase();
        if (lowered.startsWith("fav:")) return favList(raw.substring(4), limit);
        if (lowered.startsWith("up:") || lowered.startsWith("mid:")) return upVideos(raw.substring(raw.indexOf(':') + 1), limit);
        if (lowered.startsWith("season:")) {
            String[] parts = raw.split(":");
            if (parts.length < 3) throw new PluginError(1007, "合集需要 season:<mid>:<season_id>");
            return seasonList(parts[1], parts[2], limit);
        }
        if (raw.matches("\\d+")) return favList(raw, limit);
        throw new PluginError(1007, "无法识别的歌单标识: " + raw);
    }

    private JSONObject getFavFolders(JSONObject params) throws JSONException, PluginError {
        String mid = params.optString("mid", params.optString("id", "")).trim();
        JSONObject out = new JSONObject();
        JSONArray folders = new JSONArray();
        if (mid.matches("\\d+")) {
            try {
                Map<String, String> query = new HashMap<>();
                query.put("mid", mid);
                query.put("rid", "0");
                query.put("type", "21");
                query.put("page_num", "1");
                query.put("page_size", "20");
                JSONObject data = api("/x/v3/fav/folder/space/list", query, false);
                folders = readFolders(data);
            } catch (PluginError e) {
                out.put("error", e.getMessage());
            }
            out.put("owner", mid);
        } else {
            JSONObject data = api("/x/v3/fav/folder/created/list", null, false);
            folders = readFolders(data);
            out.put("owner", "self");
        }
        out.put("platform", "bilibili");
        out.put("folders", folders);
        return out;
    }

    private JSONArray readFolders(JSONObject data) throws JSONException {
        JSONArray listing = data.optJSONArray("list");
        if (listing == null) {
            JSONObject inner = data.optJSONObject("data");
            listing = inner == null ? null : inner.optJSONArray("list");
        }
        JSONArray folders = new JSONArray();
        if (listing == null) return folders;
        for (int i = 0; i < listing.length(); i++) {
            JSONObject entry = listing.optJSONObject(i);
            if (entry == null) continue;
            JSONObject folder = new JSONObject();
            folder.put("id", String.valueOf(entry.optLong("id", 0)));
            folder.put("title", stripTags(entry.optString("title", "")));
            folder.put("count", entry.optInt("media_count", 0));
            folder.put("cover", normalizeCover(entry.optString("cover", "")));
            folders.put(folder);
        }
        return folders;
    }

    // ------------------------------------------------------------ UP 主

    private JSONObject searchArtist(JSONObject params) throws JSONException, PluginError {
        String keyword = params.optString("keyword", params.optString("q", "")).trim();
        if (keyword.isEmpty()) throw new PluginError(1007, "搜索关键字不能为空");
        int page = Math.max(1, params.optInt("page", 1));
        int pageSize = Math.min(30, Math.max(1, params.optInt("page_size", 10)));
        ensureBuvid();
        Map<String, String> query = new HashMap<>();
        query.put("search_type", "bili_user");
        query.put("keyword", keyword);
        query.put("page", String.valueOf(page));
        query.put("page_size", String.valueOf(pageSize));
        JSONObject data = api("/x/web-interface/wbi/search/type", query, true);
        JSONArray raw = data.optJSONArray("result");
        JSONArray artists = new JSONArray();
        if (raw != null) {
            for (int i = 0; i < raw.length(); i++) {
                JSONObject entry = raw.optJSONObject(i);
                if (entry == null) continue;
                long mid = entry.optLong("mid", 0);
                String name = stripTags(entry.optString("uname", ""));
                if (mid == 0 || name.isEmpty()) continue;
                String pic = normalizeCover(entry.optString("upic",
                        entry.optString("cover", entry.optString("face", ""))));
                JSONObject artist = new JSONObject();
                artist.put("id", String.valueOf(mid));
                artist.put("name", name);
                artist.put("avatar", pic);
                artist.put("pic", pic);
                artist.put("alias", new JSONArray());
                artist.put("fans", entry.optLong("fans", 0));
                artist.put("videos", entry.optLong("videos", 0));
                artist.put("sign", stripTags(entry.optString("usign", "")));
                artist.put("platform", "bilibili");
                artists.put(artist);
            }
        }
        JSONObject out = new JSONObject();
        out.put("artists", artists);
        out.put("keyword", keyword);
        out.put("page", page);
        out.put("page_size", pageSize);
        out.put("total", data.optInt("numResults", artists.length()));
        return out;
    }

    /** UP 主资料：wbi 接口有风控，失败退到 web-interface/card */
    private JSONObject accInfo(String mid) throws JSONException, PluginError {
        ensureBuvid();
        try {
            Map<String, String> query = new HashMap<>();
            query.put("mid", mid);
            JSONObject data = api("/x/space/wbi/acc/info", query, true);
            JSONObject card = data.optJSONObject("card");
            if (card != null && !card.optString("name", "").isEmpty()) {
                return cardInfoOf(card, mid);
            }
        } catch (PluginError e) {
            if (e.code != 1014 && e.code != 1011) throw e;
        }
        Map<String, String> query = new HashMap<>();
        query.put("mid", mid);
        query.put("photo", "false");
        JSONObject data = api("/x/web-interface/card", query, false);
        JSONObject card = data.optJSONObject("card");
        if (card == null || card.optString("name", "").isEmpty()) {
            throw new PluginError(1007, "UP 主不存在或不可见（mid=" + mid + "）");
        }
        JSONObject out = cardInfoOf(card, mid);
        if (out.optLong("fans", 0) == 0) out.put("fans", data.optLong("follower", 0));
        if (out.optLong("videos", 0) == 0) out.put("videos", data.optLong("archive_count", 0));
        return out;
    }

    private JSONObject cardInfoOf(JSONObject card, String mid) throws JSONException {
        JSONObject out = new JSONObject();
        out.put("mid", String.valueOf(card.optLong("mid", 0) == 0 ? mid : card.optLong("mid")));
        out.put("name", stripTags(card.optString("name", "")));
        out.put("avatar", normalizeCover(card.optString("face", "")));
        out.put("sign", stripTags(card.optString("sign", "")));
        out.put("fans", card.optLong("fans", 0));
        out.put("attention", card.optLong("attention", 0));
        out.put("videos", card.optLong("archive_count", 0));
        out.put("level", card.optInt("level", 0));
        return out;
    }

    private JSONObject getArtist(JSONObject params) throws JSONException, PluginError {
        String mid = params.optString("id", params.optString("item_id", "")).trim();
        if (!mid.matches("\\d+")) throw new PluginError(1007, "缺少 UP 主 mid（数字）");
        JSONObject info = accInfo(mid);
        JSONArray hot = new JSONArray();
        try {
            JSONObject listing = upVideos(mid, 30);
            JSONArray tracks = listing.optJSONArray("tracks");
            if (tracks != null) {
                for (int i = 0; i < tracks.length(); i++) {
                    JSONObject track = tracks.optJSONObject(i);
                    if (track == null) continue;
                    JSONObject song = new JSONObject();
                    song.put("platform", "bilibili");
                    song.put("item_id", track.optString("item_id"));
                    song.put("id", track.optString("item_id"));
                    song.put("sha", track.optString("sha"));
                    song.put("name", track.optString("name"));
                    song.put("author", track.optString("author"));
                    song.put("album", "");
                    int duration = track.optInt("duration", 0);
                    song.put("duration", duration);
                    song.put("duration_seconds", duration);
                    song.put("pic", track.optString("pic", ""));
                    song.put("artist_id", mid);
                    song.put("bvid", track.optString("item_id"));
                    song.put("available", true);
                    song.put("reason", "");
                    song.put("quality", qualityJson("a192", null));
                    JSONArray qualities = new JSONArray();
                    qualities.put(qualityJson("a192", null));
                    qualities.put(qualityJson("a132", null));
                    qualities.put(qualityJson("a64", null));
                    song.put("qualities", qualities);
                    hot.put(song);
                }
            }
        } catch (PluginError e) {
            Http.log("UP " + mid + " 投稿列表获取失败（资料页仍展示）：" + e.getMessage());
        }
        JSONObject out = new JSONObject();
        out.put("platform", "bilibili");
        out.put("source_id", "bilibili:" + mid);
        out.put("id", mid);
        out.put("mid", mid);
        out.put("name", info.optString("name", ""));
        out.put("avatar", info.optString("avatar", ""));
        out.put("pic", info.optString("avatar", ""));
        out.put("alias", new JSONArray());
        out.put("brief_desc", info.optString("sign", ""));
        out.put("album_count", 0);
        out.put("music_count", info.optLong("videos", 0));
        out.put("mv_count", info.optLong("videos", 0));
        out.put("fans", info.optLong("fans", 0));
        out.put("videos", info.optLong("videos", 0));
        out.put("hot_songs", hot);
        return out;
    }

    private JSONObject getArtistMvs(JSONObject params) throws JSONException, PluginError {
        String mid = params.optString("id", params.optString("item_id", "")).trim();
        if (!mid.matches("\\d+")) throw new PluginError(1007, "缺少 UP 主 mid");
        JSONObject listing = upVideos(mid, 30);
        JSONArray tracks = listing.optJSONArray("tracks");
        JSONArray mvs = new JSONArray();
        if (tracks != null) {
            for (int i = 0; i < tracks.length(); i++) {
                JSONObject track = tracks.optJSONObject(i);
                if (track == null) continue;
                JSONObject mv = new JSONObject();
                mv.put("platform", "bilibili");
                mv.put("id", track.optString("item_id"));
                mv.put("item_id", track.optString("item_id"));
                mv.put("sha", sha256Hex("bilibili:mv:" + track.optString("item_id")));
                mv.put("title", track.optString("name"));
                mv.put("name", track.optString("name"));
                mv.put("artist", track.optString("author"));
                mv.put("author", track.optString("author"));
                mv.put("album", "");
                mv.put("pic", track.optString("pic", ""));
                mv.put("cover", track.optString("pic", ""));
                mv.put("artist_id", mid);
                mv.put("duration", track.optInt("duration", 0));
                mv.put("duration_seconds", track.optInt("duration", 0));
                mv.put("bvid", track.optString("item_id"));
                mv.put("play_count", 0);
                mvs.put(mv);
            }
        }
        JSONObject out = new JSONObject();
        out.put("mvs", mvs);
        out.put("items", mvs);
        out.put("total", mvs.length());
        out.put("keyword", "");
        out.put("page", 1);
        out.put("page_size", 30);
        return out;
    }

    // ------------------------------------------------------------ MV

    /** 视频清晰度 → (标签, 宽, 高) */
    private static final java.util.Map<Integer, String[]> VIDEO_QN = new java.util.HashMap<>();
    static {
        VIDEO_QN.put(127, new String[]{"8K", "7680", "4320"});
        VIDEO_QN.put(126, new String[]{"杜比视界", "3840", "2160"});
        VIDEO_QN.put(125, new String[]{"HDR", "3840", "2160"});
        VIDEO_QN.put(120, new String[]{"4K", "3840", "2160"});
        VIDEO_QN.put(116, new String[]{"1080P60", "1920", "1080"});
        VIDEO_QN.put(112, new String[]{"1080P+", "1920", "1080"});
        VIDEO_QN.put(80, new String[]{"1080P", "1920", "1080"});
        VIDEO_QN.put(74, new String[]{"720P60", "1280", "720"});
        VIDEO_QN.put(64, new String[]{"720P", "1280", "720"});
        VIDEO_QN.put(32, new String[]{"480P", "852", "480"});
        VIDEO_QN.put(16, new String[]{"360P", "640", "360"});
        VIDEO_QN.put(6, new String[]{"240P", "426", "240"});
    }

    private JSONObject searchMv(JSONObject params) throws JSONException, PluginError {
        String keyword = params.optString("keyword", params.optString("q", "")).trim();
        if (keyword.isEmpty()) throw new PluginError(1007, "搜索关键字不能为空");
        JSONObject result = search(params);
        JSONArray items = result.optJSONArray("items");
        JSONArray mvs = new JSONArray();
        if (items != null) {
            for (int i = 0; i < items.length(); i++) {
                JSONObject item = items.optJSONObject(i);
                if (item == null) continue;
                JSONObject mv = new JSONObject();
                String id = item.optString("item_id");
                mv.put("platform", "bilibili");
                mv.put("id", id);
                mv.put("item_id", id);
                mv.put("sha", sha256Hex("bilibili:mv:" + id));
                mv.put("title", item.optString("name"));
                mv.put("name", item.optString("name"));
                mv.put("artist", item.optString("author"));
                mv.put("author", item.optString("author"));
                mv.put("cover", item.optString("pic"));
                mv.put("pic", item.optString("pic"));
                mv.put("duration", item.optInt("duration"));
                mv.put("duration_seconds", item.optInt("duration"));
                mv.put("bvid", id);
                mv.put("play_count", 0);
                mvs.put(mv);
            }
        }
        JSONObject out = new JSONObject();
        out.put("mvs", mvs);
        out.put("items", mvs);
        out.put("total", result.optInt("total", mvs.length()));
        out.put("keyword", keyword);
        out.put("page", result.optInt("page", 1));
        out.put("page_size", result.optInt("page_size", 20));
        return out;
    }

    private JSONObject getMv(JSONObject params) throws JSONException, PluginError {
        JSONObject video = video(idOf(params));
        if (!video.optBoolean("available", true)) {
            throw new PluginError(1004, video.optString("reason", "视频不可用"));
        }
        // MV 要整段音视频：fnval=1 走 durl（混流 MP4）
        JSONObject base = api("/x/player/playurl", playurlParams(video, 1), false);
        JSONArray durl = base.optJSONArray("durl");
        if (durl == null || durl.length() == 0) {
            throw new PluginError(1021, "该视频没有可用的混流 MP4（仅有 DASH），暂时无法作为 MV 播放");
        }
        java.util.List<Integer> usable = new java.util.ArrayList<>();
        JSONArray accept = base.optJSONArray("accept_quality");
        if (accept != null) {
            for (int i = 0; i < accept.length(); i++) {
                int value = accept.optInt(i, 0);
                if (VIDEO_QN.containsKey(value)) usable.add(value);
            }
        }
        int target = 0;
        for (int value : usable) target = Math.max(target, value);
        if (target == 0) target = base.optInt("quality", 16);
        int requested = params.optInt("qn", params.optInt("quality", 0));
        if (VIDEO_QN.containsKey(requested) && (usable.isEmpty() || requested <= target)) {
            target = requested;
        }
        JSONObject data = base;
        if (target != 0 && target != base.optInt("quality", 0)) {
            try {
                Map<String, String> query = playurlParams(video, 1);
                query.put("qn", String.valueOf(target));
                data = api("/x/player/playurl", query, false);
            } catch (PluginError ignored) {
                data = base;
            }
        }
        JSONArray parts = data.optJSONArray("durl");
        if (parts == null || parts.length() == 0) parts = base.optJSONArray("durl");
        String url = "";
        if (parts != null && parts.length() > 0) {
            JSONObject first = parts.optJSONObject(0);
            if (first != null) {
                url = first.optString("url", "");
                if (url.isEmpty()) {
                    JSONArray backups = first.optJSONArray("backup_url");
                    if (backups != null && backups.length() > 0) url = backups.optString(0, "");
                }
            }
        }
        if (url.isEmpty()) throw new PluginError(1021, "bilibili 没有返回 MV 播放地址");

        int qn = data.optInt("quality", target);
        String[] spec = VIDEO_QN.get(qn);
        if (spec == null) spec = new String[]{"", "0", "0"};
        JSONObject quality = new JSONObject();
        quality.put("id", String.valueOf(qn));
        quality.put("label", spec[0]);

        JSONArray resolutions = new JSONArray();
        java.util.List<Integer> sorted = new java.util.ArrayList<>(new java.util.TreeSet<>(usable));
        java.util.Collections.reverse(sorted);
        for (int value : sorted) {
            String[] entry = VIDEO_QN.get(value);
            if (entry == null) continue;
            JSONObject item = new JSONObject();
            item.put("qn", value);
            item.put("label", entry[0]);
            item.put("width", Integer.parseInt(entry[1]));
            item.put("height", Integer.parseInt(entry[2]));
            resolutions.put(item);
        }

        String proxied = proxyUrl(url);
        JSONObject out = new JSONObject();
        out.put("platform", "bilibili");
        out.put("id", video.optString("item_id"));
        out.put("item_id", video.optString("item_id"));
        out.put("sha", sha256Hex("bilibili:mv:" + video.optString("item_id")));
        out.put("title", video.optString("display_title"));
        out.put("artist", video.optString("author"));
        out.put("album", video.optString("tname"));
        out.put("pic", video.optString("pic"));
        out.put("cover", video.optString("pic"));
        out.put("url", proxied);
        out.put("location", proxied);
        out.put("quality", quality);
        out.put("width", Integer.parseInt(spec[1]));
        out.put("height", Integer.parseInt(spec[2]));
        out.put("resolution", spec[0]);
        out.put("duration", video.optLong("duration"));
        out.put("duration_seconds", video.optLong("duration"));
        out.put("bvid", video.optString("bvid"));
        out.put("cid", video.optLong("cid"));
        out.put("page", video.optInt("page", 1));
        out.put("publish_time", video.optLong("pubdate"));
        out.put("resolutions", resolutions);
        return out;
    }

    // ------------------------------------------------------------ 工具

    private static JSONObject qualityJson(String id, JSONObject entry) {
        JSONObject spec = new JSONObject();
        try {
            switch (id) {
                case "hires":
                    spec.put("id", "hires").put("quality_id", "hires").put("label", "Hi-Res 无损")
                        .put("level", "hires").put("codec", "flac").put("lossless", true)
                        .put("bitrate", 1411).put("sample_rate", 96000).put("bit_depth", 24).put("channels", 2);
                    break;
                case "dolby":
                    spec.put("id", "dolby").put("quality_id", "dolby").put("label", "杜比全景声")
                        .put("level", "high").put("codec", "ec3").put("lossless", false)
                        .put("bitrate", 448).put("sample_rate", 48000).put("channels", 6);
                    break;
                case "a192":
                    spec.put("id", "a192").put("quality_id", "a192").put("label", "192K")
                        .put("level", "high").put("codec", "aac").put("lossless", false)
                        .put("bitrate", 192).put("sample_rate", 44100).put("channels", 2);
                    break;
                case "a132":
                    spec.put("id", "a132").put("quality_id", "a132").put("label", "132K")
                        .put("level", "medium").put("codec", "aac").put("lossless", false)
                        .put("bitrate", 132).put("sample_rate", 44100).put("channels", 2);
                    break;
                default:
                    spec.put("id", "a64").put("quality_id", "a64").put("label", "64K")
                        .put("level", "low").put("codec", "aac").put("lossless", false)
                        .put("bitrate", 64).put("sample_rate", 44100).put("channels", 2);
                    break;
            }
            if (entry != null) {
                long bandwidth = entry.optLong("bandwidth", 0);
                if (bandwidth > 0) spec.put("bitrate", Math.max(1, Math.round(bandwidth / 1000.0)));
                String codecs = entry.optString("codecs", entry.optString("codec", "")).toLowerCase();
                if (codecs.contains("flac")) spec.put("codec", "flac");
                else if (codecs.contains("ec-3") || codecs.contains("ec3") || codecs.contains("eac3")) {
                    spec.put("codec", "ec3");
                } else if (codecs.contains("mp4a") || codecs.contains("aac")) spec.put("codec", "aac");
                int sampleRate = entry.optInt("sampleRate", entry.optInt("sample_rate", 0));
                if (sampleRate > 0) spec.put("sample_rate", sampleRate);
                int bitDepth = entry.optInt("bitDepth", entry.optInt("bit_depth", 0));
                if (bitDepth > 0) spec.put("bit_depth", bitDepth);
            }
        } catch (Exception ignored) {
            // JSON 写入失败不影响主流程
        }
        return spec;
    }

    private static String normalizeCover(String url) {
        if (url == null || url.isEmpty()) return "";
        String text = url.trim();
        if (text.startsWith("//")) return "https:" + text;
        if (text.startsWith("http://")) return "https://" + text.substring(7);
        return text;
    }

    private static String stripTags(String text) {
        if (text == null || text.isEmpty()) return "";
        String cleaned = text.replaceAll("<[^>]+>", "");
        cleaned = cleaned.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
                .replace("&quot;", "\"").replace("&#39;", "'").replace("&nbsp;", " ");
        return cleaned.trim();
    }

    private static int parseDuration(Object value) {
        if (value == null) return 0;
        if (value instanceof Number) return Math.max(0, ((Number) value).intValue());
        String raw = String.valueOf(value).trim();
        if (raw.isEmpty()) return 0;
        try {
            return Integer.parseInt(raw);
        } catch (NumberFormatException ignored) {
            // 继续按 "3:54" 解析
        }
        int total = 0;
        for (String part : raw.split(":")) {
            try {
                total = total * 60 + Integer.parseInt(part);
            } catch (NumberFormatException e) {
                return 0;
            }
        }
        return total;
    }

    static String sha256Hex(String text) {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] bytes = digest.digest(text.getBytes(Charset.forName("UTF-8")));
            StringBuilder sb = new StringBuilder();
            for (byte b : bytes) sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (Exception e) {
            return "";
        }
    }

    static String md5(String text) {
        try {
            MessageDigest digest = MessageDigest.getInstance("MD5");
            byte[] bytes = digest.digest(text.getBytes(Charset.forName("UTF-8")));
            StringBuilder sb = new StringBuilder();
            for (byte b : bytes) sb.append(String.format("%02x", b));
            return sb.toString();
        } catch (Exception e) {
            return "";
        }
    }

    static String encode(String text) {
        try {
            return URLEncoder.encode(text == null ? "" : text, "UTF-8");
        } catch (UnsupportedEncodingException e) {
            return text;
        }
    }

    static String base64Url(String text) {
        try {
            return android.util.Base64.encodeToString(text.getBytes("UTF-8"),
                    android.util.Base64.URL_SAFE | android.util.Base64.NO_PADDING | android.util.Base64.NO_WRAP);
        } catch (Exception e) {
            return "";
        }
    }
}
