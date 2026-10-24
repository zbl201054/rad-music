#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Rad Music - 智能推荐插件（recommend.assistant）

一个「多音源聚合推荐」外部进程插件：宿主 radmusic_server 通过本地 HTTP JSON-RPC 调用它，
它再去拉起 / 调用真正的音源插件（网易云、B 站……），按用户偏好把候选曲目聚合、打分、排序后返回。

为什么要「自己再去调音源」而不是让宿主代劳？
    宿主是单线程处理请求的：如果推荐插件在处理宿主转发来的请求时，又回头调宿主去搜音源，
    会形成「宿主 → 推荐插件 → 宿主 → 音源」的嵌套调用并可能死锁。所以我们直接读取兄弟插件
    目录下的 plugin.json，拿到各音源的 endpoint，点对点地并行请求，干净且不会死锁。

启动：
    python plugins/recommend/main.py --port 8850
    python plugins/recommend/main.py --selftest        # 离线校验推荐引擎（无需音源/宿主）

协议（与 netease / bilibili 插件一致）：
    GET  /health          -> 200 {"ok": true}
    POST /request         -> {"op": "...", "params": {...}} -> {"code": 0, "message": "ok", "data": {...}}

支持的 op（与 plugin.json 的 capabilities 一致）：
    recommend_daily   每日推荐：基于收藏歌手 / 曲风 + 当日轮换的探索词
    roam              漫游：以新颖度优先的随机游走式发现，越听越陌生又不全是噪音
    explore           探索：给一个关键词 / 曲风 / 心情，跨音源聚合并按相关度排序
    similar           相似推荐：给一个歌手或歌曲，找风格相近的曲目
    mood              心情电台：chill / focus / energetic / nostalgia / fresh / happy / sad / sleep
    recommend_feed   推荐流：当日的每日推荐（带日内缓存）
    preferences_get / preferences_set   读写用户偏好（收藏歌手、曲风、音源权重、喜欢/不喜欢）
    feedback          like / dislike / play 反馈，用于调权
    modes            列出所有可用模式与元信息（供前端渲染）
    search           占位：返回空结果，避免污染宿主的全局搜索
"""

import argparse
import datetime
import hashlib
import json
import os
import random
import re
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "0.1.0"
HERE = os.path.dirname(os.path.abspath(__file__))
PLUGINS_DIR = os.path.dirname(HERE)          # plugins/
LOG_FILE = os.path.join(HERE, "plugin.log")
PREFS_FILE = os.path.join(HERE, "preferences.json")
SELF_ID = "recommend.assistant"
PLATFORM = "recommend"

DEFAULT_PORT = 8850
DEFAULT_TIMEOUT = 12
DEFAULT_DAILY_LIMIT = 30

# ----------------------------------------------------------------- 日志
LOG_LOCK = threading.Lock()


def log(message):
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    line = "[%s] %s\n" % (stamp, message)
    with LOG_LOCK:
        try:
            with open(LOG_FILE, "a", encoding="utf-8") as handle:
                handle.write(line)
        except OSError:
            pass


class PluginError(Exception):
    def __init__(self, code, message):
        Exception.__init__(self, message)
        self.code = code
        self.message = message


# ----------------------------------------------------------------- 工具
def sha_for(platform, item_id):
    return hashlib.sha256(("%s:%s" % (platform, item_id)).encode("utf-8")).hexdigest()


def as_int(value, default=0):
    try:
        if value is None or value == "":
            return default
        return int(float(value))
    except (TypeError, ValueError):
        return default


def as_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def norm_text(value):
    return re.sub(r"\s+", " ", str(value or "")).strip()


def seeds_for_day(seed_key=""):
    """基于日期（可叠加 seed_key）的稳定伪随机种子，保证「每日推荐」每天不同但当天稳定。"""
    today = datetime.date.today().isoformat()
    digest = hashlib.sha256((today + "|" + seed_key).encode("utf-8")).hexdigest()
    return int(digest[:16], 16)


# ----------------------------------------------------------------- 限流 / 缓存（与 netease 插件同款最小实现）
class RateLimiter:
    def __init__(self, qps):
        self.qps = max(1.0, float(qps))
        self._interval = 1.0 / self.qps
        self._next_at = 0.0
        self._lock = threading.Lock()

    def acquire(self):
        with self._lock:
            now = time.monotonic()
            wait = self._next_at - now
            if wait > 0:
                time.sleep(min(wait, 1.0))
                now = time.monotonic()
            self._next_at = max(now, self._next_at) + self._interval


# ----------------------------------------------------------------- 音源发现
class SourceRegistry:
    """扫描 plugins/*/plugin.json，找出真正的音源插件（type=source、http、支持 search）。"""

    def __init__(self):
        self._lock = threading.Lock()
        self._sources = {}          # platform -> {id, name, endpoint, capabilities}
        self.refresh()

    def refresh(self):
        found = {}
        try:
            for name in os.listdir(PLUGINS_DIR):
                dir_path = os.path.join(PLUGINS_DIR, name)
                manifest_path = os.path.join(dir_path, "plugin.json")
                if not os.path.isfile(manifest_path):
                    continue
                try:
                    with open(manifest_path, "r", encoding="utf-8") as handle:
                        manifest = json.load(handle)
                except (ValueError, OSError):
                    continue
                if manifest.get("id") == SELF_ID:
                    continue
                if manifest.get("type") != "source":
                    continue
                if manifest.get("transport") != "http":
                    continue
                endpoint = manifest.get("endpoint") or ""
                if not endpoint:
                    continue
                caps = manifest.get("capabilities") or []
                if "search" not in caps:
                    continue
                platforms = manifest.get("platforms") or []
                if not platforms:
                    continue
                for platform in platforms:
                    found[platform] = {
                        "id": manifest.get("id", ""),
                        "name": manifest.get("name", platform),
                        "endpoint": endpoint,
                        "capabilities": caps,
                    }
        except OSError as error:
            log("扫描音源插件目录失败: %s" % error)
        with self._lock:
            self._sources = found

    def platforms(self):
        with self._lock:
            return list(self._sources.keys())

    def get(self, platform):
        with self._lock:
            return self._sources.get(platform)

    def endpoints(self):
        with self._lock:
            return {p: s["endpoint"] for p, s in self._sources.items()}


# ----------------------------------------------------------------- 偏好存储
DEFAULT_PREFS = {
    "favorite_artists": [],
    "genres": [],
    "mood_weights": {"chill": 1.0, "focus": 0.8, "energetic": 0.6, "nostalgia": 0.5,
                     "fresh": 0.5, "happy": 0.4, "sad": 0.3, "sleep": 0.3},
    "source_weights": {},
    "excluded_platforms": [],
    "liked": [],
    "disliked": [],
    "config": {
        "daily_limit": DEFAULT_DAILY_LIMIT,
        "discovery_ratio": 0.5,
        "timeout_seconds": DEFAULT_TIMEOUT,
        "default_sources": "",
    },
}


class Preferences:
    def __init__(self):
        self._lock = threading.RLock()
        self.data = dict(DEFAULT_PREFS)
        self.data["config"] = dict(DEFAULT_PREFS["config"])
        self.data["mood_weights"] = dict(DEFAULT_PREFS["mood_weights"])
        self.load()

    def load(self):
        if not os.path.exists(PREFS_FILE):
            return
        try:
            with open(PREFS_FILE, "r", encoding="utf-8") as handle:
                saved = json.load(handle)
            if isinstance(saved, dict):
                for key in ("favorite_artists", "genres", "excluded_platforms", "liked", "disliked"):
                    if key in saved and isinstance(saved[key], list):
                        self.data[key] = saved[key]
                if isinstance(saved.get("mood_weights"), dict):
                    self.data["mood_weights"].update(saved["mood_weights"])
                if isinstance(saved.get("source_weights"), dict):
                    self.data["source_weights"] = saved["source_weights"]
                if isinstance(saved.get("config"), dict):
                    self.data["config"].update(saved["config"])
        except (ValueError, OSError) as error:
            log("读取偏好失败: %s" % error)

    def save(self):
        with self._lock:
            tmp = PREFS_FILE + ".tmp"
            try:
                with open(tmp, "w", encoding="utf-8") as handle:
                    json.dump(self.data, handle, ensure_ascii=False, indent=2)
                os.replace(tmp, PREFS_FILE)
            except OSError as error:
                log("写入偏好失败: %s" % error)

    def get(self):
        with self._lock:
            return {
                "favorite_artists": self.data["favorite_artists"],
                "genres": self.data["genres"],
                "mood_weights": self.data["mood_weights"],
                "source_weights": self.data["source_weights"],
                "excluded_platforms": self.data["excluded_platforms"],
                "config": self.data["config"],
                "liked_count": len(self.data["liked"]),
                "disliked_count": len(self.data["disliked"]),
            }

    def set(self, patch):
        changed = False
        with self._lock:
            for key in ("favorite_artists", "genres", "excluded_platforms"):
                if key in patch and isinstance(patch[key], list):
                    self.data[key] = patch[key]
                    changed = True
            if isinstance(patch.get("mood_weights"), dict):
                self.data["mood_weights"].update(patch["mood_weights"])
                changed = True
            if isinstance(patch.get("source_weights"), dict):
                self.data["source_weights"].update(patch["source_weights"])
                changed = True
            if isinstance(patch.get("config"), dict):
                self.data["config"].update(patch["config"])
                changed = True
        if changed:
            self.save()
        return self.get()

    def record_feedback(self, action, track):
        if not isinstance(track, dict):
            return
        entry = {
            "title": norm_text(track.get("title") or track.get("name")),
            "artist": norm_text(track.get("artist") or "/".join(track.get("artists") or [])),
            "platform": track.get("platform") or "",
            "item_id": track.get("item_id") or track.get("id") or "",
            "at": int(time.time()),
        }
        if not entry["title"] and not entry["artist"]:
            return
        with self._lock:
            if action == "like":
                self.data["liked"] = [e for e in self.data["liked"]
                                      if not _same_track(e, entry)]
                self.data["disliked"] = [e for e in self.data["disliked"]
                                         if not _same_track(e, entry)]
                self.data["liked"].insert(0, entry)
                self.data["liked"] = self.data["liked"][:500]
            elif action == "dislike":
                self.data["disliked"] = [e for e in self.data["disliked"]
                                         if not _same_track(e, entry)]
                self.data["liked"] = [e for e in self.data["liked"]
                                      if not _same_track(e, entry)]
                self.data["disliked"].insert(0, entry)
                self.data["disliked"] = self.data["disliked"][:500]
            elif action == "play":
                # 播放反馈只记最近播放，用于提升熟悉度
                self.data.setdefault("recent_plays", [])
                self.data["recent_plays"] = [e for e in self.data["recent_plays"]
                                             if not _same_track(e, entry)]
                self.data["recent_plays"].insert(0, entry)
                self.data["recent_plays"] = self.data["recent_plays"][:200]
            else:
                return
            self.save()

    def disliked_set(self):
        with self._lock:
            return {(norm_text(e.get("title")), norm_text(e.get("artist")))
                    for e in self.data["disliked"]}

    def liked_artists(self):
        with self._lock:
            return [norm_text(a) for a in self.data["favorite_artists"] if norm_text(a)]


def _same_track(a, b):
    return (norm_text(a.get("title")) == norm_text(b.get("title"))
            and norm_text(a.get("artist")) == norm_text(b.get("artist")))


# ----------------------------------------------------------------- 候选归一化
def normalize_item(raw, platform, source_name, seed_weight=0.0):
    if not isinstance(raw, dict):
        return None
    title = norm_text(raw.get("name") or raw.get("title"))
    if not title:
        return None
    artists_raw = raw.get("artists")
    if isinstance(artists_raw, list):
        artists = [norm_text(a) for a in artists_raw if norm_text(a)]
    else:
        artist = norm_text(raw.get("artist") or raw.get("author") or "")
        artists = [artist] if artist else []
    item_id = str(raw.get("item_id") or raw.get("id") or "").strip()
    if not item_id:
        return None
    duration = as_int(raw.get("duration_seconds") or raw.get("duration"))
    qualities = raw.get("qualities") if isinstance(raw.get("qualities"), list) else []
    quality = raw.get("quality") or (qualities[0] if qualities else {})
    return {
        "platform": platform,
        "source_name": source_name,
        "item_id": item_id,
        "id": item_id,
        "sha": raw.get("sha") or sha_for(platform, item_id),
        "title": title,
        "name": title,
        "artists": artists,
        "artist": " / ".join(artists),
        "album": norm_text(raw.get("album")),
        "duration": duration,
        "duration_seconds": duration,
        "pic": raw.get("pic") or raw.get("cover") or "",
        "available": raw.get("available", True) is not False,
        "reason": raw.get("reason") or "",
        "quality": quality,
        "qualities": qualities,
        "seed_weight": seed_weight,
        "group_key": (title.lower(), tuple(a.lower() for a in artists)),
    }


# ----------------------------------------------------------------- 推荐引擎
# 探索词池：漫游与每日推荐从中取样
DISCOVERY_POOL = [
    "华语金曲", "经典老歌", "民谣", "独立音乐", "后摇", "City Pop", "爵士", "蓝调",
    "轻音乐", "钢琴曲", "古风", "国风", "电子", "合成器", "蒸汽波", "Lo-Fi",
    "世界音乐", "电影原声", "动漫歌曲", "游戏音乐", "粤语金曲", "台语", "日语治愈",
    "英文经典", "乡村", "雷鬼", "放克", "灵魂乐", "氛围", "冥想", "雨声白噪音",
    "现场版", "翻唱", "纯人声", "阿卡贝拉", "说唱", "嘻哈", "金属", "后朋克",
]

# 心情电台种子词
MOOD_SEEDS = {
    "chill": ["轻音乐", "纯音乐", "钢琴", "Lo-Fi", "舒缓", "治愈", "雨天", "咖啡", "爵士"],
    "focus": ["纯音乐", "古典", "钢琴", "后摇", "ambient", "学习", "工作", "专注"],
    "energetic": ["摇滚", "电音", "电子", "燃", "舞曲", "节奏", "DJ", "说唱"],
    "nostalgia": ["经典老歌", "怀旧", "80年代", "90年代", "老歌", "青春", "粤语金曲"],
    "fresh": ["新歌", "最新", "飙升", "热歌", "首发", "本周新歌"],
    "happy": ["欢快", "流行", "甜歌", "轻快", "洗脑"],
    "sad": ["伤感", "情歌", "慢歌", "苦情", "emo"],
    "sleep": ["助眠", "白噪音", "轻音乐", "雨声", "睡前", "冥想"],
}
MOOD_LABELS = {
    "chill": "放松", "focus": "专注", "energetic": "活力", "nostalgia": "怀旧",
    "fresh": "新鲜", "happy": "快乐", "sad": "伤感", "sleep": "安眠",
}

# 模式元信息（前端 modes 用）
MODES = [
    {"id": "recommend_daily", "label": "每日推荐", "icon": "calendar",
     "desc": "根据你的收藏歌手与曲风，叠加当天轮换的探索词，每天不一样。", "needs_input": "none"},
    {"id": "roam", "label": "漫游", "icon": "globe",
     "desc": "以新颖度优先的随机游走，越听越陌生又不全是噪音。", "needs_input": "none"},
    {"id": "explore", "label": "探索", "icon": "compass",
     "desc": "输入关键词 / 曲风 / 心情，跨音源聚合并按相关度排序。", "needs_input": "keyword"},
    {"id": "similar", "label": "相似推荐", "icon": "link",
     "desc": "输入歌手或歌曲，找风格相近的曲目。", "needs_input": "artist"},
    {"id": "mood", "label": "心情电台", "icon": "heart",
     "desc": "选一种心情，自动编排契合该情绪的音乐。", "needs_input": "mood"},
]


class Recommender:
    def __init__(self, registry, prefs):
        self.registry = registry
        self.prefs = prefs
        self._daily_cache = {"date": "", "data": None}
        self._daily_lock = threading.Lock()
        self._source_limiter = RateLimiter(4)

    # ---------------------------------------------------------- 调音源
    def call_source(self, platform, op, params, timeout):
        source = self.registry.get(platform)
        if not source:
            return None
        endpoint = source["endpoint"]
        payload = json.dumps({"op": op, "params": params or {}}).encode("utf-8")
        url = endpoint.rstrip("/") + "/request"
        req = urllib.request.Request(
            url, data=payload,
            headers={"Content-Type": "application/json; charset=utf-8",
                     "Accept": "application/json"})
        self._source_limiter.acquire()
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                raw = resp.read()
        except (urllib.error.URLError, urllib.error.HTTPError, OSError, ValueError) as error:
            log("调用音源 %s 失败: %s" % (platform, error))
            return None
        try:
            parsed = json.loads(raw.decode("utf-8", "replace"))
        except ValueError:
            return None
        if not isinstance(parsed, dict) or parsed.get("code", 0) != 0:
            return None
        return parsed.get("data")

    def active_platforms(self, includes=None):
        all_platforms = self.registry.platforms()
        excluded = set(self.prefs.data["excluded_platforms"])
        if includes:
            wanted = [p.strip() for p in includes.split(",") if p.strip()]
            chosen = [p for p in wanted if p in all_platforms]
            if chosen:
                return [p for p in chosen if p not in excluded]
        configured = norm_text(self.prefs.data["config"].get("default_sources", ""))
        if configured:
            wanted = [p.strip() for p in configured.split(",") if p.strip()]
            chosen = [p for p in wanted if p in all_platforms]
            if chosen:
                return [p for p in chosen if p not in excluded]
        return [p for p in all_platforms if p not in excluded]

    def source_weight(self, platform):
        weights = self.prefs.data["source_weights"]
        if platform in weights:
            return as_float(weights[platform], 1.0)
        return 1.0

    # ---------------------------------------------------------- 取候选
    def gather(self, keywords_with_weights, platforms, per_source, timeout):
        """keywords_with_weights: [(keyword, weight), ...]；并发请求各音源。"""
        tasks = []
        for platform in platforms:
            source = self.registry.get(platform)
            if not source:
                continue
            for keyword, weight in keywords_with_weights:
                tasks.append((platform, source["name"], keyword, weight))
        results = []
        if not tasks:
            return results

        def run(task):
            platform, name, keyword, weight = task
            data = self.call_source(platform, "search", {
                "keyword": keyword, "page": 1, "page_size": per_source, "type": "song"}, timeout)
            if not data:
                return []
            items = data.get("items")
            if not isinstance(items, list):
                items = data.get("songs") or data.get("results") or []
            out = []
            for raw in items:
                item = normalize_item(raw, platform, name, weight)
                if item:
                    out.append(item)
            return out

        with ThreadPoolExecutor(max_workers=min(8, len(tasks))) as pool:
            for chunk in pool.map(run, tasks):
                results.extend(chunk)
        return results

    # ---------------------------------------------------------- 打分
    @staticmethod
    def _artist_hit(artists, title, album, fav_artists):
        hay = " ".join(artists).lower() + " " + title.lower() + " " + album.lower()
        best = 0.0
        for fav in fav_artists:
            fav_l = fav.lower()
            if not fav_l:
                continue
            if fav_l in hay:
                # 完整词匹配给更高分
                best = max(best, 3.0 if fav_l in " ".join(artists).lower() else 1.5)
        return best

    @staticmethod
    def _keyword_hits(item, keywords):
        hay = (item["title"] + " " + item["artist"] + " " + item["album"]).lower()
        return sum(1 for kw in keywords if kw and kw.lower() in hay)

    def dedupe(self, items, keep_best_quality=True):
        by_key = {}
        for item in items:
            key = item["group_key"]
            if key not in by_key:
                by_key[key] = item
            else:
                if keep_best_quality:
                    if self._quality_rank(item) > self._quality_rank(by_key[key]):
                        by_key[key] = item
        return list(by_key.values())

    @staticmethod
    def _quality_rank(item):
        order = ["hires", "lossless", "exhigh", "higher", "standard", "low"]
        q = item.get("quality") or {}
        if isinstance(q, dict):
            level = q.get("level") or q.get("quality_id") or ""
            if level in order:
                return len(order) - order.index(level)
        return 0

    # ---------------------------------------------------------- 各模式
    def daily(self, params, limit):
        prefs = self.prefs.get()
        fav_artists = self.prefs.liked_artists()
        genres = [norm_text(g) for g in prefs["genres"] if norm_text(g)]
        platforms = self.active_platforms(params.get("sources"))
        if not platforms:
            raise PluginError(1009, "没有可用的音源插件（请先启动 netease / bilibili 等）")

        rng = random.Random(seeds_for_day("daily"))
        seeds = []
        for artist in fav_artists[:8]:
            seeds.append((artist, 2.0))
        for genre in genres[:6]:
            seeds.append((genre, 1.2))
        # 当日轮换的探索词（2~3 个）
        disc = rng.sample(DISCOVERY_POOL, min(3, len(DISCOVERY_POOL)))
        for kw in disc:
            seeds.append((kw, 0.6))

        items = self.gather(seeds, platforms, 20, self._timeout())
        disliked = self.prefs.disliked_set()
        items = [it for it in items if (it["title"], it["artist"]) not in disliked]

        for it in items:
            score = it["seed_weight"] * 1.0
            score += self._artist_hit(it["artists"], it["title"], it["album"], fav_artists)
            score += 0.3 * self._keyword_hits(it, genres)
            score += 0.3 * self.source_weight(it["platform"])
            if not it["available"]:
                score -= 2.0
            score += rng.uniform(0, 0.6)        # 当日稳定抖动
            it["score"] = score

        items.sort(key=lambda x: x["score"], reverse=True)
        items = self.dedupe(items)
        items.sort(key=lambda x: x["score"], reverse=True)
        return self._package(items[:limit], "recommend_daily", [s[0] for s in seeds],
                             {"artists": fav_artists, "genres": genres})

    def roam(self, params, limit):
        prefs = self.prefs.get()
        fav_artists = self.prefs.liked_artists()
        platforms = self.active_platforms(params.get("sources"))
        if not platforms:
            raise PluginError(1009, "没有可用的音源插件（请先启动 netease / bilibili 等）")
        discovery = as_float(self.prefs.data["config"].get("discovery_ratio", 0.5), 0.5)

        seed_key = params.get("seed") or str(int(time.time() // 600))
        rng = random.Random(seeds_for_day(seed_key))
        # 漫游：以陌生曲风为主，穿插少量熟悉的歌手
        disc = rng.sample(DISCOVERY_POOL, min(10, len(DISCOVERY_POOL)))
        seeds = [(kw, 0.5) for kw in disc]
        for artist in fav_artists[:3]:
            seeds.append((artist, 0.4))

        items = self.gather(seeds, platforms, 18, self._timeout())
        disliked = self.prefs.disliked_set()
        items = [it for it in items if (it["title"], it["artist"]) not in disliked]

        seen_artists = {}
        for it in items:
            familiarity = self._artist_hit(it["artists"], it["title"], it["album"], fav_artists)
            keyword_novelty = 1.0 if self._keyword_hits(it, fav_artists + prefs["genres"]) == 0 else 0.0
            # 漫游核心：新颖度越高越好；太熟悉的降权
            score = 1.0 + discovery * 3.0 * keyword_novelty
            score += (1.0 - discovery) * familiarity * 0.5
            # 同歌手重复惩罚（越往后越狠）
            artist_key = it["artist"].lower()
            repeat = seen_artists.get(artist_key, 0)
            score -= repeat * 1.2
            seen_artists[artist_key] = repeat + 1
            score += 0.3 * self.source_weight(it["platform"])
            if not it["available"]:
                score -= 1.5
            score += rng.uniform(0, 0.8)
            it["score"] = score

        items.sort(key=lambda x: x["score"], reverse=True)
        items = self.dedupe(items)
        items.sort(key=lambda x: x["score"], reverse=True)
        return self._package(items[:limit], "roam", disc,
                             {"discovery_ratio": discovery, "seed": seed_key})

    def explore(self, params, limit):
        keyword = norm_text(params.get("keyword") or params.get("q") or "")
        mood = norm_text(params.get("mood") or "")
        if not keyword and mood and mood in MOOD_SEEDS:
            seeds = [(kw, 1.0) for kw in MOOD_SEEDS[mood]]
            keyword = MOOD_LABELS.get(mood, mood)
        elif not keyword:
            genres = [norm_text(g) for g in self.prefs.get()["genres"] if norm_text(g)]
            if genres:
                keyword = genres[0]
                seeds = [(keyword, 1.0)]
            else:
                seeds = [("华语金曲", 1.0)]
        else:
            seeds = [(keyword, 1.0)]
            if mood and mood in MOOD_SEEDS:
                for kw in MOOD_SEEDS[mood]:
                    seeds.append((kw, 0.4))

        platforms = self.active_platforms(params.get("sources"))
        if not platforms:
            raise PluginError(1009, "没有可用的音源插件（请先启动 netease / bilibili 等）")

        items = self.gather(seeds, platforms, 20, self._timeout())
        disliked = self.prefs.disliked_set()
        items = [it for it in items if (it["title"], it["artist"]) not in disliked]

        for it in items:
            score = 2.0 * self._keyword_hits(it, [keyword])
            score += 0.5 * self.source_weight(it["platform"])
            score += 0.2 * self._quality_rank(it)
            if not it["available"]:
                score -= 2.0
            it["score"] = score

        items = self.dedupe(items, keep_best_quality=True)
        items.sort(key=lambda x: x["score"], reverse=True)
        return self._package(items[:limit], "explore", [keyword], {"keyword": keyword, "mood": mood})

    def similar(self, params, limit):
        artist = norm_text(params.get("artist") or params.get("name") or "")
        title = norm_text(params.get("title") or params.get("track") or "")
        if not artist and not title:
            raise PluginError(1007, "相似推荐需要提供 artist 或 title")
        if not artist and title:
            # 先用标题搜一次，拿第一首的歌手
            platforms = self.active_platforms(params.get("sources"))
            for platform in platforms:
                data = self.call_source(platform, "search",
                                        {"keyword": title, "page": 1, "page_size": 5, "type": "song"},
                                        self._timeout())
                items = (data.get("items") if isinstance(data, dict) else None) or []
                for raw in items:
                    a = raw.get("artists") or []
                    if isinstance(a, list) and a:
                        artist = norm_text(a[0])
                        break
                if artist:
                    break
        if not artist:
            raise PluginError(1004, "无法从「%s」解析出歌手" % title)

        platforms = self.active_platforms(params.get("sources"))
        if not platforms:
            raise PluginError(1009, "没有可用的音源插件")
        seeds = [(artist, 2.0)]
        for genre in [norm_text(g) for g in self.prefs.get()["genres"] if norm_text(g)][:3]:
            seeds.append((genre, 0.6))

        items = self.gather(seeds, platforms, 20, self._timeout())
        disliked = self.prefs.disliked_set()
        items = [it for it in items if (it["title"], it["artist"]) not in disliked]

        for it in items:
            score = 0.0
            if artist.lower() in it["artist"].lower():
                score += 3.0
            score += 0.4 * self.source_weight(it["platform"])
            if not it["available"]:
                score -= 2.0
            it["score"] = score

        items = self.dedupe(items)
        # 把该歌手自己的歌排前面
        items.sort(key=lambda x: x["score"], reverse=True)
        return self._package(items[:limit], "similar", [artist], {"artist": artist})

    def mood(self, params, limit):
        mood = norm_text(params.get("mood") or "")
        if mood not in MOOD_SEEDS:
            raise PluginError(1007, "未知心情：%s（可选 %s）" % (mood, "/".join(MOOD_SEEDS.keys())))
        platforms = self.active_platforms(params.get("sources"))
        if not platforms:
            raise PluginError(1009, "没有可用的音源插件")
        seeds = [(kw, 1.0) for kw in MOOD_SEEDS[mood]]
        items = self.gather(seeds, platforms, 20, self._timeout())
        disliked = self.prefs.disliked_set()
        items = [it for it in items if (it["title"], it["artist"]) not in disliked]
        for it in items:
            score = 1.0 * self._keyword_hits(it, MOOD_SEEDS[mood])
            score += 0.4 * self.source_weight(it["platform"])
            score += 0.15 * self._quality_rank(it)
            if not it["available"]:
                score -= 2.0
            it["score"] = score
        items = self.dedupe(items)
        items.sort(key=lambda x: x["score"], reverse=True)
        return self._package(items[:limit], "mood", MOOD_SEEDS[mood],
                             {"mood": mood, "mood_label": MOOD_LABELS.get(mood, mood)})

    def feed(self, params, limit):
        today = datetime.date.today().isoformat()
        with self._daily_lock:
            if self._daily_cache["date"] == today and self._daily_cache["data"]:
                return self._daily_cache["data"]
        data = self.daily(params, limit)
        with self._daily_lock:
            self._daily_cache = {"date": today, "data": data}
        return data

    def _timeout(self):
        return max(3, as_int(self.prefs.data["config"].get("timeout_seconds"), DEFAULT_TIMEOUT))

    def _package(self, items, mode, seeds, meta):
        out_items = []
        for index, it in enumerate(items):
            out_items.append({
                "platform": it["platform"],
                "source_name": it["source_name"],
                "item_id": it["item_id"],
                "id": it["id"],
                "sha": it["sha"],
                "title": it["title"],
                "name": it["title"],
                "artists": it["artists"],
                "artist": it["artist"],
                "album": it["album"],
                "duration": it["duration"],
                "duration_seconds": it["duration_seconds"],
                "pic": it["pic"],
                "available": it["available"],
                "reason": it["reason"],
                "quality": it["quality"],
                "qualities": it["qualities"],
                "score": round(it.get("score", 0.0), 3),
            })
        return {
            "mode": mode,
            "items": out_items,
            "count": len(out_items),
            "seeds": seeds,
            "generated_at": int(time.time()),
            "platforms": self.active_platforms(),
            "meta": meta or {},
        }


# ----------------------------------------------------------------- HTTP
def ok(data):
    return {"code": 0, "message": "ok", "data": data}


def fail(code, message):
    return {"code": code, "message": message}


class Handler(BaseHTTPRequestHandler):
    registry = None
    prefs = None
    recommender = None
    server_version = "RadMusicRecommend/" + VERSION

    def log_message(self, fmt, *args):
        return  # 静默（日志走 plugin.log）

    def _send(self, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.split("?")[0] in ("/health", "/"):
            self._send({"ok": True, "id": SELF_ID, "name": "智能推荐",
                        "version": VERSION, "platforms": self.registry.platforms()})
        else:
            self._send(fail(1012, "unsupported method/path"))

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b"{}"
        try:
            payload = json.loads(raw.decode("utf-8", "replace"))
        except ValueError:
            payload = {}
        if not isinstance(payload, dict):
            payload = {}
        op = norm_text(payload.get("op") or "")
        params = payload.get("params")
        if not isinstance(params, dict):
            params = {}
        try:
            self._send(self.route(op, params))
        except PluginError as error:
            self._send(fail(error.code, error.message))
        except Exception as error:  # noqa: BLE001
            log("处理 op=%s 异常: %s" % (op, error))
            self._send(fail(1011, "推荐插件内部错误：%s" % error))

    def route(self, op, params):
        # 占位：宿主全局搜索会顺带问每个源，返回空结果即可，不污染搜索
        if op == "search":
            return ok({"items": [], "keyword": "", "page": 1, "page_size": 0,
                       "total": 0, "has_more": False, "type": "song",
                       "note": "推荐插件不提供普通搜索"})

        limit = max(5, min(80, as_int(params.get("limit"),
                                      self.prefs.data["config"].get("daily_limit", DEFAULT_DAILY_LIMIT))))

        if op == "recommend_daily":
            return ok(self.recommender.daily(params, limit))
        if op == "roam":
            return ok(self.recommender.roam(params, limit))
        if op == "explore":
            return ok(self.recommender.explore(params, limit))
        if op == "similar":
            return ok(self.recommender.similar(params, limit))
        if op == "mood":
            return ok(self.recommender.mood(params, limit))
        if op == "recommend_feed":
            return ok(self.recommender.feed(params, limit))
        if op == "preferences_get":
            return ok(self.prefs.get())
        if op == "preferences_set":
            return ok(self.prefs.set(params))
        if op == "feedback":
            action = norm_text(params.get("action") or "")
            if action not in ("like", "dislike", "play"):
                raise PluginError(1007, "feedback.action 必须是 like/dislike/play")
            self.prefs.record_feedback(action, params.get("track"))
            return ok({"ok": True, "action": action})
        if op == "modes":
            return ok({
                "platform": PLATFORM,
                "modes": MODES,
                "moods": [{"id": k, "label": MOOD_LABELS.get(k, k)} for k in MOOD_SEEDS.keys()],
                "platforms": self.registry.platforms(),
            })
        if op == "settings":
            values = dict(self.prefs.data["config"])
            return ok({
                "platform": PLATFORM,
                "plugin_id": SELF_ID,
                "version": VERSION,
                "endpoint": "http://127.0.0.1:%d" % getattr(self.server, "port", DEFAULT_PORT),
                "values": values,
                "schema": [
                    {"key": "daily_limit", "label": "每日推荐条数", "type": "int",
                     "min": 10, "max": 60, "default": DEFAULT_DAILY_LIMIT},
                    {"key": "discovery_ratio", "label": "漫游新颖度 (0-1)", "type": "float",
                     "min": 0, "max": 1, "default": 0.5},
                    {"key": "timeout_seconds", "label": "单源超时（秒）", "type": "int",
                     "min": 3, "max": 30, "default": DEFAULT_TIMEOUT},
                    {"key": "default_sources", "label": "默认音源（逗号分隔，留空=全部）",
                     "type": "string", "default": ""},
                ],
            })
        raise PluginError(1012, "不支持的 op：%s" % op)


# ----------------------------------------------------------------- 自检
def selftest():
    print("== 推荐插件离线自检 ==")
    registry = SourceRegistry()
    # 注入两个假音源，模拟 netease / bilibili
    registry._sources = {
        "netease": {"id": "netease.source", "name": "网易云", "endpoint": "http://127.0.0.1:1",
                    "capabilities": ["search"]},
        "bilibili": {"id": "bilibili.source", "name": "B站", "endpoint": "http://127.0.0.1:1",
                     "capabilities": ["search"]},
    }
    prefs = Preferences()
    prefs.set({"favorite_artists": ["周杰伦", "Taylor Swift"],
               "genres": ["流行", "摇滚"],
               "config": {"daily_limit": 12, "discovery_ratio": 0.6}})

    # 用假数据替换 call_source，避免真实网络
    class FakeRecommender(Recommender):
        def call_source(self, platform, op, params, timeout):
            kw = (params or {}).get("keyword", "")
            base = [
                {"name": "晴天", "artists": ["周杰伦"], "album": "叶惠美", "duration_seconds": 269,
                 "item_id": "1", "available": True, "qualities": []},
                {"name": "七里香", "artists": ["周杰伦"], "album": "七里香", "duration_seconds": 299,
                 "item_id": "2", "available": True, "qualities": []},
                {"name": "Blank Space", "artists": ["Taylor Swift"], "album": "1989",
                 "duration_seconds": 271, "item_id": "3", "available": True, "qualities": []},
                {"name": "流行金曲A", "artists": ["某歌手"], "album": "合辑", "duration_seconds": 240,
                 "item_id": "4", "available": True, "qualities": []},
                {"name": "摇滚灵魂", "artists": ["乐队X"], "album": "现场", "duration_seconds": 312,
                 "item_id": "5", "available": True, "qualities": []},
                {"name": "轻音乐小曲", "artists": ["钢琴家"], "album": "治愈", "duration_seconds": 180,
                 "item_id": "6", "available": True, "qualities": []},
            ]
            # 关键词命中才返回，模拟搜索
            out = []
            for i, b in enumerate(base):
                if kw and (kw in b["name"] or kw in " ".join(b["artists"]) or kw in b["album"]):
                    item = dict(b)
                    item["item_id"] = "%s_%d" % (platform, i)
                    out.append(item)
            return {"items": out}

    rec = FakeRecommender(registry, prefs)
    for mode, fn in (("recommend_daily", lambda: rec.daily({}, 12)),
                     ("roam", lambda: rec.roam({}, 12)),
                     ("explore", lambda: rec.explore({"keyword": "周杰伦"}, 12)),
                     ("similar", lambda: rec.similar({"artist": "周杰伦"}, 12)),
                     ("mood", lambda: rec.mood({"mood": "chill"}, 12))):
        try:
            data = fn()
            print("  [%s] 返回 %d 首，种子=%s" % (mode, data["count"], data["seeds"][:4]))
            assert data["count"] >= 0
        except Exception as error:  # noqa: BLE001
            print("  [%s] 失败: %s" % (mode, error))
            return 1
    print("  loop done")
    try:
        prefs.record_feedback("like", {"title": "晴天", "artist": "周杰伦", "platform": "netease", "item_id": "1"})
        print("  [feedback] liked=%d" % prefs.get()["liked_count"])
    except Exception as error:  # noqa: BLE001
        print("  [feedback] 失败: %s" % error)
        import traceback as _tb
        _tb.print_exc()
        return 1
    print("SELFTEST OK")
    return 0


# ----------------------------------------------------------------- 主入口
def main():
    parser = argparse.ArgumentParser(description="Rad Music 智能推荐插件")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--selftest", action="store_true", help="离线校验推荐引擎")
    args = parser.parse_args()

    if args.selftest:
        raise SystemExit(selftest())

    registry = SourceRegistry()
    prefs = Preferences()
    recommender = Recommender(registry, prefs)

    Handler.registry = registry
    Handler.prefs = prefs
    Handler.recommender = recommender

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.port = args.port
    log("智能推荐插件已启动：%s:%d，发现音源 %s" % (args.host, args.port, registry.platforms()))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("插件已停止")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
