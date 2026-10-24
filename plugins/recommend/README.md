# 智能推荐插件（recommend.assistant）

给 Rad Music 提供「跨音源聚合推荐」能力：每日推荐、漫游、探索、相似推荐、心情电台。它会去拉起 / 调用
真正的音源插件（网易云、B 站……），按你的偏好把候选曲目聚合、打分、排序后返回。

- 协议：外部进程 + 本地 HTTP JSON-RPC（与 netease / bilibili 插件一致，Readme 5.5）
- 依赖：仅 Python 3 标准库
- 默认端口：`8850`（`plugin.json` 的 `endpoint` / `command`）

## 为什么是独立进程、又去调别的音源？

宿主 `radmusic_server` 是单线程处理请求的。如果推荐插件在处理宿主转发来的请求时，又回头调宿主去搜音源，
会形成「宿主 → 推荐插件 → 宿主 → 音源」的嵌套调用并可能死锁。所以本插件直接读取兄弟插件目录下的
`plugin.json`，拿到各音源的 `endpoint`，**点对点并行请求**音源，干净且不会死锁。

## 启动

宿主会在 `load_remote_plugins` 阶段扫描 `plugins/*/plugin.json`。本插件放在
`plugins/recommend/` 下即被识别为 `platform=recommend` 的播放源类插件由宿主路由转发。
`plugin.json` 里带了 `command` 字段，宿主在**启动时会自动拉起本插件进程**（除非用 `--no-plugin-autostart` 关掉）；
如果进程意外退出，也可以手动启动：

```bash
# 用隔离环境里的 Python（或任意 Python 3）
python plugins/recommend/main.py --port 8850
```

启动后宿主侧 `GET /music/plugins` 会列出 `recommend.assistant`（capabilities 含 `recommend_daily` 等），
前端「发现」页即可调用。

```bash
# 离线自检（无需音源 / 宿主，验证推荐引擎逻辑）
python plugins/recommend/main.py --selftest
```

- `GET  /health` → `{"ok": true, ...}`（探活，不发起网络请求）
- `POST /request` → `{"op": "...", "params": {...}}` → `{"code": 0, "message": "ok", "data": {...}}`

`code` 沿用 Readme 第 3 节错误码：`0` 成功、`1004` 找不到、`1007` 参数错误、`1009` 无可用音源、
`1011` 内部错误、`1012` 不支持的 op。

## 支持的 op

所有推荐类 op 返回统一结构：

```json
{
  "mode": "recommend_daily",
  "items": [ { "platform", "item_id", "sha", "title", "artists", "artist",
              "album", "duration_seconds", "pic", "available", "quality", "qualities", "score" } ],
  "count": 30,
  "seeds": ["周杰伦", "流行", "..."],
  "generated_at": 1700000000,
  "platforms": ["netease", "bilibili"],
  "meta": { ... }
}
```

| op | 参数 | 说明 |
| --- | --- | --- |
| `recommend_daily` | `limit`、`sources`（逗号分隔音源，可选） | **每日推荐**：基于收藏歌手 / 曲风 + 当日轮换探索词；当天稳定、每天不同 |
| `roam` | `limit`、`seed`（可选，可复现）、`sources` | **漫游**：以新颖度优先的随机游走式发现；`discovery_ratio` 越高越偏向冷门与跨风格 |
| `explore` | `keyword`（关键词/曲风）、`mood`（可选）、`limit`、`sources` | **探索**：跨音源聚合并按相关度排序；不给关键词则用收藏曲风之一 |
| `similar` | `artist` 或 `title`、`limit`、`sources` | **相似推荐**：给歌手直接取其作品 + 曲风延伸；给歌名先解析歌手再做相似 |
| `mood` | `mood`（chill/focus/energetic/nostalgia/fresh/happy/sad/sleep）、`limit`、`sources` | **心情电台**：自动编排契合该情绪的音乐 |
| `recommend_feed` | 同 `recommend_daily` | 当日每日推荐（带日内缓存，重复调用不重复计算） |
| `preferences_get` | — | 读取偏好（收藏歌手、曲风、音源权重、喜欢/不喜欢计数、配置） |
| `preferences_set` | `favorite_artists`、`genres`、`source_weights`、`excluded_platforms`、`mood_weights`、`config` | 写入偏好（列表字段整体覆盖，权重/配置合并） |
| `feedback` | `action`(like/dislike/play)、`track{title,artist,platform,item_id}` | 反馈调权：like 加入喜欢并移除不喜欢，dislike 反之 |
| `modes` | — | 列出所有模式的元信息（id/label/icon/desc/needs_input）与可用心情、音源 |
| `settings` | — | 设置面板数据（每日条数 / 漫游新颖度 / 超时 / 默认音源） |
| `search` | — | 占位：返回空结果，避免污染宿主全局搜索 |

## 偏好怎么用

偏好保存在 `plugins/recommend/preferences.json`（首次运行生成默认值）：

```json
{
  "favorite_artists": ["周杰伦", "Taylor Swift"],
  "genres": ["流行", "摇滚"],
  "mood_weights": { "chill": 1.0, "focus": 0.8, "energetic": 0.6, "nostalgia": 0.5,
                    "fresh": 0.5, "happy": 0.4, "sad": 0.3, "sleep": 0.3 },
  "source_weights": { "netease": 1.0, "bilibili": 0.8 },
  "excluded_platforms": [],
  "liked": [], "disliked": [], "recent_plays": [],
  "config": { "daily_limit": 30, "discovery_ratio": 0.5, "timeout_seconds": 12, "default_sources": "" }
}
```

- **收藏歌手 / 曲风**：每日推荐与相似推荐的主要依据；越写越准。
- **音源权重 `source_weights`**：跨音源排序时给某音源加权（如更信任网易云）。
- **排除音源 `excluded_platforms`**：例如 `["bilibili"]` 让推荐完全忽略 B 站。
- **`discovery_ratio`**：漫游模式的新颖度，0=只听熟悉的，1=尽量陌生。
- **喜欢 / 不喜欢（dislike）**：被 dislike 的曲目在所有模式都会被过滤掉。

前端「发现」页提供「偏好」弹窗，可直接编辑收藏歌手、曲风、启用音源，无需手改文件。

## 打分逻辑（各模式差异）

所有模式都先「取候选 → 归一化 → 打分 → 去重 → 排序 → 截断」，差异在种子与打分权重：

- **每日推荐**：收藏歌手（+3）、曲风命中（+0.3）、探索词弱权重、音源权重、当日稳定抖动；同曲跨源去重。
- **漫游**：以陌生曲风为主种子，追求「新颖度」（不与已知偏好重叠的给高分），同歌手重复强惩罚，
  高抖动保证漫步感；`discovery_ratio` 调节熟悉↔陌生。
- **探索**：关键词命中主导（+2），叠加音质与可用性的加权，跨源取其最佳音质。
- **相似**：歌手名命中主导（+3），以该歌手作品 + 收藏曲风延伸。
- **心情电台**：对应心情的种子词命中主导，叠加音质与音源权重。

## 排错

| 现象 | 原因 / 处理 |
| --- | --- |
| `1009 没有可用的音源插件` | 没启动 netease / bilibili 等真实音源，或它们的 `plugin.json` 不在兄弟目录 |
| 推荐结果很少 / 为空 | 偏好里收藏歌手、曲风太少；或音源插件未就绪（先确认各音源 `GET /health` 正常） |
| 每次推荐都差不多 | 多写 `favorite_artists` / `genres`；漫游可把 `discovery_ratio` 调高 |
| 某音源总不出现 | 在偏好里把它加进 `excluded_platforms` 可彻底排除；或在 `source_weights` 里给它低权重 |
| 插件日志 | `plugins/recommend/plugin.log`（启动、错误、慢请求） |
| 改了偏好不生效 | 偏好是进程内实时读取的；若改了 `preferences.json` 文件，重启插件即可加载 |

## 版本

- 0.1.0：首版。多模式推荐（每日/漫游/探索/相似/心情电台）+ 偏好存储 + like/dislike 反馈 + 离线自检；
  通过兄弟插件 `plugin.json` 自动发现音源并点对点并行聚合。
