# 网易云音乐播放源插件（netease.source）

给 Rad Music 提供 `netease` 播放源：搜索、播放地址、音质、歌词、封面、歌单、专辑、歌手、MV，以及扫码登录。

- 协议：外部进程 + 本地 HTTP JSON-RPC（Readme 5.5）
- 依赖：仅 Python 3 标准库
- 默认端口：`8848`（`plugin.json` 的 `endpoint` / `command`）

## 启动

宿主 `radmusic_server` 会按 `plugin.json` 的 `command` 自动拉起；也可以手动：

```bash
python plugins/netease/main.py --port 8848
python plugins/netease/main.py --check          # 自检：登录态 + 搜索 + 取播放地址
```

- `GET  /health` → `{"ok": true, ...}`（探活，不发起网络请求）
- `GET  /login` → 自带的登录页面（扫码 / Cookie / 退出）
- `POST /request` → `{"op": "...", "params": {...}}` → `{"code": 0, "message": "ok", "data": {...}}`

`code` 取自 Readme 第 3 节：`0` 成功、`1004` 找不到、`1007` 参数错误、`1010` 超时、
`1011` 网络/协议失败、`1012` 未声明的 op、`1014` 限流/风控、`1021` 音质不可用、
`1030/1031/1032` 歌单/专辑/MV 不存在。歌词不可用时返回 `1015`（宿主按无歌词处理）。

## 登录

推荐扫码（无需密码，也不受服务端 IP 风控影响）：

1. 浏览器打开 `http://127.0.0.1:8848/login`
2. 用网易云音乐 App 扫码、手机确认
3. 登录成功后凭据自动写入 `plugins/netease/cookie.txt`，重启插件仍然有效

也可以用 op 走接口：

| op | 参数 | 说明 |
| --- | --- | --- |
| `check_login` | `refresh`（可选） | 当前登录态（昵称、uid、VIP、是否已配置 Cookie） |
| `login_qr_start` | — | 生成二维码：返回 `key`、`qr_url`、`qr_image_url`、`expires_in` |
| `login_qr_check` | `key` | 轮询：`800` 过期 / `801` 待扫码 / `802` 待确认 / `803` 成功（成功后自动落盘） |
| `login_cookie` | `cookie` | 导入整行 Cookie，或只给 `MUSIC_U` 的值；无效会被拒绝并回滚 |
| `login_phone` | `phone`、`password` 或 `captcha`、`country_code` | 明文接口；网易云现多返回 `401/-460`（要求加密/风控），失败会提示改用扫码 |
| `logout` | — | 退出登录：清空内存凭据、删除 `cookie.txt`、失效账号相关缓存 |

Cookie 读取优先级：环境变量 `RAD_NETEASE_COOKIE` > `cookie.txt` > `config.json` 的 `cookie`。
若设置了环境变量，登录后重启仍以环境变量为准。

登录后会重新计算曲目的可播音质（`privilege.plLevel`），无损 / Hi-Res 与 VIP 曲目随之解锁；
未登录时 VIP 曲目会标记为「需要登录（黑胶 VIP 曲目）」而不是给出假地址。

## 配置（可选 config.json）

```json
{
  "cookie": "MUSIC_U=...",
  "qps": 6,
  "timeout_seconds": 20,
  "retries": 2
}
```

- `qps`：插件侧匀速限流，避免短时间打太猛触发风控（默认 6，可用 `--qps` 覆盖）
- `timeout_seconds`：单次网易云请求超时（默认 20）
- `retries`：网络类失败重试次数（默认 2，`460`/`1014` 不重试）

## 支持的 op

| op | 参数 | 返回要点 |
| --- | --- | --- |
| `search` | `keyword`、`page`、`page_size`、`type` | `items[]`：`id`/`name`/`artists`/`album`/`duration_seconds`/`pic`/`available`/`reason`/`quality`/`qualities` |
| `get_song` | `id` | 可直接落 `base.json` 的元数据（含 `album`、`track_no`、`quality`） |
| `get_url` | `id`、`quality` | 真实播放地址 + 实际音质；播放时若降级会返回 `downgraded: true`、`expires_at` |
| `get_qualities` | `id` | 该曲目**当前账号**可播的音质；`available`/`reason` |
| `get_lyric` | `id` | 结构化行（`time_ms`/`text`/`tr`/`roma`/`words`）+ 原始 LRC |
| `get_pic` | `id`、`size`（如 `300` 或 `300x300`） | 封面地址 |
| `get_playlist` | `id`（支持歌单 URL）、`limit` | 歌单信息 + `tracks[]`（`item_id`/`name`/`author`/`duration`） |
| `get_album` | `id`（支持 `netease_xxx`） | 专辑信息 + 按曲序的 `tracks[]` |
| `search_artist` | `keyword`、`page`、`page_size` | 歌手列表：`id`/`name`/`alias`/`pic`/`music_count`/`album_count` |
| `get_artist` | `id`、`song_limit`、`album_limit`、`include_songs` | 歌手详情 + `hot_songs[]` + `albums[]` + 简介 |
| `search_album` | `keyword`、`page`、`page_size` | 专辑列表：`id`/`name`/`artist`/`pic`/`size`/`year` |
| `search_playlist` | `keyword`、`page`、`page_size` | 歌单列表：`id`/`name`/`creator`/`pic`/`track_count` |
| `search_mv` | `keyword`、`page`、`page_size` | `mvs[]`（含 `sha`，可直接加入 MV 库） |
| `get_mv` | `id` | MV 详情 + 最佳分辨率地址 + `resolutions[]` |
| `settings` | — | 设置面板数据：当前登录态 + 可配置项（QPS / 超时 / 重试） |
| `check_login` / `login_*` / `logout` | 见上 | 登录相关 |

别名：`song`→`get_song`、`url`→`get_url`、`qualities`→`get_qualities`、`lyric`→`get_lyric`、
`pic`→`get_pic`、`playlist`/`album`/`mv`、`artist`→`get_artist`、`artist_search`→`search_artist`。

### 关于搜索类型

宿主搜索接口支持 `type=song/album/artist/playlist`，本插件：

- `type=song`：常规单曲搜索
- `type=artist`：先检索歌手，**返回该歌手的热门歌曲**（可直接播放，搜索页的「歌手」筛选即可用），
  歌手实体本身（简介、专辑数、头像）用 `search_artist` / `get_artist` 获取
- `type=album`：返回专辑列表（带 `id`/`name`/`artist`/`pic`/`size`/`year`），前端可按专辑卡片展示，
  详情用 `get_album` 拉；亦可单独调用 `search_album`
- `type=playlist`：返回歌单列表（带 `id`/`name`/`creator`/`pic`/`track_count`），详情用 `get_playlist`；
  亦可单独调用 `search_playlist`

## 音质与可用性判定

- 曲目详情取 `/api/v3/song/detail`（含 `privileges`），按 `privilege.plLevel` 得出当前账号
  可播到哪一档：`none < standard < higher < exhigh < lossless < hires`
- 同时存在 `h/sq/hr` 音轨才认为该档位在平台上可用，两者取交集，**不谎报音质**
- 取地址时从请求音质逐级降级（也可向上兜底一次），再按响应里的真实 `type/br/sr` 判定实际音质
- 免费曲目（`fee=8`）未登录也能拿标准/较高/极高；`fee=1` 需要黑胶 VIP；`fee=4` 需购买专辑
- 地址统一转 `https`，并从地址里的时间戳推算 `expires_at`；短期缓存 2 分钟用于去重

## 排错

| 现象 | 原因 / 处理 |
| --- | --- |
| `1013 插件进程未响应` | 插件没起来；`python plugins/netease/main.py --port 8848` 或重启宿主 |
| `1014 ...（code=460）` | 网易云风控；降低频率、稍后重试，或登录后使用 |
| `1021 ...（-110）` | 该音质需要登录/VIP；看 `check_login` 的 `vip`，或改用标准音质 |
| 二维码图片空白 | 页面用 `api.qrserver.com` 渲染二维码；离线时改用「在网页打开」链接或 `login_cookie` |
| 手机号登录失败（401/502/-460） | 网易云已要求加密登录，请用扫码或 Cookie |
| 重启后又变未登录 | Cookie 被环境变量 `RAD_NETEASE_COOKIE` 覆盖，或 `cookie.txt` 被清 |

日志：`plugins/netease/plugin.log`（启动、错误、慢请求）。

## 版本

- 0.14.0：补 `search_album` / `search_playlist` op 与 `type=album/playlist` 搜索支持；新增 `settings` 面板数据（含 QPS/超时/重试与登录态）；调整 `type` 映射以便搜索页切换 chip 能拿到结果
- 0.13.0：新增歌手搜索（`type=artist` 返回热门歌曲）、`search_artist` / `get_artist`；`album`/`playlist` 类型降级为空结果
- 0.12.0：新增登录（扫码 / Cookie / 手机号 / 退出）、自带 `/login` 页面、凭据持久化
- 0.11.0：补 `get_playlist` / `get_album` / `search_mv` / `get_mv` / `check_login`；真实音质与可用性、限流、缓存、错误码
