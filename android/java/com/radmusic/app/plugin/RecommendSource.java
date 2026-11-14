package com.radmusic.app.plugin;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * 推荐助手（Java 版）。
 *
 * 与桌面端 plugins/recommend 的差别：
 *   · 音源固定为 app 内两个内置插件（bilibili 8849 / netease 8848），不再扫描 plugins 目录；
 *   · 打分逻辑做了简化（源权重 + 关键词命中 + 去重），但**返回字段与模式完全一致**，
 *     前端推荐页不需要区分是哪个实现；
 *   · 偏好存 app 私有目录的 JSON（桌面版存在插件目录）。
 */
final class RecommendSource implements Source {

    private static final String PLATFORM = "recommend.assistant";
    private static final int BILI_PORT = PluginBridge.BILIBILI_PORT;
    private static final int NETEASE_PORT = PluginBridge.NETEASE_PORT;

    /** 模式定义（与桌面版一字不差） */
    private static final String[][] MODES = {
            {"recommend_daily", "每日推荐", "calendar", "根据你的收藏歌手与曲风，叠加当天轮换的探索词，每天不一样。", "none"},
            {"roam", "漫游", "globe", "以新颖度优先的随机游走，越听越陌生又不全是噪音。", "none"},
            {"explore", "探索", "compass", "输入关键词 / 曲风 / 心情，跨音源聚合并按相关度排序。", "keyword"},
            {"similar", "相似推荐", "link", "输入歌手或歌曲，找风格相近的曲目。", "artist"},
            {"mood", "心情电台", "heart", "选一种心情，自动编排契合该情绪的音乐。", "mood"},
    };

    private static final String[][] MOODS = {
            {"chill", "放松", "轻音乐 纯音乐 钢琴 Lo-Fi 舒缓 治愈 雨天 咖啡 爵士"},
            {"focus", "专注", "纯音乐 古典 钢琴 后摇 ambient 学习 工作 专注"},
            {"energetic", "活力", "摇滚 电音 电子 燃 舞曲 节奏 DJ 说唱"},
            {"nostalgia", "怀旧", "经典老歌 怀旧 80年代 90年代 老歌 青春 粤语金曲"},
            {"fresh", "新鲜", "新歌 最新 飙升 热歌 首发 本周新歌"},
            {"happy", "快乐", "欢快 流行 甜歌 轻快 洗脑"},
            {"sad", "伤感", "伤感 情歌 慢歌 苦情 emo"},
            {"sleep", "安眠", "助眠 白噪音 轻音乐 雨声 睡前 冥想"},
    };

    /** 漫游/每日推荐的轮换词库 */
    private static final String[] ROAM_WORDS = {
            "宝藏翻唱", "冷门好歌", "独立音乐", "现场版", "女声 治愈", "男声 磁性",
            "钢琴 纯音乐", "吉他 弹唱", "古风", "爵士 慵懒", "电子 氛围", "说唱 中文",
    };

    private final int port;
    private final File prefFile;
    private JSONObject prefs;

    RecommendSource(int port, File prefFile) {
        this.port = port;
        this.prefFile = prefFile;
    }

    @Override
    public String platform() {
        return PLATFORM;
    }

    // ------------------------------------------------------------ 偏好

    private JSONObject preferences() throws JSONException {
        if (prefs != null) return prefs;
        prefs = null;
        try {
            if (prefFile != null && prefFile.isFile()) {
                byte[] data = new byte[(int) prefFile.length()];
                FileInputStream in = new FileInputStream(prefFile);
                int read = in.read(data);
                in.close();
                if (read > 0) prefs = new JSONObject(new String(data, 0, read, "UTF-8"));
            }
        } catch (Exception e) {
            Http.log("读取推荐偏好失败：" + e.getMessage());
        }
        if (prefs == null) prefs = new JSONObject();
        if (prefs.optJSONArray("seed_artists") == null) prefs.put("seed_artists", new JSONArray());
        if (prefs.optJSONArray("favorites") == null) prefs.put("favorites", new JSONArray());
        if (prefs.optJSONArray("excluded_platforms") == null) prefs.put("excluded_platforms", new JSONArray());
        if (prefs.optJSONObject("config") == null) prefs.put("config", new JSONObject());
        return prefs;
    }

    private void savePreferences() {
        try {
            if (prefFile == null) return;
            File parent = prefFile.getParentFile();
            if (parent != null) parent.mkdirs();
            FileOutputStream out = new FileOutputStream(prefFile);
            out.write(preferences().toString().getBytes("UTF-8"));
            out.close();
        } catch (Exception e) {
            Http.log("保存推荐偏好失败：" + e.getMessage());
        }
    }

    // ------------------------------------------------------------ 调音源

    private List<String> activePlatforms() throws JSONException {
        List<String> all = new ArrayList<>();
        all.add("bilibili");
        all.add("netease");
        JSONArray excluded = preferences().optJSONArray("excluded_platforms");
        List<String> out = new ArrayList<>();
        for (String platform : all) {
            boolean skip = false;
            for (int i = 0; excluded != null && i < excluded.length(); i++) {
                if (platform.equals(excluded.optString(i))) skip = true;
            }
            if (!skip) out.add(platform);
        }
        return out;
    }

    private int endpointOf(String platform) {
        return "netease".equals(platform) ? NETEASE_PORT : BILI_PORT;
    }

    /** 调内置音源插件拉一批搜索结果 */
    private JSONArray searchSource(String platform, String keyword, int limit) {
        try {
            JSONObject params = new JSONObject();
            params.put("keyword", keyword);
            params.put("page", 1);
            params.put("page_size", Math.min(30, Math.max(5, limit)));
            JSONObject request = new JSONObject();
            request.put("op", "search");
            request.put("params", params);
            JSONObject reply = Http.postJson("http://127.0.0.1:" + endpointOf(platform) + "/request",
                    form("json", request.toString()), Http.headers("Content-Type", "application/json"));
            if (reply.optInt("code", -1) != 0) return new JSONArray();
            JSONObject data = reply.optJSONObject("data");
            return data == null ? new JSONArray() : data.optJSONArray("items");
        } catch (Exception e) {
            Http.log("调用音源 " + platform + " 失败：" + e.getMessage());
            return new JSONArray();
        }
    }

    /** /request 只接受 JSON body，这里借 postForm 的通道发原始 JSON */
    private static java.util.Map<String, String> form(String key, String value) {
        java.util.Map<String, String> map = new java.util.HashMap<>();
        map.put(key, value);
        return map;
    }

    private JSONObject callSource(String platform, String keyword, int limit) {
        JSONObject out = new JSONObject();
        try {
            JSONObject params = new JSONObject();
            params.put("keyword", keyword);
            params.put("page", 1);
            params.put("page_size", Math.min(30, Math.max(5, limit)));
            JSONObject request = new JSONObject();
            request.put("op", "search");
            request.put("params", params);
            out = postJsonTo(platform, request.toString());
            if (out.optInt("code", -1) != 0) return new JSONObject();
            JSONObject data = out.optJSONObject("data");
            return data == null ? new JSONObject() : data;
        } catch (Exception e) {
            Http.log("调用音源 " + platform + " 失败：" + e.getMessage());
            return new JSONObject();
        }
    }

    /** 直接以原始 JSON 作为 body POST 到插件 /request */
    private JSONObject postJsonTo(String platform, String body) {
        try {
            java.net.HttpURLConnection conn =
                    (java.net.HttpURLConnection) new java.net.URL(
                            "http://127.0.0.1:" + endpointOf(platform) + "/request").openConnection();
            conn.setConnectTimeout(8000);
            conn.setReadTimeout(15000);
            conn.setRequestMethod("POST");
            conn.setDoOutput(true);
            conn.setRequestProperty("Content-Type", "application/json; charset=utf-8");
            java.io.OutputStream os = conn.getOutputStream();
            os.write(body.getBytes("UTF-8"));
            os.close();
            int code = conn.getResponseCode();
            java.io.InputStream in = code >= 400 ? conn.getErrorStream() : conn.getInputStream();
            if (in == null) return new JSONObject();
            java.io.ByteArrayOutputStream buffer = new java.io.ByteArrayOutputStream();
            byte[] chunk = new byte[8192];
            int read;
            while ((read = in.read(chunk)) > 0) buffer.write(chunk, 0, read);
            in.close();
            conn.disconnect();
            return new JSONObject(new String(buffer.toByteArray(), "UTF-8"));
        } catch (Exception e) {
            return new JSONObject();
        }
    }

    // ------------------------------------------------------------ 打散 / 去重

    private static JSONObject itemOf(JSONObject raw, String platform, double weight) throws JSONException {
        String itemId = raw.optString("item_id", raw.optString("id", ""));
        if (itemId.isEmpty()) return null;
        JSONObject item = new JSONObject();
        String title = raw.optString("name", raw.optString("title", ""));
        String artist = raw.optString("artist", raw.optString("author", ""));
        item.put("platform", platform);
        item.put("source_name", "netease".equals(platform) ? "网易云音乐" : "哔哩哔哩");
        item.put("item_id", itemId);
        item.put("id", raw.optString("id", itemId));
        item.put("sha", raw.optString("sha", ""));
        item.put("title", title);
        item.put("name", title);
        JSONArray artists = raw.optJSONArray("artists");
        if (artists == null) {
            artists = new JSONArray();
            if (!artist.isEmpty()) artists.put(artist);
        }
        item.put("artists", artists);
        item.put("artist", artist);
        item.put("album", raw.optString("album", ""));
        int duration = raw.optInt("duration", raw.optInt("duration_seconds", 0));
        item.put("duration", duration);
        item.put("duration_seconds", duration);
        item.put("pic", raw.optString("pic", raw.optString("cover", "")));
        item.put("available", raw.optBoolean("available", true));
        item.put("reason", raw.optString("reason", ""));
        item.put("quality", raw.optJSONObject("quality") == null ? new JSONObject() : raw.optJSONObject("quality"));
        item.put("qualities", raw.optJSONArray("qualities") == null ? new JSONArray()
                : raw.optJSONArray("qualities"));
        item.put("score", weight);
        return item;
    }

    /** 跨源聚合 + 去重 + 按分数排序 */
    private JSONArray gather(List<String> keywords, List<String> platforms, int limit) throws JSONException {
        List<JSONObject> collected = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        int perKeyword = Math.max(5, limit / Math.max(1, keywords.size()) / Math.max(1, platforms.size()) + 3);
        for (String keyword : keywords) {
            for (String platform : platforms) {
                JSONObject data = callSource(platform, keyword, perKeyword);
                JSONArray items = data.optJSONArray("items");
                if (items == null) continue;
                double weight = 1.0 + Math.max(0, keywords.indexOf(keyword)) * -0.05;
                for (int i = 0; i < items.length(); i++) {
                    JSONObject raw = items.optJSONObject(i);
                    if (raw == null) continue;
                    JSONObject item = itemOf(raw, platform, weight);
                    if (item == null) continue;
                    String key = platform + ":" + item.optString("item_id");
                    if (seen.contains(key)) continue;
                    seen.add(key);
                    // 源内位置越靠前分越高
                    item.put("score", weight * (1.0 - i * 0.02));
                    collected.add(item);
                }
            }
        }
        collected.sort(new java.util.Comparator<JSONObject>() {
            @Override
            public int compare(JSONObject a, JSONObject b) {
                return Double.compare(b.optDouble("score", 0), a.optDouble("score", 0));
            }
        });
        JSONArray out = new JSONArray();
        for (int i = 0; i < collected.size() && i < limit; i++) out.put(collected.get(i));
        return out;
    }

    private JSONObject pack(JSONArray items, String mode, JSONArray seeds) throws JSONException {
        JSONObject out = new JSONObject();
        out.put("mode", mode);
        out.put("items", items);
        out.put("count", items.length());
        out.put("seeds", seeds);
        out.put("generated_at", System.currentTimeMillis() / 1000);
        JSONArray platforms = new JSONArray();
        for (String platform : activePlatforms()) platforms.put(platform);
        out.put("platforms", platforms);
        out.put("meta", new JSONObject());
        return out;
    }

    // ------------------------------------------------------------ 各模式

    private JSONArray seedArtists() throws JSONException {
        JSONArray out = new JSONArray();
        JSONArray stored = preferences().optJSONArray("seed_artists");
        for (int i = 0; stored != null && i < stored.length() && i < 5; i++) {
            String name = stored.optString(i, "").trim();
            if (!name.isEmpty()) out.put(name);
        }
        return out;
    }

    private JSONObject daily(JSONObject params) throws JSONException, PluginError {
        int limit = limitOf(params);
        List<String> keywords = new ArrayList<>();
        JSONArray seeds = seedArtists();
        for (int i = 0; i < seeds.length(); i++) keywords.add(seeds.optString(i));
        // 每天轮换一个探索词，保证「每天不一样」
        long day = System.currentTimeMillis() / 86400000L;
        keywords.add(ROAM_WORDS[(int) (day % ROAM_WORDS.length)]);
        if (keywords.isEmpty()) keywords.add("每日推荐");
        JSONArray items = gather(keywords, activePlatforms(), limit);
        return pack(items, "recommend_daily", seeds);
    }

    private JSONObject roam(JSONObject params) throws JSONException, PluginError {
        int limit = limitOf(params);
        long tick = System.currentTimeMillis() / 60000L;
        List<String> keywords = new ArrayList<>();
        for (int i = 0; i < 3; i++) {
            keywords.add(ROAM_WORDS[(int) ((tick + i * 5) % ROAM_WORDS.length)]);
        }
        JSONArray items = gather(keywords, activePlatforms(), limit);
        return pack(items, "roam", new JSONArray(keywords));
    }

    private JSONObject explore(JSONObject params) throws JSONException, PluginError {
        List<String> keywords = new ArrayList<>();
        String keyword = params.optString("keyword", params.optString("q", "")).trim();
        if (keyword.isEmpty()) throw new PluginError(1007, "探索需要关键词");
        keywords.add(keyword);
        String extra = params.optString("artist", "").trim();
        if (!extra.isEmpty()) keywords.add(extra);
        JSONArray items = gather(keywords, activePlatforms(), limitOf(params));
        return pack(items, "explore", new JSONArray(keywords));
    }

    private JSONObject similar(JSONObject params) throws JSONException, PluginError {
        String artist = params.optString("artist", params.optString("keyword", "")).trim();
        if (artist.isEmpty()) throw new PluginError(1007, "相似推荐需要歌手或歌曲名");
        List<String> keywords = new ArrayList<>();
        keywords.add(artist);
        keywords.add(artist + " 相似");
        JSONArray items = gather(keywords, activePlatforms(), limitOf(params));
        JSONArray seeds = new JSONArray();
        seeds.put(artist);
        return pack(items, "similar", seeds);
    }

    private JSONObject mood(JSONObject params) throws JSONException, PluginError {
        String mood = params.optString("mood", params.optString("keyword", "")).trim();
        String words = "";
        for (String[] entry : MOODS) {
            if (entry[0].equals(mood)) words = entry[2];
        }
        if (words.isEmpty()) {
            // 允许直接传中文情绪词
            words = mood.isEmpty() ? "轻音乐" : mood;
        }
        long tick = System.currentTimeMillis() / 60000L;
        String[] pool = words.split(" ");
        List<String> keywords = new ArrayList<>();
        for (int i = 0; i < 2 && pool.length > 0; i++) {
            keywords.add(pool[(int) ((tick + i * 3) % pool.length)]);
        }
        JSONArray items = gather(keywords, activePlatforms(), limitOf(params));
        JSONArray seeds = new JSONArray(keywords);
        return pack(items, "mood", seeds);
    }

    private JSONObject feed(JSONObject params) throws JSONException, PluginError {
        return daily(params);
    }

    private static int limitOf(JSONObject params) {
        return Math.min(80, Math.max(10, params.optInt("limit", 40)));
    }

    // ------------------------------------------------------------ 分发

    @Override
    public Object request(String op, JSONObject params) throws PluginError {
        try {
            switch (op) {
                case "modes": {
                    JSONObject out = new JSONObject();
                    JSONArray modes = new JSONArray();
                    for (String[] entry : MODES) {
                        JSONObject item = new JSONObject();
                        item.put("id", entry[0]);
                        item.put("label", entry[1]);
                        item.put("icon", entry[2]);
                        item.put("desc", entry[3]);
                        item.put("needs_input", entry[4]);
                        modes.put(item);
                    }
                    JSONArray moods = new JSONArray();
                    for (String[] entry : MOODS) {
                        JSONObject item = new JSONObject();
                        item.put("id", entry[0]);
                        item.put("label", entry[1]);
                        moods.put(item);
                    }
                    out.put("platform", PLATFORM);
                    out.put("modes", modes);
                    out.put("moods", moods);
                    JSONArray platforms = new JSONArray();
                    for (String platform : activePlatforms()) platforms.put(platform);
                    out.put("platforms", platforms);
                    return out;
                }
                case "preferences_get": {
                    JSONObject out = new JSONObject();
                    out.put("platform", PLATFORM);
                    out.put("seed_artists", seedArtists());
                    out.put("favorites", preferences().optJSONArray("favorites"));
                    out.put("excluded_platforms", preferences().optJSONArray("excluded_platforms"));
                    out.put("config", preferences().optJSONObject("config"));
                    return out;
                }
                case "preferences_set": {
                    if (params.has("seed_artists")) preferences().put("seed_artists", params.optJSONArray("seed_artists"));
                    if (params.has("favorites")) preferences().put("favorites", params.optJSONArray("favorites"));
                    if (params.has("excluded_platforms")) {
                        preferences().put("excluded_platforms", params.optJSONArray("excluded_platforms"));
                    }
                    JSONObject config = params.optJSONObject("config");
                    if (config != null) preferences().put("config", config);
                    savePreferences();
                    return preferences_get();
                }
                case "feedback": {
                    // 简化：把反馈计入偏好（喜欢 → 加入 seed_artists）
                    String action = params.optString("action", "");
                    String artist = params.optString("artist", "");
                    if ("like".equals(action) && !artist.isEmpty()) {
                        JSONArray seeds = seedArtists();
                        boolean exists = false;
                        for (int i = 0; i < seeds.length(); i++) {
                            if (artist.equals(seeds.optString(i))) exists = true;
                        }
                        if (!exists && seeds.length() < 20) {
                            seeds.put(artist);
                            preferences().put("seed_artists", seeds);
                            savePreferences();
                        }
                    }
                    JSONObject out = new JSONObject();
                    out.put("platform", PLATFORM);
                    out.put("ok", true);
                    return out;
                }
                case "settings": {
                    JSONObject out = new JSONObject();
                    out.put("platform", PLATFORM);
                    out.put("plugin_id", "recommend.assistant");
                    out.put("version", "1.0.0-java");
                    out.put("endpoint", "http://127.0.0.1:" + port);
                    JSONArray platforms = new JSONArray();
                    for (String platform : activePlatforms()) platforms.put(platform);
                    out.put("platforms", platforms);
                    out.put("values", preferences().optJSONObject("config"));
                    out.put("schema", new JSONArray());
                    return out;
                }
                case "recommend_daily": return daily(params);
                case "roam": return roam(params);
                case "explore": return explore(params);
                case "similar": return similar(params);
                case "mood": return mood(params);
                case "recommend_feed": return feed(params);
                default: throw new PluginError(1012, "该插件不支持 " + op);
            }
        } catch (PluginError e) {
            throw e;
        } catch (JSONException e) {
            throw new PluginError(1011, "构造响应失败：" + e.getMessage());
        } catch (Exception e) {
            throw new PluginError(1011, "推荐失败：" + e.getMessage());
        }
    }

    private Object preferences_get() throws JSONException {
        JSONObject out = new JSONObject();
        out.put("platform", PLATFORM);
        out.put("seed_artists", seedArtists());
        out.put("favorites", preferences().optJSONArray("favorites"));
        out.put("excluded_platforms", preferences().optJSONArray("excluded_platforms"));
        out.put("config", preferences().optJSONObject("config"));
        return out;
    }
}
