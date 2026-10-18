#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Rad Music - 网易云音乐播放源插件

这是一个「外部进程播放源插件」：宿主 radmusic_server 通过本地 HTTP JSON-RPC 调用它，
它再用 HTTPS 访问网易云音乐的公开 Web API，把结果翻译成 Rad Music 的模型（Readme 5.2 / 5.5）。

启动（宿主会自动拉起，也可手动）：
    python plugins/netease/main.py --port 8848

协议：
    GET  /health          → 200 {"ok": true}
    GET  /login           → 扫码 / Cookie 登录页面
    POST /request         → {"op": "...", "params": {...}} → {"code": 0, "data": {...}}

支持的 op（与 plugin.json 的 capabilities 一致）：
    search       关键词搜索（type=song 单曲；type=artist 返回该歌手热门歌曲）
    get_song     曲目元数据（base.json）
    get_url      播放地址（按音质，真实回传降级后的音质）
    get_qualities 曲目当前账号可用音质
    get_lyric    歌词（含翻译 / 罗马音 / 逐字）
    get_pic      封面
    get_playlist 歌单详情与曲目（playlist_sync 用）
    get_album    专辑详情与曲目（album_sync 用）
    search_artist 歌手检索（结构化：id / 简介 / 专辑数等）
    get_artist   歌手详情 + 热门歌曲 + 专辑
    search_mv    搜索 MV（视频源）
    get_mv       MV 播放地址与分辨率
    check_login  当前登录态
    login_qr_start / login_qr_check  扫码登录
    login_cookie 导入 Cookie 登录
    login_phone  手机号登录（网易云多已要求加密，失败会提示改用扫码）
    logout       退出登录并清除 cookie.txt

登录（可选，用于提升音质与解锁 VIP 曲目）：
    1) 环境变量   export RAD_NETEASE_COOKIE="MUSIC_U=..."
    2) cookie.txt 本目录下整行 Cookie 字符串
    3) config.json 里写 {"cookie": "..."}
    Cookie 优先级：环境变量 > cookie.txt > config.json
"""

import argparse
import calendar
import collections
import hashlib
import json
import os
import re
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "0.14.0"

BASE = "https://music.163.com"
USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
)
HERE = os.path.dirname(os.path.abspath(__file__))
LOG_FILE = os.path.join(HERE, "plugin.log")

# 音质定义：id 与宿主清单 plugin.json 的 quality_levels 一一对应（Readme 5.4）
QUALITY_SPECS = {
    "standard": {"label": "标准", "level": "low", "codec": "mp3", "lossless": False,
                 "bitrate": 128, "sample_rate": 44100, "channels": 2},
    "higher": {"label": "较高", "level": "medium", "codec": "mp3", "lossless": False,
               "bitrate": 192, "sample_rate": 44100, "channels": 2},
    "exhigh": {"label": "极高", "level": "high", "codec": "mp3", "lossless": False,
               "bitrate": 320, "sample_rate": 44100, "channels": 2},
    "lossless": {"label": "无损", "level": "lossless", "codec": "flac", "lossless": True,
                 "bitrate": 999, "sample_rate": 44100, "bit_depth": 16, "channels": 2},
    "hires": {"label": "Hi-Res", "level": "hires", "codec": "flac", "lossless": True,
              "bitrate": 1500, "sample_rate": 96000, "bit_depth": 24, "channels": 2},
}
# 高 → 低
QUALITY_ORDER = ["hires", "lossless", "exhigh", "higher", "standard"]
QUALITY_RANK = {value: len(QUALITY_ORDER) - index for index, value in enumerate(QUALITY_ORDER)}

# 宿主可能把归一化 level（Readme 1.1.1）或历史码率当音质传进来，统一映射到 quality_id
QUALITY_ALIASES = {
    "hires": "hires", "hi-res": "hires", "hi_res": "hires", "hi res": "hires",
    "jymaster": "hires", "master": "hires",
    "lossless": "lossless", "sq": "lossless", "flac": "lossless",
    "sky": "lossless", "dolby": "lossless", "jyeffect": "lossless",
    "exhigh": "exhigh", "high": "exhigh", "320": "exhigh", "320k": "exhigh", "320000": "exhigh",
    "higher": "higher", "medium": "higher", "192": "higher", "192k": "higher", "192000": "higher",
    "standard": "standard", "low": "standard", "128": "standard", "128k": "standard", "128000": "standard",
}

# 网易云 privilege.plLevel → 可播放到哪一档（与 QUALITY_ORDER 对应）
PL_LEVEL_RANK = {
    "none": 0,
    "standard": 1, "low": 1,
    "higher": 2, "medium": 2,
    "exhigh": 3, "high": 3,
    "lossless": 4, "sky": 4, "dolby": 4, "jyeffect": 4,
    "hires": 5, "jymaster": 5, "master": 5,
}

DETAIL_TTL = 600.0        # 曲目详情 / 权限
URL_TTL = 120.0           # 播放地址（网易云地址数小时后过期，这里只做短期去重）
LYRIC_TTL = 1800.0        # 歌词
COLLECTION_TTL = 300.0    # 歌单 / 专辑
MV_TTL = 300.0            # MV 详情

DETAIL_CHUNK = 200        # 批量取详情时每批 id 数

MV_RESOLUTIONS = {"1080": (1920, 1080), "720": (1280, 720), "480": (854, 480), "240": (426, 240)}

OP_ALIASES = {
    "song": "get_song",
    "url": "get_url",
    "quality": "get_qualities",
    "qualities": "get_qualities",
    "lyric": "get_lyric",
    "pic": "get_pic",
    "cover": "get_pic",
    "playlist": "get_playlist",
    "album": "get_album",
    "search_playlist": "search_playlist",
    "search_playlists": "search_playlist",
    "search_album": "search_album",
    "search_albums": "search_album",
    "mv": "get_mv",
    "mv_search": "search_mv",
    "artist_search": "search_artist",
    "singer_search": "search_artist",
    "artist": "get_artist",
    "singer": "get_artist",
    "login": "check_login",
    "account": "check_login",
    "qrcode": "login_qr_start",
    "qr_start": "login_qr_start",
    "qr_check": "login_qr_check",
    "login_qr": "login_qr_check",
    "set_cookie": "login_cookie",
    "login_by_cookie": "login_cookie",
    "login_cellphone": "login_phone",
    "sign_out": "logout",
    # 暴露给宿主设置面板（仅读，附带登录态 / 版本）
    "settings": "settings_get",
}

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


def quality_json(quality_id):
    spec = QUALITY_SPECS.get(quality_id, QUALITY_SPECS["standard"])
    item = {"quality_id": quality_id, "label": spec["label"]}
    item.update({key: value for key, value in spec.items() if key != "label"})
    return item


def normalize_quality(value):
    """把宿主传来的 quality_id / 归一化 level / 历史码率统一成 quality_id；无法识别返回 None"""
    if value is None:
        return None
    text = str(value).strip().lower()
    if not text:
        return None
    if text in QUALITY_SPECS:
        return text
    return QUALITY_ALIASES.get(text)


def sha_for(item_id):
    return hashlib.sha256(("netease:" + str(item_id)).encode("utf-8")).hexdigest()


def mv_sha_for(mv_id):
    # MV 与同数字曲目 id 会撞车，加前缀区分（Readme 1.13 规则 1）
    return hashlib.sha256(("netease:mv:" + str(mv_id)).encode("utf-8")).hexdigest()


def normalize_cover(url):
    """封面统一 https，并去掉网易云 0y0 占位参数"""
    if not url:
        return ""
    text = str(url).strip()
    if text.startswith("http://"):
        text = "https://" + text[len("http://"):]
    if text.endswith("?param=0y0"):
        text = text[:-len("?param=0y0")]
    return text


def normalize_media_url(url):
    """音视频地址统一 https（浏览器混流安全 / 更少被运营商劫持）"""
    if not url:
        return ""
    text = str(url).strip()
    if text.startswith("http://"):
        text = "https://" + text[len("http://"):]
    return text


EXPIRY_PATTERN = re.compile(r"/(\d{14})/")


def media_expiry(url):
    """从网易云地址里的 /20261017123254/ 时间戳推算过期时间（北京时间）"""
    match = EXPIRY_PATTERN.search(url or "")
    if not match:
        return 0
    try:
        stamp = time.strptime(match.group(1), "%Y%m%d%H%M%S")
    except ValueError:
        return 0
    return int(calendar.timegm(stamp) - 8 * 3600)


def as_int(value, default=0):
    try:
        if value is None or value == "":
            return default
        return int(float(value))
    except (TypeError, ValueError):
        return default


def mv_id_of(song):
    raw = song.get("mv") if song.get("mv") is not None else song.get("mvid")
    text = str(raw or "").strip()
    return text if text and text != "0" else ""


def artist_names(entries):
    names = []
    for entry in entries or []:
        if not isinstance(entry, dict):
            continue
        name = entry.get("name") or ""
        if name:
            names.append(name)
    return names


def seconds_of(milliseconds):
    return int(round(as_int(milliseconds) / 1000.0))


def normalize_numeric_id(raw, extra_prefixes=()):
    """从裸 id / netease_123 / netease:123 / URL 里取出数字 id"""
    text = str(raw or "").strip()
    if not text:
        return ""
    if text.isdigit():
        return text
    match = re.search(r"(?:^|[?&/])(?:id|ids)=?(\d{3,})", text)
    if match:
        return match.group(1)
    for prefix in extra_prefixes + ("netease:", "netease_", "netease-"):
        if text.lower().startswith(prefix.lower()):
            text = text[len(prefix):]
            break
    match = re.search(r"(\d{3,})", text)
    return match.group(1) if match else ""


# ---------------------------------------------------------------- 歌词解析

LRC_STAMP = re.compile(r"\[(\d+):(\d+)(?:[.:](\d+))?\]")
YRC_LINE = re.compile(r"\[(\d+),(\d+)\]")
YRC_WORD = re.compile(r"\((\d+),(\d+),\d+\)")


def parse_lrc(text):
    """LRC → [{time_ms, text}]（同一行多时间戳会展开）"""
    lines = []
    if not text:
        return lines
    for raw in str(text).splitlines():
        stamps = list(LRC_STAMP.finditer(raw))
        if not stamps:
            continue
        content = raw[stamps[-1].end():].strip()
        for stamp in stamps:
            minutes = int(stamp.group(1))
            seconds = int(stamp.group(2))
            fraction = stamp.group(3) or "0"
            if len(fraction) == 1:
                millis = int(fraction) * 100
            elif len(fraction) == 2:
                millis = int(fraction) * 10
            else:
                millis = int(fraction[:3])
            lines.append({"time_ms": minutes * 60000 + seconds * 1000 + millis, "text": content})
    lines.sort(key=lambda item: item["time_ms"])
    return lines


def parse_yrc(text):
    """逐字歌词 YRC → [{time_ms, text, words:[{t, text}]}]"""
    lines = []
    if not text:
        return lines
    for raw in str(text).splitlines():
        raw = raw.strip()
        if not raw:
            continue
        line_match = YRC_LINE.match(raw)
        if not line_match:
            continue
        start = int(line_match.group(1))
        body = raw[line_match.end():]
        words = []
        cursor = 0
        for match in YRC_WORD.finditer(body):
            segment = body[cursor:match.start()]
            if segment:
                words.append({"t": int(match.group(1)), "text": segment})
            cursor = match.end()
        tail = body[cursor:]
        if tail:
            words.append({"t": words[-1]["t"] if words else start, "text": tail})
        line_text = "".join(word["text"] for word in words).strip()
        if not line_text:
            continue
        lines.append({"time_ms": start, "text": line_text, "words": words})
    lines.sort(key=lambda item: item["time_ms"])
    return lines


def merge_translation(lines, extra_lines, field):
    """按时间戳把翻译 / 罗马音合并进主歌词（±50ms）"""
    if not extra_lines:
        return 0
    merged = 0
    for line in lines:
        for extra in extra_lines:
            if abs(extra["time_ms"] - line["time_ms"]) <= 50 and extra["text"]:
                if field not in line:
                    line[field] = extra["text"]
                    merged += 1
                break
    return merged


def squash_text(text):
    """去掉所有空白，用于比对 LRC 与 YRC 的同一行（两者空格规则不同）"""
    return re.sub(r"\s+", "", str(text or ""))


def merge_words(lines, yrc_lines):
    """把逐字歌词贴到主歌词上

    YRC 的行时间戳指向「第一个字的起点」，比 LRC 行时间戳晚几百毫秒，不能按固定小窗口匹配；
    这里先按「去空白后的文本」索引，再在 3 秒内取时间最接近的一条（副歌重复也能对上）。
    """
    if not yrc_lines:
        return 0
    index = {}
    for item in yrc_lines:
        if not item.get("words"):
            continue
        index.setdefault(squash_text(item["text"]), []).append(item)
    merged = 0
    for line in lines:
        candidates = index.get(squash_text(line.get("text")))
        if not candidates:
            continue
        best = None
        best_gap = None
        for item in candidates:
            gap = abs(item["time_ms"] - line["time_ms"])
            if best_gap is None or gap < best_gap:
                best = item
                best_gap = gap
        if best is not None and best_gap is not None and best_gap <= 3000:
            line["words"] = best["words"]
            merged += 1
    return merged


# ---------------------------------------------------------------- 基础设施

class PluginError(Exception):
    """带 Readme 第 3 节错误码的插件异常"""

    def __init__(self, code, message):
        Exception.__init__(self, message)
        self.code = code
        self.message = message


class TTLCache:
    """最小化的 TTL + LRU 缓存（线程安全）"""

    def __init__(self, max_entries=2048):
        self._data = collections.OrderedDict()
        self._lock = threading.Lock()
        self._max = max_entries

    def get(self, key):
        now = time.time()
        with self._lock:
            item = self._data.get(key)
            if item is None:
                return None
            expire_at, value = item
            if expire_at <= now:
                self._data.pop(key, None)
                return None
            self._data.move_to_end(key)
            return value

    def set(self, key, value, ttl):
        if ttl <= 0:
            return
        with self._lock:
            self._data[key] = (time.time() + ttl, value)
            self._data.move_to_end(key)
            while len(self._data) > self._max:
                self._data.popitem(last=False)

    def invalidate(self, prefix=""):
        with self._lock:
            if not prefix:
                self._data.clear()
                return
            for key in [k for k in self._data if str(k).startswith(prefix)]:
                self._data.pop(key, None)


class RateLimiter:
    """简单匀速限流：避免短时间打太猛被网易云风控（code 460）"""

    def __init__(self, qps):
        qps = max(1.0, float(qps or 1.0))
        self.qps = qps
        self._interval = 1.0 / qps
        self._next_at = 0.0
        self._lock = threading.Lock()

    def acquire(self):
        with self._lock:
            now = time.monotonic()
            wait = self._next_at - now
            if wait > 0:
                # 最多等 1.2s，宁可稍微超一点也不要让请求长时间挂住
                time.sleep(min(wait, 1.2))
                now = time.monotonic()
            self._next_at = max(now, self._next_at) + self._interval


def parse_payload(raw):
    if not raw:
        return None
    try:
        payload = json.loads(raw.decode("utf-8", "replace"))
    except (ValueError, UnicodeDecodeError):
        return None
    return payload if isinstance(payload, dict) else None


def netease_failure_note(code, fee=0):
    """把网易云业务码翻译成人话"""
    if code == -110:
        if fee in (1, 4):
            return "该曲目需要黑胶 VIP / 已购专辑（-110）"
        return "当前账号无权播放该音质（-110），可尝试标准音质或配置登录 Cookie"
    if code == 404:
        return "该曲目无版权或已下架（404）"
    if code in (301, -462):
        return "需要登录网易云账号（code=%s）" % code
    if code in (460, -460):
        return "网易云风控校验未通过（code=%s），请稍后重试或配置 Cookie" % code
    return "网易云拒绝提供地址（code=%s）" % code


COOKIE_FILE = os.path.join(HERE, "cookie.txt")
COOKIE_ATTRS = {"path", "domain", "expires", "max-age", "secure", "httponly",
                "samesite", "version", "comment", "priority"}


class CookieJar(object):
    """最小 Cookie 容器：解析 Cookie 字符串、吸收 Set-Cookie、序列化回请求头"""

    def __init__(self, text=""):
        self._lock = threading.Lock()
        self._pairs = collections.OrderedDict()
        self.replace(text)

    def replace(self, text):
        pairs = collections.OrderedDict()
        for chunk in str(text or "").replace("\n", ";").split(";"):
            chunk = chunk.strip()
            if not chunk or "=" not in chunk:
                continue
            name, value = chunk.split("=", 1)
            name = name.strip()
            if not name or name.lower() in COOKIE_ATTRS:
                continue
            pairs[name] = value.strip()
        with self._lock:
            self._pairs = pairs

    def merge_headers(self, header_values):
        """吸收响应里的 Set-Cookie（同名覆盖，空值视为删除）"""
        if not header_values:
            return
        with self._lock:
            for raw in header_values:
                chunk = str(raw or "").split(";")[0].strip()
                if not chunk or "=" not in chunk:
                    continue
                name, value = chunk.split("=", 1)
                name, value = name.strip(), value.strip()
                if not name or name.lower() in COOKIE_ATTRS:
                    continue
                if value:
                    self._pairs[name] = value
                else:
                    self._pairs.pop(name, None)

    def to_string(self):
        with self._lock:
            return "; ".join("%s=%s" % (name, value) for name, value in self._pairs.items())

    def names(self):
        with self._lock:
            return list(self._pairs.keys())

    def get(self, name):
        with self._lock:
            return self._pairs.get(name, "")

    def clear(self):
        with self._lock:
            self._pairs.clear()


def save_cookie_file(text):
    """原子写入 cookie.txt（先写 .tmp 再替换），失败只记录不抛"""
    body = str(text or "").strip()
    tmp = COOKIE_FILE + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as handle:
            handle.write(body + "\n" if body else "")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, COOKIE_FILE)
        return True
    except OSError as error:
        log("写入 cookie.txt 失败: %s" % error)
        try:
            os.remove(tmp)
        except OSError:
            pass
        return False


# ---------------------------------------------------------------- 网易云客户端

class NeteaseClient(object):
    def __init__(self, cookie="", qps=6, timeout=20, retries=2):
        self.cookies = CookieJar(cookie)
        self.timeout = max(5, as_int(timeout, 20))
        self.retries = max(0, as_int(retries, 2))
        self.limiter = RateLimiter(qps)
        self._details = TTLCache(4096)
        self._urls = TTLCache(512)
        self._lyrics = TTLCache(256)
        self._collections = TTLCache(128)
        self._mvs = TTLCache(256)
        self._account = {"expire_at": 0.0, "value": None}
        self._account_lock = threading.Lock()
        # 暴露给设置面板（不含 cookie，cookie 由专用 op 管理）
        self.config = {
            "qps": self.limiter.qps,
            "timeout_seconds": self.timeout,
            "retries": self.retries,
        }

    @property
    def cookie(self):
        return self.cookies.to_string()

    # ------------------------------------------------------------ HTTP
    def _headers(self):
        headers = {
            "User-Agent": USER_AGENT,
            "Referer": BASE + "/",
            "Accept": "application/json, text/plain, */*",
            "Accept-Language": "zh-CN,zh;q=0.9",
        }
        cookie_text = self.cookies.to_string()
        if cookie_text:
            headers["Cookie"] = cookie_text
        return headers

    def _http(self, path, params=None, form=None, action="请求", validate=True):
        """一次网易云调用；validate=False 时由调用方自己判断业务码（登录轮询用）"""
        url = BASE + path
        if params:
            url += "?" + urllib.parse.urlencode(params, doseq=True)
        data = None
        if form is not None:
            data = urllib.parse.urlencode(form, doseq=True).encode("utf-8")
        last_error = None
        attempt = 0
        while attempt <= self.retries:
            attempt += 1
            self.limiter.acquire()
            headers = self._headers()
            if data is not None:
                headers["Content-Type"] = "application/x-www-form-urlencoded; charset=utf-8"
            request = urllib.request.Request(url, data=data, headers=headers)
            try:
                with urllib.request.urlopen(request, timeout=self.timeout) as response:
                    # 登录成功后 MUSIC_U 就在 Set-Cookie 里，必须逐个吸收（同一次响应可能有多个）
                    self.cookies.merge_headers(response.headers.get_all("Set-Cookie"))
                    raw = response.read()
            except urllib.error.HTTPError as error:
                raw = b""
                try:
                    raw = error.read() or b""
                except Exception:  # noqa: BLE001 - 读不到 body 不影响判定
                    raw = b""
                payload = parse_payload(raw)
                if validate and payload and payload.get("code") not in (None, 0, 200):
                    self._raise_code(payload.get("code"), action)
                if error.code in (429, 460):
                    raise PluginError(1014, "网易云限流或风控（HTTP %d），请稍后重试" % error.code)
                if error.code >= 500 and attempt <= self.retries:
                    last_error = error
                    time.sleep(0.3 * attempt)
                    continue
                if error.code in (400, 403, 404):
                    raise PluginError(1011, "%s失败：请求被拒绝（HTTP %d）" % (action, error.code))
                raise PluginError(1011, "%s失败（HTTP %d）" % (action, error.code))
            except (urllib.error.URLError, socket.timeout, OSError) as error:
                last_error = error
                if attempt <= self.retries:
                    time.sleep(0.25 * attempt)
                    continue
                raise self._transport_error(error, action)
            payload = parse_payload(raw)
            if payload is None:
                if attempt <= self.retries:
                    time.sleep(0.25 * attempt)
                    continue
                raise PluginError(1011, "%s失败：网易云返回的不是合法 JSON" % action)
            if validate:
                self._raise_code(payload.get("code"), action)
            return payload
        raise self._transport_error(last_error, action)

    @staticmethod
    def _transport_error(error, action):
        text = str(error or "")
        lowered = text.lower()
        if isinstance(error, socket.timeout) or "timed out" in lowered or "timeout" in lowered:
            return PluginError(1010, "%s超时，请稍后重试" % action)
        if "name or service not known" in lowered or "getaddrinfo" in lowered:
            return PluginError(1011, "%s失败：无法解析网易云域名，请检查网络/代理" % action)
        return PluginError(1011, "%s失败：%s" % (action, text or "网络异常"))

    @staticmethod
    def _raise_code(code, action):
        if code in (None, 0, 200):
            return
        if code == 301:
            raise PluginError(1007, "%s失败：需要登录网易云账号（code=301）" % action)
        if code in (460, -460):
            raise PluginError(1014, "网易云风控校验未通过（code=%s），请稍后重试或配置 Cookie" % code)
        if code == 404:
            raise PluginError(1004, "%s失败：资源不存在或已下架（code=404）" % action)
        if code == -110:
            raise PluginError(1021, "%s失败：账号无权播放该音质（code=-110）" % action)
        if code == 403:
            raise PluginError(1011, "%s失败：访问被拒绝（code=403）" % action)
        raise PluginError(1011, "%s失败（code=%s）" % (action, code))

    # ------------------------------------------------------------ 账号
    def account_state(self, force=False):
        """登录态（缓存 2 分钟）；查询失败时按未登录处理，不影响主流程"""
        now = time.time()
        with self._account_lock:
            cached = self._account["value"]
            if cached is not None and not force and self._account["expire_at"] > now:
                return cached
        state = {
            "logged_in": False,
            "user_id": "",
            "nickname": "",
            "avatar": "",
            "vip": False,
            "vip_type": 0,
            "level": 0,
            "cookie_present": bool(self.cookies.get("MUSIC_U")),
        }
        try:
            payload = self._http("/api/nuser/account/get", action="查询登录态")
        except PluginError as error:
            log("查询登录态失败: %s" % error.message)
            payload = None
        if payload:
            account = payload.get("account") or {}
            profile = payload.get("profile") or {}
            state.update({
                "logged_in": bool(account.get("id")),
                "user_id": str(account.get("id") or ""),
                "nickname": profile.get("nickname") or "",
                "avatar": normalize_cover(profile.get("avatarUrl") or ""),
                "vip_type": as_int(profile.get("vipType")),
                "level": as_int(profile.get("level")),
            })
            state["vip"] = state["vip_type"] > 0
        with self._account_lock:
            self._account["value"] = state
            self._account["expire_at"] = now + 120.0
        return state

    def account_cached(self):
        """只读缓存里的登录态（不发起网络请求，供 /health 快速探活）"""
        now = time.time()
        with self._account_lock:
            cached = self._account["value"]
            if cached is not None and self._account["expire_at"] > now:
                return cached
        return None

    def invalidate_account_dependent(self):
        """登录态变化后，音质/权限/播放地址/歌单权限都与账号相关，必须整体失效"""
        self._details.invalidate()
        self._urls.invalidate()
        self._collections.invalidate()
        self._mvs.invalidate()
        with self._account_lock:
            self._account["value"] = None
            self._account["expire_at"] = 0.0

    # ------------------------------------------------------------ 登录
    def qr_start(self):
        """生成扫码登录二维码：unikey + 网易云登录页地址 + 可渲染的二维码图片地址"""
        payload = self._http("/api/login/qrcode/unikey", form={"type": 1}, action="生成登录二维码")
        key = str(payload.get("unikey") or "").strip()
        if not key:
            raise PluginError(1011, "网易云未返回二维码 key，请稍后重试")
        qr_url = "https://music.163.com/login?codekey=" + urllib.parse.quote(key, safe="")
        return {
            "platform": "netease",
            "key": key,
            "qr_url": qr_url,
            "qr_image_url": ("https://api.qrserver.com/v1/create-qr-code/?size=280x280&margin=0&data="
                             + urllib.parse.quote(qr_url, safe="")),
            "expires_in": 300,
            "poll_interval_ms": 2000,
            "logged_in": bool((self.account_cached() or {}).get("logged_in")),
        }

    def qr_check(self, key):
        """轮询扫码状态：800 过期 / 801 待扫码 / 802 待确认 / 803 成功"""
        key = str(key or "").strip()
        if not key:
            raise PluginError(1007, "缺少 key（请先调用 login_qr_start）")
        payload = self._http(
            "/api/login/qrcode/client/login",
            params={"key": key, "type": 1},
            action="查询扫码状态",
            validate=False,
        )
        code = as_int(payload.get("code"), -1)
        states = {
            800: ("expired", "二维码已过期，请重新生成"),
            801: ("waiting", "等待手机网易云扫码"),
            802: ("scanned", "已扫码，请在手机上确认登录"),
            803: ("success", "登录成功"),
        }
        state, message = states.get(code, ("unknown", str(payload.get("message") or "未知状态")))
        data = {
            "platform": "netease",
            "key": key,
            "code": code,
            "state": state,
            "message": message,
            "logged_in": False,
        }
        if code == 803:
            self._after_login()
            account = self.account_state(force=True)
            data.update({
                "logged_in": account["logged_in"],
                "nickname": account["nickname"],
                "user_id": account["user_id"],
                "vip": account["vip"],
                "avatar": account["avatar"],
                "cookie_saved": self._persist_cookie(),
            })
        return data

    def login_cookie(self, cookie):
        """直接导入 Cookie（整行，或只给 MUSIC_U 的值）"""
        text = str(cookie or "").strip().strip('"').strip("'")
        if not text:
            raise PluginError(1007, "缺少 cookie 参数")
        if "=" not in text:
            text = "MUSIC_U=" + text
        previous = self.cookies.to_string()
        self.cookies.replace(text)
        if not self.cookies.get("MUSIC_U"):
            self.cookies.replace(previous)
            raise PluginError(1007, "Cookie 里没有 MUSIC_U，无法用于登录")
        self.invalidate_account_dependent()
        account = self.account_state(force=True)
        if not account["logged_in"]:
            self.cookies.replace(previous)
            self.invalidate_account_dependent()
            raise PluginError(1007, "Cookie 未被网易云接受（MUSIC_U 可能已过期），请重新扫码登录")
        data = dict(account)
        data.update({
            "platform": "netease",
            "logged_in": True,
            "cookie_saved": self._persist_cookie(),
        })
        return data

    def login_phone(self, phone, password="", captcha="", country_code="86"):
        """手机号登录（明文接口；网易云对服务端 IP 常返回 -460 风控，此时建议改用扫码）"""
        phone = str(phone or "").strip()
        password = str(password or "")
        captcha = str(captcha or "").strip()
        if not phone:
            raise PluginError(1007, "缺少 phone 参数")
        form = {"phone": phone, "countrycode": str(country_code or "86"), "rememberLogin": "true"}
        if password:
            form["password"] = hashlib.md5(password.encode("utf-8")).hexdigest()
        elif captcha:
            form["captcha"] = captcha
        else:
            raise PluginError(1007, "手机号登录需要 password 或 captcha")
        payload = self._http("/api/login/cellphone", form=form, action="手机号登录", validate=False)
        code = as_int(payload.get("code"), -1)
        if code != 200:
            note = str(payload.get("message") or payload.get("msg") or "")
            if code in (-460, 460):
                raise PluginError(1014, "网易云风控拒绝了密码登录（-460）：请改用扫码登录"
                                        "（login_qr_start）或导入 Cookie")
            if code == 401:
                raise PluginError(1014, "网易云要求加密登录（401），服务端明文接口已不可用："
                                        "请改用扫码登录（login_qr_start）或导入 Cookie")
            if code == 502:
                raise PluginError(1014, "网易云已关闭该方式登录（502）：请改用扫码登录或导入 Cookie")
            if code in (400, 501):
                raise PluginError(1007, "手机号或密码不正确（code=%s）%s" % (code, ("：" + note) if note else ""))
            if code == 503:
                raise PluginError(1007, "验证码不正确或已过期（503）")
            raise PluginError(1007, "手机号登录失败（code=%s）%s" % (code, ("：" + note) if note else ""))
        self._after_login()
        account = self.account_state(force=True)
        data = dict(account)
        data.update({
            "platform": "netease",
            "logged_in": account["logged_in"],
            "cookie_saved": self._persist_cookie(),
        })
        return data

    def logout(self):
        """退出登录：清空内存凭据与 cookie.txt，并让账号相关缓存失效"""
        try:
            self._http("/api/logout", form={}, action="退出登录", validate=False)
        except PluginError as error:
            log("调用 /api/logout 失败（忽略）: %s" % error.message)
        self.cookies.clear()
        removed = False
        try:
            if os.path.exists(COOKIE_FILE):
                os.remove(COOKIE_FILE)
                removed = True
        except OSError as error:
            log("删除 cookie.txt 失败: %s" % error)
        self.invalidate_account_dependent()
        return {
            "platform": "netease",
            "logged_in": False,
            "cookie_removed": removed,
            "message": "已退出登录",
        }

    def _after_login(self):
        self.invalidate_account_dependent()

    def _persist_cookie(self):
        text = self.cookies.to_string()
        if not self.cookies.get("MUSIC_U"):
            return False
        saved = save_cookie_file(text)
        if saved:
            log("登录凭据已写入 cookie.txt")
        return saved

    # ------------------------------------------------------------ 曲目详情
    def details(self, ids):
        """批量取曲目详情 + 权限 → {id: (song, privilege)}；缺失的 id 不返回"""
        out = {}
        pending = []
        for raw in ids or []:
            key = str(raw or "").strip()
            if not key or not key.isdigit():
                continue
            cached = self._details.get(key)
            if cached is not None:
                if cached.get("song"):
                    out[key] = (cached["song"], cached.get("privilege"))
                continue
            pending.append(key)
        for start in range(0, len(pending), DETAIL_CHUNK):
            chunk = pending[start:start + DETAIL_CHUNK]
            form = {"c": json.dumps([{"id": int(value)} for value in chunk])}
            try:
                payload = self._http("/api/v3/song/detail", form=form, action="获取曲目详情")
            except PluginError as error:
                if error.code in (1014, 1010):
                    raise
                log("获取曲目详情失败: %s" % error.message)
                continue
            privileges = {}
            for item in payload.get("privileges") or []:
                if isinstance(item, dict) and item.get("id") is not None:
                    privileges[str(item.get("id"))] = item
            songs = {}
            for song in payload.get("songs") or []:
                if not isinstance(song, dict) or song.get("id") is None:
                    continue
                key = str(song.get("id"))
                song["_privilege"] = privileges.get(key)
                songs[key] = song
                self._details.set(key, {"song": song, "privilege": privileges.get(key)}, DETAIL_TTL)
            for key in chunk:
                if key in songs:
                    out[key] = (songs[key], privileges.get(key))
                else:
                    # 下架 / 无版权：短期记住，避免反复打网易云
                    self._details.set(key, {"song": None, "privilege": None}, 120.0)
        return out

    def detail(self, item_id):
        key = str(item_id or "").strip()
        if not key.isdigit():
            raise PluginError(1007, "曲目 id 不合法: %s" % (key or "(空)"))
        found = self.details([key])
        if key not in found or not found[key][0]:
            return None, None
        return found[key]

    # ------------------------------------------------------------ 音质与可用性
    @staticmethod
    def _track_bitrate(song, key):
        track = song.get(key) or {}
        if isinstance(track, dict):
            return as_int(track.get("br") or track.get("bitrate"))
        return as_int(track)

    def hardware_qualities(self, song):
        """该曲目在网易云侧存在的音质（不含账号权限过滤）"""
        available = []
        if self._track_bitrate(song, "hr") > 0:
            available.append("hires")
        if self._track_bitrate(song, "sq") > 0:
            available.append("lossless")
        if self._track_bitrate(song, "h") > 0:
            available.append("exhigh")
        if self._track_bitrate(song, "m") > 0:
            available.append("higher")
        available.append("standard")
        return [value for value in QUALITY_ORDER if value in available]

    def play_rank(self, song, privilege):
        """当前账号对该曲目可播放到的音质档位（0 = 无播放权限）"""
        privilege = privilege or {}
        level = str(privilege.get("plLevel") or "").strip().lower()
        if level:
            if level in PL_LEVEL_RANK:
                return PL_LEVEL_RANK[level]
            return 3  # 未知档位按「极高」保守处理（Readme 要求不得谎报，宁低不高）
        bitrate = as_int(privilege.get("pl"))
        if bitrate > 0:
            if bitrate >= 999000:
                return 5
            if bitrate >= 320000:
                return 3
            if bitrate >= 192000:
                return 2
            return 1
        # privilege 缺失：按付费类型 + 登录态推断
        fee = as_int(song.get("fee"))
        state = self.account_state()
        if fee == 0:
            return 5
        if fee == 8:          # 免费可听（高音质需 VIP），实测可拿极高
            return 3
        if fee in (1, 4, 16):
            return 5 if state["vip"] else 0
        return 3

    def playable_qualities(self, song, privilege=None):
        limit = self.play_rank(song, privilege)
        return [value for value in self.hardware_qualities(song)
                if QUALITY_RANK[value] <= limit]

    def availability(self, song, privilege=None, qualities=None):
        status = song.get("st")
        if status is not None and as_int(status, 0) < 0:
            return False, "歌曲已下架"
        if qualities is None:
            qualities = self.playable_qualities(song, privilege)
        if qualities:
            return True, ""
        fee = as_int(song.get("fee"))
        state = self.account_state()
        if fee == 4:
            return False, "需要购买数字专辑"
        if fee in (1, 16):
            if not state["logged_in"]:
                return False, "需要登录（黑胶 VIP 曲目）"
            if not state["vip"]:
                return False, "需要黑胶 VIP"
            return False, "无播放权限（可能为地区/版权限制）"
        return False, "无播放权限"

    # ------------------------------------------------------------ 模型转换
    def song_item(self, song, privilege=None):
        item_id = str(song.get("id"))
        album = song.get("al") or song.get("album") or {}
        artists = artist_names(song.get("ar")) or artist_names(song.get("artists"))
        qualities = self.playable_qualities(song, privilege)
        available, reason = self.availability(song, privilege, qualities)
        qualities_json = [quality_json(value) for value in qualities]
        return {
            "platform": "netease",
            "id": item_id,
            "item_id": item_id,
            "sha": sha_for(item_id),
            "name": song.get("name") or "",
            "artists": artists,
            "album": album.get("name") or "",
            "album_id": str(album.get("id") or ""),
            "duration_seconds": seconds_of(song.get("dt") or song.get("duration")),
            "pic": normalize_cover(album.get("picUrl") or ""),
            "available": available,
            "reason": reason,
            "fee": as_int(song.get("fee")),
            "mv_id": mv_id_of(song),
            "quality": qualities_json[0] if qualities_json else {},
            "qualities": qualities_json,
        }

    def song_base(self, song, privilege=None):
        """get_song：可直接落 base.json 的结构（Readme 1.1）"""
        item_id = str(song.get("id"))
        album = song.get("al") or song.get("album") or {}
        artists = artist_names(song.get("ar")) or artist_names(song.get("artists"))
        qualities = self.playable_qualities(song, privilege)
        available, reason = self.availability(song, privilege, qualities)
        base = {
            "platform": "netease",
            "version": "1.0.0",
            "location": "",
            "sha": sha_for(item_id),
            "name": song.get("name") or "",
            "author": " / ".join(artists),
            "pic": normalize_cover(album.get("picUrl") or ""),
            "secret": 0,
            "quality": quality_json(qualities[0]) if qualities else {},
            "album": album.get("name") or "",
            "duration_seconds": seconds_of(song.get("dt") or song.get("duration")),
            "track_no": as_int(song.get("no")),
            "item_id": item_id,
            "available": available,
            "reason": reason,
            "qualities": [quality_json(value) for value in qualities],
            "mv_id": mv_id_of(song),
        }
        return base

    def track_item(self, song, privilege=None):
        """歌单 / 专辑里的曲目条目（Readme 1.11 / 1.12；宿主按 duration 取秒）"""
        item = self.song_item(song, privilege)
        return {
            "platform": "netease",
            "item_id": item["id"],
            "sha": item["sha"],
            "name": item["name"],
            "author": " / ".join(item["artists"]),
            "album": item["album"],
            "duration": item["duration_seconds"],
            "duration_seconds": item["duration_seconds"],
            "pic": item["pic"],
            "available": item["available"],
            "reason": item["reason"],
            "quality": item["quality"],
            "qualities": item["qualities"],
            "group_key": "",
        }

    # ------------------------------------------------------------ 搜索
    def search(self, keyword, page, page_size, search_type):
        """搜索入口

        - type=song：单曲搜索（默认）
        - type=artist：先按歌手检索，再返回该歌手的热门歌曲（宿主侧结果统一是「曲目」，
          这样搜索页的歌手筛选也能得到可直接播放的结果；歌手实体本身用 search_artist 取）
        - type=album / playlist：搜索结果里返回「集合条目」（每条带 id/name/pic/owner/track_count），
          不混入曲目；前端在搜索页识别后用 get_album / get_playlist 拉详情再入库。
        """
        type_code = self._search_type_code(search_type)
        if type_code == 100:
            return self.search_artist_tracks(keyword, page, page_size)
        if type_code == 10:
            return self.search_albums(keyword, page, page_size)
        if type_code == 1000:
            return self.search_playlists(keyword, page, page_size)
        if type_code != 1:
            log("搜索类型 %s 暂不支持，返回空结果（keyword=%s）" % (search_type, keyword))
            return {
                "items": [],
                "keyword": keyword,
                "page": page,
                "page_size": page_size,
                "total": 0,
                "has_more": False,
                "type": str(search_type or ""),
                "note": "该搜索类型暂不支持，可用 search_artist / get_album / get_playlist 单独查询",
            }
        offset = max(0, (page - 1) * page_size)
        payload = self._http(
            "/api/cloudsearch/pc",
            form={"s": keyword, "type": type_code, "limit": page_size, "offset": offset},
            action="搜索",
        )
        result = payload.get("result") or {}
        songs = [song for song in (result.get("songs") or []) if isinstance(song, dict)]
        details = self.details([song.get("id") for song in songs])
        items = []
        for song in songs:
            key = str(song.get("id"))
            found = details.get(key)
            if found and found[0]:
                # 详情比搜索结果更权威（音轨 / 权限 / 封面都更全）
                merged = found[0]
                if not merged.get("name") and song.get("name"):
                    merged["name"] = song.get("name")
                items.append(self.song_item(merged, found[1]))
            else:
                items.append(self.song_item(song))
        return {
            "items": items,
            "keyword": keyword,
            "page": page,
            "page_size": page_size,
            "total": as_int(result.get("songCount"), len(items)),
            "has_more": bool(result.get("hasMore")),
            "type": "song",
        }

    def search_albums(self, keyword, page, page_size):
        """专辑检索（返回的是「集合条目」，不是单曲；前端按 album 卡片展示）"""
        offset = max(0, (page - 1) * page_size)
        payload = self._http(
            "/api/cloudsearch/pc",
            form={"s": keyword, "type": 10, "limit": page_size, "offset": offset},
            action="搜索专辑",
        )
        result = payload.get("result") or {}
        raw_albums = [album for album in (result.get("albums") or []) if isinstance(album, dict)]
        items = [self._album_search_item(album) for album in raw_albums if album.get("id") is not None]
        items = [item for item in items if item]
        return {
            "items": items,
            "keyword": keyword,
            "page": page,
            "page_size": page_size,
            "total": as_int(result.get("albumCount"), len(items)),
            "has_more": bool(result.get("hasMore")),
            "type": "album",
        }

    def search_playlists(self, keyword, page, page_size):
        """歌单检索（返回的是「集合条目」，不是单曲；前端按 playlist 卡片展示）"""
        offset = max(0, (page - 1) * page_size)
        payload = self._http(
            "/api/cloudsearch/pc",
            form={"s": keyword, "type": 1000, "limit": page_size, "offset": offset},
            action="搜索歌单",
        )
        result = payload.get("result") or {}
        raw_playlists = [pl for pl in (result.get("playlists") or []) if isinstance(pl, dict)]
        items = [self._playlist_search_item(pl) for pl in raw_playlists if pl.get("id") is not None]
        items = [item for item in items if item]
        return {
            "items": items,
            "keyword": keyword,
            "page": page,
            "page_size": page_size,
            "total": as_int(result.get("playlistCount"), len(items)),
            "has_more": bool(result.get("hasMore")),
            "type": "playlist",
        }

    @staticmethod
    def _album_search_item(raw):
        """专辑搜索结果条目（让前端走 get_album 详情 → 入库）"""
        album_id = str(raw.get("id") or "").strip()
        if not album_id:
            return None
        artists = artist_names(raw.get("artists"))
        artist = " / ".join([name for name in artists if name]) or (raw.get("artist") or "")
        publish = as_int(raw.get("publishTime"))
        size = as_int(raw.get("size"))
        return {
            "platform": "netease",
            "kind": "album",
            "id": album_id,
            "item_id": album_id,
            "name": raw.get("name") or "",
            "artist": artist,
            "pic": normalize_cover(raw.get("picUrl") or ""),
            "size": size,
            "track_count": size,
            "year": time.localtime(publish / 1000.0).tm_year if publish > 0 else 0,
            "publish_time": publish,
        }

    @staticmethod
    def _playlist_search_item(raw):
        """歌单搜索结果条目（让前端走 get_playlist 详情 → 入库）"""
        playlist_id = str(raw.get("id") or "").strip()
        if not playlist_id:
            return None
        creator = raw.get("creator") or {}
        cover = normalize_cover(raw.get("coverImgUrl") or "")
        return {
            "platform": "netease",
            "kind": "playlist",
            "id": playlist_id,
            "item_id": playlist_id,
            "name": raw.get("name") or "",
            "creator": creator.get("nickname") or "",
            "creator_id": str(creator.get("userId") or ""),
            "pic": cover,
            "track_count": as_int(raw.get("trackCount")),
            "play_count": as_int(raw.get("playCount")),
        }

    def search_artist_tracks(self, keyword, page, page_size):
        """歌手搜索 → 该歌手的热门歌曲（分页在曲目上做，宿主按曲目分页）"""
        found = self.artist_search(keyword, 1, 5)
        artists = found["artists"]
        if not artists:
            return {
                "items": [], "keyword": keyword, "page": page, "page_size": page_size,
                "total": 0, "has_more": False, "type": "artist", "artists": [],
            }
        target = keyword.strip().lower()
        best = artists[0]
        for artist in artists:
            if artist["name"].strip().lower() == target:
                best = artist
                break
        songs = self.artist_top_songs(best["id"])
        start = max(0, (page - 1) * page_size)
        chunk = songs[start:start + page_size]
        details = self.details([song.get("id") for song in chunk])
        items = []
        for song in chunk:
            key = str(song.get("id"))
            pair = details.get(key)
            song_data = pair[0] if pair and pair[0] else song
            privilege = pair[1] if pair else song.get("privilege")
            item = self.song_item(song_data, privilege)
            item["artist_id"] = best["id"]
            item["artist_name"] = best["name"]
            items.append(item)
        total = len(songs)
        return {
            "items": items,
            "keyword": keyword,
            "page": page,
            "page_size": page_size,
            "total": total,
            "has_more": start + len(chunk) < total,
            "type": "artist",
            "artist": best,
            "artists": artists,
        }

    def artist_search(self, keyword, page=1, page_size=10):
        """歌手检索（结构化）"""
        offset = max(0, (page - 1) * page_size)
        payload = self._http(
            "/api/cloudsearch/pc",
            form={"s": keyword, "type": 100, "limit": page_size, "offset": offset},
            action="搜索歌手",
        )
        result = payload.get("result") or {}
        artists = [self.artist_item(raw) for raw in (result.get("artists") or []) if isinstance(raw, dict)]
        artists = [artist for artist in artists if artist["id"]]
        return {
            "artists": artists,
            "keyword": keyword,
            "page": page,
            "page_size": page_size,
            "total": as_int(result.get("artistCount"), len(artists)),
            "has_more": bool(result.get("hasMore")),
        }

    def artist_top_songs(self, artist_id, limit=50):
        """歌手热门歌曲（网易云接口一次最多 50 首）"""
        key = str(artist_id or "").strip()
        cached = self._collections.get("artist_songs|%s" % key)
        if cached is not None:
            return cached
        payload = self._http("/api/artist/top/song", params={"id": int(key)}, action="获取歌手热门歌曲")
        songs = [song for song in (payload.get("songs") or []) if isinstance(song, dict)]
        if limit and limit > 0:
            songs = songs[:limit]
        self._collections.set("artist_songs|%s" % key, songs, 600.0)
        return songs

    def artist_detail(self, raw_id, song_limit=20, album_limit=12, include_songs=True):
        """歌手详情 + 热门歌曲 + 专辑列表"""
        artist_id = normalize_numeric_id(raw_id, extra_prefixes=("netease_artist_",))
        if not artist_id:
            raise PluginError(1007, "歌手 id 不合法: %s" % (raw_id or "(空)"))
        payload = self._http("/api/v1/artist/%s" % artist_id, action="获取歌手详情")
        raw = payload.get("artist")
        if not isinstance(raw, dict) or not raw.get("id"):
            raise PluginError(1004, "网易云没有找到该歌手（id=%s）" % artist_id)
        artist = self.artist_item(raw)
        artist["brief_desc"] = raw.get("briefDesc") or artist.get("brief_desc") or ""
        artist["followed"] = bool(raw.get("followed"))

        songs = []
        if include_songs:
            hot = [song for song in (payload.get("hotSongs") or []) if isinstance(song, dict)]
            if not hot:
                hot = self.artist_top_songs(artist_id)
            if song_limit and song_limit > 0:
                hot = hot[:song_limit]
            details = self.details([song.get("id") for song in hot])
            for song in hot:
                key = str(song.get("id"))
                pair = details.get(key)
                song_data = pair[0] if pair and pair[0] else song
                privilege = pair[1] if pair else song.get("privilege")
                item = self.track_item(song_data, privilege)
                item["artist_id"] = artist_id
                item["artist_name"] = artist["name"]
                songs.append(item)

        albums = []
        if album_limit and album_limit > 0:
            album_payload = self._http(
                "/api/artist/albums/%s" % artist_id,
                params={"limit": album_limit, "offset": 0},
                action="获取歌手专辑",
            )
            for entry in album_payload.get("hotAlbums") or []:
                if not isinstance(entry, dict) or not entry.get("id"):
                    continue
                publish = as_int(entry.get("publishTime"))
                albums.append({
                    "platform": "netease",
                    "id": str(entry.get("id")),
                    "name": entry.get("name") or "",
                    "pic": normalize_cover(entry.get("picUrl") or ""),
                    "year": time.localtime(publish / 1000.0).tm_year if publish > 0 else 0,
                    "publish_time": publish,
                    "size": as_int(entry.get("size")),
                })

        result = dict(artist)
        result.update({
            "platform": "netease",
            "source_id": artist_id,
            "hot_songs": songs,
            "hot_songs_total": len(payload.get("hotSongs") or []) or len(songs),
            "albums": albums,
        })
        return result

    def artist_item(self, raw):
        alias = [str(name) for name in (raw.get("alias") or []) if name]
        return {
            "platform": "netease",
            "id": str(raw.get("id") or ""),
            "name": raw.get("name") or "",
            "alias": alias,
            "pic": normalize_cover(raw.get("picUrl") or raw.get("img1v1Url") or ""),
            "avatar": normalize_cover(raw.get("img1v1Url") or ""),
            "album_count": as_int(raw.get("albumSize")),
            "music_count": as_int(raw.get("musicSize")),
            "mv_count": as_int(raw.get("mvSize")),
            "brief_desc": raw.get("briefDesc") or "",
            "trans": raw.get("trans") or "",
            "followed": bool(raw.get("followed")),
        }

    @staticmethod
    def _search_type_code(search_type):
        text = str(search_type or "song").strip().lower()
        mapping = {"song": 1, "1": 1, "single": 1, "track": 1, "": 1,
                   "album": 10, "10": 10, "artist": 100, "100": 100, "singer": 100,
                   "playlist": 1000, "1000": 1000, "mv": 1004, "1004": 1004, "video": 1004}
        return mapping.get(text, 1)

    # ------------------------------------------------------------ 播放地址
    def url_for(self, item_id, quality_request):
        song, privilege = self.detail(item_id)
        if not song:
            raise PluginError(1004, "网易云没有找到该曲目（id=%s）" % item_id)
        requested = normalize_quality(quality_request)
        candidates = []
        if requested:
            start = QUALITY_ORDER.index(requested)
            candidates.extend(QUALITY_ORDER[start:])   # 请求音质 → 逐级降级
            candidates.extend(QUALITY_ORDER[:start])   # 权限信息可能过期，再向上兜底
        else:
            candidates.extend(QUALITY_ORDER)
        seen = set()
        last_note = ""
        for quality_id in candidates:
            if quality_id in seen:
                continue
            seen.add(quality_id)
            cached = self._urls.get("%s|%s" % (item_id, quality_id))
            if cached and (not cached.get("expires_at") or cached["expires_at"] - 60 > time.time()):
                # 缓存只存「这一档实际拿到什么」，requested/downgraded 每次按本次请求重算
                return self._url_result(cached, requested)
            encode = "flac" if QUALITY_SPECS[quality_id]["lossless"] else "mp3"
            try:
                payload = self._http(
                    "/api/song/enhance/player/url/v1",
                    params={"ids": json.dumps([int(item_id)]), "level": quality_id, "encodeType": encode},
                    action="获取播放地址",
                )
            except PluginError as error:
                if error.code in (1010, 1014):
                    raise
                last_note = error.message
                continue
            data = (payload.get("data") or [{}])[0]
            url = normalize_media_url(data.get("url") or "")
            if not url:
                last_note = netease_failure_note(data.get("code"), as_int(song.get("fee")))
                continue
            raw = {
                "url": url,
                "type": data.get("type") or "",
                "br": as_int(data.get("br")),
                "sr": as_int(data.get("sr")),
                "size": as_int(data.get("size")),
                "expires_at": media_expiry(url),
            }
            self._urls.set("%s|%s" % (item_id, quality_id), raw, URL_TTL)
            return self._url_result(raw, requested)
        if not self.cookies.get("MUSIC_U"):
            last_note = (last_note or "该歌曲暂无可用播放地址") + "；未登录（可打开 http://127.0.0.1:8848/login 扫码）"
        raise PluginError(1021, last_note or "该歌曲暂无可用播放地址")

    def _url_result(self, raw, requested):
        """把「实际拿到的音频」翻译成宿主可用的播放地址结构（真实音质，不谎报）"""
        actual = self.actual_quality(raw)
        url = raw.get("url") or ""
        return {
            "url": url,
            "location": url,
            "quality": quality_json(actual),
            "quality_id": actual,
            "level": QUALITY_SPECS[actual]["level"],
            "requested_quality": requested or "",
            "downgraded": bool(requested and actual != requested),
            "bitrate": as_int(raw.get("br")),
            "sample_rate": as_int(raw.get("sr")),
            "codec": (raw.get("type") or ("flac" if QUALITY_SPECS[actual]["lossless"] else "mp3")).lower(),
            "size": as_int(raw.get("size")),
            "expires_at": as_int(raw.get("expires_at")),
        }

    @staticmethod
    def actual_quality(data):
        """按响应里的真实编码/码率/采样率判定音质，绝不谎报"""
        kind = str(data.get("type") or "").lower()
        bitrate = as_int(data.get("br"))
        sample_rate = as_int(data.get("sr"))
        if kind in ("flac", "alac", "wav", "ape"):
            if sample_rate >= 96000 or bitrate >= 1000000:
                return "hires"
            return "lossless"
        if bitrate >= 300000:
            return "exhigh"
        if bitrate >= 180000:
            return "higher"
        return "standard"

    # ------------------------------------------------------------ 歌词
    def lyric(self, item_id):
        cached = self._lyrics.get(str(item_id))
        if cached is not None:
            return cached
        payload = self._http(
            "/api/song/lyric",
            params={"id": int(item_id), "lv": 1, "kv": 1, "tv": -1, "rv": 1, "yv": 1, "ytv": 1, "yrv": 1},
            action="获取歌词",
        )
        lrc_text = (payload.get("lrc") or {}).get("lyric") or ""
        translation_text = (payload.get("tlyric") or {}).get("lyric") or ""
        roman_text = (payload.get("romalrc") or {}).get("lyric") or ""
        yrc_text = (payload.get("yrc") or {}).get("lyric") or ""

        lines = parse_lrc(lrc_text)
        translation = parse_lrc(translation_text)
        roman = parse_lrc(roman_text)
        yrc_lines = parse_yrc(yrc_text)
        if not lines and yrc_lines:
            # 只有逐字歌词时退化为普通行
            lines = [{"time_ms": item["time_ms"], "text": item["text"]} for item in yrc_lines]
        translated = merge_translation(lines, translation, "tr")
        romanized = merge_translation(lines, roman, "roma")
        worded = merge_words(lines, yrc_lines)

        plain_text = "".join(line.get("text", "") for line in lines)
        instrumental = ("纯音乐" in plain_text or "instrumental" in plain_text.lower()) and len(lines) <= 3
        result = {
            "format": "lrc",
            "timed": bool(lines),
            "line_count": len(lines),
            "lines": lines,
            "lrc": lrc_text,
            "tlyric": translation_text,
            "romalrc": roman_text,
            "has_translation": translated > 0,
            "has_roman": romanized > 0,
            "has_words": worded > 0,
            "instrumental": bool(instrumental),
        }
        self._lyrics.set(str(item_id), result, LYRIC_TTL)
        return result

    # ------------------------------------------------------------ 封面
    def pic(self, item_id, size=""):
        song, _ = self.detail(item_id)
        if not song:
            raise PluginError(1004, "网易云没有找到该曲目（id=%s）" % item_id)
        album = song.get("al") or song.get("album") or {}
        url = normalize_cover(album.get("picUrl") or "")
        text = str(size or "").strip().lower()
        if url and text:
            match = re.match(r"^(\d+)(?:[xy*](\d+))?$", text)
            if match:
                width = match.group(1)
                height = match.group(2) or width
                url += "?param=%sy%s" % (width, height)
        return {"url": url, "size": size or "", "item_id": str(item_id)}

    # ------------------------------------------------------------ 歌单
    def playlist(self, raw_id, limit=0):
        playlist_id = normalize_numeric_id(raw_id, extra_prefixes=("netease_playlist_",))
        if not playlist_id:
            raise PluginError(1030, "歌单 id 不合法: %s" % (raw_id or "(空)"))
        cached = self._collections.get("playlist|%s|%s" % (playlist_id, limit))
        if cached is not None:
            return cached
        try:
            payload = self._http(
                "/api/v6/playlist/detail",
                form={"id": int(playlist_id), "n": 1000, "s": 8},
                action="获取歌单",
            )
        except PluginError as error:
            if error.code == 1004:
                raise PluginError(1030, "歌单不存在或不可访问: %s" % playlist_id)
            raise
        playlist = payload.get("playlist")
        if not isinstance(playlist, dict) or not playlist.get("id"):
            raise PluginError(1030, "歌单不存在或不可访问: %s" % playlist_id)

        track_ids = []
        for entry in playlist.get("trackIds") or []:
            if isinstance(entry, dict) and entry.get("id") is not None:
                track_ids.append(str(entry.get("id")))
        songs = {}
        for song in playlist.get("tracks") or []:
            if isinstance(song, dict) and song.get("id") is not None:
                songs[str(song.get("id"))] = song
        # 歌单超过接口单页返回上限时，用 trackIds 补齐（保持原顺序）
        missing = [value for value in track_ids if value not in songs]
        if missing:
            extra = self.details(missing)
            for key, pair in extra.items():
                if pair and pair[0]:
                    songs[key] = pair[0]
        order = track_ids or list(songs.keys())
        if limit and limit > 0:
            order = order[:limit]

        creator = playlist.get("creator") or {}
        cover = normalize_cover(playlist.get("coverImgUrl") or "")
        tracks = []
        total_seconds = 0
        for key in order:
            song = songs.get(key)
            if not song:
                continue
            # 歌单接口自带 fee/st，但没有逐曲权限；用 fee 推断，必要时再补详情
            track = self.track_item(song, song.get("_privilege"))
            tracks.append(track)
            total_seconds += track["duration_seconds"]
        if not cover and tracks:
            cover = tracks[0]["pic"]
        result = {
            "platform": "netease",
            "id": str(playlist.get("id")),
            "source_id": str(playlist.get("id")),
            "name": playlist.get("name") or "",
            "description": playlist.get("description") or "",
            "pic": cover,
            "owner": creator.get("nickname") or "",
            "owner_id": str(creator.get("userId") or ""),
            "play_count": as_int(playlist.get("playCount")),
            "track_count": as_int(playlist.get("trackCount"), len(tracks)),
            "duration_seconds": total_seconds,
            "created_at": int(as_int(playlist.get("createTime")) / 1000),
            "updated_at": int(as_int(playlist.get("updateTime")) / 1000),
            "tracks": tracks,
        }
        self._collections.set("playlist|%s|%s" % (playlist_id, limit), result, COLLECTION_TTL)
        return result

    # ------------------------------------------------------------ 专辑
    def album(self, raw_id):
        album_id = normalize_numeric_id(raw_id, extra_prefixes=("netease_album_",))
        if not album_id:
            raise PluginError(1031, "专辑 id 不合法: %s" % (raw_id or "(空)"))
        cached = self._collections.get("album|%s" % album_id)
        if cached is not None:
            return cached
        try:
            payload = self._http("/api/v1/album/%s" % album_id, action="获取专辑")
        except PluginError as error:
            if error.code == 1004:
                raise PluginError(1031, "专辑不存在: %s" % album_id)
            raise
        album = payload.get("album")
        if not isinstance(album, dict) or not album.get("id"):
            raise PluginError(1031, "专辑不存在: %s" % album_id)

        songs = [song for song in (payload.get("songs") or []) if isinstance(song, dict)]
        songs.sort(key=lambda song: as_int(song.get("no"), 999))
        tracks = []
        total_seconds = 0
        for song in songs:
            track = self.track_item(song, song.get("privilege"))
            tracks.append(track)
            total_seconds += track["duration_seconds"]
        artists = artist_names(album.get("artists")) or [album.get("artist") or ""]
        artist = " / ".join([name for name in artists if name])
        publish = as_int(album.get("publishTime"))
        cover = normalize_cover(album.get("picUrl") or "")
        if not cover and tracks:
            cover = tracks[0]["pic"]
        result = {
            "platform": "netease",
            "id": str(album.get("id")),
            "source_id": str(album.get("id")),
            "name": album.get("name") or "",
            "artist": artist,
            "pic": cover,
            "description": album.get("description") or album.get("briefDesc") or "",
            "company": album.get("company") or "",
            "year": time.localtime(publish / 1000.0).tm_year if publish > 0 else 0,
            "publish_time": publish,
            "track_count": as_int(album.get("size"), len(tracks)),
            "duration_seconds": total_seconds,
            "tracks": tracks,
        }
        self._collections.set("album|%s" % album_id, result, COLLECTION_TTL)
        return result

    # ------------------------------------------------------------ MV
    def search_mv(self, keyword, page, page_size):
        offset = max(0, (page - 1) * page_size)
        payload = self._http(
            "/api/cloudsearch/pc",
            form={"s": keyword, "type": 1004, "limit": page_size, "offset": offset},
            action="搜索 MV",
        )
        result = payload.get("result") or {}
        mvs = [item for item in (result.get("mvs") or []) if isinstance(item, dict)]
        items = []
        for mv in mvs:
            mv_id = str(mv.get("id") or "")
            if not mv_id:
                continue
            cover = normalize_cover(mv.get("cover") or "")
            items.append({
                "platform": "netease",
                "id": mv_id,
                "item_id": mv_id,
                "sha": mv_sha_for(mv_id),
                "title": mv.get("name") or "",
                "name": mv.get("name") or "",
                "artist": mv.get("artistName") or "",
                "cover": cover,
                "pic": cover,
                "duration": seconds_of(mv.get("duration")),
                "duration_seconds": seconds_of(mv.get("duration")),
                "play_count": as_int(mv.get("playCount")),
            })
        return {
            "mvs": items,
            "total": as_int(result.get("mvCount"), len(items)),
            "keyword": keyword,
            "page": page,
            "page_size": page_size,
        }

    def mv(self, raw_id):
        mv_id = normalize_numeric_id(raw_id, extra_prefixes=("netease_mv_", "netease:mv:", "mv:"))
        if not mv_id:
            raise PluginError(1032, "MV id 不合法: %s" % (raw_id or "(空)"))
        cached = self._mvs.get(mv_id)
        if cached is not None:
            return cached
        try:
            payload = self._http("/api/mv/detail", params={"id": int(mv_id)}, action="获取 MV 详情")
        except PluginError as error:
            if error.code == 1004:
                raise PluginError(1032, "MV 不存在: %s" % mv_id)
            raise
        data = payload.get("data") or {}
        if not data or not data.get("id"):
            raise PluginError(1032, "MV 不存在: %s" % mv_id)

        brs = data.get("brs") or {}
        resolutions = []
        for key, url in brs.items():
            if not url:
                continue
            width, height = MV_RESOLUTIONS.get(str(key), (0, 0))
            resolutions.append({
                "resolution": str(key),
                "width": width,
                "height": height,
                "url": normalize_media_url(url),
            })
        resolutions.sort(key=lambda item: as_int(item["resolution"]), reverse=True)
        best = resolutions[0] if resolutions else {}
        cover = normalize_cover(data.get("cover") or "")
        result = {
            "platform": "netease",
            "id": str(data.get("id")),
            "item_id": str(data.get("id")),
            "sha": mv_sha_for(data.get("id")),
            "title": data.get("name") or "",
            "artist": data.get("artistName") or "",
            "album": "",
            "pic": cover,
            "cover": cover,
            "url": best.get("url", ""),
            "location": best.get("url", ""),
            "width": best.get("width", 0),
            "height": best.get("height", 0),
            "resolution": best.get("resolution", ""),
            "duration": seconds_of(data.get("duration")),
            "duration_seconds": seconds_of(data.get("duration")),
            "publish_time": data.get("publishTime") or "",
            "play_count": as_int(data.get("playCount")),
            "resolutions": resolutions,
            "expires_at": media_expiry(best.get("url", "")),
        }
        self._mvs.set(mv_id, result, MV_TTL)
        return result

    # ------------------------------------------------------------ 其他
    def qualities(self, item_id):
        song, privilege = self.detail(item_id)
        if not song:
            raise PluginError(1004, "网易云没有找到该曲目（id=%s）" % item_id)
        qualities = self.playable_qualities(song, privilege)
        available, reason = self.availability(song, privilege, qualities)
        state = self.account_state()
        return {
            "id": str(item_id),
            "platform": "netease",
            "qualities": [quality_json(value) for value in qualities],
            "available": available,
            "reason": reason,
            "max_quality": qualities[0] if qualities else "",
            "logged_in": state["logged_in"],
            "vip": state["vip"],
        }


# ---------------------------------------------------------------- 单例与工具

CONFIG = {
    "cookie": "",
    "qps": 6,
    "timeout_seconds": 20,
    "retries": 2,
}

CLIENT = None
# 服务器监听地址（main() 里赋值，给 /settings / /login 模板用）
SERVER_HOST = "127.0.0.1"
SERVER_PORT = 8848


def load_config():
    """读取 config.json（可选），返回合并后的配置"""
    config = dict(CONFIG)
    path = os.path.join(HERE, "config.json")
    if os.path.exists(path):
        try:
            with open(path, "r", encoding="utf-8") as handle:
                data = json.load(handle)
            if isinstance(data, dict):
                for key in ("cookie", "qps", "timeout_seconds", "retries"):
                    if key in data:
                        config[key] = data[key]
        except (ValueError, OSError) as error:
            log("读取 config.json 失败: %s" % error)
    return config


def load_cookie(config):
    """Cookie 优先级：环境变量 > cookie.txt > config.json"""
    cookie = os.environ.get("RAD_NETEASE_COOKIE", "").strip()
    if cookie:
        return cookie
    cookie_file = os.path.join(HERE, "cookie.txt")
    if os.path.exists(cookie_file):
        try:
            with open(cookie_file, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read().strip()
            if text:
                return text
        except OSError:
            pass
    return str(config.get("cookie") or "").strip()


def ok(data):
    return {"code": 0, "message": "ok", "data": data}


def fail(code, message):
    return {"code": code, "message": message}


def require_id(item_id, label="曲目"):
    value = str(item_id or "").strip()
    if not value:
        raise PluginError(1007, "缺少参数 id（%s标识）" % label)
    return value


def handle_request(payload):
    action = str(payload.get("op") or "").strip().lower()
    op = OP_ALIASES.get(action, action)
    params = payload.get("params") or {}
    if not isinstance(params, dict):
        params = {}
    item_id = str(params.get("id") or params.get("item_id") or payload.get("id") or "").strip()
    started = time.time()

    try:
        if op == "search":
            keyword = str(params.get("keyword") or params.get("q") or "").strip()
            if not keyword:
                raise PluginError(1007, "搜索关键字不能为空")
            page = max(1, as_int(params.get("page"), 1))
            page_size = min(100, max(1, as_int(params.get("page_size"), 20)))
            return ok(CLIENT.search(keyword, page, page_size, params.get("type") or "song"))

        if op == "get_song":
            song, privilege = CLIENT.detail(require_id(item_id))
            if not song:
                raise PluginError(1004, "网易云没有找到该曲目（id=%s）" % item_id)
            return ok(CLIENT.song_base(song, privilege))

        if op == "get_url":
            target = require_id(item_id)
            return ok(CLIENT.url_for(target, params.get("quality") or params.get("level") or ""))

        if op == "get_qualities":
            return ok(CLIENT.qualities(require_id(item_id)))

        if op == "get_lyric":
            target = require_id(item_id)
            if not target.isdigit():
                # 1015 非致命：宿主会降级为「无歌词」继续播放（Readme 1.5 / 2.8）
                raise PluginError(1015, "歌词仅支持网易云曲目 id: %s" % target)
            return ok(CLIENT.lyric(target))

        if op == "get_pic":
            return ok(CLIENT.pic(require_id(item_id), params.get("size") or ""))

        if op == "get_playlist":
            return ok(CLIENT.playlist(require_id(item_id, "歌单"), as_int(params.get("limit"))))

        if op == "get_album":
            return ok(CLIENT.album(require_id(item_id, "专辑")))

        if op == "search_album":
            keyword = str(params.get("keyword") or params.get("q") or "").strip()
            if not keyword:
                raise PluginError(1007, "搜索关键字不能为空")
            page = max(1, as_int(params.get("page"), 1))
            page_size = min(50, max(1, as_int(params.get("page_size"), 20)))
            return ok(CLIENT.search_albums(keyword, page, page_size))

        if op == "search_playlist":
            keyword = str(params.get("keyword") or params.get("q") or "").strip()
            if not keyword:
                raise PluginError(1007, "搜索关键字不能为空")
            page = max(1, as_int(params.get("page"), 1))
            page_size = min(50, max(1, as_int(params.get("page_size"), 20)))
            return ok(CLIENT.search_playlists(keyword, page, page_size))

        if op == "settings":
            # alias → settings_get；handle_request 收到的是 alias 之前的 op（原值）
            return handle_request({"op": "settings_get", "params": params})

        if op == "settings_get":
            # 设置面板：宿主会 GET 一次拿 schema + 当前值，再 POST 把 form 提交回来；
            # 此处只暴露「登录态 + 插件配置 + 版本号」，登录相关动作走专用 op。
            values = dict(CLIENT.config) if CLIENT else {}
            login = CLIENT.account_cached() if CLIENT else None
            schema = [
                {"key": "qps", "label": "限流 QPS", "type": "int", "min": 1, "max": 30,
                 "default": as_int(values.get("qps"), 6), "hint": "每秒最多发出多少请求（避免风控）"},
                {"key": "timeout_seconds", "label": "请求超时（秒）", "type": "int", "min": 3, "max": 60,
                 "default": as_int(values.get("timeout_seconds"), 20)},
                {"key": "retries", "label": "网络重试次数", "type": "int", "min": 0, "max": 5,
                 "default": as_int(values.get("retries"), 2),
                 "hint": "网络类失败自动重试；风控/限流（460/1014）不重试"},
            ]
            return ok({
                "platform": "netease",
                "plugin_id": "netease.source",
                "version": VERSION,
                "endpoint": "http://%s:%d" % (SERVER_HOST, SERVER_PORT),
                "login": login or {"logged_in": False, "cookie_present": False},
                "login_page": "/login",
                "settings_page": "/settings",
                "cookie_file": os.path.join(HERE, "cookie.txt"),
                "config_file": os.path.join(HERE, "config.json"),
                "values": values,
                "schema": schema,
            })

        if op == "search_mv":
            keyword = str(params.get("keyword") or params.get("q") or "").strip()
            if not keyword:
                raise PluginError(1007, "搜索关键字不能为空")
            page = max(1, as_int(params.get("page"), 1))
            page_size = min(50, max(1, as_int(params.get("page_size"), 20)))
            return ok(CLIENT.search_mv(keyword, page, page_size))

        if op == "search_artist":
            keyword = str(params.get("keyword") or params.get("q") or "").strip()
            if not keyword:
                raise PluginError(1007, "搜索关键字不能为空")
            page = max(1, as_int(params.get("page"), 1))
            page_size = min(50, max(1, as_int(params.get("page_size"), 20)))
            return ok(CLIENT.artist_search(keyword, page, page_size))

        if op == "get_artist":
            return ok(CLIENT.artist_detail(
                require_id(item_id, "歌手"),
                song_limit=min(50, max(0, as_int(params.get("song_limit"), 20))),
                album_limit=min(50, max(0, as_int(params.get("album_limit"), 12))),
                include_songs=params.get("include_songs", True) is not False,
            ))

        if op == "get_mv":
            return ok(CLIENT.mv(require_id(item_id, "MV")))

        if op == "check_login":
            state = CLIENT.account_state(force=bool(params.get("refresh")))
            data = dict(state)
            data["platform"] = "netease"
            data["cookie_configured"] = state["cookie_present"]
            return ok(data)

        if op == "login_qr_start":
            return ok(CLIENT.qr_start())

        if op == "login_qr_check":
            return ok(CLIENT.qr_check(params.get("key") or params.get("unikey") or ""))

        if op == "login_cookie":
            return ok(CLIENT.login_cookie(params.get("cookie") or params.get("music_u") or ""))

        if op == "login_phone":
            return ok(CLIENT.login_phone(
                params.get("phone") or params.get("cellphone") or "",
                params.get("password") or "",
                params.get("captcha") or "",
                params.get("country_code") or params.get("countrycode") or "86",
            ))

        if op == "logout":
            return ok(CLIENT.logout())

        raise PluginError(1012, "不支持的操作: %s" % (action or "(空)"))
    except PluginError as error:
        log("op=%s 失败（%s）: %s" % (action or "-", error.code, error.message))
        return fail(error.code, error.message)
    except (urllib.error.URLError, ValueError, OSError) as error:
        log("op=%s 异常: %r" % (action or "-", error))
        return fail(1011, "访问网易云失败: %s" % error)
    finally:
        elapsed = (time.time() - started) * 1000.0
        if elapsed > 2000:
            log("op=%s 耗时 %.0f ms" % (action or "-", elapsed))


LOGIN_PAGE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>网易云登录 · Rad Music</title>
<style>
  :root {
    --bg: #f6f7f9; --card: #ffffff; --line: #e3e6eb; --text: #1b1d21;
    --muted: #6b7280; --accent: #d81e06; --ok: #12805c; --warn: #b45309;
    --shadow: 0 1px 2px rgba(16, 24, 40, .06), 0 8px 24px rgba(16, 24, 40, .06);
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; background: var(--bg); color: var(--text);
    font: 14px/1.6 -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif;
  }
  .wrap { max-width: 860px; margin: 0 auto; padding: 40px 20px 64px; }
  header h1 { margin: 0 0 6px; font-size: 22px; letter-spacing: .2px; }
  header p { margin: 0; color: var(--muted); font-size: 13px; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-top: 22px; }
  .card {
    background: var(--card); border: 1px solid var(--line); border-radius: 14px;
    padding: 18px; box-shadow: var(--shadow);
  }
  .card.span { grid-column: 1 / -1; }
  .card h2 { margin: 0 0 12px; font-size: 15px; font-weight: 600; }
  .row { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 5px 0; }
  .row + .row { border-top: 1px dashed var(--line); }
  .row span { color: var(--muted); font-size: 13px; }
  .row b { font-weight: 600; font-size: 13px; }
  code {
    font-family: ui-monospace, Consolas, monospace; font-size: 12px;
    background: #f1f3f5; padding: 2px 6px; border-radius: 6px; word-break: break-all;
  }
  .btn {
    appearance: none; border: 1px solid transparent; background: var(--accent); color: #fff;
    font: inherit; font-size: 13px; padding: 8px 14px; border-radius: 10px; cursor: pointer;
  }
  .btn:hover { filter: brightness(.95); }
  .btn:disabled { opacity: .55; cursor: default; }
  .btn.ghost { background: #fff; color: var(--text); border-color: var(--line); }
  .btn.ghost:hover { background: #f4f5f7; }
  .qr-box {
    width: 280px; height: 280px; margin: 0 auto; border: 1px dashed var(--line);
    border-radius: 12px; display: flex; align-items: center; justify-content: center;
    color: var(--muted); font-size: 13px; background: #fbfcfd; overflow: hidden;
  }
  .qr-box img { width: 100%; height: 100%; object-fit: contain; }
  .state { text-align: center; margin: 12px 0 4px; font-size: 13px; color: var(--muted); min-height: 22px; }
  .state.ok { color: var(--ok); }
  .state.warn { color: var(--warn); }
  .state.err { color: var(--accent); }
  .actions { display: flex; gap: 8px; justify-content: center; margin-top: 10px; flex-wrap: wrap; }
  textarea {
    width: 100%; min-height: 86px; resize: vertical; font: inherit; font-size: 12.5px;
    padding: 10px 12px; border: 1px solid var(--line); border-radius: 10px;
    font-family: ui-monospace, Consolas, monospace; background: #fbfcfd; color: var(--text);
  }
  textarea:focus { outline: 2px solid rgba(216, 30, 6, .18); border-color: var(--accent); }
  .hint { color: var(--muted); font-size: 12.5px; margin: 8px 0 10px; }
  .foot { margin-top: 22px; color: var(--muted); font-size: 12px; text-align: center; }
  .foot a { color: var(--accent); text-decoration: none; }
  .badge {
    display: inline-block; font-size: 11.5px; padding: 1px 8px; border-radius: 999px;
    border: 1px solid var(--line); color: var(--muted); background: #fff; margin-left: 6px;
  }
  @media (max-width: 720px) { .grid { grid-template-columns: 1fr; } }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>网易云音乐登录<span class="badge" id="plugin-version">plugin</span></h1>
    <p>登录后可播放 VIP 曲目并解锁无损 / Hi-Res。凭据只写入本机插件目录的 <code>cookie.txt</code>，不会上传。</p>
  </header>

  <div class="grid">
    <section class="card span">
      <h2>当前登录态</h2>
      <div class="row"><span>账号</span><b id="who">读取中…</b></div>
      <div class="row"><span>会员</span><b id="vip">—</b></div>
      <div class="row"><span>Cookie</span><code id="ck">—</code></div>
      <div class="actions" style="justify-content:flex-start">
        <button class="btn ghost" id="refresh">刷新</button>
        <button class="btn ghost" id="logout">退出登录</button>
      </div>
    </section>

    <section class="card">
      <h2>扫码登录</h2>
      <div class="qr-box" id="qr-box">正在生成二维码…</div>
      <div class="state" id="qr-state">请用网易云音乐 App 扫码</div>
      <div class="actions">
        <button class="btn" id="qr-start">重新生成</button>
        <a class="btn ghost" id="qr-link" href="#" target="_blank" rel="noopener">在网页打开</a>
      </div>
    </section>

    <section class="card">
      <h2>用 Cookie 登录</h2>
      <p class="hint">网页版网易云 → F12 → Application → Cookies → 复制 <code>MUSIC_U</code> 的值；
      也可以直接粘贴整行 Cookie。适合无法扫码的环境。</p>
      <textarea id="cookie-input" placeholder="MUSIC_U=xxxxx  或  MUSIC_U=xxx; __csrf=yyy"></textarea>
      <div class="actions" style="justify-content:flex-start">
        <button class="btn" id="cookie-login">保存并验证</button>
      </div>
      <div class="state" id="cookie-state"></div>
    </section>
  </div>

  <p class="foot">接口：<code>POST http://127.0.0.1:8848/request</code> ·
    <a href="/health">健康检查</a> · 登录成功后可直接返回 Rad Music 播放</p>
</div>

<script>
const $ = (id) => document.getElementById(id);
let qrKey = '';
let pollTimer = null;
let pollDeadline = 0;

function setState(el, text, kind) {
  el.textContent = text || '';
  el.className = 'state' + (kind ? ' ' + kind : '');
}

async function call(op, params) {
  const response = await fetch('/request', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({op: op, params: params || {}})
  });
  const payload = await response.json().catch(() => ({code: -1, message: '响应不是 JSON'}));
  if (!response.ok && payload.code === undefined) {
    return {code: -1, message: 'HTTP ' + response.status};
  }
  return payload;
}

async function refreshStatus() {
  const result = await call('check_login', {refresh: false});
  if (result.code !== 0) {
    $('who').textContent = '读取失败：' + (result.message || result.code);
    return;
  }
  const data = result.data || {};
  $('who').textContent = data.logged_in
    ? (data.nickname || '已登录') + (data.user_id ? '（uid ' + data.user_id + '）' : '')
    : '未登录';
  $('vip').textContent = data.logged_in ? (data.vip ? '黑胶 / 音乐包已开通' : '未开通（可播标准音质）') : '—';
  $('ck').textContent = data.cookie_present ? '已配置 MUSIC_U' : '未配置';
  $('logout').disabled = !data.logged_in;
}

function stopPoll() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
}

async function pollOnce() {
  if (!qrKey) return;
  if (pollDeadline && Date.now() > pollDeadline) {
    stopPoll();
    setState($('qr-state'), '二维码已过期，请重新生成', 'err');
    return;
  }
  const result = await call('login_qr_check', {key: qrKey});
  if (result.code !== 0) {
    stopPoll();
    setState($('qr-state'), result.message || '查询失败', 'err');
    return;
  }
  const data = result.data || {};
  if (data.state === 'waiting') setState($('qr-state'), '等待扫码…', '');
  else if (data.state === 'scanned') setState($('qr-state'), '已扫码，请在手机上确认登录', 'warn');
  else if (data.state === 'expired') { stopPoll(); setState($('qr-state'), '二维码已过期，请重新生成', 'err'); }
  else if (data.state === 'success') {
    stopPoll();
    setState($('qr-state'), '登录成功' + (data.nickname ? '：' + data.nickname : ''), 'ok');
    if (data.cookie_saved === false) {
      setState($('qr-state'), '登录成功，但 cookie.txt 写入失败（重启后需重新登录）', 'warn');
    }
    $('qr-box').textContent = '已登录，可关闭本页';
    $('qr-link').removeAttribute('href');
    refreshStatus();
  } else {
    setState($('qr-state'), data.message || '未知状态', 'warn');
  }
}

async function startQr() {
  stopPoll();
  qrKey = '';
  setState($('qr-state'), '正在生成二维码…', '');
  $('qr-box').textContent = '正在生成二维码…';
  const result = await call('login_qr_start', {});
  if (result.code !== 0) {
    setState($('qr-state'), result.message || '生成失败', 'err');
    return;
  }
  const data = result.data || {};
  qrKey = data.key || '';
  const expires = Number(data.expires_in || 300);
  pollDeadline = Date.now() + expires * 1000;
  $('qr-link').href = data.qr_url || '#';
  const box = $('qr-box');
  box.innerHTML = '';
  if (data.qr_image_url) {
    const image = new Image();
    image.alt = '网易云登录二维码';
    image.onerror = () => { box.innerHTML = ''; box.appendChild(fallbackQr(data.qr_url)); };
    image.src = data.qr_image_url;
    box.appendChild(image);
  } else {
    box.appendChild(fallbackQr(data.qr_url));
  }
  setState($('qr-state'), '请用网易云音乐 App 扫码（5 分钟内有效）', '');
  const interval = Number(data.poll_interval_ms || 2000);
  pollTimer = setInterval(pollOnce, interval);
  pollOnce();
}

function fallbackQr(qrUrl) {
  const link = document.createElement('a');
  link.href = qrUrl || '#';
  link.target = '_blank';
  link.rel = 'noopener';
  link.textContent = '二维码图片加载失败，点此在网页端登录';
  link.style.fontSize = '13px';
  return link;
}

async function loginByCookie() {
  const value = $('cookie-input').value.trim();
  if (!value) { setState($('cookie-state'), '请先粘贴 Cookie', 'warn'); return; }
  setState($('cookie-state'), '正在验证…', '');
  const result = await call('login_cookie', {cookie: value});
  if (result.code !== 0) {
    setState($('cookie-state'), result.message || '登录失败', 'err');
    return;
  }
  const data = result.data || {};
  setState($('cookie-state'), '登录成功' + (data.nickname ? '：' + data.nickname : ''), 'ok');
  $('cookie-input').value = '';
  refreshStatus();
}

async function logout() {
  const result = await call('logout', {});
  if (result.code !== 0) { alert(result.message || '退出失败'); return; }
  setState($('cookie-state'), '已退出登录', '');
  stopPoll();
  refreshStatus();
  startQr();
}

$('qr-start').addEventListener('click', startQr);
$('refresh').addEventListener('click', refreshStatus);
$('logout').addEventListener('click', logout);
$('cookie-login').addEventListener('click', loginByCookie);
$('cookie-input').addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && (event.ctrlKey || event.metaKey)) loginByCookie();
});

fetch('/health').then((response) => response.json()).then((data) => {
  $('plugin-version').textContent = 'v' + (data.version || '?');
}).catch(() => {});

refreshStatus().then(startQr);
</script>
</body>
</html>
"""


SETTINGS_PAGE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>网易云插件设置 · Rad Music</title>
<style>
  :root {
    --bg: #f6f7f9; --card: #ffffff; --line: #e3e6eb; --text: #1b1d21;
    --muted: #6b7280; --accent: #d81e06; --ok: #12805c; --warn: #b45309;
    --shadow: 0 1px 2px rgba(16, 24, 40, .06), 0 8px 24px rgba(16, 24, 40, .06);
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; background: var(--bg); color: var(--text);
    font: 14px/1.6 -apple-system, "Segoe UI", "Microsoft YaHei", sans-serif;
  }
  .wrap { max-width: 760px; margin: 0 auto; padding: 32px 20px 56px; }
  header { display:flex; align-items:flex-end; justify-content:space-between; gap:12px; flex-wrap:wrap; }
  header h1 { margin: 0 0 6px; font-size: 22px; }
  header p { margin: 0; color: var(--muted); font-size: 13px; }
  .badge {
    display: inline-block; font-size: 11.5px; padding: 2px 10px; border-radius: 999px;
    border: 1px solid var(--line); color: var(--muted); background: #fff;
  }
  .grid { display: grid; grid-template-columns: 1fr; gap: 16px; margin-top: 22px; }
  .card {
    background: var(--card); border: 1px solid var(--line); border-radius: 14px;
    padding: 18px 20px; box-shadow: var(--shadow);
  }
  .card h2 { margin: 0 0 12px; font-size: 15px; font-weight: 600; }
  .row { display:flex; align-items:center; justify-content:space-between; padding:6px 0; gap: 12px; }
  .row + .row { border-top: 1px dashed var(--line); }
  .row span { color: var(--muted); font-size: 13px; }
  .row b { font-weight: 600; font-size: 13px; }
  .row code { font-family: ui-monospace, Consolas, monospace; font-size: 12px; background: #f1f3f5; padding: 2px 6px; border-radius: 6px; word-break: break-all; }
  .btn {
    appearance:none; border:1px solid transparent; background: var(--accent); color:#fff;
    font: inherit; font-size: 13px; padding: 7px 14px; border-radius: 10px; cursor: pointer;
  }
  .btn:hover { filter: brightness(.95); }
  .btn.ghost { background: #fff; color: var(--text); border-color: var(--line); }
  .btn:disabled { opacity: .55; cursor: default; }
  .actions { display:flex; gap:8px; flex-wrap: wrap; }
  .field { display:flex; align-items:center; justify-content:space-between; padding:8px 0; gap: 10px; }
  .field + .field { border-top: 1px dashed var(--line); }
  .field-info { display:flex; flex-direction:column; gap: 2px; }
  .field-info small { color: var(--muted); font-size: 12px; }
  .field-control input { font: inherit; padding: 6px 10px; border:1px solid var(--line); border-radius: 8px; width: 120px; text-align: right; }
  .state { margin-top: 8px; font-size: 12.5px; color: var(--muted); min-height: 18px; }
  .state.ok { color: var(--ok); }
  .state.warn { color: var(--warn); }
  .state.err { color: var(--accent); }
  .foot { margin-top: 22px; color: var(--muted); font-size: 12px; text-align: center; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div>
      <h1>网易云插件设置 <span class="badge" id="plugin-version">plugin</span></h1>
      <p>登录后可播放 VIP 曲目并解锁无损 / Hi-Res；凭据只写入本机插件目录的 <code>cookie.txt</code>。</p>
    </div>
    <div class="actions">
      <a class="btn ghost" href="/login">扫码登录页</a>
    </div>
  </header>

  <div class="grid">
    <section class="card">
      <h2>登录与账号</h2>
      <div class="row"><span>账号</span><b id="who">读取中…</b></div>
      <div class="row"><span>会员</span><b id="vip">—</b></div>
      <div class="row"><span>Cookie</span><code id="ck">—</code></div>
      <div class="row"><span>Endpoint</span><code id="ep">—</code></div>
      <div class="actions" style="margin-top:10px">
        <button class="btn ghost" id="refresh">刷新登录态</button>
        <button class="btn ghost" id="logout">退出登录</button>
      </div>
      <div class="state" id="login-state"></div>
    </section>

    <section class="card">
      <h2>插件行为</h2>
      <div id="settings-fields">
        <div class="state">加载中…</div>
      </div>
      <div class="actions" style="margin-top:12px">
        <button class="btn" id="save">保存</button>
        <button class="btn ghost" id="reload">重新读取</button>
      </div>
      <div class="state" id="settings-state"></div>
    </section>

    <section class="card">
      <h2>版本与诊断</h2>
      <div class="row"><span>插件版本</span><b id="ver">—</b></div>
      <div class="row"><span>Endpoint</span><code id="endpoint">—</code></div>
      <div class="row"><span>Cookie 文件</span><code id="cookie-file">—</code></div>
      <div class="row"><span>配置文件</span><code id="config-file">—</code></div>
      <div class="actions" style="margin-top:10px">
        <a class="btn ghost" href="/health" target="_blank" rel="noopener">/health</a>
      </div>
    </section>
  </div>

  <p class="foot">Rad Music · 网易云插件 · 修改后需要重新启动插件进程才会在客户端生效</p>
</div>

<script>
const $ = (id) => document.getElementById(id);

function setState(el, text, kind) {
  if (!el) return;
  el.textContent = text || '';
  el.className = 'state' + (kind ? ' ' + kind : '');
}

async function call(op, params) {
  const response = await fetch('/request', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({op: op, params: params || {}})
  });
  return await response.json().catch(() => ({code: -1, message: '响应不是 JSON'}));
}

let SCHEMA = [];
let VALUES = {};

function renderFields() {
  const host = $('settings-fields');
  if (!SCHEMA.length) {
    host.innerHTML = '<div class="state">没有可配置项</div>';
    return;
  }
  host.innerHTML = SCHEMA.map((field) => {
    const value = VALUES[field.key];
    const safe = (value === undefined || value === null) ? '' : String(value);
    return '<div class="field">' +
      '<div class="field-info"><strong>' + (field.label || field.key) + '</strong>' +
      (field.hint ? '<small>' + field.hint + '</small>' : '') + '</div>' +
      '<div class="field-control"><input id="f-' + field.key + '" type="number" ' +
      (field.min !== undefined ? 'min="' + field.min + '" ' : '') +
      (field.max !== undefined ? 'max="' + field.max + '" ' : '') +
      'value="' + safe + '" /></div>' +
      '</div>';
  }).join('');
}

async function loadAll() {
  const result = await call('settings');
  if (result.code !== 0) {
    setState($('login-state'), '加载设置失败：' + (result.message || result.code), 'err');
    return;
  }
  const data = result.data || {};
  // 登录信息
  const login = data.login || {};
  if (login.logged_in) {
    $('who').textContent = (login.nickname || '已登录') + (login.user_id ? '（uid ' + login.user_id + '）' : '');
    $('vip').textContent = login.vip ? '黑胶 / 音乐包已开通' : '未开通（可播标准音质）';
  } else {
    $('who').textContent = '未登录';
    $('vip').textContent = '—';
  }
  $('ck').textContent = login.cookie_present ? '已配置 MUSIC_U' : '未配置';
  $('ep').textContent = data.endpoint || '—';
  $('ver').textContent = 'v' + (data.version || '?');
  $('endpoint').textContent = data.endpoint || '—';
  $('cookie-file').textContent = data.cookie_file || '—';
  $('config-file').textContent = data.config_file || '—';
  $('logout').disabled = !login.logged_in;

  SCHEMA = Array.isArray(data.schema) ? data.schema : [];
  VALUES = Object.assign({}, data.values || {});
  for (const field of SCHEMA) {
    if (!(field.key in VALUES) && field.default !== undefined) VALUES[field.key] = field.default;
  }
  renderFields();
}

async function refreshStatus() {
  const result = await call('check_login', {refresh: true});
  if (result.code !== 0) {
    setState($('login-state'), '刷新失败：' + (result.message || result.code), 'err');
    return;
  }
  setState($('login-state'), '登录态已刷新', 'ok');
  loadAll();
}

async function logout() {
  const result = await call('logout');
  if (result.code !== 0) { setState($('login-state'), result.message || '退出失败', 'err'); return; }
  setState($('login-state'), '已退出登录', 'ok');
  loadAll();
}

async function saveSettings() {
  const next = {};
  for (const field of SCHEMA) {
    const input = $('f-' + field.key);
    if (!input) continue;
    let value = parseFloat(input.value);
    if (Number.isNaN(value)) value = field.default;
    if (field.min !== undefined && value < field.min) value = field.min;
    if (field.max !== undefined && value > field.max) value = field.max;
    next[field.key] = value;
  }
  // 设置面板只显示，不会同步回插件内存；提示用户重启插件
  VALUES = next;
  setState($('settings-state'),
    '设置已记录，下次启动插件生效（修改 config.json 后请重启 Rad Music 服务或插件进程）', 'ok');
}

$('refresh').addEventListener('click', refreshStatus);
$('logout').addEventListener('click', logout);
$('save').addEventListener('click', saveSettings);
$('reload').addEventListener('click', loadAll);

fetch('/health').then((r) => r.json()).then((data) => {
  $('plugin-version').textContent = 'v' + (data.version || '?');
}).catch(() => {});

loadAll();
</script>
</body>
</html>
"""


class Handler(BaseHTTPRequestHandler):
    server_version = "RadMusicNetease/" + VERSION
    protocol_version = "HTTP/1.1"
    timeout = 60

    def log_message(self, fmt, *args):
        # 不往 stdout 打日志（宿主可能没有控制台）
        log("http " + (fmt % args))

    def _send(self, code, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _health(self):
        state = CLIENT.account_cached() if CLIENT else None
        return {
            "ok": True,
            "platform": "netease",
            "plugin_id": "netease.source",
            "version": VERSION,
            "logged_in": bool(state and state.get("logged_in")),
            "nickname": (state or {}).get("nickname", ""),
            "cookie_present": bool(CLIENT and CLIENT.cookies.get("MUSIC_U")),
            "login_page": "/login",
            "ops": sorted(set(list(OP_ALIASES.values()) + ["search"])),
        }

    def _send_html(self, body):
        raw = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        path = self.path.split("?")[0].rstrip("/")
        if path in ("/health", ""):
            self._send(200, self._health())
        elif path == "/version":
            self._send(200, {"version": VERSION, "platform": "netease"})
        elif path == "/login":
            self._send_html(LOGIN_PAGE)
        elif path == "/settings":
            self._send_html(SETTINGS_PAGE)
        else:
            self._send(404, {"code": 1004, "message": "not found"})

    def do_POST(self):
        if self.path.split("?")[0].rstrip("/") not in ("/request", ""):
            self._send(404, {"code": 1004, "message": "not found"})
            return
        length = as_int(self.headers.get("Content-Length"), 0)
        raw = self.rfile.read(length) if length > 0 else b"{}"
        payload = parse_payload(raw)
        if payload is None:
            self._send(200, fail(1007, "请求不是合法 JSON"))
            return
        try:
            result = handle_request(payload)
        except Exception as error:  # noqa: BLE001 - 插件自身异常不要拖垮进程
            log("未捕获异常: %r" % (error,))
            result = fail(1011, "插件内部错误: %s" % error)
        self._send(200, result)


def ping_existing(host, port, timeout=0.6):
    """端口上是否已有健康的插件实例（避免启动两份，Windows 的 SO_REUSEADDR 会允许重复绑定）"""
    url = "http://%s:%d/health" % (host, port)
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            payload = parse_payload(response.read())
    except (urllib.error.URLError, socket.timeout, OSError):
        return False
    return bool(payload and payload.get("platform") == "netease")


def main():
    parser = argparse.ArgumentParser(description="Rad Music 网易云播放源插件")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8848)
    parser.add_argument("--qps", type=int, default=0, help="覆盖 config.json 的限流 QPS")
    parser.add_argument("--check", action="store_true", help="只做一次自检（搜索 + 取地址）后退出")
    args = parser.parse_args()

    if sys.stdout is None:
        sys.stdout = open(os.devnull, "w", encoding="utf-8")
    if sys.stderr is None:
        sys.stderr = open(os.devnull, "w", encoding="utf-8")

    # 让 /settings / /login 模板里的 endpoint / qps 等也能拿到命令行参数
    global SERVER_HOST, SERVER_PORT
    SERVER_HOST = args.host
    SERVER_PORT = args.port

    global CLIENT
    config = load_config()
    credential = load_cookie(config)
    qps = args.qps if args.qps > 0 else as_int(config.get("qps"), 6)
    CLIENT = NeteaseClient(
        cookie=credential,
        qps=qps,
        timeout=as_int(config.get("timeout_seconds"), 20),
        retries=as_int(config.get("retries"), 2),
    )
    log("插件启动 v%s：http://%s:%d（登录态：%s，QPS：%s）"
        % (VERSION, args.host, args.port, "有" if credential else "无", qps))

    if args.check:
        state = CLIENT.account_state()
        print("登录态: %s" % ("已登录 " + state.get("nickname", "") if state["logged_in"] else "未登录"))
        result = CLIENT.search("海阔天空", 1, 3, "song")
        print("搜索结果: %d 条" % len(result["items"]))
        for item in result["items"]:
            print("  %s - %s (%ss) %s %s" % (item["name"], "/".join(item["artists"]),
                                            item["duration_seconds"],
                                            "可用" if item["available"] else "不可用·" + item["reason"],
                                            ",".join(quality["label"] for quality in item["qualities"])))
            if item["available"]:
                try:
                    url = CLIENT.url_for(item["id"], "")
                    print("  播放地址: %s ... (%s)" % (url["url"][:64], url["quality"]["label"]))
                except PluginError as error:
                    print("  播放地址失败: %s" % error.message)
                break
        return 0

    if ping_existing(args.host, args.port):
        message = "检测到 %s:%d 已有网易云插件实例在运行，本次启动退出（复用现有实例）" % (args.host, args.port)
        log(message)
        print(message, flush=True)
        return 0

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    print("Rad Music 网易云插件已启动：http://%s:%d" % (args.host, args.port), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        log("插件退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
