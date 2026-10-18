# 哔哩哔哩（bilibili）播放源插件

给 Rad Music 增加 `bilibili` 音源：把 B 站视频当音乐听，也可以把视频当 MV 看。

| 能力 | 说明 |
| --- | --- |
| **听视频** | 一个视频 = 一首曲目，只取 DASH 音轨（不拉画面、不占带宽），可搜索、可入歌单、可歌词 |
| **MV** | 一个视频 = 一条 MV，取混流 MP4（画面 + 声音），支持在 MV 页搜索、入库、播放 |
| 搜索 | 走 bilibili 官方搜索（WBI 签名），结果直接进全局搜索，与其它音源一起折叠 |
| 歌词 | 有 CC 字幕的视频，字幕自动转成 LRC；没有字幕时返回 `1015`（非致命，静默无歌词） |
| 歌单 | 收藏夹 `fav:<收藏夹id>`、UP 主投稿 `up:<mid>`、合集 `season:<mid>:<season_id>`（后两者建议先登录） |
| 登录（可选） | 粘贴 Cookie 后解锁 1080P+ / 大会员音质、收藏夹、UP 主投稿 |

## 启用

插件是「外部进程播放源」，宿主会自动扫描 `plugins/<id>/plugin.json` 并在需要时拉起进程：

```text
plugins/bilibili/
├─ plugin.json   清单（id=bilibili.source，platform=bilibili，endpoint=127.0.0.1:8849）
├─ main.py       插件本体（纯标准库，无第三方依赖）
├─ README.md
├─ cookie.txt    登录凭据（登录后生成，可选）
└─ config.json   可选配置（见下）
```

1. 启动 Rad Music 服务端，插件会被自动拉起（`command: python main.py --port 8849`）；
2. 打开「设置 → 插件」，确认 `bilibili.source` 处于启用状态；
3. 在「设置 → 音源」把 MV 视频源选为 `bilibili`（也可以直接在 MV 页的下拉框里选）；
4. 在搜索页搜任意视频标题，点播放就是「听视频」；在 MV 页搜关键词即可加入 MV 库。

手动启动（排查问题用）：

```bash
python plugins/bilibili/main.py --port 8849
curl http://127.0.0.1:8849/health
```

## 登录（可选）

不登录也能听、能看，只是音质上限受限（通常 192K 音频 / 480P~720P 视频）。登录后：

- 音频：可能拿到 Hi-Res 无损（FLAC，大会员）/ 杜比全景声；
- 视频：1080P / 1080P+ / 4K（大会员）；
- 歌单：收藏夹、UP 主投稿可读。

三种配置方式（优先级从高到低）：

| 方式 | 做法 |
| --- | --- |
| 环境变量 | `set RAD_BILIBILI_COOKIE=SESSDATA=...; bili_jct=...` |
| 登录页 | 浏览器打开 <http://127.0.0.1:8849/login>，粘贴 Cookie 或 SESSDATA 的值 |
| 文件 | 本目录 `cookie.txt` 写一行 Cookie，或 `config.json` 里写 `{"cookie": "..."}` |

Cookie 只写在本机插件目录，不会上传。获取方式：浏览器登录 bilibili → F12 → Application → Cookies →
`https://www.bilibili.com` → 复制 `SESSDATA` 的值（一长串）。

## 为什么必须有一个本地媒体代理

bilibili 的音视频 CDN 校验 `Referer`（缺了直接 403），并返回 `Content-Type: application/octet-stream`。
浏览器的 `<audio>` / `<video>` 既改不了 Referer，也认不出 MIME。所以插件在 `127.0.0.1:8849/media`
起了一个只读代理：

- 回源时补 `Referer: https://www.bilibili.com/` 和浏览器 UA；
- 透传 `Range`（拖动进度条必需），支持 `206 Partial Content`；
- 把 MIME 纠正成 `audio/mp4` / `video/mp4`；
- CDN 边缘节点传输中断时，用 Range 自动从断点续传（最多 2 次）；
- 地址带 HMAC 签名 + 过期时间，并拒绝回环/内网地址，不会被当成 SSRF 跳板。

## 支持的操作（`POST /request`）

| op | 参数 | 说明 |
| --- | --- | --- |
| `search` | `keyword, page, page_size` | 搜索视频（作为曲目） |
| `get_song` | `id` | 视频元数据，可直接落 `base.json` |
| `get_url` | `id, quality?` | 听视频：返回 DASH 音轨的代理地址 |
| `get_qualities` | `id` | 可用音频音质 |
| `get_lyric` | `id` | CC 字幕 → LRC（无字幕返回 1015） |
| `get_pic` | `id` | 封面 |
| `search_mv` | `keyword, page, page_size` | 搜索 MV |
| `get_mv` | `id` | MV 播放地址（混流 MP4） |
| `get_playlist` | `id` | `fav:<id>` / `up:<mid>` / `season:<mid>:<season_id>` |
| `check_login` | `refresh?` | 当前登录态 |
| `login_cookie` | `cookie` 或 `sessdata` | 用 Cookie 登录 |
| `logout` | — | 退出登录 |
| `settings` | — | 设置面板数据：当前登录态 + 可配置项（QPS / 超时 / 重试） + 版本号 |

`id` 支持 `BV1xxxxxxxxx`、`av12345`、纯数字 aid、视频链接；多 P 视频用 `BV1xxx?p=2` 指定分 P。

## 音质映射

| quality_id | 展示名 | level | DASH 音频 id | 说明 |
| --- | --- | --- | --- | --- |
| `hires` | Hi-Res 无损 | hires | 30251 | 大会员，FLAC |
| `dolby` | 杜比全景声 | high | 30250 | 大会员，EC-3 |
| `a192` | 192K | high | 30280 | 常规最高 |
| `a132` | 132K | medium | 30232 | |
| `a64` | 64K | low | 30216 | |

宿主传 `high` / `medium` / `low` / `auto` 这类归一化等级时，插件会自动映射；
目标音质不可用时降到「不高于目标」的最高一档，并在返回里给出真实的 `quality_id`。

## 配置（可选 config.json）

```json
{
  "cookie": "",
  "qps": 5,
  "timeout": 20,
  "retries": 2
}
```

命令行 `--port` / `--qps` / `--timeout` 可临时覆盖。

## 常见问题

| 现象 | 原因与处理 |
| --- | --- |
| `端口 8849 已被占用` | 已经有一个插件实例在跑；宿主会直接复用，无需处理 |
| `需要登录 bilibili`（1021） | 该清晰度 / 收藏夹需要登录，去登录页粘贴 Cookie |
| `触发 bilibili 风控`（1014） | 请求过快或被临时限流，等几分钟，或登录后重试 |
| 播放 403 / 502 | 媒体地址过期，重新点一次播放（插件会重新取址） |
| 没有歌词 | 该视频没有 CC 字幕；bilibili 不提供逐句歌词，属正常现象 |
| MV 只有 720P | 未登录时混流 MP4 最高一般到 720P；登录且是大会员可选 1080P+ |
| MV 提示「没有可用的混流 MP4」 | 极少数视频只给 DASH 单轨，无法在浏览器里直接合并播放 |

## 实现备注

- 纯 Python 标准库（`urllib` + `http.server`），零依赖，随宿主自动拉起；
- WBI 签名：`nav` 取 `img_url`/`sub_url` → 重排取 32 位 mixin key → `w_rid`，密钥缓存 15 分钟，`-403` 时自动刷新重签；
- 无登录时自动取 `buvid3`/`buvid4`，降低被风控概率；
- 元数据 / 播放地址 / 字幕都有短 TTL 缓存，避免拖动进度条时反复打 bilibili 接口；
- 日志写 `plugin.log`（超过 4 MiB 轮转一份 `.1`），不打印 Cookie 明文。
