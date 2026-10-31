# Rad music API — C++ 实现说明

本目录是 [Readme.md](Readme.md)（Rad music API 标准 v0.9）的 C++17 实现。
**零第三方依赖**：JSON、SHA-256、MD5、HMAC-SHA256、PBKDF2、Base64、AES-256-GCM 全部自带实现。

## 构建与测试

```bash
# 方式一：Makefile（MSYS2/MinGW，已实测）
mingw32-make            # 编译测试与 HTTP 服务端
mingw32-make test       # 运行全部测试
mingw32-make server     # 只编译 HTTP 服务端
mingw32-make test FILTER=storage   # 只跑名字含 storage 的用例

# 方式二：CMake
cmake -S . -B build-cmake && cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

Windows 上 HTTP 传输层需要链接 **ws2_32**（Makefile 与 CMakeLists 已处理）。

### 跑起来

```bash
.build/radmusic_server.exe --root radmusic-data --host 127.0.0.1 --port 8765 --demo

curl http://127.0.0.1:8765/music/status
curl -X POST http://127.0.0.1:8765/music/search -H "Content-Type: application/json" \
     --data-binary '{"keyword":"demo"}'
curl -X POST http://127.0.0.1:8765/music/request -H "Content-Type: application/json" \
     --data-binary '{"source":"demo","op":"get_lyric","params":{}}'
```

`--demo` 会注册一个内置演示播放源（无网络依赖），用于手工联调搜索/折叠/音质/歌词链路；
真实使用需换成真实播放源插件。Windows 上 `curl.exe -d '{...}'` 会被 shell 吞掉引号，
请用 `--data-binary @文件`。

已验证环境：`g++ (Rev5, MSYS2) 16.1.0` + `mingw32-make`，`-std=c++17 -Wall -Wextra` 零警告，
`mingw32-make test` 退出码 0；HTTP 层另用 `curl.exe` 对运行中的服务端做过端到端验证。
本机未安装 `cmake`，因此 `CMakeLists.txt` 只做了静态编写（标准用法，未实机验证）。

## 目录结构

```text
include/radmusic/
├─ error.hpp        错误码（Readme 第 3 节：0 / 1001–1032）与 Status/Result<T>
├─ json.hpp         极小 JSON（保序对象、Int/Double 区分、最短往返浮点输出）
├─ crypto.hpp       SHA-256 / MD5 / HMAC-SHA256 / PBKDF2 / Base64 / AES-256-GCM
├─ quality.hpp      音质定义（1.1.1）：归一化等级、比较、quality_key、插件音质规格
├─ groupkey.hpp     归一化、折叠键 group_key、缓存目录键 song_key、文件名安全化
├─ model.hpp        BaseInfo（base.json）、搜索模型、播放状态、SMTC 元数据
├─ storage.hpp      LibraryPaths / MetaCache / LibraryIndex / LibraryLock
├─ plugin.hpp       Function 插件标准（第 5 节）：清单、播放源类与其他类接口、注册表
├─ lyrics.hpp       LRC 解析 / 归一化 / 偏移 / 当前行定位（1.5）
├─ decrypt.hpp      usec.json / sec.json、口令校验、加解密（1.4）
├─ engine.hpp       核心引擎：加载与播放、歌词、SMTC 上报（1.3 / 1.5 / 1.6）
├─ search.hpp       多源搜索与折叠聚合（2.5 / 2.7）
├─ tags.hpp         本地音乐标签解析与音频流探测（1.10）
├─ cache.hpp        缓存统计 / 清理 / 重建 / LRU（1.9 / 2.11）
├─ download.hpp     下载任务队列（1.8 / 2.10）
├─ import.hpp       导入编排（1.10 / 2.12）
├─ api.hpp          API 层：请求 → 领域模块 → {code, message, data}（第 2 节）
├─ http.hpp         HTTP/1.1 传输层：请求解析、响应构造、极简客户端、可嵌入服务端
├─ collections.hpp  歌单 / 专辑 / MV 持久化（1.11–1.13）
└─ log.hpp          运行日志：分级、分类、控制台/文件/内存三类 sink（1.14）

src/                对应实现（一个 .hpp 一个 .cpp）
tools/              radmusic_server.cpp：把各模块接线成可直接运行的 HTTP 服务
tests/              零依赖测试框架 + 各模块测试（test_*.cpp）
```

### 构建注意（重要）

`Makefile` 使用 **`-MMD -MP`** 生成头文件依赖。这不仅是便利：`ApiDispatcher` / `LibraryPaths` 等类布局一旦变化，
若旧的 `.o` 未重编，混链会产生**随机堆损坏/段错误**（本项目实际踩过：修改 `api.hpp` 后测试随机崩在析构里）。
改动头文件后请务必 `mingw32-make clean`（或依赖 `-MMD` 自动重编）。

## Readme 章节 → 代码映射

| Readme 章节 | 实现位置 | 关键约束 |
| --- | --- | --- |
| 1.1 base.json 与存放位置 | `model.hpp` `storage.hpp` | `cache/meta/<song_key>/<quality_key>/base.json`，一首歌一个文件夹、每个音质一个文件夹；路径即唯一键 |
| 1.1.1 音质定义 | `quality.hpp` | 归一化等级 `hires>lossless>high>medium>low`；`quality_key` = `<platform>.<quality_id>` 或 `local.<level>-<codec>-<bitrate>` |
| 1.2 platform 与插件 | `plugin.hpp` | 只有播放源类可声明 `platforms` 与 `quality_levels`；音质定义唯一来源是插件清单 |
| 1.3 加载/播放 | `engine.hpp/.cpp` | base 缺失先重建（1028）；platform 检查（1001）；sha 不一致仅警告（1002 非致命）；`secret=1` 走解密；入缓存 `cache/audio/` |
| 1.4 解密 | `decrypt.hpp/.cpp` | v1 兼容 MD5 校验值；v2 用 PBKDF2 + AES-256-GCM；口令不符 1003，认证失败 1006 |
| 1.5 歌词 | `lyrics.hpp/.cpp` | LRC/增强 LRC/纯文本；同时间戳多行 = 原文/翻译/罗马音三轨；失败 1015 不阻断播放 |
| 1.6 SMTC | `engine.hpp`（`SmtcSink`） | 元数据/状态/时间轴上报 + 按钮可用性；**失败不影响播放**（1016） |
| 1.7 调音 | `api.hpp`（`EqState`） | EQ 参数与预设持久化到 `presets/eq/`；参数非法 1007 |
| 1.8 下载 | `download.hpp/.cpp` | 不同音质各自独立存储；同曲同质去重；校验失败 1022；落盘 `tracks/` |
| 1.9 缓存 | `cache.hpp/.cpp` | 分层统计；清理不碰 `tracks/`、`presets/`；in-use 条目标记 pending eviction；LRU 淘汰 |
| 1.10 音乐导入 | `import.hpp/.cpp` | 标签解析失败回退文件名（1024 非致命）；生成 base.json 入缓存；唯一性/冲突规则（1023）；`library.lock` 防并发（1026） |
| 1.11 歌单 | `collections.hpp/.cpp`（`PlaylistStore`） | 用户数据存 `library/playlists/<id>.json`；只存引用、幂等去重、保序、原子写；未知 id → 1030 |
| 1.12 专辑 | `collections.hpp/.cpp`（`AlbumStore`） | `library/albums/<id>.json`；在线来源 id = `<platform>_<source_id>`（幂等同步）；未知 id → 1031 |
| 1.13 MV | `collections.hpp/.cpp`（`MvStore`） | `mv/<sha>/mv.json`；`sha` 与 `(platform,item_id)` 双唯一，仅同 `sha` 可覆写，冲突 → 1023；未知 sha → 1032 |
| 1.14 运行日志 | `log.hpp/.cpp` | 分级/分类、控制台+文件（8 MiB 轮转）+内存环形缓冲；默认静默；`GET /music/logs` 读取 |
| 第 2 节 API | `api.hpp/.cpp` | 统一 `{code, message, data}`；`code=0` 成功 |
| 2.1 调用形态 | `http.hpp/.cpp` | 本地库直接调用 = REST 语义；HTTP 只做传输映射 |
| 2.5 / 2.7 搜索与折叠 | `search.hpp/.cpp` | 时长容差 ±3 秒折叠；单源失败整体仍成功；同一源多音质不拆行；`sources[].id` 保留插件真实 id |
| 2.6 request | `api.cpp` `h_request` + `plugin.hpp` | 以插件清单 `capabilities` 为准，未声明 → 1012 |
| 2.9 调音 / 2.11 缓存 / 2.12 导入 / 2.13 音质 | `api.cpp` | 未装配的模块返回 1011 |
| 2.14 歌单 / 2.15 专辑 / 2.16 MV / 2.17 日志 | `api.cpp` `h_playlist` / `h_album` / `h_mv` / `h_logs` | 列表含派生字段 `track_count` / `total_duration`；日志支持 `lines` / `level` / `category` 过滤与运行时改级别 |
| 2.20 检查更新 | `update.hpp/.cpp`（`UpdateService`）、`archive.hpp/.cpp`、`httpclient.hpp/.cpp`、`version.hpp`、`settings.hpp`（`update_threads` / `update_channel`）、`web/settings.js`（「版本」页） | 检查与下载都异步（宿主 idle hook 调 `step()` 分片推进，不阻塞主循环）；一次拉 30 条 Release 按 tag 后缀分频道（release/dev/canary）取头部版本，`compare_version_full` 带预发布后缀比较；只匹配本平台产物；解压逐条目 CRC-32 校验；Windows 用替换脚本延迟覆盖（运行中的 exe 无法覆盖），其他平台直接覆盖后 execv |

## 与 Readme 对应的「不可覆写」保证

`base.json` 的写入遵循 1.1 / 1.10 的规则，测试覆盖在 `tests/test_storage.cpp`：

1. **路径即唯一键** `song_key` + `quality_key`，不同音质天然不同目录；
2. 写入前 **读-比较-再写**：同内容 → `skipped`；有可补全字段 → `merged`；同曲同质不同内容 → `base conflict (1023)`，默认**拒绝覆写**，只有显式 `overwrite` 才替换；
3. **原子提交**：先写同目录 `base.json.tmp`，再同卷原子替换（Windows 用 `MoveFileExW`，其他平台 `rename`），崩溃时回退读 `.bak`；
4. **一致性反推校验**：目录名必须能由 `base.json` 内容重算，否则报 `1029 meta path mismatch` 并视为损坏条目；
5. **平台隔离**：`netease.lossless` 与 `radish.lossless` 是不同目录；
6. **可重建**：缓存被清理后由库索引 `index/` + 音频/在线源重建到同一路径。

## 尚未实现 / 需要宿主补齐

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| HTTP 服务器 | **已实现** | `HttpServer`（自带 WinSock/POSIX 实现，零第三方库）：请求解析、Content-Length body、状态码、可配 IO 超时；`tools/radmusic_server.cpp` 是可直接运行的接线版本 |
| HTTP 取舍 | 已文档化 | 每连接只服务一次请求（固定 `Connection: close`）；不支持 chunked（411）；不做 TLS（应由反向代理终止）；业务结果放在 JSON 的 `code`，HTTP 状态码只表示传输层结果 |
| 真实下载器 | 接口已定 | `DownloadManager::fetch` 默认只支持本地/`file://` 分片复制；http(s) 必须由宿主注入 |
| SMTC 后端 | 接口已定 | 实现 `SmtcSink`（Windows 用 WinRT `SystemMediaTransportControls`）并 `set_smtc_sink()` |
| 真实播放后端 | 接口已定 | `CoreEngine::backend_start/backend_stop` 注入解码器；否则引擎只维护状态机 |
| 调音 DSP | 参数层已实现 | 真正的音频处理链（EQ 滤波器/限幅）需宿主或音效插件实现，`EqState` 只做参数与预设管理 |
| 插件加载器 | 未实现 | `PluginRegistry` 支持注册与校验清单；从 `plugins/source/`、`plugins/function/` 动态库加载需要平台相关代码 |
| Ogg/Opus 标签、GBK 转码 | 部分实现 | 见 `src/tags.cpp` 头部注释与 Readme 第 7 节待定项 |

## 测试覆盖

```text
tests/test_json.cpp      JSON 解析/输出、转义、代理对、错误定位
tests/test_crypto.cpp    SHA-256/MD5/HMAC/PBKDF2/Base64/AES-256-GCM 已知答案向量
tests/test_storage.cpp   base.json 布局、防覆写、冲突策略、路径一致性、库索引、库锁
tests/test_lyrics.cpp    LRC/增强 LRC/三轨双语/偏移/当前行定位/回环
tests/test_search.cpp    折叠（±3 秒）、同源多音质合并、单源失败、分页
tests/test_tags.cpp      合成 ID3v2/FLAC/WAV/MP4/APE/Ogg 的标签与流参数
tests/test_engine.cpp    加载/播放状态机、重建、sha 警告、解密流程、歌词、SMTC 失败降级
tests/test_cache.cpp     分层统计、按层清理、in-use 跳过、LRU 淘汰
tests/test_download.cpp  任务状态机、同曲同质去重、校验失败、落盘与索引
tests/test_import.cpp    导入模式、回退文件名、冲突处理、dry_run、库锁
tests/test_api.cpp       API 层：接口路由、返回结构、错误码、搜索折叠、请求能力、音质、调音
tests/test_http.cpp      HTTP 层：请求解析/错误码、响应序列化、真实 socket 往返（含 408/411/400）
tests/test_log.cpp       日志：级别过滤、环形缓冲、文件轮转、记录格式、configure 装配
tests/test_collections.cpp       歌单/专辑/MV 存储：id、去重、排序、损坏 JSON 容错、MV 防覆写
tests/test_api_collections.cpp   v0.10 API：歌单/专辑/MV 生命周期、日志 API、**向后兼容断言**
```

当前状态（本机实测，`g++ 16.1`，`-Wall -Wextra` 零警告）：

```text
$ mingw32-make clean && mingw32-make test
合计: 167 个用例, 通过 167, 失败 0        # 退出码 0（连跑两次一致）
```

规模：`src/` 20 个文件、`include/radmusic/` 20 个头文件、`tests/` 17 个文件、`tools/` 1 个，合计约 20k 行。

HTTP 层端到端验证（真实进程 + 外部客户端 `curl.exe`）：

```text
GET  /music/status   → {"code":0,...,"state":"stopped"}
POST /music/search   → total=1，sources[0].id 保留插件真实 id
POST /music/request  → get_lyric 返回 lrc；未声明 op → 1012
GET  /music/quality  → 插件清单里的音质定义
GET  /music/cache    → 各层用量（meta/audio/stream/lyric/cover/tmp）
GET  /music/nope     → HTTP 200 + {"code":1011}
POST /music/playlist → pl_0001（track_count / total_duration 派生字段）
POST /music/album    → netease_12345（在线来源 id 幂等）
POST /music/mv       → mv/<sha>/mv.json 落盘；同源不同 sha → 1023
GET  /music/logs     → 内存日志（time/level/category/message/file/line）
POST /music/logs/level → 运行时切级别；logs/radmusic.log 为 UTF-8、8 MiB 轮转
```

运行方式：`mingw32-make test`，或 `build/radmusic_tests.exe -v`（列出每个用例名）、
`build/radmusic_tests.exe storage`（按名字过滤）。

## v0.10 的兼容性（与 Readme 6.1 对应）

| 维度 | 实现方式 |
| --- | --- |
| `base.json` | 未新增任何必填字段；旧文件照常解析（测试 `test_api_backward_compatibility_v010` + 既有 storage 用例） |
| 缓存布局 | `cache/meta/<song_key>/<quality_key>/base.json` 未变；新内容全在新目录 |
| 新目录 | `library/`、`mv/`、`logs/` 由 `LibraryPaths::ensure_layout()` **按需创建**，旧曲库缺省不报错 |
| API 响应 | 既有字段只增不改；歌单/专辑/MV/日志是**新接口**；`track_count` / `total_duration` 为新增派生字段 |
| 错误码 | 0 / 1001–1029 语义不变，新增 1030–1032 |
| 日志 | 库默认静默；只有 `log_configure()` / `--log-level` 显式开启后才产生输出与 `logs/` 文件 |

## v0.11：搜索筛选 / 本地搜索 / 专辑曲库选取

| 能力 | 实现位置 | 说明 |
| --- | --- | --- |
| 按音源搜索 | `search.cpp execute_search()` | `request.sources` 现在真正生效：platform 不在列表中的插件直接跳过 |
| 本地搜索 | `api.cpp local_search_entries()` | `sources` 含 `local` 或缺省时，把 `cache/meta` 里 `platform=local` 的条目作为 `local` 源参与折叠；`sources_status` 增补 `local` 状态与 `result_count` |
| 筛选 | `search.cpp item_matches_filters()/sort_items()` | `quality_min` / `duration_min` / `duration_max` / `available_only` / `sort`，在折叠后、分页前执行，`total` 与分页计数一致 |
| 专辑批量加入 | `api.cpp h_album("add_many")`、`collections.cpp AlbumStore::add_tracks()` | 一次读盘-合并-落盘，自动去重，供「从曲库选取」使用 |
| 专辑编辑 | `api.cpp h_album`（`POST /music/album/{id}`）、`AlbumStore::update()` | 名称 / 艺术家 / 年份 / 封面 / 简介，空字段不修改 |
| 歌单批量加入 | `h_playlist("add_many")`、`PlaylistStore::add_tracks()` | 与专辑同构 |

## v0.13：歌单分享 / 曲库导出导入

| 能力 | 实现位置 | 说明 |
| --- | --- | --- |
| `.rdl` 歌单分享 | `share.cpp share_rdl_from_playlist()` / `share_playlist_from_rdl()`、`api.cpp h_share("playlist")` | JSON 封装（`format/version` 头 + 元数据 + 曲目引用）；解析时 `format` 非空则校验、版本高于 1 拒绝、缺 `tracks` 或无有效曲目 → `1007` |
| 常规文件导出 | `share.cpp share_export_tracks()`（`as_package=false`） | 复制到 `<曲库>/exports/<名字>/`，文件名 `序号 - 歌名 - 歌手.ext`，`share_safe_filename()` 过滤 Windows 非法字符并做 UTF-8 安全截断；同名目录自动加 `-2` |
| `.rdm` 音频包导出 | 同上（`as_package=true`）+ `archive.cpp write_zip()` | zip（stored）内含 `base.json` + `music/…` + `covers/…`；写盘分两遍（先流式算 CRC 再拷数据），峰值内存与文件大小无关；先写 `.part` 再 rename |
| `.rdl` / `.rdm` / 音频导入 | `api.cpp share_import()`、`http.cpp`（`/music/share/import` 走原始字节分支） | 按扩展名分流；`.rdm` 用 `archive.cpp extract_zip_streamed()` 解压到 `cache/tmp/package_*`，音频交给 `ImportManager`，再按清单 `sha`（缺失按文件名）还原歌单、补封面、删临时目录 |
| 导出产物下载 | `http.cpp make_export_download_response()`、`api.cpp export_file_for()` | 只允许 `exports/` 下**单层文件名**；中文名同时给 `filename` 与 `filename*=UTF-8''` |
| 安全边界 | `archive.cpp is_safe_relative_path()`（沿用）、`share.cpp resolve_audio_file()` | 导出只写 `exports/`；导入只解压到 `cache/tmp`；zip 条目拒绝绝对路径 / `..` / 符号链接，CRC 不符即失败 |
