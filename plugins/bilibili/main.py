#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Rad Music - 哔哩哔哩（bilibili）播放源插件

这是一个「外部进程播放源插件」：宿主 radmusic_server 通过本地 HTTP JSON-RPC 调用它，
它再访问 bilibili 的 Web API，把视频翻译成 Rad Music 的模型（Readme 5.2 / 5.5）：
  · 听视频：一个视频 = 一首曲目，只取 DASH 音轨（不拉画面），可当音乐听
  · MV    ：一个视频 = 一条 MV，取混流 MP4（画面 + 声音）

启动（宿主会自动拉起，也可手动）：
    python plugins/bilibili/main.py --port 8849

协议：
    GET  /health                  → 200 {"ok": true, "version": "..."}
    POST /request                 → {"op": "...", "params": {...}} → {"code": 0, "data": {...}}
    GET  /media?u=&k=&e=&s=       → 媒体代理（带 Referer 回源 + Range 透传，bilibili 防盗链必需）
    GET  /login                   → Cookie 登录页（网页操作，写本机 cookie.txt）

支持的 op（与 plugin.json 的 capabilities 一致）：
    search        关键词搜索视频（作为「听视频」的曲目来源）
    get_song      视频元数据（可直接落 base.json）
    get_url       听视频：按音质返回 DASH 音轨地址（经本机代理）
    get_qualities 该视频可用的音频音质
    get_lyric     CC 字幕转 LRC（没有字幕时返回 1015，非致命）
    get_pic       封面
    get_playlist  收藏夹（fav:<id>）/ UP 主投稿（up:<mid>）/ 合集（season:<mid>:<season_id>）
    search_mv     搜索 MV（视频）
    get_mv        MV 播放地址（混流 MP4，带声音）
    check_login   当前登录态
    login_cookie  用 Cookie / SESSDATA 登录
    logout        退出登录（清除本机 Cookie）

登录（可选，用于 1080P+、大会员音质、收藏夹、UP 主投稿）：
    1) 环境变量   set RAD_BILIBILI_COOKIE="SESSDATA=...; bili_jct=..."
    2) cookie.txt 本目录下整行 Cookie 字符串
    3) config.json 里写 {"cookie": "..."}
    4) 浏览器打开 http://127.0.0.1:8849/login 直接粘贴
    Cookie 优先级：环境变量 > cookie.txt > config.json

为什么必须走 /media 代理：
    bilibili 的音视频 CDN 校验 Referer（缺 Referer 直接 403），并且返回
    Content-Type: application/octet-stream；浏览器的 <audio>/<video> 既改不了
    Referer，也认不出 MIME。所以插件在本地起一个只读代理，回源时补上 Referer 与 UA、
    透传 Range（拖动进度条必需），并把 Content-Type 纠正成 audio/mp4 / video/mp4。
"""

import argparse
import base64
import hashlib
import hmac
import html
import ipaddress
import json
import os
import re
import secrets
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = "1.1.0"

API = "https://api.bilibili.com"
REFERER = "https://www.bilibili.com/"
USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
)
HERE = os.path.dirname(os.path.abspath(__file__))
LOG_FILE = os.path.join(HERE, "plugin.log")
COOKIE_FILE = os.path.join(HERE, "cookie.txt")
CONFIG_FILE = os.path.join(HERE, "config.json")

DEFAULT_PORT = 8849
PORT = DEFAULT_PORT            # /media 代理地址要用它拼绝对 URL
TIMEOUT = 20
RETRIES = 2
QPS = 5
PROXY_SECRET = secrets.token_hex(16)   # 每次进程启动重新生成：签名 URL 只在本进程有效
PROXY_TTL = 4 * 3600

# 音质定义：id 与宿主清单 plugin.json 的 quality_levels 一一对应（Readme 5.4）
QUALITY_SPECS = {
    "hires": {"label": "Hi-Res 无损", "level": "hires", "codec": "flac", "lossless": True,
              "bitrate": 1411, "sample_rate": 96000, "bit_depth": 24, "channels": 2},
    "dolby": {"label": "杜比全景声", "level": "high", "codec": "ec3", "lossless": False,
              "bitrate": 448, "sample_rate": 48000, "channels": 6},
    "a192": {"label": "192K", "level": "high", "codec": "aac", "lossless": False,
             "bitrate": 192, "sample_rate": 44100, "channels": 2},
    "a132": {"label": "132K", "level": "medium", "codec": "aac", "lossless": False,
             "bitrate": 132, "sample_rate": 44100, "channels": 2},
    "a64": {"label": "64K", "level": "low", "codec": "aac", "lossless": False,
            "bitrate": 64, "sample_rate": 44100, "channels": 2},
}
# 高 → 低
QUALITY_ORDER = ["hires", "dolby", "a192", "a132", "a64"]
QUALITY_RANK = {qid: len(QUALITY_ORDER) - index for index, qid in enumerate(QUALITY_ORDER)}

# bilibili DASH 音轨 id → 音质 id
AUDIO_QUALITY_BY_ID = {
    30251: "hires",   # Hi-Res 无损 FLAC（大会员）
    30250: "dolby",   # 杜比全景声（大会员）
    30280: "a192",    # 192K
    30232: "a132",    # 132K
    30216: "a64",     # 64K
}

# 宿主可能把归一化 level 或码率当音质传进来，统一映射到 quality_id
QUALITY_ALIASES = {
    "hires": "hires", "hi-res": "hires", "hi_res": "hires", "hi res": "hires",
    "lossless": "hires", "flac": "hires", "无损": "hires", "hires_lossless": "hires",
    "dolby": "dolby", "atmos": "dolby", "杜比": "dolby", "杜比全景声": "dolby",
    "a192": "a192", "192": "a192", "192k": "a192", "192000": "a192",
    "high": "a192", "exhigh": "a192", "320": "a192",
    "a132": "a132", "132": "a132", "132k": "a132", "132000": "a132", "medium": "a132",
    "a64": "a64", "64": "a64", "64k": "a64", "64000": "a64",
    "low": "a64", "standard": "a64",
}

# 视频清晰度（qn）：用于 MV 的显示规格
VIDEO_QN = {
    127: ("8K", 7680, 4320), 126: ("杜比视界", 3840, 2160), 125: ("HDR", 3840, 2160),
    120: ("4K", 3840, 2160), 116: ("1080P60", 1920, 1080), 112: ("1080P+", 1920, 1080),
    80: ("1080P", 1920, 1080), 74: ("720P60", 1280, 720), 64: ("720P", 1280, 720),
    32: ("480P", 852, 480), 16: ("360P", 640, 360), 6: ("240P", 426, 240),
}

# WBI 签名的字符重排表（bilibili 前端固定常量）
MIXIN_KEY_ENC_TAB = [
    46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35, 27, 43, 5, 49,
    33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13, 37, 48, 7, 16, 24, 55, 40, 61,
    26, 17, 0, 1, 60, 51, 30, 4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36,
    20, 34, 44, 52,
]

OP_ALIASES = {
    "song": "get_song", "info": "get_song", "detail": "get_song", "video": "get_song",
    "url": "get_url", "play_url": "get_url", "stream": "get_url", "audio": "get_url",
    "quality": "get_qualities", "qualities": "get_qualities",
    "lyric": "get_lyric", "subtitle": "get_lyric", "cc": "get_lyric",
    "pic": "get_pic", "cover": "get_pic",
    "playlist": "get_playlist", "fav": "get_playlist", "favorites": "get_playlist",
    "favlist": "get_playlist", "season": "get_playlist",
    "mv": "get_mv", "mv_search": "search_mv", "search_video": "search_mv",
    "login": "check_login", "state": "check_login", "account": "check_login",
    "cookie": "login_cookie",
    "settings": "settings_get",
}


# ---------------------------------------------------------------- 基础工具
def log(message):
    """写插件日志（超过 4 MiB 轮转一份 .1）"""
    try:
        if os.path.exists(LOG_FILE) and os.path.getsize(LOG_FILE) > 4 * 1024 * 1024:
            try:
                os.replace(LOG_FILE, LOG_FILE + ".1")
            except OSError:
                pass
        with open(LOG_FILE, "a", encoding="utf-8") as handle:
            handle.write("[%s] %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), message))
    except OSError:
        pass


def as_int(value, default=0):
    try:
        if value is None or value == "":
            return default
        return int(float(value))
    except (TypeError, ValueError):
        return default


def sha_for(item_key):
    return hashlib.sha256(("bilibili:" + str(item_key)).encode("utf-8")).hexdigest()


def mv_sha_for(item_key):
    # MV 与同名曲目 id 会撞车，加前缀区分（Readme 1.13 规则 1）
    return hashlib.sha256(("bilibili:mv:" + str(item_key)).encode("utf-8")).hexdigest()


def normalize_cover(url):
    """封面统一 https（bilibili 常返回 //i0.hdslb.com/... 协议相对地址）"""
    if not url:
        return ""
    text = str(url).strip()
    if text.startswith("//"):
        text = "https:" + text
    elif text.startswith("http://"):
        text = "https://" + text[len("http://"):]
    return text


def strip_tags(text):
    """去掉搜索结果的 <em class="keyword"> 高亮并还原 HTML 实体"""
    if not text:
        return ""
    cleaned = re.sub(r"<[^>]+>", "", str(text))
    return html.unescape(cleaned).strip()


def parse_duration(text):
    """bilibili 的 "3:54" / "845:22" / "1:02:03" → 秒"""
    if isinstance(text, (int, float)):
        return max(0, int(text))
    raw = str(text or "").strip()
    if not raw:
        return 0
    if raw.isdigit():
        return int(raw)
    total = 0
    for part in raw.split(":"):
        if not part.isdigit():
            return 0
        total = total * 60 + int(part)
    return total


def quality_json(quality_id):
    spec = QUALITY_SPECS.get(quality_id)
    if not spec:
        return {}
    data = dict(spec)
    data["id"] = quality_id
    data["quality_id"] = quality_id
    return data


def normalize_quality(value):
    """把宿主传来的音质（id / level / 码率 / 中文）归一化到 quality_id；无法识别返回 None"""
    text = str(value or "").strip().lower()
    if not text or text in ("auto", "best", "default", "0"):
        return None
    if text in QUALITY_SPECS:
        return text
    return QUALITY_ALIASES.get(text)


def pick_audio(audios, requested):
    """从高到低排列的音轨里挑一条：目标档不可用时降到「不高于目标」的最高档"""
    if not audios:
        return None
    wanted = normalize_quality(requested)
    if wanted is None:
        return audios[0]
    rank = QUALITY_RANK.get(wanted, 0)
    for audio in audios:
        if QUALITY_RANK.get(audio["quality_id"], 0) <= rank:
            return audio
    return audios[-1]


def parse_item_id(raw):
    """视频标识 → (bvid, aid, 分P)；支持 BV 号 / av 号 / 数字 aid / 视频链接"""
    text = str(raw or "").strip()
    if not text:
        raise PluginError(1007, "缺少参数 id（视频标识：BV 号 / av 号 / 视频链接）")

    page = 1
    match = re.search(r"[?&#]p=(\d+)", text)
    if match:
        page = max(1, as_int(match.group(1), 1))
    else:
        match = re.search(r":p(\d+)$", text, re.IGNORECASE)
        if match:
            page = max(1, as_int(match.group(1), 1))

    body = re.split(r"[?&#]", text)[0].strip().rstrip("/")
    match = re.search(r"bv([0-9a-zA-Z]{10})", body, re.IGNORECASE)
    if match:
        return "BV" + match.group(1), None, page
    match = re.search(r"av(\d+)", body, re.IGNORECASE)
    if match:
        return None, int(match.group(1)), page
    if body.isdigit():
        return None, int(body), page
    raise PluginError(1007, "无法识别的视频标识: %s（支持 BV 号 / av 号 / 视频链接）" % raw)


def item_key_of(bvid, page):
    return bvid if page <= 1 else "%s?p=%d" % (bvid, page)


def mask_cookie(text):
    """日志/接口里脱敏 Cookie，只留 SESSDATA 前 6 位"""
    match = re.search(r"SESSDATA=([^;]+)", text or "")
    if not match:
        return "(未配置)"
    value = match.group(1)
    return "SESSDATA=%s…" % value[:6]


# ---------------------------------------------------------------- 异常与基础设施
class PluginError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


class TtlCache:
    def __init__(self, max_entries=1024):
        self._data = {}
        self._lock = threading.Lock()
        self._max = max_entries

    def get(self, key):
        with self._lock:
            entry = self._data.get(key)
            if not entry:
                return None
            expires, value = entry
            if expires < time.time():
                self._data.pop(key, None)
                return None
            return value

    def set(self, key, value, ttl):
        with self._lock:
            if len(self._data) >= self._max:
                # 简单清理：先丢已过期的，再丢最早写入的
                now = time.time()
                for old in [k for k, v in self._data.items() if v[0] < now][:64]:
                    self._data.pop(old, None)
                if len(self._data) >= self._max:
                    self._data.pop(next(iter(self._data)), None)
            self._data[key] = (time.time() + ttl, value)

    def invalidate(self, prefix=""):
        with self._lock:
            for key in [k for k in self._data if k.startswith(prefix)]:
                self._data.pop(key, None)


class RateLimiter:
    def __init__(self, qps):
        self.qps = qps
        self._interval = 1.0 / qps if qps > 0 else 0.0
        self._next = 0.0
        self._lock = threading.Lock()

    def acquire(self):
        if self._interval <= 0:
            return
        with self._lock:
            now = time.time()
            if now < self._next:
                time.sleep(min(1.0, self._next - now))
                now = self._next
            self._next = now + self._interval


class CookieStore:
    """Cookie 持久化：环境变量 > cookie.txt > config.json"""

    def __init__(self, config):
        self.text = ""
        self.source = ""
        self._config = config or {}
        self.load()

    def load(self):
        env = str(os.environ.get("RAD_BILIBILI_COOKIE") or "").strip()
        if env:
            self.text, self.source = env, "环境变量"
            return
        try:
            with open(COOKIE_FILE, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read().strip()
            if text:
                self.text, self.source = text, "cookie.txt"
                return
        except OSError:
            pass
        configured = str((self._config or {}).get("cookie") or "").strip()
        if configured:
            self.text, self.source = configured, "config.json"
        else:
            self.text, self.source = "", ""

    def save(self, text):
        text = str(text or "").strip()
        with open(COOKIE_FILE, "w", encoding="utf-8") as handle:
            handle.write(text + "\n")
        self.text = text
        self.source = "cookie.txt"

    def clear(self):
        self.text = ""
        self.source = ""
        try:
            if os.path.exists(COOKIE_FILE):
                os.remove(COOKIE_FILE)
        except OSError:
            pass

    def has_sessdata(self):
        return "SESSDATA=" in (self.text or "")

    def sessdata(self):
        match = re.search(r"SESSDATA=([^;]+)", self.text or "")
        return match.group(1) if match else ""


# ---------------------------------------------------------------- bilibili 客户端
class WbiSigner:
    """bilibili WBI 签名：nav 接口拿 img_key/sub_key → 重排取 32 位 mixin key → md5 拼 w_rid"""

    def __init__(self, client):
        self._client = client
        self._mixin = ""
        self._stamp = 0.0
        self._lock = threading.Lock()

    def invalidate(self):
        with self._lock:
            self._mixin = ""
            self._stamp = 0.0

    def _key(self):
        with self._lock:
            if self._mixin and time.time() - self._stamp < 900:
                return self._mixin
        data = self._client.http("/x/web-interface/nav", action="获取签名密钥", tolerate=(-101,))
        images = data.get("wbi_img") or {}
        img = str(images.get("img_url") or "").rsplit("/", 1)[-1].split(".")[0]
        sub = str(images.get("sub_url") or "").rsplit("/", 1)[-1].split(".")[0]
        if not img or not sub:
            raise PluginError(1011, "bilibili 未返回签名密钥（wbi_img 为空）")
        origin = img + sub
        mixin = "".join(origin[index] for index in MIXIN_KEY_ENC_TAB if index < len(origin))[:32]
        with self._lock:
            self._mixin = mixin
            self._stamp = time.time()
        return mixin

    def sign(self, params):
        payload = {}
        for key, value in (params or {}).items():
            if value is None:
                continue
            payload[str(key)] = value if isinstance(value, str) else str(value)
        payload["wts"] = str(int(time.time()))
        query = urllib.parse.urlencode(sorted(payload.items()))
        query = "".join(char for char in query if char not in "!'()*")
        return query + "&w_rid=" + hashlib.md5((query + self._key()).encode("utf-8")).hexdigest()


class BiliClient:
    def __init__(self, cookies):
        self.cookies = cookies
        self.cache = TtlCache()
        self.limiter = RateLimiter(QPS)
        self.wbi = WbiSigner(self)
        self._buvid = ""
        self._buvid_lock = threading.Lock()
        # 暴露给设置面板（不含 cookie，cookie 由专用 op 管理）
        self.config = {
            "qps": QPS,
            "timeout_seconds": TIMEOUT,
            "retries": RETRIES,
        }

    # -------------------------------------------------- HTTP
    def _headers(self, referer=REFERER, extra=None):
        headers = {
            "User-Agent": USER_AGENT,
            "Referer": referer,
            "Accept": "application/json, text/plain, */*",
            "Accept-Language": "zh-CN,zh;q=0.9",
        }
        cookie = self.cookie_header()
        if cookie:
            headers["Cookie"] = cookie
        if extra:
            headers.update(extra)
        return headers

    def cookie_header(self):
        parts = []
        if self._buvid:
            parts.append(self._buvid)
        if self.cookies.text:
            parts.append(self.cookies.text)
        return "; ".join(parts)

    def ensure_buvid(self):
        """没有登录 Cookie 时补一个 buvid3/buvid4，降低被风控的概率"""
        if self._buvid or self.cookies.has_sessdata():
            return
        with self._buvid_lock:
            if self._buvid:
                return
            try:
                data = self.http("/x/frontend/finger/spi", action="获取设备标识")
                b3 = str(data.get("b_3") or "")
                b4 = str(data.get("b_4") or "")
                if b3:
                    self._buvid = "buvid3=%s" % b3 + ("; buvid4=%s" % b4 if b4 else "")
            except PluginError as error:
                log("获取 buvid 失败：%s" % error.message)

    def http(self, path, params=None, action="请求", sign=False, tolerate=(), data=None, referer=REFERER):
        """GET bilibili API；返回 data 字段。业务码非 0 抛 PluginError（tolerate 里的码放行）"""
        base = path if re.match(r"^https?://", path) else API + path
        last_error = None
        for attempt in range(RETRIES + 1):
            url = base
            if params:
                query = self.wbi.sign(params) if sign else urllib.parse.urlencode(params)
                url += ("&" if "?" in url else "?") + query
            self.limiter.acquire()
            try:
                request = urllib.request.Request(url, data=data, headers=self._headers(referer=referer))
                with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
                    body = response.read()
            except urllib.error.HTTPError as error:
                body = b""
                try:
                    body = error.read()
                except Exception:  # noqa: BLE001 - 读取错误体失败不影响主流程
                    pass
                if error.code == 412:
                    raise PluginError(1014, "%s被 bilibili 风控拦截（412），请稍后重试或配置登录 Cookie" % action)
                if error.code == 404:
                    raise PluginError(1004, "%s失败：资源不存在（404）" % action)
                if error.code in (429, 500, 502, 503, 504) and attempt < RETRIES:
                    last_error = error
                    time.sleep(0.6 * (attempt + 1))
                    continue
                raise PluginError(1011, "%s失败：HTTP %d %s" % (
                    action, error.code, body[:120].decode("utf-8", "replace")))
            except (urllib.error.URLError, OSError, ValueError) as error:
                last_error = error
                if attempt < RETRIES:
                    time.sleep(0.5 * (attempt + 1))
                    continue
                raise PluginError(1011, "%s失败：无法访问 bilibili（%s）" % (action, error))

            try:
                payload = json.loads(body.decode("utf-8", "replace") or "{}")
            except ValueError:
                raise PluginError(1011, "%s失败：返回的不是合法 JSON（可能被风控或需要登录）" % action)
            code = as_int(payload.get("code"), 0)
            if code == 0 or code in tolerate:
                return payload.get("data") or {}
            # 签名失效（-403）时刷新 mixin key 重签一次
            if code == -403 and sign and attempt == 0:
                self.wbi.invalidate()
                continue
            if code in (-799, -509) and attempt < RETRIES:
                last_error = PluginError(1014, "请求过于频繁（%d）" % code)
                time.sleep(0.8 * (attempt + 1))
                continue
            raise self._business_error(code, action)
        raise PluginError(1014, "请求失败：%s" % (last_error or "重试次数用尽"))

    @staticmethod
    def _business_error(code, action):
        messages = {
            -101: "该内容需要登录 bilibili（可在插件登录页粘贴 Cookie）",
            -400: "请求参数错误",
            -403: "访问权限不足（可能需要登录或签名失效）",
            -404: "视频不存在或已失效",
            -352: "触发 bilibili 风控（-352），请稍后重试或配置登录 Cookie",
            -799: "请求过于频繁（-799），请稍后重试",
            87007: "该清晰度需要大会员",
            87008: "该清晰度需要大会员",
            -10403: "该视频存在地区限制，当前网络无法播放",
            62002: "稿件不可见",
            62003: "稿件审核中",
            62004: "稿件审核中",
        }
        text = messages.get(code)
        if text is None:
            text = "bilibili 返回错误码 %d" % code
        if code in (-101, 87007, 87008, -10403):
            return PluginError(1021, text)
        if code in (-404, 62002, 62003, 62004):
            return PluginError(1004, text)
        if code == -400:
            return PluginError(1007, "视频标识不合法或请求参数错误")
        if code in (-352, -799, -509):
            return PluginError(1014, text)
        return PluginError(1011, "%s（%s）" % (text, action))

    # -------------------------------------------------- 账号
    def nav(self):
        data = self.http("/x/web-interface/nav", action="检查登录态", tolerate=(-101,))
        return data

    def account_state(self, refresh=False):
        if not refresh:
            cached = self.cache.get("account")
            if cached is not None:
                return cached
        data = self.nav()
        vip = as_int(data.get("vipStatus"), 0)
        state = {
            "logged_in": bool(data.get("isLogin")),
            "uname": data.get("uname") or "",
            "mid": data.get("mid") or "",
            "face": normalize_cover(data.get("face")),
            "level": as_int((data.get("level_info") or {}).get("current_level")),
            "vip": vip == 1,
            "vip_label": {0: "普通用户", 1: "大会员", 2: "年度大会员"}.get(as_int(data.get("vipType")), "普通用户"),
            "cookie_configured": bool(self.cookies.text),
            "cookie_masked": mask_cookie(self.cookies.text),
            "cookie_source": self.cookies.source,
            "money": as_int(data.get("money")),
        }
        self.cache.set("account", state, 60)
        return state

    def login_with_cookie(self, text):
        text = str(text or "").strip()
        if not text:
            raise PluginError(1007, "Cookie 不能为空（至少需要 SESSDATA）")
        # 允许只粘贴 SESSDATA 的值
        if "=" not in text:
            text = "SESSDATA=" + text
        text = re.sub(r"\s+", " ", text).strip().strip(";")
        if "SESSDATA=" not in text:
            raise PluginError(1007, "Cookie 里没有 SESSDATA，无法登录（可只粘贴 SESSDATA 的值）")
        previous = self.cookies.text
        self.cookies.text = text
        self.cache.invalidate("account")
        self._buvid = ""
        try:
            state = self.account_state(refresh=True)
        except PluginError:
            self.cookies.text = previous
            self.cache.invalidate("account")
            raise
        if not state["logged_in"]:
            self.cookies.text = previous
            self.cache.invalidate("account")
            raise PluginError(1003, "Cookie 无效或已过期（nav 返回未登录）")
        self.cookies.save(text)
        self.cache.invalidate()
        log("登录成功：%s（uid=%s，%s）" % (state["uname"], state["mid"], state["vip_label"]))
        return state

    def logout(self):
        self.cookies.clear()
        self.cache.invalidate()
        self._buvid = ""
        log("已退出登录")
        return {"logged_in": False, "cookie_configured": False}

    # -------------------------------------------------- 视频
    def video(self, raw_id, force=False):
        bvid, aid, page = parse_item_id(raw_id)
        cache_key = "view:%s:%s:%d" % (bvid or "", aid or "", page)
        if not force:
            cached = self.cache.get(cache_key)
            if cached is not None:
                return cached

        params = {"bvid": bvid} if bvid else {"aid": aid}
        data = self.http("/x/web-interface/view", params, action="获取视频信息")
        if not data or not data.get("bvid"):
            raise PluginError(1004, "视频不存在: %s" % raw_id)

        pages = data.get("pages") or []
        index = min(max(page, 1), len(pages)) - 1 if pages else 0
        part = pages[index] if pages and index < len(pages) else {}
        cid = part.get("cid") or data.get("cid")
        if not cid:
            raise PluginError(1004, "视频缺少 cid，无法取播放地址: %s" % raw_id)

        canonical = item_key_of(data["bvid"], index + 1)
        title = strip_tags(data.get("title") or "")
        part_title = strip_tags(part.get("part") or "")
        if len(pages) > 1 and part_title and part_title not in title:
            display = "%s - P%d %s" % (title, index + 1, part_title)
        else:
            display = title
        owner = data.get("owner") or {}
        state = as_int(data.get("state"), 0)
        video = {
            "bvid": data["bvid"],
            "aid": as_int(data.get("aid")),
            "cid": as_int(cid),
            "item_id": canonical,
            "page": index + 1,
            "page_count": len(pages),
            "part_title": part_title,
            "title": title,
            "display_title": display,
            "author": strip_tags(owner.get("name") or ""),
            "mid": as_int(owner.get("mid")),
            "pic": normalize_cover(data.get("pic")),
            "duration": as_int(part.get("duration") or data.get("duration")),
            "pubdate": as_int(data.get("pubdate")),
            "desc": data.get("desc") or "",
            "tname": data.get("tname_v2") or data.get("tname") or "",
            "state": state,
            "is_ugc_pay": as_int((data.get("rights") or {}).get("ugc_pay")),
            "stat": data.get("stat") or {},
            "dimension": data.get("dimension") or {},
            "available": state == 0,
            "reason": "" if state == 0 else "稿件当前不可见（state=%d）" % state,
        }
        self.cache.set(cache_key, video, 600)
        return video

    def playurl(self, video, fnval=4048, qn=0):
        """取播放地址；优先 WBI 端点，失败退回旧端点"""
        cache_key = "playurl:%s:%d:%d:%d" % (video["bvid"], video["cid"], fnval, qn)
        cached = self.cache.get(cache_key)
        if cached is not None:
            return cached
        params = {"bvid": video["bvid"], "cid": video["cid"], "fnval": fnval, "fnver": 0, "fourk": 1}
        if qn:
            params["qn"] = qn
        try:
            self.ensure_buvid()
            data = self.http("/x/player/wbi/playurl", params, action="获取播放地址", sign=True)
        except PluginError as error:
            if error.code not in (1011, 1014):
                raise
            data = self.http("/x/player/playurl", params, action="获取播放地址")
        if not data:
            raise PluginError(1021, "bilibili 没有返回播放地址")
        self.cache.set(cache_key, data, 60)
        return data

    def dash_audios(self, video):
        """DASH 音轨列表（按音质从高到低）"""
        data = self.playurl(video, fnval=4048)
        dash = data.get("dash") or {}
        audios = []
        seen = set()
        for entry in (dash.get("audio") or []):
            if not isinstance(entry, dict):
                continue
            url = entry.get("baseUrl") or entry.get("base_url") or ""
            if not url:
                backups = entry.get("backupUrl") or entry.get("backup_url") or []
                url = backups[0] if backups else ""
            if not url:
                continue
            audio_id = as_int(entry.get("id"))
            quality_id = AUDIO_QUALITY_BY_ID.get(audio_id)
            if not quality_id:
                bandwidth = as_int(entry.get("bandwidth"))
                if bandwidth >= 180000:
                    quality_id = "a192"
                elif bandwidth >= 90000:
                    quality_id = "a132"
                else:
                    quality_id = "a64"
            if quality_id in seen:
                continue
            seen.add(quality_id)
            audios.append({
                "id": audio_id,
                "quality_id": quality_id,
                "bandwidth": as_int(entry.get("bandwidth")),
                "codecs": entry.get("codecs") or "",
                "mime": entry.get("mimeType") or "audio/mp4",
                "url": url,
            })
        audios.sort(key=lambda item: QUALITY_RANK.get(item["quality_id"], 0), reverse=True)
        return audios

    def qualities_of(self, video):
        """(音质列表, 是否可播放, 不可用原因)；网络异常时降级为声明音质"""
        try:
            audios = self.dash_audios(video)
            if audios:
                return [audio["quality_id"] for audio in audios], True, ""
            return ["a192", "a132", "a64"], True, ""
        except PluginError as error:
            if error.code in (1004, 1021):
                return [], False, error.message
            return ["a192", "a132", "a64"], True, error.message

    # -------------------------------------------------- 搜索
    def search_videos(self, keyword, page, page_size):
        page = max(1, as_int(page, 1))
        page_size = min(50, max(1, as_int(page_size, 20)))
        self.ensure_buvid()
        data = self.http(
            "/x/web-interface/wbi/search/type",
            {"search_type": "video", "keyword": keyword, "page": page, "page_size": page_size},
            action="搜索视频",
            sign=True,
        )
        raw_items = data.get("result") or []
        items = []
        for raw in raw_items:
            if not isinstance(raw, dict):
                continue
            bvid = str(raw.get("bvid") or "").strip()
            if not bvid.startswith("BV"):
                continue  # 课堂 / 专栏等非视频结果直接跳过
            items.append(self.search_item_from_bili(raw, bvid))
        return {
            "items": items,
            "keyword": keyword,
            "page": page,
            "page_size": page_size,
            "total": as_int(data.get("numResults"), len(items)),
            "has_more": page < as_int(data.get("numPages"), 0),
        }

    def search_item_from_bili(self, raw, bvid):
        duration = parse_duration(raw.get("duration"))
        title = strip_tags(raw.get("title") or "")
        author = strip_tags(raw.get("author") or "")
        pic = normalize_cover(raw.get("pic") or "")
        return {
            "platform": "bilibili",
            "id": bvid,
            "item_id": bvid,
            "sha": sha_for(bvid),
            "name": title,
            "title": title,
            "artists": [author] if author else [],
            "artist": author,
            "author": author,
            "album": "",
            "duration_seconds": duration,
            "pic": pic,
            "cover": pic,
            "available": True,
            "reason": "",
            "aid": as_int(raw.get("aid")),
            "pubdate": as_int(raw.get("pubdate")),
            "description": strip_tags(raw.get("description") or ""),
            "quality": quality_json("a192"),
            "qualities": [quality_json("a192"), quality_json("a132"), quality_json("a64")],
        }

    # -------------------------------------------------- 媒体代理
    def proxy_url(self, upstream, kind, ttl=PROXY_TTL):
        expires = int(time.time()) + ttl
        payload = "%s\n%s\n%d" % (upstream, kind, expires)
        signature = hmac.new(PROXY_SECRET.encode("utf-8"), payload.encode("utf-8"),
                             hashlib.sha256).hexdigest()[:32]
        encoded = base64.urlsafe_b64encode(upstream.encode("utf-8")).decode("ascii").rstrip("=")
        return "http://127.0.0.1:%d/media?u=%s&k=%s&e=%d&s=%s" % (
            PORT, encoded, kind, expires, signature)

    # -------------------------------------------------- 字幕 → 歌词
    def lyric(self, video):
        try:
            data = self.http("/x/player/wbi/v2",
                             {"bvid": video["bvid"], "cid": video["cid"]},
                             action="获取字幕", sign=True)
        except PluginError:
            data = self.http("/x/player/v2",
                             {"bvid": video["bvid"], "cid": video["cid"]},
                             action="获取字幕")
        subtitles = ((data.get("subtitle") or {}).get("subtitles") or [])
        if not subtitles:
            raise PluginError(1015, "该视频没有 CC 字幕")
        chosen = None
        for wanted in ("zh-CN", "zh-Hans", "ai-zh", "zh-Hant"):
            for entry in subtitles:
                if str(entry.get("lan") or "") == wanted and entry.get("subtitle_url"):
                    chosen = entry
                    break
            if chosen:
                break
        if chosen is None:
            for entry in subtitles:
                if entry.get("subtitle_url"):
                    chosen = entry
                    break
        if chosen is None:
            raise PluginError(1015, "该视频没有可用字幕")
        url = normalize_cover(chosen.get("subtitle_url") or "")
        try:
            request = urllib.request.Request(url, headers=self._headers(referer=REFERER))
            with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
                body = json.loads(response.read().decode("utf-8", "replace"))
        except Exception as error:  # noqa: BLE001 - 字幕失败不阻断播放
            raise PluginError(1015, "字幕下载失败：%s" % error)
        lyric = subtitle_to_lyric(body, chosen.get("lan") or "", chosen.get("lan_doc") or "")
        if not lyric["lines"]:
            raise PluginError(1015, "字幕内容为空")
        return lyric


def subtitle_to_lyric(body, lan="", lan_doc=""):
    """bilibili CC 字幕 JSON（body[].from/to/content）→ Rad Music 的歌词结构"""
    lines = []
    for entry in (body.get("body") or []):
        if not isinstance(entry, dict):
            continue
        text = strip_tags(entry.get("content") or "")
        if not text:
            continue
        lines.append({"time_ms": int(float(entry.get("from") or 0) * 1000), "text": text})
    lines.sort(key=lambda item: item["time_ms"])
    return {
        "source": "bilibili",
        "format": "lrc",
        "timed": True,
        "offset_ms": 0,
        "lan": lan,
        "lan_doc": lan_doc,
        "lines": lines,
        "lrc": lines_to_lrc(lines),
    }


def lines_to_lrc(lines):
    def stamp(milliseconds):
        total = max(0, milliseconds)
        minutes, remainder = divmod(total, 60000)
        seconds, millis = divmod(remainder, 1000)
        return "[%02d:%02d.%02d]" % (minutes, seconds, millis // 10)

    return "\n".join(stamp(line["time_ms"]) + line["text"] for line in lines)


# ---------------------------------------------------------------- 业务：模型转换与 op 实现
CLIENT = None  # 在 main() 里装配
SERVER_HOST = "127.0.0.1"
SERVER_PORT = DEFAULT_PORT


def song_base(video):
    qualities, available, reason = CLIENT.qualities_of(video)
    best = qualities[0] if qualities else ""
    return {
        "platform": "bilibili",
        "version": "1.0.0",
        "location": "",
        "sha": sha_for(video["item_id"]),
        "name": video["display_title"],
        "author": video["author"],
        "pic": video["pic"],
        "secret": 0,
        # 宿主 BaseInfo::from_json 读 duration（秒）；duration_seconds 兼容其它调用方
        "duration": video["duration"],
        "duration_seconds": video["duration"],
        "track_no": video["page"],
        "quality": quality_json(best) if best else {},
        "qualities": [quality_json(quality) for quality in qualities],
        "album": video["tname"],
        "item_id": video["item_id"],
        "bvid": video["bvid"],
        "aid": video["aid"],
        "cid": video["cid"],
        "page": video["page"],
        "page_count": video["page_count"],
        "pubdate": video["pubdate"],
        "available": available and video["available"],
        "reason": reason or video["reason"],
    }


def track_item(video):
    """歌单条目（Readme 1.11 / 1.12）：宿主要 duration（秒）"""
    return {
        "platform": "bilibili",
        "item_id": video["item_id"],
        "sha": sha_for(video["item_id"]),
        "name": video["display_title"],
        "author": video["author"],
        "album": video["tname"],
        "duration": video["duration"],
        "duration_seconds": video["duration"],
        "pic": video["pic"],
        "available": video["available"],
        "reason": video["reason"],
        "quality": quality_json("a192"),
        "qualities": [quality_json("a192"), quality_json("a132"), quality_json("a64")],
        "group_key": "",
    }


def mv_item(video):
    return {
        "platform": "bilibili",
        "id": video["item_id"],
        "item_id": video["item_id"],
        "sha": mv_sha_for(video["item_id"]),
        "title": video["display_title"],
        "name": video["display_title"],
        "artist": video["author"],
        "author": video["author"],
        "album": video["tname"],
        "pic": video["pic"],
        "cover": video["pic"],
        "duration": video["duration"],
        "duration_seconds": video["duration"],
        "bvid": video["bvid"],
        "cid": video["cid"],
        "width": as_int(video["dimension"].get("width")),
        "height": as_int(video["dimension"].get("height")),
        "available": video["available"],
        "reason": video["reason"],
    }


def resolve_video(item_id):
    video = CLIENT.video(item_id)
    if not video["available"]:
        raise PluginError(1004, video["reason"] or "视频不可用")
    return video


def op_search(params):
    keyword = str(params.get("keyword") or params.get("q") or "").strip()
    if not keyword:
        raise PluginError(1007, "搜索关键字不能为空")
    return CLIENT.search_videos(keyword, params.get("page"), params.get("page_size"))


def op_get_song(params):
    return song_base(CLIENT.video(params.get("id") or params.get("item_id")))


def op_get_url(params):
    video = resolve_video(params.get("id") or params.get("item_id"))
    requested = params.get("quality") or params.get("level") or params.get("quality_id")
    try:
        audios = CLIENT.dash_audios(video)
    except PluginError as error:
        if error.code in (1004, 1021):
            raise
        audios = []

    if audios:
        audio = pick_audio(audios, requested)
        if audio is None:
            raise PluginError(1021, "该视频没有可用音轨")
        proxied = CLIENT.proxy_url(audio["url"], "audio")
        return {
            "url": proxied,
            "location": proxied,
            "quality": quality_json(audio["quality_id"]),
            "quality_id": audio["quality_id"],
            "requested_quality": normalize_quality(requested) or "auto",
            "mime": "audio/mp4",
            "codec": audio["codecs"],
            "bandwidth": audio["bandwidth"],
            "audio_only": True,
            "duration": video["duration"],
            "duration_seconds": video["duration"],
            "bvid": video["bvid"],
            "cid": video["cid"],
            "page": video["page"],
        }

    # 老视频可能只有混流 MP4（DASH 里没有音轨）：退回 durl，浏览器 <audio> 同样能放出声
    data = CLIENT.playurl(video, fnval=1)
    durl = data.get("durl") or []
    if not durl:
        raise PluginError(1021, "该视频没有可用的音轨地址")
    url = durl[0].get("url") or ((durl[0].get("backup_url") or [""])[0])
    if not url:
        raise PluginError(1021, "该视频没有可用的音轨地址")
    proxied = CLIENT.proxy_url(url, "video")
    return {
        "url": proxied,
        "location": proxied,
        "quality": quality_json("a64"),
        "quality_id": "a64",
        "audio_only": False,
        "note": "该视频只有混流 MP4，已退回整段音视频",
        "duration": video["duration"],
        "duration_seconds": video["duration"],
        "bvid": video["bvid"],
        "cid": video["cid"],
    }


def op_get_qualities(params):
    video = CLIENT.video(params.get("id") or params.get("item_id"))
    qualities, available, reason = CLIENT.qualities_of(video)
    state = CLIENT.account_state()
    return {
        "id": video["item_id"],
        "platform": "bilibili",
        "qualities": [quality_json(quality) for quality in qualities],
        "available": available and video["available"],
        "reason": reason or video["reason"],
        "max_quality": qualities[0] if qualities else "",
        "logged_in": state["logged_in"],
        "vip": state["vip"],
        "bvid": video["bvid"],
        "cid": video["cid"],
    }


def op_get_lyric(params):
    video = CLIENT.video(params.get("id") or params.get("item_id"))
    return CLIENT.lyric(video)


def op_get_pic(params):
    video = CLIENT.video(params.get("id") or params.get("item_id"))
    return {"platform": "bilibili", "id": video["item_id"], "pic": video["pic"], "url": video["pic"]}


def op_search_mv(params):
    keyword = str(params.get("keyword") or params.get("q") or "").strip()
    if not keyword:
        raise PluginError(1007, "搜索关键字不能为空")
    result = CLIENT.search_videos(keyword, params.get("page"), params.get("page_size"))
    mvs = []
    for item in result["items"]:
        mvs.append({
            "platform": "bilibili",
            "id": item["id"],
            "item_id": item["item_id"],
            "sha": mv_sha_for(item["item_id"]),
            "title": item["title"],
            "name": item["title"],
            "artist": item["author"],
            "author": item["author"],
            "cover": item["pic"],
            "pic": item["pic"],
            "duration": item["duration_seconds"],
            "duration_seconds": item["duration_seconds"],
            "bvid": item["id"],
            "play_count": 0,
        })
    return {
        "mvs": mvs,
        "items": mvs,
        "total": result["total"],
        "keyword": keyword,
        "page": result["page"],
        "page_size": result["page_size"],
    }


def op_get_mv(params):
    video = resolve_video(params.get("id") or params.get("item_id"))
    base = CLIENT.playurl(video, fnval=1)
    if not (base.get("durl") or []):
        raise PluginError(1021, "该视频没有可用的混流 MP4（仅有 DASH），暂时无法作为 MV 播放")

    accept = [as_int(value) for value in (base.get("accept_quality") or [])]
    usable = [value for value in accept if value in VIDEO_QN]
    requested = as_int(params.get("qn") or params.get("quality"), 0)
    target = max(usable) if usable else as_int(base.get("quality"), 16)
    if requested in VIDEO_QN and (not usable or requested <= max(usable)):
        target = requested
    data = base
    if target and target != as_int(base.get("quality"), 0):
        try:
            data = CLIENT.playurl(video, fnval=1, qn=target)
        except PluginError:
            data = base
    durl = data.get("durl") or []
    if not durl:
        durl = base.get("durl") or []
    url = durl[0].get("url") or ((durl[0].get("backup_url") or [""])[0])
    if not url:
        raise PluginError(1021, "bilibili 没有返回 MV 播放地址")
    if len(durl) > 1:
        log("MV %s 返回了 %d 个分片，只播放第一段" % (video["item_id"], len(durl)))
    proxied = CLIENT.proxy_url(url, "video")

    qn = as_int(data.get("quality"), target)
    label, width, height = VIDEO_QN.get(qn, ("", 0, 0))
    resolutions = []
    for value in sorted(set(usable), reverse=True):
        item_label, item_width, item_height = VIDEO_QN[value]
        resolutions.append({"qn": value, "label": item_label,
                            "width": item_width, "height": item_height})
    return {
        "platform": "bilibili",
        "id": video["item_id"],
        "item_id": video["item_id"],
        "sha": mv_sha_for(video["item_id"]),
        "title": video["display_title"],
        "artist": video["author"],
        "album": video["tname"],
        "pic": video["pic"],
        "cover": video["pic"],
        "url": proxied,
        "location": proxied,
        "quality": {"id": str(qn), "label": label},
        "width": width,
        "height": height,
        "resolution": label,
        "duration": video["duration"],
        "duration_seconds": video["duration"],
        "bvid": video["bvid"],
        "cid": video["cid"],
        "page": video["page"],
        "publish_time": video["pubdate"],
        "resolutions": resolutions,
    }


def op_get_playlist(params):
    raw = str(params.get("id") or params.get("item_id") or "").strip()
    limit = min(100, max(1, as_int(params.get("limit"), 50)))
    if not raw:
        raise PluginError(1007, "缺少歌单标识（fav:<收藏夹id> / up:<UP主mid> / season:<mid>:<season_id>）")
    lowered = raw.lower()
    if lowered.startswith("fav:"):
        return fav_list(raw.split(":", 1)[1], limit)
    if lowered.startswith("up:") or lowered.startswith("mid:"):
        return up_videos(raw.split(":", 1)[1], limit)
    if lowered.startswith("season:"):
        parts = raw.split(":")[1:]
        return season_list(parts, limit)
    if raw.isdigit():
        return fav_list(raw, limit)
    raise PluginError(1007, "无法识别的歌单标识: %s（用 fav:<收藏夹id> / up:<mid> / season:<mid>:<season_id>）" % raw)


def fav_list(media_id, limit):
    state = CLIENT.account_state()
    if not state["logged_in"]:
        raise PluginError(1021, "收藏夹需要登录 bilibili（可在插件登录页粘贴 Cookie）")
    data = CLIENT.http("/x/v3/fav/resource/list",
                       {"media_id": as_int(media_id), "ps": limit, "pn": 1, "platform": "web"},
                       action="读取收藏夹")
    info = data.get("info") or {}
    tracks = []
    for media in (data.get("medias") or []):
        if not isinstance(media, dict):
            continue
        bvid = str(media.get("bvid") or "")
        if not bvid.startswith("BV") or as_int(media.get("type"), 2) != 2:
            continue
        tracks.append({
            "platform": "bilibili",
            "item_id": bvid,
            "sha": sha_for(bvid),
            "name": strip_tags(media.get("title") or ""),
            "author": strip_tags((media.get("upper") or {}).get("name") or ""),
            "duration": as_int(media.get("duration")),
            "group_key": "",
        })
    return {
        "platform": "bilibili",
        "id": "fav:%s" % media_id,
        "name": strip_tags(info.get("title") or ("收藏夹 " + str(media_id))),
        "total": as_int(info.get("media_count"), len(tracks)),
        "tracks": tracks,
    }


def up_videos(mid, limit):
    try:
        data = CLIENT.http("/x/space/wbi/arc/search",
                           {"mid": as_int(mid), "ps": min(50, limit), "pn": 1, "order": "pubdate"},
                           action="读取 UP 主投稿", sign=True)
    except PluginError as error:
        if error.code == 1014:
            raise PluginError(1014, "读取 UP 主投稿被风控（bilibili 对未登录访问投稿列表限制较严），"
                                    "请先登录，或改用收藏夹 / 合集")
        raise
    listing = ((data.get("list") or {}).get("vlist") or [])
    tracks = []
    for entry in listing:
        bvid = str(entry.get("bvid") or "")
        if not bvid.startswith("BV"):
            continue
        tracks.append({
            "platform": "bilibili",
            "item_id": bvid,
            "sha": sha_for(bvid),
            "name": strip_tags(entry.get("title") or ""),
            "author": strip_tags(entry.get("author") or ""),
            "duration": parse_duration(entry.get("length")),
            "group_key": "",
        })
    return {
        "platform": "bilibili",
        "id": "up:%s" % mid,
        "name": "UP 主 %s 的投稿" % mid,
        "total": len(tracks),
        "tracks": tracks,
    }


def season_list(parts, limit):
    if len(parts) < 2:
        raise PluginError(1007, "合集需要 season:<mid>:<season_id>（mid 为 UP 主 id）")
    mid, season_id = as_int(parts[0]), as_int(parts[1])
    data = CLIENT.http("/x/polymer/web-space/seasons_archives_list",
                       {"mid": mid, "season_id": season_id, "page_num": 1, "page_size": limit},
                       action="读取合集")
    archives = data.get("archives") or []
    meta = data.get("meta") or {}
    tracks = []
    for entry in archives:
        bvid = str(entry.get("bvid") or "")
        if not bvid.startswith("BV"):
            continue
        tracks.append({
            "platform": "bilibili",
            "item_id": bvid,
            "sha": sha_for(bvid),
            "name": strip_tags(entry.get("title") or ""),
            "author": strip_tags((meta.get("name") or "")),
            "duration": as_int(entry.get("duration")),
            "group_key": "",
        })
    return {
        "platform": "bilibili",
        "id": "season:%d:%d" % (mid, season_id),
        "name": strip_tags(meta.get("title") or ("合集 " + str(season_id))),
        "total": len(tracks),
        "tracks": tracks,
    }


def op_check_login(params):
    state = CLIENT.account_state(refresh=bool(params.get("refresh")))
    data = dict(state)
    data["platform"] = "bilibili"
    data["logged_in"] = state["logged_in"]
    return data


def op_login_cookie(params):
    state = CLIENT.login_with_cookie(params.get("cookie") or params.get("sessdata") or "")
    data = dict(state)
    data["platform"] = "bilibili"
    return data


def op_logout(params):
    data = CLIENT.logout()
    data["platform"] = "bilibili"
    return data


def op_settings_get(params):
    """设置面板：返回登录态 + 插件配置 + 版本号（只读，配置通过启动参数覆盖）"""
    state = CLIENT.account_state(refresh=bool(params.get("refresh")))
    config = CLIENT.config
    return {
        "platform": "bilibili",
        "plugin_id": "bilibili.source",
        "version": VERSION,
        "endpoint": "http://%s:%d" % (SERVER_HOST, SERVER_PORT),
        "login": {
            "logged_in": state.get("logged_in", False),
            "cookie_present": state.get("cookie_present", False),
            "user_id": state.get("user_id", ""),
            "nickname": state.get("nickname", ""),
            "vip": state.get("vip", False),
            "vip_label": state.get("vip_label", ""),
        },
        "login_page": "/login",
        "settings_page": "/settings",
        "cookie_file": COOKIE_FILE,
        "config_file": CONFIG_FILE,
        "values": config,
        "schema": [
            {"key": "qps", "label": "限流 QPS", "type": "int", "min": 1, "max": 30,
             "default": config.get("qps", QPS), "hint": "每秒最多发出多少请求"},
            {"key": "timeout_seconds", "label": "请求超时（秒）", "type": "int", "min": 3, "max": 60,
             "default": config.get("timeout_seconds", TIMEOUT)},
            {"key": "retries", "label": "网络重试次数", "type": "int", "min": 0, "max": 5,
             "default": config.get("retries", RETRIES)},
        ],
    }


HANDLERS = {
    "search": op_search,
    "get_song": op_get_song,
    "get_url": op_get_url,
    "get_qualities": op_get_qualities,
    "get_lyric": op_get_lyric,
    "get_pic": op_get_pic,
    "search_mv": op_search_mv,
    "get_mv": op_get_mv,
    "get_playlist": op_get_playlist,
    "check_login": op_check_login,
    "login_cookie": op_login_cookie,
    "logout": op_logout,
    "settings_get": op_settings_get,
}


def ok(data):
    return {"code": 0, "message": "ok", "data": data}


def fail(code, message):
    return {"code": code, "message": message}


def handle_request(payload):
    action = str(payload.get("op") or "").strip().lower()
    op = OP_ALIASES.get(action, action)
    params = payload.get("params") or {}
    if not isinstance(params, dict):
        params = {}
    item_id = str(params.get("id") or params.get("item_id") or payload.get("id") or "").strip()
    if item_id and not params.get("id"):
        params = dict(params, id=item_id)
    started = time.time()
    try:
        handler = HANDLERS.get(op)
        if handler is None:
            raise PluginError(1012, "不支持的操作: %s" % (action or "(空)"))
        return ok(handler(params))
    except PluginError as error:
        log("op=%s 失败（%s）: %s" % (action or "-", error.code, error.message))
        return fail(error.code, error.message)
    except (urllib.error.URLError, OSError, ValueError) as error:
        log("op=%s 异常: %r" % (action or "-", error))
        return fail(1011, "访问 bilibili 失败: %s" % error)
    finally:
        elapsed = (time.time() - started) * 1000.0
        if elapsed > 2000:
            log("op=%s 耗时 %.0f ms" % (action or "-", elapsed))


# ---------------------------------------------------------------- 媒体代理
def proxy_allowed(host):
    """只代理公网 http(s) 地址，拒绝回环/内网，防止被当成 SSRF 跳板"""
    if not host:
        return False
    lowered = host.lower().strip(".")
    if lowered == "localhost" or lowered.endswith(".local") or lowered.endswith(".internal"):
        return False
    try:
        address = ipaddress.ip_address(lowered)
    except ValueError:
        address = None
    if address is not None:
        return not (address.is_private or address.is_loopback or address.is_link_local
                    or address.is_reserved or address.is_multicast)
    return True


def content_type_for(url, kind):
    path = urllib.parse.urlparse(url).path.lower()
    if path.endswith(".mp4"):
        return "video/mp4"
    if kind == "audio":
        return "audio/flac" if path.endswith(".flac") else "audio/mp4"
    if kind == "video":
        return "video/mp4"
    if path.endswith((".jpg", ".jpeg")):
        return "image/jpeg"
    if path.endswith(".png"):
        return "image/png"
    return "application/octet-stream"


# ---------------------------------------------------------------- HTTP 服务
class Handler(BaseHTTPRequestHandler):
    server_version = "RadBili/" + VERSION
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # 关掉 stderr 噪音，只记错误到 plugin.log
        return

    # -------------------------------------------------- 基础响应
    def _json(self, payload, status=200):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _html(self, text, status=200):
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self):
        length = as_int(self.headers.get("Content-Length"), 0)
        raw = self.rfile.read(length) if length > 0 else b""
        if not raw:
            return {}
        try:
            payload = json.loads(raw.decode("utf-8", "replace"))
        except ValueError:
            return {}
        return payload if isinstance(payload, dict) else {}

    # -------------------------------------------------- 路由
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"
        if path == "/health":
            self._json({"ok": True, "version": VERSION, "platform": "bilibili",
                        "port": PORT, "logged_in": bool(CLIENT.cookies.text)})
        elif path == "/media":
            self._media(urllib.parse.parse_qs(parsed.query), head=False)
        elif path == "/login":
            self._html(LOGIN_PAGE)
        elif path == "/settings":
            self._html(SETTINGS_PAGE)
        elif path == "/favicon.ico":
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
        else:
            self._json({"code": 1007, "message": "未知路径: %s" % parsed.path}, status=404)

    def do_HEAD(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path.rstrip("/") == "/media":
            self._media(urllib.parse.parse_qs(parsed.query), head=True)
        else:
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Range, Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, HEAD, OPTIONS")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"
        if path == "/request":
            payload = self._read_json()
            self._json(handle_request(payload))
            return
        if path == "/login/api/state":
            try:
                state = CLIENT.account_state(refresh=True)
                self._json({"code": 0, "data": state})
            except PluginError as error:
                self._json({"code": error.code, "message": error.message})
            return
        if path == "/login/api/cookie":
            payload = self._read_json()
            try:
                state = CLIENT.login_with_cookie(payload.get("cookie") or payload.get("sessdata") or "")
                self._json({"code": 0, "data": state})
            except PluginError as error:
                self._json({"code": error.code, "message": error.message})
            return
        if path == "/login/api/logout":
            self._json({"code": 0, "data": CLIENT.logout()})
            return
        self._json({"code": 1007, "message": "未知路径: %s" % parsed.path}, status=404)

    # -------------------------------------------------- 媒体代理
    def _media(self, query, head=False):
        encoded = (query.get("u") or [""])[0]
        kind = (query.get("k") or ["audio"])[0]
        expires = as_int((query.get("e") or ["0"])[0])
        signature = (query.get("s") or [""])[0]
        try:
            padded = encoded + "=" * (-len(encoded) % 4)
            upstream = base64.urlsafe_b64decode(padded.encode("ascii")).decode("utf-8")
        except (ValueError, UnicodeDecodeError):
            self._plain(400, "媒体地址参数不合法")
            return

        parsed = urllib.parse.urlparse(upstream)
        if parsed.scheme not in ("http", "https") or not proxy_allowed(parsed.hostname):
            self._plain(403, "拒绝代理该地址")
            return
        if expires < int(time.time()):
            self._plain(410, "媒体地址已过期，请重新加载曲目")
            return
        expected = hmac.new(PROXY_SECRET.encode("utf-8"),
                            ("%s\n%s\n%d" % (upstream, kind, expires)).encode("utf-8"),
                            hashlib.sha256).hexdigest()[:32]
        if not hmac.compare_digest(expected, signature):
            self._plain(403, "媒体地址签名不匹配")
            return

        headers = {
            "User-Agent": USER_AGENT,
            "Referer": REFERER,
            "Accept": "*/*",
            "Accept-Encoding": "identity",
        }
        range_header = self.headers.get("Range") or ""
        if range_header:
            headers["Range"] = range_header
        match = re.match(r"bytes=(\d+)-(\d*)", range_header)
        range_start = as_int(match.group(1), 0) if match else 0
        range_end = as_int(match.group(2), -1) if (match and match.group(2)) else -1
        if range_end >= 0 and range_end < range_start:
            range_end = -1

        response = None
        for attempt in range(2):
            try:
                response = urllib.request.urlopen(urllib.request.Request(upstream, headers=headers), timeout=40)
                break
            except urllib.error.HTTPError as error:
                log("media 回源失败 HTTP %s（%s）" % (error.code, parsed.netloc))
                self._plain(502 if error.code >= 500 else error.code,
                            "bilibili 回源失败：HTTP %d（地址可能已过期，请重新加载）" % error.code)
                return
            except (urllib.error.URLError, OSError) as error:
                if attempt == 0:
                    time.sleep(0.4)
                    continue
                log("media 回源异常：%r" % (error,))
                self._plain(502, "bilibili 回源异常：%s" % error)
                return

        length = response.headers.get("Content-Length")
        budget = as_int(length, 0)
        self.send_response(getattr(response, "status", 200))
        self.send_header("Content-Type", content_type_for(upstream, kind))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "public, max-age=600")
        if length:
            self.send_header("Content-Length", length)
        else:
            self.send_header("Connection", "close")
            self.close_connection = True
        content_range = response.headers.get("Content-Range")
        if content_range:
            self.send_header("Content-Range", content_range)
        self.end_headers()
        if head:
            try:
                response.close()
            except Exception:  # noqa: BLE001
                pass
            return

        # bilibili 的 PCDN 边缘节点偶尔会在传输中途卡住：用 Range 从断点续传，
        # 保证 Content-Length 承诺的字节数一定发完，浏览器端只是「慢一点」而不会报错。
        sent = 0
        resumes = 0
        current = response
        while True:
            try:
                while True:
                    want = 128 * 1024
                    if budget:
                        left = budget - sent
                        if left <= 0:
                            break
                        want = min(want, left)
                    chunk = current.read(want)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    sent += len(chunk)
                break
            except (BrokenPipeError, ConnectionResetError):
                break  # 浏览器拖动进度条会主动断开，属正常
            except (TimeoutError, socket.timeout, OSError, ValueError) as error:
                try:
                    current.close()
                except Exception:  # noqa: BLE001
                    pass
                if budget and sent >= budget:
                    break
                if resumes >= 2:
                    log("media 续传放弃（已传 %d/%s 字节）：%r" % (sent, budget, error))
                    break
                resumes += 1
                resume_at = range_start + sent
                log("media 传输中断（已传 %d/%s 字节），从 %d 续传（第 %d 次）"
                    % (sent, budget, resume_at, resumes))
                retry_headers = dict(headers)
                retry_headers["Range"] = "bytes=%d-%s" % (resume_at, "" if range_end < 0 else range_end)
                try:
                    current = urllib.request.urlopen(
                        urllib.request.Request(upstream, headers=retry_headers), timeout=40)
                except (urllib.error.URLError, OSError) as retry_error:
                    log("media 续传回源失败：%r" % (retry_error,))
                    break
        try:
            current.close()
        except Exception:  # noqa: BLE001
            pass

    def _plain(self, status, message):
        body = json.dumps({"code": status, "message": message}, ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except OSError:
            pass


# ---------------------------------------------------------------- 登录页
LOGIN_PAGE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>哔哩哔哩登录 · Rad Music</title>
<style>
  :root { --bg:#f6f7f9; --card:#fff; --line:#e3e6eb; --text:#1b1d21; --muted:#6b7280;
          --accent:#fb7299; --ok:#12805c; --warn:#b45309; --err:#d81e06;
          --shadow:0 1px 2px rgba(16,24,40,.06),0 8px 24px rgba(16,24,40,.06); }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--bg); color:var(--text);
         font:14px/1.6 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif; }
  .wrap { max-width:720px; margin:0 auto; padding:40px 20px 64px; }
  header h1 { margin:0 0 6px; font-size:22px; }
  header p { margin:0; color:var(--muted); font-size:13px; }
  .card { background:var(--card); border:1px solid var(--line); border-radius:14px;
          padding:18px; box-shadow:var(--shadow); margin-top:16px; }
  .card h2 { margin:0 0 12px; font-size:15px; font-weight:600; }
  .row { display:flex; align-items:center; justify-content:space-between; gap:12px; padding:6px 0; }
  .row + .row { border-top:1px dashed var(--line); }
  .row span { color:var(--muted); font-size:13px; }
  .row b { font-weight:600; font-size:13px; }
  code { font-family:ui-monospace,Consolas,monospace; font-size:12px; background:#f1f3f5;
         padding:2px 6px; border-radius:6px; word-break:break-all; }
  textarea { width:100%; min-height:96px; resize:vertical; font:inherit; font-size:12.5px;
             padding:10px 12px; border:1px solid var(--line); border-radius:10px;
             font-family:ui-monospace,Consolas,monospace; background:#fbfcfd; color:var(--text); }
  textarea:focus { outline:2px solid rgba(251,114,153,.25); border-color:var(--accent); }
  .btn { appearance:none; border:1px solid transparent; background:var(--accent); color:#fff;
         font:inherit; font-size:13px; padding:8px 16px; border-radius:10px; cursor:pointer; }
  .btn:disabled { opacity:.55; cursor:default; }
  .btn.ghost { background:#fff; color:var(--text); border-color:var(--line); }
  .btn.ghost:hover { background:#f4f5f7; }
  .actions { display:flex; gap:8px; margin-top:12px; flex-wrap:wrap; align-items:center; }
  .hint { color:var(--muted); font-size:12.5px; margin:10px 0 0; }
  .hint a { color:var(--accent); text-decoration:none; }
  .state { min-height:20px; font-size:13px; color:var(--muted); }
  .state.ok { color:var(--ok); } .state.err { color:var(--err); } .state.warn { color:var(--warn); }
  .foot { margin-top:22px; color:var(--muted); font-size:12px; text-align:center; }
  ol { margin:8px 0 0; padding-left:20px; color:var(--muted); font-size:13px; }
  ol b { color:var(--text); font-weight:600; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>哔哩哔哩登录</h1>
    <p>登录后可解锁 1080P+ / 大会员音质，并使用收藏夹与 UP 主投稿。凭据只写入本机插件目录的 <code>cookie.txt</code>。</p>
  </header>

  <section class="card">
    <h2>当前登录态</h2>
    <div class="row"><span>账号</span><b id="who">读取中…</b></div>
    <div class="row"><span>UID</span><b id="mid">—</b></div>
    <div class="row"><span>会员</span><b id="vip">—</b></div>
    <div class="row"><span>Cookie</span><code id="ck">—</code></div>
    <div class="actions">
      <button class="btn ghost" id="refresh">刷新</button>
      <button class="btn ghost" id="logout">退出登录</button>
      <span class="state" id="msg"></span>
    </div>
  </section>

  <section class="card">
    <h2>用 Cookie 登录</h2>
    <ol>
      <li>浏览器打开 <a href="https://www.bilibili.com" target="_blank" rel="noopener">bilibili.com</a> 并确认已登录</li>
      <li>按 <b>F12</b> → <b>Application / 应用</b> → <b>Cookies</b> → <code>https://www.bilibili.com</code></li>
      <li>复制 <b>SESSDATA</b> 的值（可只粘贴这一串），或复制整行 Cookie 粘贴到下面</li>
    </ol>
    <textarea id="cookie" placeholder="SESSDATA=xxxxxxxx%2Cxxxxxxxxxx%2Cxxxxx%2C*xxxxxxxx"></textarea>
    <div class="actions">
      <button class="btn" id="save">保存并验证</button>
      <span class="hint">Cookie 等同于账号密码，请勿分享给他人。</span>
    </div>
    <p class="hint">也可以直接用环境变量 <code>RAD_BILIBILI_COOKIE</code> 或本目录的 <code>cookie.txt</code> 配置。</p>
  </section>

  <div class="foot">Rad Music · 哔哩哔哩插件 <span id="ver"></span></div>
</div>
<script>
const $ = (id) => document.getElementById(id);
function say(text, kind) { const el = $('msg'); el.textContent = text || ''; el.className = 'state' + (kind ? ' ' + kind : ''); }
async function post(path, body) {
  const response = await fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) });
  return response.json();
}
function render(state) {
  if (!state) return;
  $('who').textContent = state.logged_in ? (state.uname || '已登录') : '未登录';
  $('mid').textContent = state.mid ? String(state.mid) : '—';
  $('vip').textContent = state.logged_in ? (state.vip_label || '普通用户') : '—';
  $('ck').textContent = state.cookie_configured ? (state.cookie_masked || '已配置') : '未配置';
}
async function refresh() {
  say('读取中…');
  const data = await post('/login/api/state');
  if (data.code === 0) { render(data.data); say(''); }
  else say(data.message || '读取失败', 'err');
}
$('refresh').addEventListener('click', refresh);
$('save').addEventListener('click', async () => {
  const value = $('cookie').value.trim();
  if (!value) { say('请先粘贴 Cookie 或 SESSDATA', 'warn'); return; }
  $('save').disabled = true; say('验证中…');
  const data = await post('/login/api/cookie', { cookie: value });
  $('save').disabled = false;
  if (data.code === 0) { render(data.data); $('cookie').value = ''; say('登录成功', 'ok'); }
  else say(data.message || '登录失败', 'err');
});
$('logout').addEventListener('click', async () => {
  const data = await post('/login/api/logout');
  render({ logged_in: false, cookie_configured: false });
  say(data.code === 0 ? '已退出登录' : '退出失败', data.code === 0 ? 'ok' : 'err');
});
fetch('/health').then((r) => r.json()).then((info) => { $('ver').textContent = 'v' + (info.version || ''); }).catch(() => {});
refresh();
</script>
</body>
</html>
"""


# ---------------------------------------------------------------- 设置页
SETTINGS_PAGE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>哔哩哔哩插件设置 · Rad Music</title>
<style>
  :root { --bg:#f6f7f9; --card:#fff; --line:#e3e6eb; --text:#1b1d21; --muted:#6b7280;
          --accent:#fb7299; --ok:#12805c; --warn:#b45309;
          --shadow:0 1px 2px rgba(16,24,40,.06),0 8px 24px rgba(16,24,40,.06); }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--bg); color:var(--text);
         font:14px/1.6 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif; }
  .wrap { max-width:760px; margin:0 auto; padding:32px 20px 56px; }
  header { display:flex; align-items:flex-end; justify-content:space-between; gap:12px; flex-wrap:wrap; }
  header h1 { margin:0 0 6px; font-size:22px; }
  header p { margin:0; color:var(--muted); font-size:13px; }
  .badge {
    display:inline-block; font-size:11.5px; padding:2px 10px; border-radius:999px;
    border:1px solid var(--line); color:var(--muted); background:#fff;
  }
  .grid { display:grid; grid-template-columns:1fr; gap:16px; margin-top:22px; }
  .card { background:var(--card); border:1px solid var(--line); border-radius:14px;
          padding:18px 20px; box-shadow:var(--shadow); }
  .card h2 { margin:0 0 12px; font-size:15px; font-weight:600; }
  .row { display:flex; align-items:center; justify-content:space-between; gap:12px; padding:6px 0; }
  .row + .row { border-top:1px dashed var(--line); }
  .row span { color:var(--muted); font-size:13px; }
  .row b { font-weight:600; font-size:13px; }
  .row code { font-family:ui-monospace,Consolas,monospace; font-size:12px; background:#f1f3f5;
              padding:2px 6px; border-radius:6px; word-break:break-all; }
  .btn { appearance:none; border:1px solid transparent; background:var(--accent); color:#fff;
         font:inherit; font-size:13px; padding:7px 14px; border-radius:10px; cursor:pointer; }
  .btn.ghost { background:#fff; color:var(--text); border-color:var(--line); }
  .btn:disabled { opacity:.55; cursor:default; }
  .actions { display:flex; gap:8px; flex-wrap:wrap; }
  .field { display:flex; align-items:center; justify-content:space-between; padding:8px 0; gap:10px; }
  .field + .field { border-top:1px dashed var(--line); }
  .field-info { display:flex; flex-direction:column; gap:2px; }
  .field-info small { color:var(--muted); font-size:12px; }
  .field-control input { font:inherit; padding:6px 10px; border:1px solid var(--line); border-radius:8px;
                         width:120px; text-align:right; }
  .state { margin-top:8px; font-size:12.5px; color:var(--muted); min-height:18px; }
  .state.ok { color:var(--ok); } .state.warn { color:var(--warn); } .state.err { color:#d81e06; }
  .foot { margin-top:22px; color:var(--muted); font-size:12px; text-align:center; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div>
      <h1>哔哩哔哩插件设置 <span class="badge" id="plugin-version">plugin</span></h1>
      <p>登录后可解锁 1080P+、大会员音质、收藏夹与 UP 主投稿；凭据只写入本机插件目录的 <code>cookie.txt</code>。</p>
    </div>
    <div class="actions">
      <a class="btn ghost" href="/login">Cookie 登录页</a>
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

  <p class="foot">Rad Music · 哔哩哔哩插件 · 修改后需要重新启动插件进程才会在客户端生效</p>
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
  const login = data.login || {};
  if (login.logged_in) {
    $('who').textContent = (login.nickname || '已登录') + (login.user_id ? '（uid ' + login.user_id + '）' : '');
    $('vip').textContent = login.vip ? ('大会员已开通' + (login.vip_label ? '·' + login.vip_label : '')) : '未开通（仅标准画质）';
  } else {
    $('who').textContent = '未登录';
    $('vip').textContent = '—';
  }
  $('ck').textContent = login.cookie_present ? '已配置 SESSDATA' : '未配置';
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
  VALUES = next;
  setState($('settings-state'),
    '设置已记录，下次启动插件生效（修改 config.json / 启动参数后请重启 Rad Music 服务或插件进程）', 'ok');
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


# ---------------------------------------------------------------- 配置与入口
def load_config():
    config = {"cookie": "", "qps": QPS, "timeout": TIMEOUT, "retries": RETRIES}
    try:
        with open(CONFIG_FILE, "r", encoding="utf-8") as handle:
            parsed = json.load(handle)
        if isinstance(parsed, dict):
            config.update(parsed)
    except (OSError, ValueError):
        pass
    return config


def main():
    global CLIENT, PORT, QPS, TIMEOUT, RETRIES, SERVER_HOST, SERVER_PORT
    parser = argparse.ArgumentParser(description="Rad Music - 哔哩哔哩播放源插件")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="本地 HTTP 端口（默认 8849）")
    parser.add_argument("--host", default="127.0.0.1", help="监听地址（默认仅本机）")
    parser.add_argument("--qps", type=int, default=0, help="覆盖 config.json 的每秒请求上限")
    parser.add_argument("--timeout", type=int, default=0, help="覆盖 config.json 的网络超时（秒）")
    args = parser.parse_args()

    config = load_config()
    PORT = args.port
    QPS = args.qps or as_int(config.get("qps"), QPS)
    TIMEOUT = args.timeout or as_int(config.get("timeout"), TIMEOUT)
    RETRIES = max(0, as_int(config.get("retries"), RETRIES))
    SERVER_HOST = args.host
    SERVER_PORT = PORT

    cookies = CookieStore(config)
    CLIENT = BiliClient(cookies)
    CLIENT.config = {"qps": QPS, "timeout_seconds": TIMEOUT, "retries": RETRIES}
    if cookies.text:
        CLIENT.ensure_buvid()

    try:
        server = ThreadingHTTPServer((args.host, PORT), Handler)
    except OSError as error:
        log("端口 %d 绑定失败：%s" % (PORT, error))
        sys.stderr.write("端口 %d 已被占用（可能已有一个插件实例在运行）：%s\n" % (PORT, error))
        return 1
    server.daemon_threads = True

    log("插件启动 v%s：http://%s:%d（登录态：%s，QPS：%d）" % (
        VERSION, args.host, PORT, "有" if cookies.has_sessdata() else "无", QPS))
    sys.stdout.write("Rad Music 哔哩哔哩插件 v%s 已启动：http://%s:%d（登录页 /login）\n"
                     % (VERSION, args.host, PORT))
    sys.stdout.flush()
    try:
        server.serve_forever(0.5)
    except KeyboardInterrupt:
        sys.stdout.write("\n已停止\n")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
