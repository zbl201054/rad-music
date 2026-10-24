# Rad music API 标准 #

> 项目总览（功能、架构、构建与运行）见 [../Readme.md](../Readme.md)。

> C++17 实现见 [IMPLEMENTATION.md](IMPLEMENTATION.md)（零第三方依赖，含构建、测试与章节映射）。

> 本文档为 Rad music 的 API 标准，包括 API 调用、定义、功能。
>
> **文档状态**：草案 v0.10
> **标记约定**：
> - 「原文」= 初版已有内容，语义保持不变；
> - 「补充」= 本次新增的草案内容，实现时可按实际情况调整；
> - 「待定」= 尚未确定，需要评审后再固化。

## 目录 ##

- [0. 阅读指引](#0-阅读指引)
- [1. 基本功能](#1-基本功能)
  - [1.1 基本信息（base.json）](#11-基本信息basejson)
  - [1.1.1 音质定义（quality）](#111-音质定义quality补充)
  - [1.2 platform 与插件适配](#12-platform-与插件适配)
  - [1.3 播放和加载音乐](#13-播放和加载音乐)
  - [1.4 解密](#14-解密)
  - [1.5 歌词](#15-歌词补充)
  - [1.6 系统媒体控件（SMTC）](#16-系统媒体控件smtc补充)
  - [1.7 调音（音频处理链）](#17-调音音频处理链补充)
  - [1.8 下载](#18-下载补充)
  - [1.9 缓存](#19-缓存补充)
  - [1.10 音乐导入](#110-音乐导入补充)
  - [1.11 歌单](#111-歌单补充)
  - [1.12 专辑](#112-专辑补充)
  - [1.13 MV](#113-mv补充)
  - [1.14 运行日志](#114-运行日志补充)
- [2. API 调用](#2-api-调用补充)
  - [2.1 调用形态](#21-调用形态)
  - [2.2 通用返回结构](#22-通用返回结构)
  - [2.3 接口一览](#23-接口一览)
  - [2.4 调用示例（草案）](#24-调用示例草案)
  - [2.5 搜索 API（search）](#25-搜索-apisearch补充)
  - [2.6 request API（多源请求）](#26-request-api多源请求补充)
  - [2.7 多源聚合与折叠显示](#27-多源聚合与折叠显示补充)
  - [2.8 歌词 API（lyric）](#28-歌词-apilyric补充)
  - [2.9 调音 API（eq / dsp）](#29-调音-apieq--dsp补充)
  - [2.10 下载 API（download）](#210-下载-apidownload补充)
  - [2.11 缓存 API（cache）](#211-缓存-apicache补充)
  - [2.12 导入 API（import）](#212-导入-apiimport补充)
  - [2.13 音质 API（quality）](#213-音质-apiquality补充)
  - [2.14 歌单 API（playlist）](#214-歌单-apiplaylist补充)
  - [2.15 专辑 API（album）](#215-专辑-apialbum补充)
  - [2.16 MV API（mv）](#216-mv-apimv补充)
  - [2.17 日志 API（logs）](#217-日志-apilogs补充)
- [3. 错误码](#3-错误码补充)
- [4. 文件与目录约定](#4-文件与目录约定补充)
- [5. Function 插件标准](#5-function-插件标准补充)
  - [5.1 插件分类](#51-插件分类补充)
  - [5.2 播放源类插件接口](#52-播放源类插件接口补充)
  - [5.3 其他类（有界面）插件接口](#53-其他类有界面插件接口补充)
  - [5.4 插件清单 plugin.json](#54-插件清单-pluginjson补充)
  - [5.5 加载、权限与沙箱](#55-加载权限与沙箱补充)
- [6. 版本与兼容性](#6-版本与兼容性补充)
- [7. 待补充 / 待定事项](#7-待补充--待定事项)
- [8. 变更记录](#8-变更记录)

---

## 0. 阅读指引 ##

**补充**

| 项目 | 说明 |
| --- | --- |
| 文档目的 | 统一 Rad music 的曲目元数据、加载/播放/解密流程、错误约定与插件接口 |
| 目标读者 | 播放器/客户端开发者、平台插件作者、本地库调用方 |
| 适用范围 | 本地曲库（`local`）与各平台适配曲目（`netease`、`radish` 等） |
| 约定 | 字段名统一小写下划线；时间戳为 Unix 秒；`sha` 为小写十六进制 SHA-256；JSON 文件统一 UTF-8 无 BOM |

术语：

- **platform**：曲目来源平台标识。
- **base.json**：单曲元数据定义文件。**存放在缓存目录内**，按「一首歌一个文件夹 → 每个音质一个文件夹」组织：`cache/meta/<song_key>/<quality_key>/base.json`（防止不同音质互相覆写）。属于可重建数据——音频本体与标签才是持久真相（见 1.1「存放位置」、1.9、1.10）。
- **sec.json**：加密曲目的密钥/校验信息文件。
- **usec.json**：用户输入的密钥（会话）信息文件。
- **plugin**：某个 platform 的适配实现，负责把平台曲目转换为统一的 `base.json` 语义。
- **Function 插件**：能力扩展单元，**分为播放源类和其他类**——播放源类**无界面**、通过提供 `platform` 支持与 `request` 能力来扩展音源；其他类**有界面**、提供与音源无关的功能。详见第 5 节。
- **播放源 / 音源**：一个 `platform` 提供曲目的能力，通常由播放源类插件实现。
- **折叠**：同一首歌存在多个播放源时，聚合为一条记录展示，展开后可查看/切换各源。
- **歌词（LRC）**：与曲目同步的时间轴文本，支持标准/增强（逐字）LRC、翻译与罗马音。详见 1.5。
- **SMTC**：System Media Transport Controls，系统媒体控件（系统媒体面板、多媒体键、锁屏控制）。功能必须支持，详见 1.6。
- **调音 / DSP**：播放输出上的实时音频处理链（均衡器、增益、音效节点），由宿主统一实现、其他类插件提供节点。详见 1.7。
- **下载**：把在线曲目**永久**保存进本地曲库，与缓存不同（缓存可随时清理）。详见 1.8。
- **缓存**：加速加载/播放的临时数据（音频、歌词、封面、元数据），可随时清理且不影响曲库完整性。详见 1.9。
- **导入**：从本地音乐文件解析标签、计算 `sha` 并生成 `base.json` 纳入曲库；**必须保证 `base.json` 之间不冲突**，详见 1.10。
- **`song_key` / `quality_key`**：`base.json` 在缓存内的两级目录名——`song_key` 标识一首歌（由 `group_key` 派生），`quality_key` 标识音质（`<platform>.<quality_id>` 或 `local.<level>-<codec>-<bitrate>`）；路径即唯一键，用于**防止不同音质互相覆写**（见 1.1「存放位置」）。
- **库索引**：唯一键（`sha` / `platform:id` / `location` / `group_key`+音质）到曲目目录的映射，用于冲突检测与快速查询；索引与磁盘不一致时以磁盘为准。
- **音质（quality）**：曲目的编码与规格描述（编码、码率、采样率、位深、是否无损）加上归一化等级（`hires`/`lossless`/`high`/`medium`/`low`）。定义见 1.1.1，平台音质由播放源插件声明（5.4）。
- **同曲同质 / 同曲不同质**：同一首歌（同一 `group_key`）下，同一音质只能有一条记录；**不同音质必须各自独立存储**（重复存储是预期行为）。

---

## 1. 基本功能 ##

### 1.1 基本信息 ###

#### base.json ####

*原文*

* 以下为功能定义
* platform 平台 如 netease(网易云)，radish(本平台)，local(本地)
* version 音乐版本
* location 文件地址
* sha 音乐的文件信息（使用sha256校验）
* name 音乐名
* author 作者
* pic 音乐封面地址
* secret 是否加密

**补充：存放位置**

`base.json` **存放在缓存内**，不放在音频目录，且按「**一首歌一个文件夹 → 每个音质一个文件夹**」分层：

```text
cache/meta/
├─ .staging/<song_key>/<quality_key>/base.json.tmp   # 原子提交暂存
└─ <song_key>/                    # ① 一首歌一个文件夹（歌曲级目录）
   ├─ <quality_key>/              # ② 该歌曲的每个音质一个文件夹（音质级目录）
   │  └─ base.json                # ③ 该音质的元数据（一条记录 = 一个文件）
   ├─ <quality_key>/base.json
   └─ ...
```

| 层级 | 目录名 | 派生方式（必须可逆校验） |
| --- | --- | --- |
| 歌曲级 | `song_key` | 由 2.7 的 `group_key` 派生：`<安全化 title - artist>_<group_key 的 sha256 前 12 位>`（不含时长，保证跨来源稳定） |
| 音质级 | `quality_key` | 在线：`<platform>.<quality_id>`（如 `netease.lossless`）；本地/已下载：`local.<level>-<codec>-<bitrate>`（如 `local.lossless-flac-1411`） |
| 记录 | `base.json` | 该 (歌曲, 音质) 组合下的**唯一**元数据文件 |

**这样分层的目的就是防覆写**：

1. **路径即唯一键**：`song_key` + `quality_key` 唯一确定一个 `base.json`；不同音质天然落在不同目录，**结构上不可能互相覆写**。
2. **同目录内才可能冲突**：只有当"同一首歌 + 同一音质"（即同一唯一键）时才会命中同一文件；此时必须**读-比较-再写**——内容一致则 `skip`/`merge`，不一致则报 `1023 base conflict` 并**默认拒绝写入**，绝不静默覆盖。
3. **平台隔离**：`quality_key` 含 `platform`，不同平台同名音质（如 `netease.lossless` 与 `radish.lossless`）必然不同目录，不会互相覆写。
4. **原子性**：先写同目录的 `base.json.tmp`，校验通过后**同卷文件级原子重命名**覆盖 `base.json`，杜绝半个文件与目录级半成品。
5. **一致性校验**：目录名必须能由 `base.json` 内容重新推导（`song_key` ← `group_key`、`quality_key` ← `platform` + `quality`）；推导不出或不一致即视为损坏条目，拒绝加载并标记重建（`1029 meta path mismatch`）。

- 音频文件、`sec.json`、歌词等**持久资源**留在曲库目录（`tracks/<sha>/`）；`base.json` 属于**元数据缓存**，随缓存清理而消失。
- **必须可重建**：`base.json` 丢失或失效时，本地曲目通过重新解析标签 + 重算 `sha` 重建（1.10），在线曲目通过重新拉取重建（2.6）；重建必须落在**同一路径**（同一 `song_key`/`quality_key`），不得产生第二份记录。
- 文件名安全化：去除 `\ / : * ? " < > |` 与控制字符、去掉结尾的点与空格、规避 Windows 保留名（`CON`/`PRN`/`AUX`/`NUL`/`COM1`…）、整条路径不超过 240 字符（超长时截断 slug，靠 hash 保持唯一）。
- `song_key` 是否引入时长/专辑以区分同名不同曲：待定。

**补充：字段规范**

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `platform` | string | 是 | `netease` / `radish` / `local`，或插件自定义标识；未知标识报 `platform error` |
| `version` | string | 是 | 音乐版本，与 `sha` 组合用于一致性校验 |
| `location` | string | 是 | 文件地址：相对 `base.json` 的路径、绝对路径或 `http(s)` URL |
| `sha` | string | 是 | 文件 SHA-256（小写十六进制，64 字符） |
| `name` | string | 是 | 音乐名 |
| `author` | string | 否 | 作者 / 歌手 |
| `pic` | string | 否 | 封面地址，缺省时使用默认封面 |
| `secret` | int | 是 | `1` 表示加密，需密码；`0` 表示明文 |
| `quality` | object | 否* | **音质定义**（见 1.1.1）；旧 `base.json` 无此字段时按 `unknown` 处理并依据文件实际参数推断 |

\* 新增实现**应当**写入 `quality`；缺失不视为非法（兼容旧文件）。

#### 1.1.1 音质定义（quality）（补充） ####

`base.json` 内的 `quality` 是该音频文件的**归一化音质描述**，同时也保留来源平台的音质标识：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `level` | string | 是 | 归一化音质等级（见下方枚举），用于跨平台比较与排序 |
| `codec` | string | 是 | 编码：`flac` / `alac` / `mp3` / `aac` / `opus` / `vorbis` / `pcm` 等 |
| `lossless` | bool | 是 | 是否无损 |
| `bitrate` | int | 否 | 码率 kbps；VBR 或无损可为空/填名义值 |
| `sample_rate` | int | 否 | 采样率 Hz |
| `bit_depth` | int | 否 | 位深（无损） |
| `channels` | int | 否 | 声道数 |
| `quality_id` | string | 否 | **来源平台**的音质标识（如 `netease` 的 `lossless` / `hires`）；`local` 可省略 |
| `label` | string | 否 | 展示名（如「无损」「Hi-Res」），来自播放源插件的音质定义 |

**归一化等级枚举**（顺序即比较顺序，由高到低）：

| `level` | 语义 |
| --- | --- |
| `hires` | 无损且 `sample_rate > 48 kHz` 或 `bit_depth > 16`（Hi-Res） |
| `lossless` | 无损，CD 级（44.1/48 kHz、16 bit） |
| `high` | 有损，≥ 256 kbps（含 320k） |
| `medium` | 有损，160–255 kbps（含 192k） |
| `low` | 有损，< 160 kbps（含 128k） |
| `unknown` | 旧数据或无法推断 |

**比较与排序**：先比 `level`，同级依次比 `sample_rate`、`bit_depth`、`bitrate`，最后比 `codec`（无损优先）。VBR 与平台口径差异的处理：待定。

**与平台的关系**：`level` 由宿主按上表归一化；`quality_id` / `label` 由**播放源插件**声明（见 5.2、5.4），宿主不得自行编造平台音质标识。

**补充：示例**

```json
{
  "platform": "local",
  "version": "1.0.0",
  "location": "tracks/example.flac",
  "sha": "9f2c1e0b7a4d5c6f8e0a1b2c3d4e5f60718293a4b5c6d7e8f9012345678abcd",
  "name": "Example Song",
  "author": "Example Artist",
  "pic": "covers/example.jpg",
  "secret": 0,
  "quality": {
    "level": "lossless",
    "codec": "flac",
    "lossless": true,
    "bitrate": 1411,
    "sample_rate": 44100,
    "bit_depth": 16,
    "channels": 2,
    "quality_id": "lossless",
    "label": "无损"
  }
}
```

**补充：校验规则**

1. `sha` 必须与 `location` 指向文件的实际 SHA-256 一致。
2. `version` + 实际文件 `sha` 必须与 `base.json` 中的组合一致，否则触发 `sha error`（见 1.3）。
3. 字段缺失、类型错误或 JSON 解析失败时视为无效元数据，禁止进入加载流程。
4. **唯一性**：`sha`、`platform + id`、`location`（规范化后）在曲库内必须唯一，**任何两个 `base.json` 都不得互相冲突**；唯一性规则与冲突处理见 [1.10](#110-音乐导入补充)。
5. **音质一致**：`quality` 与实际文件参数不符时，以文件实际值修正并记录警告；`quality_id` 必须能在对应平台的音质定义中找到，否则报 `1027 quality undefined`。
6. **不同音质各自成条**：同一首歌的不同音质 `sha` 不同，必须**各自独立存储**（独立记录、独立目录），互不覆盖、互不合并（见 1.8 / 1.10）。

### 1.2 platform 与插件适配 ###

**补充**

- `local`：直接读取 `location`，不经过网络与插件。
- 其他取值：按 `plugins/source/<plugin-id>/` 查找**播放源类** Function 插件实现；插件不存在、被禁用或加载失败时报 `platform error`。
- 插件职责：解析平台曲目标识 → 生成统一的 `base.json` 语义（含 `sha`、`version`、`pic`、`quality`）→ 提供或代理 `location` 的可读数据流 → 对外暴露 `request` 能力（见 2.6）。
- **音质定义由播放源插件提供**：每个平台可用的音质等级、标识与展示名（`quality_id` / `label` / `level` / 编码参数）由该平台的播放源插件声明（见 5.2、5.4）；宿主只负责归一化与比较（见 1.1.1），不得自行编造平台音质。
- 同一首歌在不同平台、不同音质下的 `location` 与 `sha` 均不同，必须视为**不同记录**分别存储（见 1.10）。
- 插件加载顺序（待定）：内置注册表 → `plugins/` 分类扫描 → 远程插件清单。
- 只有**播放源类**插件可以声明 `platform`；其他类插件（有界面）不得提供 platform 支持。

### 1.3 播放和加载音乐 ###

#### 加载 load.cpp ####

*原文*

* a.检查platform （非local则检测是否有插件定义及适配对应platform，没有则报错"platform error"
* b.检查本地文件的sha,若version+音乐文件的sha ！=sha+version则警告"sha error"
* c.secret=1 则提示加密音乐输入密码并且加载resecrect.cpp解密
* d.若全部通过则将文件加载至缓存并等待调用播放

**补充：加载状态机**

```text
idle ──load()──> resolving_base ──> checking_platform ──> verifying_sha ──> [secret=1] asking_password ──> decrypting
                        │                  │                     │                     │                        │
                   base 缺失则重建      platform error ──────────┴── sha error ─────────┴── usec error ──────────┘
                                                                  │
                                                   loaded  <───────┘（校验通过并写入缓存）
```

**补充：流程伪代码**

```cpp
// load.cpp（草案）
Result load(std::string key) {                   // key = sha（本地/已下载）或 online:<platform>:<id>
    // base.json 在缓存内（1.1「存放位置」）：按 song_key/quality_key 定位，缺失或失效则先重建
    auto base = MetaCache::Get(key);             // 命中 cache/meta/<song_key>/<quality_key>/base.json
    if (!base) {
        auto r = RebuildBase(key);               // 本地：重解析标签+重算 sha；在线：重新拉取（1.10 / 2.6）
        if (!r.ok()) return r;                   // 1028 base rebuild failed
        base = r.value;
    }

    if (base->platform != "local" && !PluginRegistry::has(base->platform))
        return Err(PlatformError);              // a. platform error

    if (!FileExists(base->location))
        return Err(FileNotFound);               // 补充：文件缺失

    if (!VerifySha(base->location, base->sha, base->version))
        Warn(ShaError);                         // b. sha error（原文为“警告”，是否阻断见待定项）

    std::string path = base->location;
    if (base->secret == 1) {                    // c. 加密曲目
        auto key2 = PromptPassword();           //    提示输入密码
        auto r = resecrect(path, key2);         //    调用解密（usec error 由该模块返回）
        if (!r.ok()) return r;
        path = r.decrypted_path;
    }

    Cache::Put(base.sha, path);                 // d. 写入缓存，等待播放
    return Ok();
}
```

**补充：约束**

1. 未加载成功时调用播放接口，返回 `not loaded`，不得隐式触发加载（是否允许自动加载：待定）。
2. `sha error` 为警告而非阻断（依原文），但必须在日志中记录，且不得覆盖缓存中的同名条目。
3. 解密结果只在内存/受控缓存中保留，不写回用户原始目录（待定）。
4. 缓存写入、淘汰与清理遵循 [1.9 缓存](#19-缓存补充)；需要长期保存时走下载（[1.8](#18-下载补充)）。
5. **`base.json` 来自缓存**：加载前若缓存中不存在或已失效，必须先按 `song_key`/`quality_key` 重建（本地重解析 / 在线重拉取），重建失败返回 `1028 base rebuild failed`；路径与内容不一致的条目视为损坏（`1029`），必须重建而非直接使用；重建过程不得阻塞已有播放。

#### 播放 start.cpp ####

**补充（原文仅列出文件，未定义内容）**

播放控制接口：

| 接口 | 参数 | 说明 | 失败返回 |
| --- | --- | --- | --- |
| `start()` | 无（或曲目 `sha`） | 开始播放已加载曲目 | `not loaded` |
| `pause()` | 无 | 暂停，保留播放位置 | `not playing` |
| `resume()` | 无 | 从暂停位置继续 | `not paused` |
| `stop()` | 无 | 停止并释放播放资源 | — |
| `seek(sec)` | 位置（秒，浮点） | 跳转到指定位置，越界则钳制到 [0, duration] | `not loaded` |
| `volume(level)` | 0.0 – 1.0 | 设置音量 | `invalid argument` |
| `status()` | 无 | 返回播放状态与进度 | — |

播放状态：`stopped` → `playing` ⇄ `paused` → `ended`；缓冲区不足时为 `buffering`。

**补充：事件回调**

`on_start`、`on_progress(position, duration)`、`on_pause`、`on_resume`、`on_end`、`on_error(code, message)`。

- 播放触发后必须同步：歌词按 [1.5](#15-歌词补充) 解析并跟随进度高亮；状态、进度与元数据必须同步到系统媒体控件（[1.6 SMTC](#16-系统媒体控件smtc补充)）。

### 1.4 解密 ###

#### resecrect.cpp ####

*原文*

* 从usec.json读取用户输入的密钥，并计算md5值是否与sec.json相等，否则返回报错"usec error"
* 若密钥相等则使用密钥解密（未规划详细步骤，待补充）

**补充：usec.json 结构（草案）**

```json
{
  "usec": "<用户输入的密钥，或指向会话密钥的引用>",
  "updated_at": 1735689600
}
```

- 是否落盘明文密钥：待定（建议仅保存会话内引用，进程退出即失效）。
- 文件权限建议：仅当前用户可读写（Windows ACL / `chmod 600`）。

**补充：sec.json 结构（草案）**

```json
{
  "alg": "AES-256-GCM",
  "kdf": "PBKDF2-HMAC-SHA256",
  "salt": "<base64>",
  "iter": 200000,
  "verifier": "<密钥校验值>",
  "nonce": "<base64>",
  "sha_plain": "<解密后明文的 sha256>"
}
```

**补充：解密流程（草案，替代原文“待补充”）**

1. 读取 `usec.json` 中的用户密钥；文件不存在或为空则返回 `usec error`。
2. 计算密钥校验值并与 `sec.json` 的 `verifier` 比较；不相等返回 `usec error`。
3. 校验通过后按 `kdf` 参数派生解密密钥（`salt` + `iter`）。
4. 使用 `alg` 指定的算法解密 `location` 指向的文件，校验认证标签。
5. 计算明文 SHA-256 并与 `sha_plain` 比较，一致才写入缓存并返回成功。
6. 任一步骤失败：返回对应对错误码，清理临时明文，不得留下可读副本。

**补充：安全建议**

- 原文使用 MD5 比较密钥：MD5 不适合口令校验（无盐、速度过快）。建议新版本改用 PBKDF2 / Argon2id 派生 + 常量时间比较；仅为兼容旧曲目保留 MD5 分支，并加盐。
- 密文算法建议 AES-256-GCM（或 XChaCha20-Poly1305），禁止使用 ECB；每个文件独立随机 `nonce`。
- 解密后的明文建议只存在于内存，或写入系统临时目录的加密/受限路径，退出时清理。

### 1.5 歌词（补充） ###

#### 来源优先级 ####

1. **本地歌词**：`tracks/<sha>/lyric.lrc`，或与音频同目录同名的 `<audio 同名>.lrc`。
2. **播放源插件**：`request(source, op = "get_lyric")`（如 `netease`、`radish`）。
3. 用户手动导入的歌词（写入本地歌词目录，覆盖以上结果）。

#### 歌词文件约定 ####

| 项目 | 约定 |
| --- | --- |
| 格式 | 标准 LRC（`[mm:ss.xx]`）、增强 LRC（逐字 `<mm:ss.xx>`）、纯文本（无时间轴） |
| 编码 | UTF-8 无 BOM；检测到 GBK/GB18030 时应转码（兼容读取：待定） |
| 文件名 | `lyric.lrc`（原文）、`lyric.tr.lrc`（翻译）、`lyric.roma.lrc`（罗马音） |
| 标签 | 可选 `[ti:]` `[ar:]` `[al:]` `[offset:]`；`offset` 单位毫秒，正值表示歌词提前 |
| 多语言 | 同一时间戳多行，或使用 `tr` / `roma` 独立文件；两条轨道之间用行时间戳对齐 |

#### lyric.json（归一化结果，草案） ####

```json
{
  "sha": "9f2c…abcd",
  "source": "radish",
  "format": "lrc",
  "timed": true,
  "offset_ms": 0,
  "lines": [
    {
      "time_ms": 12340,
      "text": "Example line",
      "tr": "示例行",
      "roma": "Example line",
      "words": [{ "t": 12340, "text": "Exa" }, { "t": 12580, "text": "mple" }]
    }
  ]
}
```

- `time_ms`：相对歌曲开始的毫秒；`words` 仅逐字歌词存在。
- `timed = false` 表示无时间轴歌词，只做滚动展示，不做高亮同步。

#### 加载与同步规则 ####

1. **加载时机**：`load.cpp` 加载曲目时按来源优先级解析歌词并缓存（`cache/lyric/<sha>.json`）；也可在 `start` 前懒加载，但不得阻塞播放起播。
2. **失败降级**：歌词获取失败**不得阻断播放**，降级为无歌词并记录警告（`1015 lyric unavailable`，非致命）。
3. **加密曲目**：`secret = 1` 时，歌词视为曲目附属资源，随曲目一起解密后再解析（歌词是否单独加密：待定）。
4. **同步**：`start.cpp` 按当前 `position` 定位当前行（对 `lines` 二分查找）；`offset_ms` 为用户可调偏移，正值表示歌词提前。
5. **逐字**：存在 `words` 时按字高亮，否则整行高亮。
6. **切源**：在折叠条目内切换音源时，若新源歌词时长与曲目时长差异超过容差（±3 秒），需重新拉取歌词并重置偏移。

### 1.6 系统媒体控件（SMTC）（补充） ###

**目标**：功能**必须支持 SMTC**（System Media Transport Controls，Windows 系统媒体控件），使系统媒体面板（音量弹窗 / 快捷设置）、键盘多媒体键、锁屏与耳机按键能够显示当前歌曲并控制播放。

**必须支持的能力**

| 能力 | 说明 |
| --- | --- |
| 会话注册 | 注册系统媒体会话（WinRT `SystemMediaTransportControls`；其他平台对应等价物） |
| 元数据同步 | `title`（歌名）、`artist`（作者）、`album`、`duration`、封面（`pic`）；封面需转为平台可接受格式与尺寸 |
| 播放状态同步 | `playing` / `paused` / `stopped`；`buffering` 的映射：待定 |
| 时间轴同步 | 上报 `position` / `duration`，并声明是否支持 `seek` |
| 控制响应 | `play` / `pause` / `stop` / `next` / `previous` / `seek` / `rate`，回调到播放器并返回执行结果 |
| 按钮可用性 | 无播放列表时禁用 `next` / `previous`；无曲目时禁用全部传输控件 |

**同步规则**

1. `start` 成功后立即更新元数据与状态；曲目切换、**折叠条目内切换音源**时必须重新更新（歌名相同也要刷新封面与时长）。
2. 状态变化即时上报；进度按固定间隔上报（建议 ≤ 1 秒，避免系统限流）。
3. 线程约束：SMTC 调用必须在 UI/主线程执行，其余线程通过宿主事件队列投递。
4. 失败降级：SMTC 注册/更新失败**不得影响播放**，记录警告（`1016 smtc unavailable`）并可重试注册。
5. 系统发起的 `seek` 必须真实作用于播放位置，并回传新的 `position`（避免控件跳动）。

**与插件的关系**

- 播放源类插件**不得**直接接管 SMTC 会话；SMTC 由宿主统一实现，保证元数据结构一致。
- 其他类（有界面）插件若需扩展系统集成（桌面歌词、托盘、悬浮窗），只能通过宿主 API 订阅播放事件与元数据，禁止自行创建第二个媒体会话。
- 歌词**不**通过 SMTC 展示（系统不支持）；歌词由播放器界面或桌面歌词类插件渲染。

### 1.7 调音（音频处理链）（补充） ###

**定位**：调音（EQ / 音效）属于**其他类 Function 插件（有界面）**的能力扩展。宿主提供统一的 DSP 处理链，插件只声明处理节点与参数，不直接接触音频输出设备。

**处理链顺序（草案）**

```text
解码 → 增益 / 响度标准化 → 均衡器（EQ） → 音效节点（其他类插件） → 限幅 / 防削波 → 输出设备
```

（节点顺序是否可配置：待定）

**支持的调音类型**

| 类型 | 说明 | 主要参数 |
| --- | --- | --- |
| 图形均衡器 | 10 / 15 / 31 段 | 每段增益 dB（建议 −12 … +12） |
| 参数均衡器 | 自定义频点 | `freq`(Hz)、`gain`(dB)、`Q` |
| 预设 | 内置 + 用户预设 | 名称、曲线、是否随设备/曲目自动切换 |
| 增益 / 响度 | ReplayGain、响度标准化 | `track` / `album` 模式、目标响度 LUFS |
| 音效节点 | 由其他类插件提供 | 插件自定义（低音增强、环绕、混响等） |
| 输出 | 输出设备、独占模式、采样率 | 待定 |

**规则**

1. **实时生效**：参数变更应在 ≤ 100 ms 内作用于播放，且不得产生爆音或中断。
2. **失败旁路**：DSP 或某个节点初始化失败时，必须自动旁路该节点并继续播放（`1017 dsp init failed`，**非致命**）。
3. **持久化**：预设保存为 `presets/eq/<name>.json`（按用户维度）；设备级预设切换：待定。
4. **不改变原文件**：调音只作用于播放输出，不影响文件内容与校验值（导出处理后音频：待定）。
5. **音频线程约束**：DSP 节点必须声明 `permissions: ["audio_dsp"]`，且禁止在音频回调中做 I/O、加锁或分配大内存。
6. 与 SMTC（1.6）无关：调音不改变上报的元数据与进度。

### 1.8 下载（补充） ###

**定位**：下载把在线播放源曲目**永久**保存进本地曲库；与[缓存](#19-缓存补充)不同——缓存随时可清理，下载文件长期保留。

**下载任务字段**

| 字段 | 说明 |
| --- | --- |
| `id` | 任务 id |
| `source` / `item_id` | 音源与曲目标识；折叠条目（2.7）**必须显式指定音源**，不允许隐式选源 |
| `quality` | 目标音质：`auto`（默认，取该源最高可用）/ 归一化 `level`（`hires`/`lossless`/`high`/`medium`/`low`）/ 平台音质 `quality_id`（如 `netease:lossless`）；**不同音质重复存储**，见下方规则 |
| `target_dir` | 目标目录（默认按曲库规则生成） |
| `filename` | 命名模板，如 `{artist} - {title}`，可含 `{quality}`（默认模板：待定） |
| `state` | `queued` / `running` / `paused` / `done` / `failed` / `canceled` |
| `progress` | 已下载 / 总字节、速度、剩余时间 |
| `sha` | 下载完成后计算，用于校验与入库 |
| `quality_resolved` | 实际落盘音质（按 1.1.1 归一化；可能与请求不同，源降级时必须回传真实值） |

**流程**

1. 取下载地址：`request(source, op = "get_url", params.quality)`（可能带鉴权与时效，见 2.6）。
2. 下载到 `cache/tmp/<task-id>.part`，支持暂停/恢复与分片续传（是否强制支持断点续传：待定）。
3. 完成后计算 SHA-256 并与源元数据校验；不一致返回 `1022 checksum mismatch`，重试或换源。
4. 原子移动到目标目录，**不覆盖**用户已有文件（同名策略：待定）。
5. 入库：生成 `base.json` **写入元数据缓存** `cache/meta/<song_key>/<quality_key>/base.json`（`platform` = 源、`location` = 本地路径、`sha` = 实际值、`secret = 0`、`quality` = 实际落盘音质，见 1.1.1 / 1.1「存放位置」）；不同音质写入不同 `quality_key` 目录，互不覆写。同时保存封面与歌词（1.5），刷新库索引。
6. 失败：保留 `.part` 供重试，任务标记 `failed` 并记录原因（残留清理：待定）。

**规则**

1. **不同音质重复存储**：同一首歌的每个音质**各生成独立记录与独立文件**（`sha` 不同 → 独立 `tracks/<sha>/`），**不得**覆盖、替换或"升级"已有音质；再下载一个音质等于多占一份磁盘，这是预期行为。
2. **同曲同质去重**：同一首歌 + 同一音质已存在时，默认 `skip` 并返回已有记录（`re-download` 覆盖策略：待定）。
3. 并发与限速：全局/单源并发上限可配置，超限**排队**而不是失败；宿主强制执行。
4. 磁盘不足：暂停任务并返回 `1019 insufficient space`，**不得**删除已有曲库文件或缓存以外内容。
5. 下载完成的曲目再次播放时走 `local` 分支（1.2），不再请求网络。
6. 需要登录态的源，下载前先 `check_login`；无权限时明确提示，不得静默失败。
7. 下载任务与缓存清理互不影响：清理缓存**不得**中断或删除已完成的下载。

### 1.9 缓存（补充） ###

**定位**：缓存用于加速加载与播放，**随时可被清理且不影响曲库完整性**；下载（1.8）内容不受缓存清理影响。

**缓存分层**

| 层 | 内容 | 位置 | 说明 |
| --- | --- | --- | --- |
| L1 内存 | 解码音频数据、当前元数据与歌词 | 进程内存 | 容量小，进程退出即失效 |
| L2 音频 | 加载/解密的音频数据、流媒体分片 | `cache/audio/<sha>.<ext>`、`cache/stream/` | 受容量上限约束 |
| L2 附属 | 歌词、封面、**`base.json` 元数据** | `cache/lyric/<sha>.json`、`cache/cover/<hash>.<ext>`、`cache/meta/<song_key>/<quality_key>/base.json` | 小文件；`meta` 层可重建（见下） |
| 临时 | 未完成任务与部分下载 | `cache/tmp/` | 启动时清理残留 |

**键规则**

- 音频缓存键：`sha`；无 `sha` 的在线曲目用 `platform:id`。
- **`base.json` 缓存键**：`<song_key>/<quality_key>`（歌曲级目录源自 `group_key`，音质级目录 = `<platform>.<quality_id>` 或 `local.<level>-<codec>-<bitrate>`），**分层的目的就是防覆写**。
- 封面缓存键：`pic` URL 的哈希（避免超长文件名与非法字符）。
- 歌词缓存键：`sha`（或 `platform:id`）。
- 缓存条目必须能用键反查元数据，禁止仅按文件名匹配。

**策略**

1. **容量上限**：按层分别配置（音频层显著大于附属层），默认值：待定。
2. **淘汰**：音频层用 LRU；附属层优先保留；**正在播放/正在下载的条目不得被淘汰**（标记 pending eviction，播放结束后再清理）。
3. **清理时机**：启动时清理 `cache/tmp` 残留；超限即淘汰；提供手动清理接口（2.11）。清缓存**不得**删除 `tracks/`、下载内容与用户预设。
4. **写入**：先写临时文件再原子重命名，避免半成品被当作有效缓存；写入前完成 sha 校验（1.3）。
5. **安全**：解密后的明文**不得**长期落盘，进程退出或超时后清理（策略：待定）；`cache/` 权限建议仅当前用户可读。
6. **一致性**：`base.json` 的 `version` 或 `sha` 变化后，对应缓存条目立即失效并重建。
7. **预读**（待定）：下一首/流媒体分片预取，不得抢占当前播放的 I/O 与带宽。
8. **`base.json` 属于缓存**：它位于 `cache/meta/<song_key>/<quality_key>/base.json`（一首歌一个文件夹、每个音质一个文件夹，见 1.1「存放位置」），**可被清理且必须能重建**：
   - 重建来源：本地曲目重新解析标签并重算 `sha`（1.10）；在线曲目重新拉取（2.6）；持久指针由库索引 `index/` 提供。
   - 清理 `meta` 层时，正在播放/播放列表引用的条目**先重建再清理**（或标记 `pending eviction`），不得让正在播放的曲目失去元数据。
   - 清理 `meta` 层**不影响**音频本体、`sec.json`、歌词（均在 `tracks/`）与下载内容。
   - 重建必须落在**同一 `song_key`/`quality_key` 路径**（覆盖自身记录），**不得产生第二份 `base.json`**，也不得把不同音质写入同一目录。

### 1.10 音乐导入（补充） ###

**目标**：从**本地音乐文件**解析元数据、生成 `base.json` 并纳入曲库；导入必须保证 **`base.json` 之间不互相冲突**（唯一性规则见下文）。

**导入流程**

1. **扫描**：枚举候选文件（`mp3` / `flac` / `m4a` / `aac` / `wav` / `ape` / `ogg` / `opus` 等），跳过不支持格式（`1025`）与已入库内容。
2. **解析**：
   - 优先读取内嵌标签（MP3 ID3v2、FLAC Vorbis Comment、M4A/MP4 atoms、APE、WAV INFO）；标签编码需检测（UTF-8 / UTF-16 / GBK，见待定项）。
   - 读取音频流的**音质参数**（编码、码率、采样率、位深、声道、是否无损）并按 1.1.1 归一化为 `quality`（`level` / `codec` / `lossless` / `bitrate` / `sample_rate` / `bit_depth` / `channels`；`quality_id` 仅在线来源才有）。
   - 标签缺失或不完整时回退**文件名模板**（默认 `{artist} - {title}`，可配置），并标记 `tag_source = "filename"`（`1024` 为非致命提示）。
   - 提取内嵌封面 → `covers/<hash>.<ext>`；同目录同名 `.lrc` 按 1.5 作为歌词导入。
   - 流式计算 SHA-256（不得整文件读入内存）。
3. **冲突预检**：按下方唯一性规则检测冲突，生成冲突报告（**不写盘**）。
4. **生成 `base.json`**（草案）：

```json
{
  "platform": "local",
  "version": "1.0.0",
  "location": "tracks/9f2c1e0b7a4d5c6f8e0a1b2c3d4e5f60718293a4b5c6d7e8f9012345678abcd/example.flac",
  "sha": "9f2c1e0b7a4d5c6f8e0a1b2c3d4e5f60718293a4b5c6d7e8f9012345678abcd",
  "name": "Example Song",
  "author": "Example Artist",
  "pic": "covers/1a2b3c4d.jpg",
  "secret": 0,
  "album": "Example Album",
  "duration": 215,
  "track_no": 3,
  "tag_source": "id3v2",
  "quality": {
    "level": "lossless",
    "codec": "flac",
    "lossless": true,
    "bitrate": 1411,
    "sample_rate": 44100,
    "bit_depth": 16,
    "channels": 2,
    "quality_id": "lossless",
    "label": "无损"
  }
}
```

`album` / `duration` / `track_no` / `tag_source` 为导入扩展字段，**必须可选**（旧实现忽略，见第 6 节兼容性）。

5. **落盘**：音频（或引用）、封面、歌词写入 `tracks/<sha>/` 等持久位置；`base.json` 写入**缓存** `cache/meta/<song_key>/<quality_key>/base.json`（见 1.1「存放位置」）；随后更新库索引。
6. **报告**：返回成功 / 跳过 / 失败数量与冲突明细。

**导入模式**

| 模式 | 行为 | 说明 |
| --- | --- | --- |
| `copy`（建议默认） | 复制文件到 `tracks/<sha>/` | 原文件保留，最安全 |
| `move` | 移动到 `tracks/<sha>/` | 节省空间，需用户确认 |
| `link`（原地引用） | 不复制，`location` 指向原路径 | 原路径移动/删除后即失效；是否允许：待定 |

**`base.json` 唯一性（防冲突）规则**

| 唯一键 | 范围 | 约束 |
| --- | --- | --- |
| `sha` | 全局（本地曲目） | 内容哈希唯一：同一文件内容只能存在一条 `base.json` |
| `platform` + `id` | 全局（在线曲目） | 同一平台同一曲目 id 只能存在一条 |
| `location`（规范化后） | 全局 | 禁止两个 `base.json` 指向同一物理文件 |
| `version` + `sha` | 单条记录内 | 必须与文件实际值一致（1.3 校验） |
| 目录名 = 内容 | 单条记录内 | `cache/meta/<song_key>/<quality_key>/base.json` 必须能由内容反推：`song_key` ← `group_key`，`quality_key` ← `platform` + `quality`；不符即损坏条目（`1029`） |
| `group_key` + 音质 | 歌曲维度 | 同一首歌（2.7 的 `group_key`）的**同一音质**只能有一条记录；**不同音质不受此约束**（落在不同 `quality_key` 目录） |

**冲突类型与处理**

| 冲突 | 判定 | 默认处理 |
| --- | --- | --- |
| 重复导入（同内容） | `sha` 已存在 | `skip`：返回已有条目，不重复建目录 |
| 同曲同质重复 | `group_key` + 音质相同，但 `sha` 不同（如重新压制/不同码率同档） | 同一 `quality_key` 目录内：`base conflict`（`1023`），保留既有，按 `conflict` 策略 merge / overwrite，**默认拒绝覆写** |
| **同曲不同音质** | `group_key` 相同、音质不同 | **不是冲突**：落在不同 `quality_key` 目录，各自独立存储（重复存储） |
| 同一文件多条元数据 | `location` 相同但 `sha`/标签不同 | `base conflict`（`1023`）：保留既有，按 `conflict` 策略 merge / overwrite |
| 同内容不同标签 | `sha` 相同、标签不同 | `merge`：保留首条并补全缺失字段，不新建目录 |
| 在线与本地重复 | `platform:id` 相同 | 只保留一条；本地文件存在时优先 `local` |
| 并发写入 | 多进程/多线程同时导入 | 库写锁（`library.lock`）串行化提交；取锁失败返回 `1026 library locked` |

**规则**

1. **一处一曲**：任何两个 `base.json` 不得共享同一 `sha`、同一 `location` 或同一 `platform:id`。
2. **不同音质各自独立**：同一首歌的不同音质 `sha` 不同，**必须各自建条、各自建目录、重复存储**，不得互相覆盖、替换或合并为一条。
3. **禁止跨内容覆盖**：`conflict = overwrite` **仅允许覆盖同一 `sha`（同内容）**或同曲同质的条目，绝不允许用不同内容的记录覆盖既有曲目。
4. **原子提交**：先写 `cache/meta/.staging/<song_key>/<quality_key>/base.json.tmp`，校验通过后**同卷原子重命名**为 `base.json`；音频等持久资源的暂存同样用 `tracks/.staging/`。崩溃不得留下半个 `base.json`。
5. **索引一致**：库索引以唯一键为主键；索引与磁盘不一致时**以磁盘为准**并重建索引。
6. **非破坏性**：导入失败或冲突不得删除、移动用户原有文件（`move` 模式仅在成功后删除源文件）。
7. **参与折叠**：导入曲目的 `platform = "local"`，与在线音源按 2.7 规则折叠；折叠**不合并存储**——每个音质仍是独立记录。
8. **可重复执行（幂等）**：导入是"生成/刷新元数据缓存"的过程，`base.json` 被清理后**重新导入同一目录必须得到相同 `song_key` / `quality_key` 与相同结果**，不得产生第二条记录。
9. **防覆写**：写入前必须检查目标 `cache/meta/<song_key>/<quality_key>/base.json` 是否已存在；存在且内容不一致时**默认拒绝写入**并报 `1023`，只有同 `sha`（同内容）或显式 `conflict = overwrite` 才允许覆盖同一音质目录。

### 1.11 歌单（补充） ###

**定位**：歌单是**用户数据**（与缓存相反，不可被清理），持久保存在 `library/playlists/<id>.json`；
在线歌单（`platform != "local"`）只保存引用与冗余展示字段，可随时用 `source_id` 重新同步。

**结构（playlist.json）**

```json
{
  "id": "pl_0001",
  "name": "我的收藏",
  "description": "",
  "pic": "covers/x.jpg",
  "owner": "user",
  "platform": "local",
  "source_id": "",
  "created_at": 1735689600,
  "updated_at": 1735689600,
  "tracks": [
    {
      "platform": "local",
      "item_id": "9f2c…abcd",
      "sha": "9f2c…abcd",
      "name": "Example Song",
      "author": "Example Artist",
      "duration_seconds": 215,
      "group_key": "example song|example artist|215",
      "added_at": 1735689600
    }
  ]
}
```

**规则**

1. **只存引用**：曲目不复制音频、不重复保存元数据；`platform` + `item_id`（本地为 `sha`）为唯一标识。
2. **去重**：同一 `platform + item_id` 在同一歌单中**只能出现一次**，重复添加为幂等（不报错、不新增）。
3. **排序**：`tracks` 顺序即播放顺序，可拖拽调整（保序写入）；列表按 `updated_at` 降序返回。
4. **ID**：用户歌单 `pl_0001` 递增；在线歌单用 `<platform>_<source_id>`（同一来源重复同步得到同一 id，幂等）。
5. **原子写**：先写 `<id>.json.tmp` 再替换，崩溃不留半个文件；单条损坏不影响其他歌单（列表跳过并记日志）。
6. **与折叠的关系**：歌单条目保留 `group_key`，播放时仍按 2.7 折叠选择音源；切换音源不改变歌单条目。

### 1.12 专辑（补充） ###

**定位**：专辑是"按发行单位组织的曲目集合"，持久保存在 `library/albums/<id>.json`。
与歌单的区别：专辑有 `artist` / `year` / 曲序（`tracks` 顺序 = 曲目序号），且通常由**播放源插件**提供（`get_album`）。

```json
{
  "id": "netease_12345",
  "name": "Example Album",
  "artist": "Example Artist",
  "pic": "covers/a.jpg",
  "description": "",
  "platform": "netease",
  "source_id": "12345",
  "year": 2024,
  "created_at": 1735689600,
  "updated_at": 1735689600,
  "tracks": [ { "platform": "netease", "item_id": "2002", "name": "Track 1", "duration_seconds": 215 } ]
}
```

**规则**：与歌单一致（引用、去重、保序、原子写、幂等同步）；`year` 未知填 `0`；专辑封面缺失时用第一首曲目的 `pic`。

### 1.13 MV（补充） ###

**定位**：MV 是与曲目关联的视频资源，组织方式与 `base.json` 对齐：`mv/<sha>/mv.json`（+ 视频文件）。

```text
mv/<sha>/
├─ mv.json          # MV 元数据
└─ video.mp4        # 已下载时存在；在线播放时 location 为 URL
```

```json
{
  "sha": "7d1e…ffff",
  "platform": "netease",
  "item_id": "mv_001",
  "version": "1.0.0",
  "title": "Example Song (MV)",
  "artist": "Example Artist",
  "album": "Example Album",
  "location": "https://…/mv.mp4",
  "pic": "covers/mv.jpg",
  "duration_seconds": 240,
  "width": 1920,
  "height": 1080,
  "created_at": 1735689600
}
```

**规则**

1. **唯一性**：`sha` 与 `(platform, item_id)` 各唯一；`sha` 不同但同一来源同一 id → `1023 base conflict`。
2. **防覆写**：只有**同一 `sha`** 的 MV 才允许覆盖（与 base.json 一致）；写入同样先 `.tmp` 再原子替换。
3. **未下载**：`location` 可以是 URL（流式播放）或本地相对路径；下载完成后更新 `location` 并写 `video.*`。
4. **与曲目的关系**：MV 通过 `(platform, item_id)` / `album` / `title+artist` 与曲目关联，**不**强制一对一（一首歌可有多个 MV 版本，靠 `sha` 区分）。
5. MV 不参与 2.7 的音频折叠；搜索结果里以独立的 `mvs[]` 返回（见 2.16）。

### 1.14 运行日志（补充） ###

**定位**：可观测性基础设施。所有模块通过统一 logger 输出，落盘在 `logs/radmusic.log`，并可经 API 读取最近若干条。

**日志行格式**

```text
2026-10-11T14:30:55.123Z [info] [http] GET /music/status -> 200 (12 ms) (http.cpp:512)
```

**级别**：`trace` < `debug` < `info` < `warn` < `error` < `off`（默认 `info`）。

**分类**：`http` / `api` / `engine` / `storage` / `plugin` / `import` / `download` / `cache` / `decrypt` / `eq`。

**规则**

1. **默认静默**：库本身不主动产生输出；宿主显式配置（`--log-level` / `--log-file` 或 `log_configure()`）后生效。
2. **落盘与轮转**：默认单文件 8 MiB，超出则轮转为 `radmusic.log.1`（保留一份历史）。
3. **必须记录的事件**：加载/播放状态变化、`base conflict`/`sha error`/解密失败、插件能力缺失（1012）、单源搜索失败（1010）、下载失败（1018/1022）、缓存清理与淘汰、HTTP 请求与传输层错误。
4. **不得记录敏感信息**：`password`、`usec`、Cookie/Token、完整 URL 中的鉴权参数一律脱敏（打印为 `***`）。
5. **内存缓冲**：保留最近 N 条（默认 500）供 `GET /music/logs` 与问题排查；进程重启后清空。
6. **线程**：当前服务端为单线程 poll 模型，logger 未加锁；若宿主引入多线程，需在外部串行化（待定）。

---

## 2. API 调用（补充） ##

### 2.1 调用形态 ###

- **本地库**：C++ 接口（`load.cpp` / `start.cpp` / `resecrect.cpp` 对应模块），供播放器直接链接。
- **HTTP 服务（可选）**：若需要跨进程/跨语言调用，在本地库之上暴露 REST 接口，语义与本地库保持一致。

### 2.2 通用返回结构 ###

```json
{
  "code": 0,
  "message": "ok",
  "data": {}
}
```

`code = 0` 表示成功，其余见[第 3 节错误码](#3-错误码补充)；`message` 为人类可读描述（历史错误字符串如 `platform error` 作为 `message` 保留）。

### 2.3 接口一览 ###

| 方法 | HTTP | 对应模块 | 说明 |
| --- | --- | --- | --- |
| `load` | `POST /music/load` | load.cpp | 按缓存中的 `base.json` 加载曲目（缺失先重建；校验 + 可选解密 + 入缓存） |
| `start` | `POST /music/start` | start.cpp | 开始播放 |
| `pause` / `resume` | `POST /music/pause`、`/music/resume` | start.cpp | 暂停 / 继续 |
| `stop` | `POST /music/stop` | start.cpp | 停止播放 |
| `seek` | `POST /music/seek` | start.cpp | 跳转位置 |
| `status` | `GET /music/status` | start.cpp | 播放状态与进度 |
| `info` | `GET /music/info` | base.json | 读取曲目元数据 |
| `verify` | `POST /music/verify` | resecrect.cpp | 校验密码是否有效（不执行解密） |
| `search` | `GET/POST /music/search` | 播放源类插件 | 跨多个播放源搜索，结果按同一首歌折叠聚合（见 2.5） |
| `request` | `POST /music/request` | 播放源类插件 | 统一请求入口，路由到具备对应能力的播放源插件（见 2.6） |
| `lyric` | `GET /music/lyric` | 歌词模块（1.5） | 获取归一化歌词，本地优先、可回退到播放源插件（见 2.8） |
| `eq` / `dsp` | `GET/POST /music/eq`、`/music/dsp/*` | 调音模块（1.7） | 读取/设置均衡器与音效节点（见 2.9） |
| `download` | `POST/GET/DELETE /music/download` | 下载模块（1.8） | 创建/查询/暂停/取消下载任务（见 2.10） |
| `cache` | `GET/POST /music/cache*` | 缓存模块（1.9） | 统计、清理、重建（含 `base.json`）与配置缓存（见 2.11） |
| `import` | `POST/GET /music/import*` | 导入模块（1.10） | 扫描本地音乐、生成 `base.json` 入库、冲突报告（见 2.12） |
| `quality` | `GET /music/quality*` | 音质定义（1.1.1 / 5.4） | 查询各播放源声明的音质定义与曲目可用音质（见 2.13） |
| `data` | `GET /music/data`、`POST /music/data/purge` | 数据管理 | 统计各分类占用、按分类清除全部数据（见 2.18） |
| `library` | `GET /music/library`、`POST /music/library/merge` 等 | 曲库 / 合并（merge.cpp） | 曲库列表（含合并标记）、自定义合并 / 取消合并（见 2.19） |
| `update` | `GET /music/update`、`POST /music/update/check` 等 | 检查更新（update.cpp） | 查询 GitHub Releases、分片下载安装包、解压覆盖并重启（见 2.20） |

### 2.4 调用示例（草案） ###

```http
POST /music/load HTTP/1.1
Content-Type: application/json

{ "meta": "9f2c…abcd", "password": null }
```

```json
{ "code": 0, "message": "ok", "data": { "sha": "9f2c…abcd", "cached": true } }
```

### 2.5 搜索 API（search）（补充） ###

**目的**：一次调用即可跨**多个播放源**（支持多类源：`netease`、`radish`、`local` 及第三方播放源插件）搜索，结果聚合后返回。

**请求**

| 参数 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `keyword` | string | 是 | 搜索关键字 |
| `sources` | string[] | 否 | 指定播放源（platform 列表）；缺省表示全部**已启用**的播放源 |
| `type` | string | 否 | `song`（默认）/ `album` / `artist` / `playlist` |
| `page` | int | 否 | 页码，从 1 开始，默认 1 |
| `page_size` | int | 否 | 每页**折叠后**的条目数，默认 20，上限 100 |
| `timeout_ms` | int | 否 | 单个源的超时，默认 5000 |
| `strategy` | string | 否 | `fanout`（并发全部源，默认）/ `race`（先返回者优先） |
| `fold` | bool | 否 | 是否按同一首歌折叠，默认 `true`；折叠规则见 2.7 |
| `quality_min` | string | 否 | 最低音质：`low` / `medium` / `high` / `lossless` / `hires`；缺省不限 |
| `duration_min` / `duration_max` | int | 否 | 时长区间（秒），`0`/缺省表示不限；时长未知（0）的条目不参与区间过滤 |
| `available_only` | bool | 否 | 只保留至少有一个可用音源的条目，并从 `sources[]` 中移除不可用源 |
| `sort` | string | 否 | `relevance`（默认，标题序）/ `title` / `artist` / `duration` / `quality` |

> **本地搜索**：`sources` 中出现 `local`，或 `sources` 缺省时，宿主会把本地曲库（`platform=local` 的 base.json）作为 `local` 源一并搜索；
> 本地结果与在线结果**一起参与折叠**，因此同一首歌的本地/在线版本会合并到同一行的 `sources[]` 里。
> `type` 不是 `song` 时只查询在线源。

**响应**

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "keyword": "example",
    "page": 1,
    "page_size": 20,
    "total": 1,
    "sources_status": [
      { "platform": "local",   "code": 0, "elapsed_ms": 0,   "result_count": 3 },
      { "platform": "radish",  "code": 0, "elapsed_ms": 180, "result_count": 20 },
      { "platform": "netease", "code": 0, "elapsed_ms": 320, "result_count": 18 },
      { "platform": "third_x", "code": 1010, "message": "source timeout" }
    ],
    "results": [
      {
        "group_key": "example song|example artist|215",
        "title": "Example Song",
        "artists": ["Example Artist"],
        "album": "Example Album",
        "duration": 215,
        "source_count": 2,
        "folded": true,
        "best_source": "radish",
        "sources": [
          {
            "platform": "radish",
            "id": "radish:t_1001",
            "available": true,
            "base": { "name": "Example Song", "author": "Example Artist", "pic": "covers/example.jpg", "secret": 0 },
            "location": "radish://stream/t_1001",
            "quality": { "quality_id": "lossless", "label": "无损", "level": "lossless", "codec": "flac", "bitrate": 1411, "sample_rate": 44100, "bit_depth": 16, "lossless": true },
            "qualities": [
              { "quality_id": "lossless", "label": "无损", "level": "lossless", "codec": "flac", "bitrate": 1411, "lossless": true },
              { "quality_id": "exhigh", "label": "极高", "level": "high", "codec": "mp3", "bitrate": 320, "lossless": false }
            ]
          },
          {
            "platform": "netease",
            "id": "netease:2002",
            "available": false,
            "reason": "vip required",
            "quality": { "quality_id": "exhigh", "label": "极高", "level": "high", "codec": "mp3", "bitrate": 320, "lossless": false },
            "qualities": []
          }
        ]
      }
    ]
  }
}
```

**规则**

1. **单源失败不影响整体**：总体 `code = 0`，失败源在 `sources_status` 中标注；仅当没有任何可用源、或全部源都失败时返回 `1009`。
2. `results` 为折叠后的顺序；`sources[]` 内部按「源优先级 + 音质」排序，源优先级由用户配置（默认 `radish` 优先，其余按启用顺序）。
3. 分页以**折叠后**的条目计数，不得因折叠导致条目丢失或重复（跨页合并：待定）。
4. 播放折叠条目时必须显式指定 `platform` + `id`（或 `base`），**不允许**由服务端隐式挑选音源。
5. 搜索本身的实现即 `request(source, op = "search")`，字段必须满足本节与 2.7 的归一化要求。
6. **音质维度**：每个源条目用 `quality`（最高可用，见 1.1.1）与 `qualities[]`（该源可用音质列表）表达，**不得**把同一源的不同音质拆成多个源条目；`qualities[]` 由插件定义（见 5.4）。

### 2.6 request API（多源请求）（补充） ###

**定位**：播放源类 Function 插件除提供 `platform` 支持外，还通过统一的 `request` API 对外暴露该源的请求能力——**播放源类 function 越多，可用的 request 源就越多**。调用方不需要了解各平台的协议、签名、Cookie、加密与限流细节，由宿主统一代理。

**请求**

| 参数 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `source` | string | 是 | 目标播放源（platform），必须由某个已启用的播放源类插件提供 |
| `op` | string | 是 | 操作名，由插件在 `capabilities` 中声明：`search` / `get_song` / `get_url` / `get_qualities` / `get_lyric` / `get_pic` / `get_playlist` / `check_login` 等 |
| `params` | object | 否 | 操作参数，结构由 `op` 决定 |
| `session` | string | 否 | 登录态/会话引用；Cookie、Token 由宿主托管，禁止调用方直接传递凭据 |
| `timeout_ms` | int | 否 | 超时，默认 5000 |
| `retry` | int | 否 | 重试次数，默认 1（仅对幂等 `op` 生效） |
| `cache_ttl` | int | 否 | 结果缓存秒数，`0` 表示不缓存；`get_url` 类结果不得缓存超过其 `expires_at` |
| `raw` | bool | 否 | `true` 返回插件原始响应，默认 `false` 返回归一化数据 |

**响应**

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "source": "netease",
    "op": "get_url",
    "elapsed_ms": 142,
    "result": { "url": "https://…", "quality": "320k", "expires_at": 1735693200 }
  }
}
```

**插件侧接口**

```cpp
virtual std::vector<std::string> ops() const;                        // 声明支持的 op
virtual Result<Json> request(std::string_view op, const Json& params);
```

**规则**

1. `op` 未在清单 `capabilities` 中声明时返回 `1012 unsupported capability`。
2. 网络 I/O、重试、限流、Cookie/Token 托管、代理配置由宿主统一实现；插件只做协议适配与数据归一化（是否允许插件自行发起网络请求：待定）。
3. 同一 `source` 的并发上限与 QPS 由清单 `limits` 声明，宿主强制执行，超限返回 `1014 rate limited`。
4. 需要登录态的操作必须在清单中声明 `permissions: ["account"]`，否则拒绝执行。
5. 任何 `request` 结果都不得改变本地曲库状态；写操作需走独立接口（待定）。
6. **音质参数**：`get_url` 的 `params.quality` 可传平台 `quality_id` 或归一化 `level`（见 1.1.1）；未指定时取该源最高可用；**实际音质可能与请求不同**（源降级/无该音质），响应必须回传真实 `quality`，不得谎报。
7. `get_qualities` 返回该曲目在该源可用的音质列表（结构同 2.5 的 `qualities[]`）；音质定义本身来自插件清单（5.4），不是运行时编造。

### 2.7 多源聚合与折叠显示（补充） ###

**要求**：同一首歌存在于多个播放源时，**必须折叠为一条**展示（默认折叠），不得把同一个 `group_key` 的结果拆成多行。

**折叠键**

```text
group_key = normalize(title) + "|" + normalize(主作者) + "|" + round(duration)
```

- `normalize`：转小写、全角转半角、去除首尾与内部连续空白、去除非字母数字汉字字符、去掉括号内容。
- `duration` 容差 ±3 秒视为同一首歌（容差值待定，可配置）。
- 可选参与字段：`album`（默认不参与，避免同名专辑干扰）。
- 平台后缀（`(Live)`、`(Remastered)`、`feat.` 等）默认保留；是否忽略：待定。

**交互（草案）**

```text
┌─ Example Song — Example Artist        2 个音源  ▸ ┐   ← 默认折叠为 1 行
│  ▾ 展开后：
│    ● radish   flac   1411 kbps   可用（默认选中）
│    ○ netease  320k    320 kbps   不可用 · vip required
└──────────────────────────────────────────────────┘
```

- 折叠行显示：歌名、作者、时长、音源数量、最高可用音质。
- 展开后列出各源，可手动选择/切换；默认选中优先级最高且可用的源。
- 不可用源默认仍展示（灰显 + 原因），用户可开启「隐藏不可用音源」。
- 播放历史、收藏、歌单以 `group_key` 为主键，音源可随时替换；替换失败（如源下架）需回退到同组其他可用源。
- **本地曲目同样参与折叠**：导入的曲目 `platform = "local"`（见 1.10），与在线音源按同一 `group_key` 折叠为一条，`sources[]` 中作为其中一个源。
- **音质是折叠条目内的第二维度**：折叠行显示各源最高可用音质；展开后按「源 → 音质」两级选择，同一源的不同音质**不拆成多行**；选择音质后播放或下载时按 1.8 / 2.10 各自独立存储。

### 2.8 歌词 API（lyric）（补充） ###

**对应模块**：歌词加载（1.5）；底层实现即 `request(source, op = "get_lyric")`。

**请求**

| 参数 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `sha` | string | 二选一 | 本地曲目标识（优先使用，可命中本地/缓存歌词） |
| `platform` + `id` | string | 二选一 | 未入库的在线曲目 |
| `sources` | string[] | 否 | 允许拉取歌词的播放源及顺序；缺省按用户源优先级 |
| `prefer` | string | 否 | `auto`（默认）/ `local`（仅本地）/ `online`（仅在线） |
| `format` | string | 否 | `json`（默认，归一化结构）/ `lrc`（原始文本） |
| `include` | string[] | 否 | `tr`（翻译）、`roma`（罗马音）、`words`（逐字） |
| `offset_ms` | int | 否 | 临时覆盖用户偏移，正值表示歌词提前 |
| `persist` | bool | 否 | 是否把该偏移或拉取结果写回本地，默认 `false` |
| `refresh` | bool | 否 | 跳过缓存重新拉取，默认 `false` |

**响应（`format = json`）**

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "sha": "9f2c…abcd",
    "origin": "local",
    "source": null,
    "format": "lrc",
    "timed": true,
    "offset_ms": 0,
    "line_count": 42,
    "lyric": { "lines": [{ "time_ms": 12340, "text": "Example line", "tr": "示例行" }] }
  }
}
```

`format = lrc` 时 `data.lyric` 为原始 LRC 文本。

**规则**

1. 查找顺序：本地歌词文件 → 歌词缓存 → 播放源插件（按 `sources` 顺序取第一个非空结果）。
2. 正常情况 `offset_ms` 只影响返回与渲染，**不修改**原始歌词文件；`persist = true` 时才写回本地（是否需要用户确认：待定）。
3. 全部来源失败返回 `1015 lyric unavailable`，**属于非致命错误**，调用方应降级为无歌词继续播放。
4. 在线歌词按 `sha`（或 `platform + id`）缓存到 `cache/lyric/<sha>.json`；在线结果的过期策略：待定。
5. 返回的 `lines` 必须按 `time_ms` 升序，时间戳重复视为同一时刻的多语言行合并。

### 2.9 调音 API（eq / dsp）（补充） ###

**对应模块**：调音处理链（1.7）。

| 接口 | HTTP | 说明 |
| --- | --- | --- |
| `eq_get` | `GET /music/eq` | 获取当前处理链、均衡器参数与生效预设 |
| `eq_set` | `POST /music/eq` | 设置 EQ / 增益 / 预设，需实时生效（≤ 100 ms） |
| `eq_preset_list` / `eq_preset_save` / `eq_preset_delete` | `GET/POST/DELETE /music/eq/presets` | 预设管理 |
| `dsp_nodes` | `GET /music/dsp/nodes` | 列出已注册的音效节点（来自其他类插件）及其参数 |
| `dsp_bypass` | `POST /music/dsp/bypass` | 旁路/启用某个节点（排障用） |

**请求示例**

```json
{
  "enabled": true,
  "preset": "user:my_curve",
  "preamp_db": -2.0,
  "bands": [
    { "type": "peak",      "freq": 60,    "gain": 3.0, "q": 1.0 },
    { "type": "peak",      "freq": 170,   "gain": -1.5, "q": 1.0 },
    { "type": "high_shelf","freq": 10000, "gain": 2.0, "q": 0.7 }
  ]
}
```

**响应示例**

```json
{ "code": 0, "message": "ok", "data": { "applied": true, "engine": "builtin", "latency_ms": 12 } }
```

**规则**

1. 参数校验失败返回 `1007 invalid metadata`；未知节点返回 `1012 unsupported capability`。
2. 引擎初始化失败返回 `1017`（**非致命**），播放器应旁路调音并继续播放。
3. 未提供的字段保持原值；`enabled = false` 表示整体旁路但保留参数。
4. 全频段参数与预设由宿主持久化（`presets/eq/`），插件卸载后其节点参数失效。

### 2.10 下载 API（download）（补充） ###

**对应模块**：下载（1.8）。

| 接口 | HTTP | 说明 |
| --- | --- | --- |
| `download_create` | `POST /music/download` | 创建下载任务（可批量） |
| `download_list` | `GET /music/download` | 查询任务列表与进度（可按状态过滤） |
| `download_pause` / `download_resume` | `POST /music/download/{id}/pause`、`/resume` | 暂停 / 继续（保留断点） |
| `download_cancel` | `POST /music/download/{id}/cancel` | 取消任务并清理 `.part` |
| `download_remove` | `DELETE /music/download/{id}` | 移除任务记录（`?delete_file=true` 时同时删除文件，需二次确认） |

**请求示例**

```json
{
  "tasks": [
    { "source": "radish", "item_id": "radish:t_1001", "quality": "lossless" },
    { "source": "radish", "item_id": "radish:t_1001", "quality": "exhigh" },
    { "source": "netease", "item_id": "netease:2002", "quality": "high", "filename": "{artist} - {title} [{quality}]" }
  ]
}
```

> 上例同时下载同一首歌的 `lossless` 与 `exhigh` 两个音质：**两条独立任务、两份独立存储**，互不覆盖（见 1.8 规则 1）。

**响应示例**

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "tasks": [
      { "id": "dl_0001", "state": "queued", "quality": "lossless", "quality_resolved": { "level": "lossless", "codec": "flac", "lossless": true } },
      { "id": "dl_0002", "state": "queued", "quality": "exhigh", "quality_resolved": { "level": "high", "codec": "mp3", "bitrate": 320, "lossless": false } },
      { "id": "dl_0003", "state": "running", "quality": "high", "received": 1048576, "total": 8388608 }
    ]
  }
}
```

**事件**：`on_download_progress(id, received, total, speed)`、`on_download_done(id, sha, path, quality)`、`on_download_failed(id, code, message)`。

**规则**

1. 折叠条目（2.7）下载时**必须显式指定 `source`**；缺省音源一律拒绝（`1007`）。
2. **不同音质各建一条任务、各存一份文件**；同一首歌多个音质不得合并为一条记录。
3. 请求的音质该源不支持时返回 `1021 quality unsupported`；音质标识无法识别时返回 `1027 quality undefined`（响应中附上该源可用音质）。
4. 超出并发上限的任务进入 `queued`，不返回失败。
5. 校验失败返回 `1022` 并保留任务记录供换源重试。
6. 磁盘不足返回 `1019` 并暂停全部运行中任务。

### 2.11 缓存 API（cache）（补充） ###

**对应模块**：缓存（1.9）。

| 接口 | HTTP | 说明 |
| --- | --- | --- |
| `cache_stats` | `GET /music/cache` | 各层用量、条目数、命中率（可选 `?type=meta,audio,lyric`） |
| `cache_clear` | `POST /music/cache/clear` | 按类型/全部清理，返回释放空间（清理 `meta` 层 = 删除 `base.json`，之后按需重建） |
| `cache_rebuild` | `POST /music/cache/rebuild` | 重建 `meta` 层的 `base.json`（支持 `?song_key=`、`?song_key=&quality_key=` 精确定位；本地重解析标签，在线重拉取） |
| `cache_config` | `POST /music/cache/config` | 设置容量上限与淘汰策略 |
| `cache_preload` | `POST /music/cache/preload` | 预取指定曲目（待定，可用于下一首预读） |

**响应示例**

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "usage_bytes": 1073741824,
    "limit_bytes": 5368709120,
    "layers": { "meta": 64000000, "audio": 900000000, "lyric": 1200000, "cover": 8000000, "tmp": 0 },
    "hit_rate": { "meta": 0.99, "audio": 0.82, "lyric": 0.97 }
  }
}
```

**规则**

1. `cache_clear` **不得**删除 `tracks/`（音频、`sec.json`、歌词）、已完成的下载与用户预设（`presets/`）。
2. 清理 `meta` 层即删除 `base.json`：清理前必须保证其可重建（`index/` 中有指针，本地文件或在线源可达）；正在播放/播放列表引用的条目**先重建或标记 `pending eviction`**，不得让播放中的曲目失去元数据。
3. 清理后首次加载/查询该曲目时按 1.3 规则 5 自动重建；重建失败返回 `1028 base rebuild failed`，路径与内容不符返回 `1029 meta path mismatch`；重建必须落回同一 `song_key`/`quality_key` 路径，**不得产生第二份 `base.json`、不得跨音质写同一目录**（1.1 / 1.10）。
4. 正在播放或正在下载的条目标记为 `pending eviction`，播放结束后才可清理，清理响应需报告 `skipped` 数量。
5. 清理与预取为异步操作，应返回 `task`/`accepted` 状态而非阻塞等待（超时约束：待定）。
6. `cache_config` 缩小上限时按 LRU 立即淘汰，不得影响当前播放；`meta` 层被淘汰的条目按需重建。

### 2.12 导入 API（import）（补充） ###

**对应模块**：音乐导入（1.10）。

| 接口 | HTTP | 说明 |
| --- | --- | --- |
| `import_scan` | `POST /music/import/scan` | 预扫描：返回候选文件、解析结果与**冲突预检报告**，**不写盘** |
| `import_start` | `POST /music/import` | 执行导入（可批量），支持 `dry_run` |
| `import_status` | `GET /music/import/{id}` | 进度：已处理/总数、当前文件、跳过/失败数 |
| `import_cancel` | `POST /music/import/{id}/cancel` | 取消任务（**已原子提交的条目保留**） |
| `import_report` | `GET /music/import/{id}/report` | 冲突与失败明细 |

**请求示例**

```json
{
  "paths": ["D:/Music/Example Artist"],
  "recursive": true,
  "mode": "copy",
  "conflict": "skip",
  "tag_fallback": "filename",
  "filename_template": "{artist} - {title}",
  "dry_run": false
}
```

**响应示例**

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "import_id": "imp_0001",
    "scanned": 120,
    "imported": 118,
    "skipped": 1,
    "failed": 1,
    "conflicts": [
      {
        "type": "duplicate_sha",
        "sha": "9f2c…abcd",
        "existing": "cache/meta/example-song_1a2b3c4d5e6f/netease.lossless/base.json",
        "action": "skip"
      }
    ]
  }
}
```

**事件**：`on_import_progress(import_id, done, total, current)`、`on_import_conflict(entry)`、`on_import_done(report)`。

**规则**

1. `import_scan` 必须**无副作用**；`dry_run = true` 时 `import_start` 也只返回将要写入的结果。
2. `conflict` 取值 `skip`（默认）/ `merge` / `overwrite`；`overwrite` 仅限同一 `sha`（见 1.10）。
3. 单条失败不影响其余条目，但**每条必须独立原子提交**，不得留下半个 `base.json`。
4. 曲库被其他进程或导入任务锁定时返回 `1026 library locked`，**不得强行写入**。
5. 导入完成后必须刷新库索引；索引重建以磁盘为准（1.10 规则 4）。

### 2.13 音质 API（quality）（补充） ###

**对应模块**：音质定义（1.1.1）与播放源插件的音质声明（5.2 / 5.4）。

| 接口 | HTTP | 说明 |
| --- | --- | --- |
| `quality_levels` | `GET /music/quality` | 列出各播放源声明的音质定义；`?platform=netease` 过滤单个源，`?enabled_only=true` 仅返回已启用源 |
| `quality_of` | `GET /music/quality/{source}/{item_id}` | 查询某曲目在该源可用的音质（转发 `request(op = "get_qualities")`） |

**响应示例（`GET /music/quality`）**

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "sources": [
      {
        "platform": "netease",
        "plugin_id": "netease.source",
        "levels": [
          { "quality_id": "standard", "label": "标准",   "level": "low",      "codec": "mp3",  "bitrate": 128,  "lossless": false },
          { "quality_id": "lossless", "label": "无损",   "level": "lossless", "codec": "flac", "bitrate": 1411, "sample_rate": 44100, "lossless": true },
          { "quality_id": "hires",    "label": "Hi-Res", "level": "hires",    "codec": "flac", "sample_rate": 96000, "bit_depth": 24, "lossless": true }
        ]
      }
    ]
  }
}
```

**规则**

1. 音质定义的**唯一来源**是播放源插件清单；宿主只归一化、排序与展示，不得新增或改写平台音质。
2. `level` 必须是 1.1.1 的归一化枚举之一；`quality_id` 仅在所属 `platform` 内有意义，跨平台同名不表示等价。
3. 未声明音质定义的源：接口返回该源但 `levels` 为空（曲目 `quality.level = unknown`）；请求未知 `quality_id` 时返回 `1027 quality undefined`。
4. 请求音质时必须同时给出 `platform`（或 `source`）与音质标识，禁止只传 `quality_id` 造成跨平台歧义（`1007 invalid metadata`）。
5. 本接口只读；改变用户偏好（默认音质、自动降级顺序）走 `cache_config` 同级的偏好接口（待定）。

### 2.14 歌单 API（playlist）（补充） ###

**对应模块**：歌单（1.11）。

| 接口 | HTTP | 说明 |
| --- | --- | --- |
| `playlist_list` | `GET /music/playlist` | 歌单列表（按 `updated_at` 降序），含 `track_count` / `total_duration` |
| `playlist_get` | `GET /music/playlist/{id}` | 歌单详情（含 `tracks[]`） |
| `playlist_create` | `POST /music/playlist` | 创建：`{name, description?, platform?, source_id?, pic?}` |
| `playlist_update` | `POST /music/playlist/{id}` | 改名/改描述：`{name?, description?}` |
| `playlist_delete` | `DELETE /music/playlist/{id}` | 删除歌单（不删除曲目文件） |
| `playlist_add` | `POST /music/playlist/{id}/add` | 加歌：`{track:{platform, item_id, sha?, name?, author?, duration?}}` 或 `{sha}` |
| `playlist_remove` | `POST /music/playlist/{id}/remove` | 移除：`{platform, item_id}` |
| `playlist_move` | `POST /music/playlist/{id}/move` | 调整顺序：`{platform, item_id, index}`（越界钳制） |
| `playlist_sync` | `POST /music/playlist/{id}/sync` | 在线歌单重新同步（走 `request(op="get_playlist")`） |

**规则**

1. 歌单操作**只影响引用**，绝不删除 `tracks/` 下的音频文件。
2. `add` 去重（同一 `platform + item_id` 只保留一条），重复添加返回现有歌单且 `code=0`。
3. 未知 `id` → `1030 playlist not found`；参数缺失/类型错误 → `1007`。
4. `sync` 需要对应播放源插件声明 `get_playlist`，否则 `1012`。

### 2.15 专辑 API（album）（补充） ###

**对应模块**：专辑（1.12）。

| 接口 | HTTP | 说明 |
| --- | --- | --- |
| `album_list` | `GET /music/album` | 专辑列表（按 `updated_at` 降序） |
| `album_get` | `GET /music/album/{id}` | 专辑详情（含曲目与曲序） |
| `album_create` | `POST /music/album` | 创建/收藏：`{name, artist?, platform?, source_id?, year?}` |
| `album_delete` | `DELETE /music/album/{id}` | 删除专辑记录 |
| `album_update` | `POST /music/album/{id}` | 编辑专辑：`{name?, artist?, year?, pic?, description?}`（空字段不修改） |
| `album_add` / `album_add_many` / `album_remove` | `POST /music/album/{id}/add`、`/add_many`、`/remove` | 增删曲目；`add_many` 接受 `{items:[{platform,item_id,sha?,name,author?,duration?}]}`，一次落盘、自动去重 |
| `album_sync` | `POST /music/album/{id}/sync` | 从播放源重新拉取（`request(op="get_album")`） |

**规则**：与歌单一致；未知 `id` → `1031 album not found`；`tracks` 顺序即曲序。

### 2.16 MV API（mv）（补充） ###

**对应模块**：MV（1.13）。

| 接口 | HTTP | 说明 |
| --- | --- | --- |
| `mv_list` | `GET /music/mv` | MV 列表（按 `created_at` 降序） |
| `mv_get` | `GET /music/mv/{sha}` | 单个 MV 详情 |
| `mv_put` | `POST /music/mv` | 登记/更新 MV（`sha` 必填；同 `sha` 才允许覆盖） |
| `mv_delete` | `DELETE /music/mv/{sha}` | 删除 MV 记录与 `mv/<sha>/` 目录 |
| `mv_of_track` | `GET /music/mv?platform=&id=` | 按来源查找某曲目关联的 MV（`find_by_source`） |
| `mv_open` | `POST /music/mv/{sha}/open` | 需要播放地址时走 `request(op="get_mv")` 刷新 `location` |

**规则**：未知 `sha` → `1032 mv not found`；同 `(platform,item_id)` 被不同 `sha` 占用 → `1023`；`sha` 缺失 → `1007`。
搜索接口（2.5）在 `type=mv` 时会额外返回 `data.mvs[]`（**附加字段，不影响旧的 `results`**）。

### 2.17 日志 API（logs）（补充） ###

**对应模块**：运行日志（1.14）。

| 接口 | HTTP | 说明 |
| --- | --- | --- |
| `logs_list` | `GET /music/logs?lines=200&level=warn&category=http` | 返回最近 N 条内存日志（默认 200，上限 500） |
| `logs_level` | `POST /music/logs/level` | 运行时调整级别：`{level:"debug"}` |

**响应示例**

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "level": "info",
    "total": 3,
    "records": [
      { "ts_ms": 1735689600123, "time": "2026-10-11T14:30:00.123Z", "level": "info",
        "category": "http", "message": "GET /music/status -> 200", "file": "http.cpp", "line": 512 }
    ]
  }
}
```

**规则**：日志接口**只读**，且必须脱敏（不得返回密钥/Token）；未知 `level` → `1007`。

---

### 2.18 数据管理 API（data）（补充） ###

**对应模块**：数据目录（第 4 节）与设置页「数据」页。

| 接口 | HTTP | 说明 |
| --- | --- | --- |
| `data_stats` | `GET /music/data` | 按分类统计占用：文件数、字节数、是否存在 |
| `data_purge` | `POST /music/data/purge` | 按分类清除数据（危险操作，需确认短语） |

**分类（id）**

| id | 名称 | 覆盖路径 |
| --- | --- | --- |
| `tracks` | 曲目与元数据 | `tracks/`、`cache/meta/` |
| `cache` | 缓存文件 | `cache/` 下的 audio / stream / lyric / cover / tmp、`cache.json`、`downloads.json` |
| `covers` | 封面 | `covers/` |
| `library` | 歌单与专辑 | `library/` |
| `mv` | MV | `mv/` |
| `index` | 索引与统计 | `index/`、`usec.json` |
| `logs` | 运行日志 | `logs/` |
| `presets` | 音效预设 | `presets/` |
| `plugins` | 插件 | `plugins/` |
| `settings` | 应用设置 | `settings.json` |

**响应示例（统计）**

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "root": "G:/Rad music/api/radmusic-data",
    "usage_bytes": 1670030978,
    "total_files": 42,
    "tracks_count": 2,
    "groups": [
      { "id": "tracks", "name": "曲目与元数据", "description": "…",
        "bytes": 1669323861, "files": 30, "present": true }
    ]
  }
}
```

**请求示例（清除）**

```http
POST /music/data/purge
Content-Type: application/json

{ "targets": ["tracks", "cache"], "confirm": "PURGE" }
```

`targets` 可传 `["all"]` 表示全部分类；`dry_run: true` 只统计不删除。

**响应示例（清除）**

```json
{ "code": 0, "message": "ok",
  "data": { "dry_run": false, "root": "…", "purged": ["tracks", "cache"],
            "removed_files": 32, "removed_bytes": 1669328116, "failed": 0 } }
```

**规则**

1. `confirm` 必须等于 `PURGE`（`dry_run` 除外），否则返回 `1011` 且**不删除任何文件**；未知分类 → `1007`。
2. 只删除 `<root>` 之内、且命中分类清单的路径，根目录本身与清单外文件一律不动。
3. 清除 `tracks` 会连带删除 `cache/meta`，避免曲库残留无法播放的条目；清除 `index` / `tracks` / `cache` 后索引被清空并重建。
4. 清除 `logs` 前会释放日志文件句柄，删除后按原配置重新打开日志文件。
5. 清除 `settings` 后回写一份默认 `settings.json`；已加载的插件清单在清除 `plugins` 后同步卸载。
6. 删除后重建目录布局，服务无需重启；正在播放的会话会被复位。
7. 被占用而无法删除的文件（非日志）计入 `failed`，不影响其余删除。

### 2.19 曲库合并 API（library merge）（补充） ###

**目的**：同一首歌可能因标签 / 识别误差被拆成多条（歌名多了后缀、歌手写法不同、时长差几秒），在曲库里表现为互不相关的多个 `song_key`。合并让用户手动把它们归一到同一首歌：全部条目搬进同一个 `song_key` 目录，按 `platform + quality` 堆叠成音质列表（同音质多实体用 `<quality_key>~2` 变体目录）。

**持久化**：合并记录保存在 `library/merge.json`（用户数据，不可被缓存清理）。成员的原始歌名 / 歌手 / 时长同时写入各条目 `base.json` 的 `orig_name` / `orig_author` / `orig_duration`（可选字段），取消合并时还原。

| 接口 | HTTP | 说明 |
| --- | --- | --- |
| `merge` | `POST /music/library/merge` | 合并：`{ target, sources, title?, artist?, duration? }`（song_key 列表）。`target` 提供合并后的规范身份；`title` / `artist` / `duration` 可覆盖 |
| `unmerge` | `POST /music/library/unmerge` | 取消合并：`{ song_key, members? }`。`members` 缺省整组拆散；指定时只拆出这些成员（剩 1 个成员时自动整组撤销） |
| `merges` | `GET /music/library/merges` | 列出全部合并组 |

**行为**

1. 合并后，被合并条目的歌名 / 歌手 / 时长改写为规范值（原始值进 `orig_*`）；此后再导入 / 下载同曲（即使标签仍是旧写法）会经 `path_for` 重定向自动落进同一 `song_key`。
2. `GET /music/library` 的每条 track 额外返回 `merged`（bool）、`merge_song_key`、`merge_count`、`merge_title`，以及存在时的 `orig_name` / `orig_author` / `orig_duration`。
3. 同音质冲突（同 `platform + quality` 但内容不同）不丢数据：后者落 `~2` / `~3` 变体目录，并在响应 `conflicts` 数组中报告。
4. 撤销合并按 `orig_*`（缺失时用 merge.json 成员记录）还原元数据，条目回到各自的原始 `song_key`。

**响应示例（合并）**

```json
{ "code": 0, "message": "ok",
  "data": { "song_key": "晴天 - 周杰伦_ab12cd34ef56", "title": "晴天", "artist": "周杰伦",
            "duration": 269, "moved": 2, "skipped": 0, "conflicts": [],
            "group": { "song_key": "…", "title": "晴天", "members": [ … ] } } }
```

---

### 2.20 检查更新 API（update）（补充） ###

**目的**：让宿主从 GitHub Releases 检查新版本、下载本平台安装包，并在用户确认后覆盖程序文件、重启自身。曲库、下载记录与宿主设置都在数据根里，更新过程不触碰它们。

**版本来源**：版本号在构建时注入（`RADMUSIC_VERSION` 宏，发布流水线取 git tag），默认仓库见 `version.hpp` 的 `RADMUSIC_UPDATE_REPO`（可用同名宏覆盖）。

| 接口 | HTTP | 说明 |
| --- | --- | --- |
| `status` | `GET /music/update`（别名 `/music/update/status`） | 读取当前状态、最近一次检查结果、下载进度、频道信息（`channel` / `build_channel` / `channels`）与该频道的发布历史（`releases`） |
| `check` | `POST /music/update/check` | 触发检查（异步）：查询 `/repos/<repo>/releases?per_page=30`，按频道挑选头部发布并按 `-<slug>.zip`（Windows）/`-<slug>.tar.gz`（其他平台）挑选本平台产物。可选 `{ "channel"?: "release\|dev\|canary", "allow_downgrade"?: bool }` |
| `download` | `POST /music/update/download` | 开始下载（异步）：`{ "threads"?: 1..16 }` 可选，缺省读宿主设置 `update_threads`（默认 4）；多线程时按 Range 分段并发，单线程时 4MB 一片推进并支持续传 |
| `apply` | `POST /music/update/apply` | 解压 → 校验 → 覆盖 → 重启；必须带 `{ "confirm": "UPDATE" }` |

**发布频道**

一次 `check` 拉取最近 30 条 Release，按 tag 后缀把每条归入频道，同一频道取版本最高（同版本取发布时间最新）的一条作为该频道的头部版本：

| 频道 | 判定规则 | 用途 |
| --- | --- | --- |
| `release` | tag 无 `-` 后缀、且 GitHub 未标 prerelease（如 `v0.12.0`） | 正式版，默认频道 |
| `dev` | 后缀含 `dev` / `alpha` / `beta` / `rc` / `preview`，或无后缀但被标为 prerelease | 开发版 |
| `canary` | 后缀含 `canary` / `nightly` / `insider` | 金丝雀，最新最不稳定 |

* 版本比较对齐 semver：`0.12.0 > 0.12.0-canary.2`，`canary.10 > canary.9`（`compare_version_full()`）。
* `has_update` 为真当且仅当频道头部版本更新；若头部版本比当前构建更旧（如本地 dev 构建停在正式版频道），`behind` 为真并**不**提示更新。
* 用户显式切换频道时前端会带 `allow_downgrade: true`，此时允许安装该频道的头部版本（哪怕版本号更小，属于预期的换取频道行为）。
* `POST /music/update/check` 里的 `channel` 会写回 `settings.json` 的 `update_channel`；`GET /music/update` 每次都会按设置同步频道与线程数。

**行为**

1. **不阻塞主循环**：`check` / `download` 只置状态并立即返回，真正的网络 I/O 由宿主 idle hook 调 `UpdateService::step()` 分片推进；`status.state` 依次为 `idle → checking → up_to_date | available → downloading → downloaded → applying`，任何一步失败进入 `failed` 并在 `error` 里给出原因。
2. **只下本平台包**：按 `win64` / `linux-x86_64` / `macos-arm64` / `macos-x86_64` 匹配文件名后缀；该版本没有本平台产物时 `has_update` 为真但 `can_download` 为假，提示到 Release 页面手动下载。
3. **多线程分段下载**：`threads > 1` 且包 ≥ 1MB 时按 Range 把文件切成等长段（`plan_update_segments()`，每段至少 512KB，最多 16 段），每段一个独立工作线程并发下载并直接写入 `.part` 文件的对应偏移（各线程自成连接与文件句柄，互不加锁）。`status.download.segments` 是本次实际段数，`status.parallel` 表示是否走并发。
   * 分段无法断点续传：开始时会重建 `.part` 并预分配到最终大小；
   * 下载源不支持 Range（返回 200 或范围不符）时**自动退回单线程重下一次**，不影响成功；
   * 线程数在下载期间不可改；改设置只影响下一次下载。
4. **完整性**：下载完成后比对字节数与 Release 元数据，解压时逐条目校验 CRC-32；任一不符即删除安装包并报错，绝不覆盖程序文件。
5. **路径安全**：归档内条目必须是相对路径，含 `..`、绝对路径、盘符或符号链接一律跳过（zip slip 防护）。
6. **安装（Windows）**：正在运行的 `radmusic_server.exe` 无法被覆盖，因此 `apply` 只做三件事——解压到 `updates/stage`、写出 `updates/apply_update.ps1` 与 `updates/relaunch.args`（原始启动参数，UTF-8）、启动该脚本并请求退出。脚本等旧进程退出后备份为 `radmusic_server.exe.bak`、覆盖文件、用原参数拉起新进程，全过程写入 `updates/apply_update.log`。
7. **安装（Linux / macOS）**：可直接覆盖运行中的二进制，`apply` 覆盖后调用宿主重启回调（`execv`）拉起新版本。
8. **目录**：安装包、替换脚本与 staging 都在**可执行文件同级**的 `updates/` 下（替换脚本要操作安装目录，不能跟着数据根走）；数据根与设置完全不受影响。

**设置项**：`update_threads`（1–16，默认 4）与 `update_channel`（`release` / `dev` / `canary`，默认 `release`），对应设置页「版本」页的下载线程数与频道选择；`set_api_base()` 可把 API 基址指到内网镜像 / GitHub Enterprise（默认 `https://api.github.com`）。

**响应示例**

```json
{ "code": 0, "message": "ok",
  "data": { "current": "0.11.0", "latest": "0.12.0", "tag": "v0.12.0",
            "has_update": true, "state": "available", "platform": "win64",
            "repo": "zbl201054/rad-music", "notes": "…", "published_at": "2026-10-24T08:00:00Z",
            "page_url": "https://github.com/zbl201054/rad-music/releases/tag/v0.12.0",
            "error": "", "checked_at": "2026-10-24T09:10:00",
            "updates_dir": "C:\\…\\RadMusic\\updates", "install_dir": "C:\\…\\RadMusic",
            "busy": false, "can_download": true, "can_apply": false, "is_dev_version": false,
            "channel": "release", "channel_label": "正式版", "build_channel": "release",
            "has_release": true, "behind": false,
            "channels": [
              { "id": "release", "label": "正式版", "available": true, "selected": true,
                "tag": "v0.12.0", "version": "0.12.0", "published_at": "2026-10-24T08:00:00Z", "count": 3 },
              { "id": "dev", "label": "开发版", "available": true, "selected": false,
                "tag": "v0.12.1-dev.2", "version": "0.12.1-dev.2", "published_at": "2026-10-25T03:00:00Z", "count": 5 },
              { "id": "canary", "label": "金丝雀", "available": false, "tag": "", "version": "" } ],
            "releases": [
              { "tag": "v0.12.0", "version": "0.12.0", "channel": "release", "prerelease": false,
                "published_at": "2026-10-24T08:00:00Z", "page_url": "https://…",
                "asset_name": "rad-music-v0.12.0-win64.zip", "asset_size": 2241504,
                "notes": "…", "is_current": false } ],
            "threads": 4, "parallel": false,
            "asset": { "name": "rad-music-v0.12.0-win64.zip", "url": "https://…", "size": 2241504 },
            "download": { "received": 0, "total": 2241504, "segments": 4, "path": "" } } }
```

---

## 3. 错误码（补充） ##

| code | 标识 | 含义 | 处理建议 |
| --- | --- | --- | --- |
| 0 | `ok` | 成功 | — |
| 1001 | `platform error` | platform 未知或无可用插件 | 检查插件是否安装/加载 |
| 1002 | `sha error` | `version` + 文件 `sha` 与元数据不一致 | 警告并记录日志（原文为警告） |
| 1003 | `usec error` | 密钥缺失或与 `sec.json` 校验值不符 | 重新输入密码 |
| 1004 | `file not found` | `location` 指向的文件不存在 | 检查路径与挂载 |
| 1005 | `not loaded` | 未成功加载即调用播放接口 | 先调用 `load` |
| 1006 | `decrypt failed` | 解密或认证失败 | 校验密文完整性/算法参数 |
| 1007 | `invalid metadata` | `base.json` 缺失字段、JSON 非法，或插件类型与声明不符 | 修正元数据 / 清单 |
| 1008 | `unsupported version` | `version` 超出当前实现支持范围 | 升级播放器或插件 |
| 1009 | `search no source` | 没有可用的播放源，或全部源搜索失败 | 安装/启用播放源插件 |
| 1010 | `source timeout` | 某个源超时 | 仅该源失败，其余结果照常返回 |
| 1011 | `request failed` | request API 执行失败（网络/协议/解析错误） | 重试或切换源 |
| 1012 | `unsupported capability` | 插件未声明该 `op` | 检查插件能力或改用其他源 |
| 1013 | `plugin disabled` | 插件被禁用、未加载或已崩溃隔离 | 在插件管理中启用/重启 |
| 1014 | `rate limited` | 触发源并发或 QPS 上限 | 降低频率、稍后重试 |
| 1015 | `lyric unavailable` | 本地与在线均无可用歌词 | **非致命**，降级为无歌词继续播放 |
| 1016 | `smtc unavailable` | 系统媒体控件注册/更新失败 | **非致命**，记录日志并稍后重试注册 |
| 1017 | `dsp init failed` | 调音处理链或音效节点初始化失败 | **非致命**，旁路该节点继续播放 |
| 1018 | `download failed` | 下载任务失败（网络中断/源不可用） | 保留断点，可重试或换源 |
| 1019 | `insufficient space` | 磁盘空间不足 | 暂停任务并提示清理缓存，不得删除曲库 |
| 1020 | `task not found` | 下载任务 id 不存在 | 刷新任务列表 |
| 1021 | `quality unsupported` | 该源不支持目标音质/格式 | 降级音质或更换音源 |
| 1022 | `checksum mismatch` | 下载完成校验失败（与源元数据不符） | 删除该文件后重试或换源 |
| 1023 | `base conflict` | `base.json` 唯一性冲突（`sha` / `location` / `platform:id` 重复） | 按 `conflict` 策略处理或人工确认 |
| 1024 | `tag parse failed` | 标签解析失败，已回退到文件名模板 | **非致命**，检查标签或命名模板 |
| 1025 | `unsupported format` | 音频格式不被支持 | 跳过该文件或先转码 |
| 1026 | `library locked` | 曲库被其他进程/导入任务占用 | 稍后重试，不得强行写入 |
| 1027 | `quality undefined` | 播放源未声明该音质，或 `quality_id` 无法映射 | 使用插件声明的音质 id/level，或降级 |
| 1028 | `base rebuild failed` | 缓存中的 `base.json` 缺失且重建失败（重解析/重拉取失败） | 重新导入该曲目，或检查源与文件可用性 |
| 1029 | `meta path mismatch` | `cache/meta/<song_key>/<quality_key>/` 目录名与 `base.json` 内容不一致（无法反推） | 视为损坏条目，删除该目录并重建 |
| 1030 | `playlist not found` | 歌单不存在 | 检查 id 或先创建歌单 |
| 1031 | `album not found` | 专辑不存在 | 检查 id 或先创建专辑 |
| 1032 | `mv not found` | MV 不存在 | 检查 sha，或先用 `mv_put` 登记 |

---

## 4. 文件与目录约定（补充） ##

```text
<music root>/
├─ api/
│  ├─ Readme.md           # 本文档
│  ├─ load.cpp            # 加载
│  ├─ start.cpp           # 播放
│  └─ resecrect.cpp       # 解密
├─ tracks/<sha>/          # 单曲持久目录（目录名 = sha，唯一）：只放音频等持久资源
│  ├─ sec.json            # 加密曲目密钥信息（secret=1 时存在）
│  ├─ lyric.lrc           # 歌词（可含 lyric.tr.lrc / lyric.roma.lrc）
│  └─ <audio file>
├─ tracks/.staging/       # 持久资源暂存（原子提交，崩溃后清理）
├─ covers/<hash>.<ext>    # 导入/在线提取的封面
├─ index/                 # 库索引（唯一键 → 音频路径/平台 id；与磁盘不一致时以磁盘为准）
├─ library.lock           # 库写锁（防止并发导入产生冲突）
├─ library/               # 用户数据（v0.10：歌单 / 专辑；不可被缓存清理）
│  ├─ playlists/<id>.json # 歌单（1.11）
│  └─ albums/<id>.json    # 专辑（1.12）
├─ mv/<sha>/              # MV（1.13）：mv.json + video.*（与 tracks/<sha>/ 组织方式对齐）
├─ logs/                  # 运行日志（1.14）：radmusic.log（+ .1 轮转）
├─ usec.json              # 用户密钥（会话级）
├─ presets/eq/            # 调音预设（用户维度，1.7）
├─ cache/                 # 缓存（可随时清理，1.9）
│  ├─ meta/.staging/<song_key>/<quality_key>/base.json.tmp  # 原子提交暂存
│  ├─ meta/<song_key>/         # 【base.json 存放位置】一首歌一个文件夹
│  │  └─ <quality_key>/        # 每个音质一个文件夹（防覆写）
│  │     └─ base.json          # 该音质唯一的元数据
│  ├─ audio/<sha>.<ext>   # 加载/解密的音频缓存
│  ├─ stream/             # 流媒体分片缓存
│  ├─ lyric/<sha>.json    # 归一化歌词缓存（含在线歌词）
│  ├─ cover/<hash>.<ext>  # 封面缓存（键为 pic URL 哈希）
│  └─ tmp/                # 未完成任务、下载分片（启动时清理）
└─ plugins/
   ├─ source/<plugin-id>/    # 播放源类 Function 插件（无界面，提供 platform + request 能力）
   │  ├─ plugin.json
   │  └─ <lib>
   └─ function/<plugin-id>/  # 其他类 Function 插件（有界面）
      ├─ plugin.json
      └─ ui/
```

- 缓存命名：`cache/audio/<sha>.<ext>`（无 `sha` 的在线曲目用 `platform:id`）；容量上限与淘汰策略见 [1.9 缓存](#19-缓存补充)。
- 缓存条目必须能用键反查元数据，避免仅按文件名匹配。
- **`base.json` 位于缓存内**：`cache/meta/<song_key>/<quality_key>/base.json`——**一首歌一个文件夹、每个音质一个文件夹**，路径本身即唯一键，从结构上防止不同音质互相覆写；缓存被清理后由索引（`index/`）+ 音频文件/在线源**重建到同一路径**，重建不得产生第二份记录、不得跨音质写同一目录。
- **用户数据与缓存分离**（v0.10）：`library/`、`mv/`、`presets/`、`logs/` 属于**持久数据**，`clear` / 淘汰**绝不删除**它们（见 2.11 规则 1）。
- 下载（1.8）与导入（1.10）把音频写入 `tracks/`、把 `base.json` 写入 `cache/meta/`：**清理缓存不得影响已入库音频与下载内容**。
- 封面区分两处：入库存档用 `covers/<hash>.<ext>`（永久，`base.json.pic` 指向它），在线临时封面用 `cache/cover/`（可清理）。
- **音质**：同一首歌的不同音质是不同的 `sha`，各自独立占用 `tracks/<sha>/`（音频）与 `cache/meta/<song_key>/<quality_key>/base.json`（元数据，**不同音质 = 不同目录，天然不覆写**），互不覆盖、重复存储（见 1.8 / 1.10）；目录与文件名可含音质标记便于人工识别（命名规则：待定）。
- 插件按类型分目录存放，宿主据此校验清单中的 `type`（目录与 `type` 不符则不加载）。

---

## 5. Function 插件标准（补充） ##

Function（功能）插件是 Rad music 的能力扩展单元，**分为两类：播放源类和其他类**。

### 5.1 插件分类（补充） ###

| 类型 | 清单 `type` | 界面 | 职责 | 能否提供 platform | 示例 |
| --- | --- | --- | --- | --- | --- |
| **播放源类** | `"source"` | **无界面** | 提供对一个或多个 `platform` 的支持，**声明该平台的音质定义**，并对外提供该源的 `request` 能力（搜索、取歌、取播放地址、取音质、歌词、封面等） | 能 | `netease`、`radish`、第三方音源（`local` 为内置源，无需插件） |
| **其他类** | `"function"` | **有界面** | 提供与音源无关的功能扩展，通过宿主 API 调用播放/搜索/request | 不能 | 歌词面板、均衡器、下载管理、桌面歌词、主题、音效、快捷键 |

规则：

1. **播放源类无界面**：不得注册任何 UI 入口，只能以 capability 形式被宿主或其他插件调用；其价值在于「提供更多播放源」。
2. **其他类有界面**：必须在清单中声明 UI 入口（页面/面板/菜单/设置项），且只在宿主提供的 UI 容器内渲染。
3. 只有播放源类插件能声明 `platforms`；其他类插件声明 `platforms` 视为非法（`1007 invalid metadata`）。
4. `request` 的可用源数量等于「已启用的播放源类插件」数量——**播放源类 function 越多，可用的 request 源越多**（见 2.6）。
5. 一个插件包只能声明一种类型（是否允许同包双类型：待定）。

### 5.2 播放源类插件接口（补充） ###

```cpp
// 草案：播放源类 Function 插件（无界面，提供 platform 支持 + request 能力）
class ISourcePlugin {
public:
    virtual ~ISourcePlugin() = default;
    virtual std::string id() const = 0;                          // 插件唯一标识
    virtual std::vector<std::string> platforms() const = 0;      // 提供的 platform 列表
    virtual bool supports(std::string_view version) const = 0;   // version 兼容性
    virtual std::vector<std::string> ops() const = 0;            // request 能力：search/get_url/...
    virtual std::vector<QualityLevel> quality_levels() const = 0; // 【本平台音质定义】

    virtual Result<BaseInfo> resolve(std::string_view id) = 0;               // 曲目标识 -> 元数据
    virtual Result<SearchItem> search(std::string_view kw, const Json& opt) = 0; // 搜索（多源之一）
    virtual Result<std::string> open(std::string_view id) = 0;               // 提供可读数据流/本地路径
    virtual Result<Json> request(std::string_view op, const Json& params) = 0;   // 多源 request
    virtual Result<std::vector<QualityLevel>> qualities(std::string_view id) = 0; // 曲目可用音质
};
```

- `quality_levels()` 是**该平台音质定义的唯一来源**：可用音质、标识（`quality_id`）、展示名（`label`）、归一化等级（`level`，取值见 1.1.1）与编码参数。宿主只做归一化、比较与展示，不得自行编造平台音质。

- 插件需声明支持的 `platforms`、`ops` 与 `version_range`；不兼容时报 `1008 unsupported version`。
- 搜索项必须归一化为 2.5 / 2.7 约定的字段（含 `duration`、`quality`、`available`），否则无法参与折叠。
- 插件不得绕过 `load.cpp` 的 sha 校验与 `resecrect.cpp` 的解密流程。

### 5.3 其他类（有界面）插件接口（补充） ###

```cpp
// 草案：其他类 Function 插件（有界面，不提供 platform）
class IFunctionPlugin {
public:
    virtual ~IFunctionPlugin() = default;
    virtual std::string id() const = 0;
    virtual std::vector<UiEntry> ui_entries() const = 0;   // panel / menu / settings / 悬浮窗 ...
    virtual Result<void> on_enable(HostApi& host) = 0;     // 注入宿主 API
    virtual Result<void> on_disable() = 0;
};
```

- 其他类插件通过 `HostApi` 调用既有能力：`load` / `start` / `search` / `request` / 事件订阅 / 曲库读写（需权限）；**不得自行实现 platform 支持**。
- 界面入口类型建议：`panel`（主面板）、`menu`（菜单项）、`settings`（设置页）、`overlay`（悬浮窗）。
- 界面技术栈（Web 视图 / 原生控件）与 UI API：待定。

### 5.4 插件清单 plugin.json（补充） ###

播放源类示例：

```json
{
  "id": "netease.source",
  "name": "网易云音乐源",
  "type": "source",
  "platforms": ["netease"],
  "api_version": "1",
  "version_range": ">=1.0.0",
  "entry": "netease_source.dll",
  "capabilities": ["search", "get_song", "get_url", "get_qualities", "get_lyric", "get_pic"],
  "quality_levels": [
    { "id": "standard", "label": "标准",   "level": "low",      "codec": "mp3",  "bitrate": 128,  "lossless": false },
    { "id": "higher",   "label": "较高",   "level": "medium",   "codec": "mp3",  "bitrate": 192,  "lossless": false },
    { "id": "exhigh",   "label": "极高",   "level": "high",     "codec": "mp3",  "bitrate": 320,  "lossless": false },
    { "id": "lossless", "label": "无损",   "level": "lossless", "codec": "flac", "bitrate": 1411, "sample_rate": 44100, "bit_depth": 16, "lossless": true },
    { "id": "hires",    "label": "Hi-Res", "level": "hires",    "codec": "flac", "sample_rate": 96000, "bit_depth": 24, "lossless": true }
  ],
  "limits": { "max_concurrency": 4, "qps": 5 },
  "permissions": ["network", "account"],
  "ui": null
}
```

其他类示例：

```json
{
  "id": "lyrics.panel",
  "name": "歌词面板",
  "type": "function",
  "platforms": [],
  "api_version": "1",
  "entry": "lyrics_panel.dll",
  "capabilities": [],
  "permissions": ["library_read"],
  "ui": { "panel": "ui/panel.html", "settings": "ui/settings.html", "menu": ["工具", "歌词面板"] }
}
```

- 播放源类：`ui` 必须为 `null`（无界面）；其他类：`ui` 必须至少有一个入口（有界面）。
- **音质定义（`quality_levels`）**：仅播放源类可声明，用于描述该平台提供的音质：
  - `id` 为该平台的音质标识（在同一插件内唯一），`label` 为展示名，`level` 必须取自 1.1.1 的归一化枚举；
  - 同一 `level` 可有多条（如 Hi-Res 96k/192k），宿主按 1.1.1 的比较规则排序；
  - 未声明 `quality_levels` 的播放源：其曲目 `quality.level` 记为 `unknown`（可用但不可比较）；
  - 其他类插件声明 `quality_levels` 视为非法（`1007 invalid metadata`）。
- 类型与 `ui` 字段不匹配、或 `platforms` 与类型矛盾的清单，一律拒绝加载（`1007 invalid metadata`）。

### 5.5 加载、权限与沙箱（补充） ###

- 插件按 `plugins/source/`、`plugins/function/` 分类加载；清单缺失、`api_version` 不兼容或类型与目录不符则不加载（`1008` / `1007`）。
- 权限最小化：`network`（由宿主代理请求）、`account`（可访问登录态）、`library_read` / `library_write`（读写曲库）、`audio_dsp`（注册调音节点，见 1.7）。未声明的权限一律拒绝。
- 宿主要求：统一代理网络请求、强制限流与超时、托管凭据、隔离插件崩溃（标记 `1013 plugin disabled`，不得带崩主程序）。
- 排序与优先级（待定）：多个插件声明同一 `platform` 时，按用户配置的源优先级只启用其一，或按优先级依次降级。

---

## 6. 版本与兼容性（补充） ##

- `base.json.version` 表示曲目元数据/文件版本，格式建议 `主.次.修订`。
- API 版本：`v1` 冻结后，新增字段必须可选，删除/改名视为破坏性变更。
- 插件 `api_version` 与宿主 API 版本不匹配时，拒绝加载并提示升级（`1008`）。
- 破坏性变更必须提升文档主版本号（本文档当前 v0.10），并在[变更记录](#8-变更记录)中说明。

### 6.1 v0.10（歌单 / 专辑 / MV / 日志）的兼容性承诺 ###

v0.10 是**纯增量**版本：老数据、老客户端、老插件都不需要改动。

| 维度 | 承诺 |
| --- | --- |
| `base.json` | **未新增必填字段**；`quality` / `album` / `duration` / `track_no` / `tag_source` / `item_id` 仍为可选；v0.9 及更早的 `base.json` 可直接读取 |
| 缓存布局 | `cache/meta/<song_key>/<quality_key>/base.json` 不变；`cache/` 下新增内容只出现在不冲突的位置 |
| 目录 | `library/`、`mv/`、`logs/` 都是**新增目录**，旧曲库缺少它们时按需创建，不视为损坏 |
| API 响应 | 所有既有接口的字段**只增不改**；搜索响应的 `results` 仍是曲目数组，专辑/MV 结果放在**新增的** `albums` / `mvs` 字段里 |
| 错误码 | 0 / 1001–1029 语义不变；新增 1030–1032 |
| 插件 | `plugin.json` 未新增必填项；`capabilities` 里新增 `get_playlist` / `get_album` / `get_mv` 属可选能力，未声明的插件行为不变（调用时返回 `1012`） |
| 日志 | 默认**不输出**（保持库的静默性），只有宿主显式配置后才产生日志与 `logs/` 目录 |

客户端实现建议：**忽略未知字段、未知错误码按 `message` 展示**，即可与后续小版本兼容。

---

## 7. 待补充 / 待定事项 ##

- [ ] 解密详细步骤（原文：未规划详细步骤，待补充；本版已在 1.4 给出草案，待评审）
- [ ] `sha error` 是警告放行还是阻断加载
- [ ] 是否允许播放接口隐式触发加载
- [ ] 解密后的明文落盘策略与清理时机
- [ ] `usec.json` 是否允许保存明文密钥
- [ ] 插件加载顺序与远程插件清单机制
- [ ] 多个插件声明同一 `platform` 时的优先级/降级策略
- [ ] 一个插件包是否允许同时是播放源类与其他类
- [ ] 其他类插件的界面技术栈与 UI API（Web 视图 / 原生控件）
- [ ] 折叠键细节：时长容差、是否忽略 `(Live)`/`(Remastered)`/`feat.` 后缀、是否参与 `album`
- [ ] 搜索默认并发数、单源超时与整体超时
- [ ] 跨页折叠（同一首歌出现在不同页）如何合并计数
- [ ] 是否允许插件自行发起网络请求，还是必须经宿主 `request` 代理
- [ ] request 的写操作（如收藏、下载）是否需要独立接口与额外确认
- [ ] 缓存容量上限与淘汰策略
- [ ] 歌词编码检测（GBK/GB18030）与转码策略
- [ ] 加密曲目的歌词是否单独加密
- [ ] 歌词偏移持久化（`persist`）是否需要用户二次确认
- [ ] 在线歌词缓存的有效期与失效策略
- [ ] 无时间轴歌词（`timed = false`）的展示方式
- [ ] SMTC `buffering` 状态映射（playing / paused）与进度上报间隔取值
- [ ] 非 Windows 平台的系统媒体控件等价实现及其能力差异
- [ ] 调音链节点顺序是否可配置、是否支持第三方音效格式
- [ ] 调音是否支持离线导出处理后的音频
- [ ] 调音的设备级预设与采样率/独占输出策略
- [ ] 下载命名模板默认值、同名文件处理与元数据写入策略
- [ ] 断点续传是否强制支持、`.part` 残留的过期清理
- [ ] 下载并发/限速默认值，以及下载与播放的带宽优先级
- [ ] 缓存容量默认值与各层配额比例
- [ ] 解密明文的缓存留存时长与清理策略
- [ ] 缓存预读（下一首预取）是否默认开启
- [ ] 导入模式默认值（`copy` / `move` / `link`）与 `link` 是否允许绝对路径 `location`
- [ ] 冲突默认策略（`skip` / `merge`）及 `merge` 时的字段优先级
- [ ] 标签编码检测范围（UTF-8 / UTF-16 / GBK）与文件名模板默认规则
- [ ] 库索引存储格式（JSON 文件 vs SQLite）与规模上限
- [ ] 重复导入时是否顺带更新已有条目的封面/歌词
- [ ] 导入曲目的初始 `version` 取值规则
- [ ] 导入扩展字段（`album` / `duration` / `track_no` / `tag_source`）是否升为标准字段
- [ ] 归一化音质等级集合是否固定为 `hires`/`lossless`/`high`/`medium`/`low`，是否再细分（如无损 24bit 独立等级）
- [ ] VBR 与无损无码率时的音质比较、排序口径
- [ ] 同曲同质的判定是否计入 `codec`（320k mp3 与 320k aac 是否视为同一音质）
- [ ] 同一音质等级下不同采样率/Hi-Res 规格是否允许共存
- [ ] 平台新增音质时是否需要宿主白名单或版本门限
- [ ] 同曲同质重复下载的默认策略（`skip` / `re-download`）
- [ ] 多音质记录的目录/文件命名与人工识别规则
- [ ] 默认音质偏好与自动降级顺序的存储位置（2.13 规则 5）
- [ ] 歌单是否支持嵌套（歌单文件夹）、共享与导入导出格式
- [ ] 专辑是否需要多艺术家/合辑（Various Artists）字段
- [ ] MV 是否支持多清晰度（多版本目录）与字幕轨
- [ ] MV 与曲目的关联是否需要在 `base.json` 中显式记录 `mv_sha`
- [ ] 日志保留天数与滚动策略（当前只保留一份 `.1`）
- [ ] 日志是否支持按模块过滤写入文件、以及结构化 JSON 行格式
- [ ] 多线程宿主下 logger 的串行化方案
- [ ] `base.json` 在缓存中的留存策略（是否随缓存清理一并清空、是否保留最近 N 条）
- [ ] `song_key` 派生是否引入时长/专辑以区分同名不同曲（当前只用归一化「标题 + 主作者」，保证跨来源稳定）
- [ ] `quality_key` 对本地多版本（同 `level`+`codec` 但采样率/位深不同）如何唯一化（当前 `local.<level>-<codec>-<bitrate>` 可能碰撞）
- [ ] 同一 `quality_key` 目录内出现不同内容实体（不同压制版）时的默认策略（当前拒绝覆写并报 `1023`）
- [ ] 库索引 `index/` 是否也属于缓存（当前为持久指针）及其存储格式
- [ ] 在线 `base.json` 的有效期与失效判定（依赖 token 时效还是固定 TTL）
- [ ] `meta` 层全量重建的触发方式（启动扫描 / 懒重建 / 手动）
- [ ] HTTP 服务是否属于标准的一部分（当前为可选）
- [ ] MD5 兼容分支的退出计划

---

## 8. 变更记录 ##

| 版本 | 日期 | 说明 |
| --- | --- | --- |
| v0.1 | — | 初版：基本信息、加载、解密（部分） |
| v0.2 | — | 补充 base.json 字段规范与示例、加载状态机与伪代码、播放控制接口与事件、usec.json / sec.json 结构与解密流程、错误码、目录约定、插件标准、版本策略、待定事项 |
| v0.3 | — | 新增 Function 插件定义（播放源类无界面 / 其他类有界面）、播放源类与其他类接口、plugin.json 清单与权限沙箱；新增搜索 API（多源聚合）与折叠显示要求（2.5 / 2.7）；新增 request API（由播放源类插件提供更多源，2.6）；补充错误码 1009–1014；目录结构与术语同步更新 |
| v0.4 | — | 新增歌词加载标准（1.5：来源优先级、LRC/逐字/翻译/罗马音、归一化 lyric.json、同步与偏移规则）与歌词 API（2.8）；新增系统媒体控件 SMTC 支持要求（1.6：会话注册、元数据/状态/时间轴同步、控制响应、降级与插件边界）；补充错误码 1015–1016；目录结构增加歌词与歌词缓存；术语、待定事项同步更新 |
| v0.5 | — | 新增调音（音频处理链，1.7：EQ/增益/音效节点、实时生效、失败旁路、预设）与调音 API（2.9）；新增下载（1.8：任务字段、取址校验入库流程、并发与磁盘规则）与下载 API（2.10）；新增缓存（1.9：分层、键规则、LRU/待淘汰标记、写入与安全策略）与缓存 API（2.11）；补充错误码 1017–1022；目录结构增加 presets/eq 与缓存分层；明确「缓存可清理 vs 下载永久」边界；插件权限增加 `audio_dsp` |
| v0.6 | — | 新增音乐导入（1.10：扫描/标签解析/文件名回退/封面歌词提取/生成 base.json 流程、导入模式、**base.json 唯一性与防冲突规则**、冲突类型与处理、原子提交与库锁）与导入 API（2.12：scan/start/status/cancel/report、dry_run 与冲突报告）；1.1 增加唯一性约束；补充错误码 1023–1026；目录结构增加 covers/、index/、tracks/.staging/、library.lock；2.7 明确本地曲目同样参与折叠 |
| v0.7 | — | `base.json` 新增 **`quality` 音质定义**（1.1.1：归一化等级 `hires`/`lossless`/`high`/`medium`/`low`、编码/码率/采样率/位深/声道/无损、平台 `quality_id` 与 `label`、比较规则）；明确**不同音质必须重复存储、各自独立成条**（1.8 规则 1、1.10 唯一性与冲突表）；播放源插件**声明所属平台的音质定义**（5.2 `quality_levels()`、5.4 `quality_levels` 清单与校验规则、`get_qualities` op）；新增音质 API（2.13）；2.5 搜索响应的音质字段改为对象并新增 `qualities[]`；2.7 增加音质维度；2.10 支持按音质分别下载并回传 `quality_resolved`；1.10 导入解析音质参数；补充错误码 1027 |
| v0.8 | — | **`base.json` 改为存放在缓存内**（`cache/meta/<key>/base.json`，键 = `sha` 或 `platform_id`）：1.1 新增「存放位置」与可重建要求；1.9 缓存分层与策略明确 `meta` 层可清理、可重建、清理前保护正在播放的条目；1.3 加载流程增加 `resolving_base`（缺失先重建）；1.8 / 1.10 落盘路径改为「音频入 `tracks/`、`base.json` 入 `cache/meta/`」，原子提交用 `cache/meta/.staging/`；2.11 新增 `cache_rebuild` 与 `meta` 层统计；目录结构、2.4 示例、2.12 报告路径同步；补充错误码 1028 |
| v0.9 | — | `base.json` 缓存改为**两级目录：一首歌一个文件夹、每个音质一个文件夹**（`cache/meta/<song_key>/<quality_key>/base.json`），**以路径作为唯一键防止覆写**：1.1「存放位置」给出目录树、`song_key`/`quality_key` 派生规则（含文件名安全化与长度限制）、5 条防覆写规则与一致性反推校验；1.9 键规则与策略、1.10 落盘/唯一性/冲突表/规则 8-9（幂等与写入前检查）、1.8 入库路径、4 节目录树、2.11 `cache_rebuild` 精确定位、2.12 冲突报告路径同步；新增错误码 1029 `meta path mismatch`；术语补充 `song_key`/`quality_key` |
| v0.10 | — | 新增**歌单（1.11）**、**专辑（1.12）**、**MV（1.13）**、**运行日志（1.14）**四块能力与对应 API（2.14–2.17）；新增错误码 1030–1032；目录新增 `library/playlists/`、`library/albums/`、`mv/<sha>/`、`logs/`；6.1 给出**纯增量兼容性承诺**（base.json 无新增必填字段、旧缓存布局不变、旧接口字段只增不改、插件新能力全可选、日志默认静默）；插件 `capabilities` 新增可选 `get_playlist`/`get_album`/`get_mv` |
| v0.11 | — | 新增**数据管理 API（2.18）**：`GET /music/data` 按分类统计占用，`POST /music/data/purge` 按分类清除全部数据（需 `confirm: "PURGE"`，支持 `dry_run`）；设置页新增「数据」页；`tracks` 类连带 `cache/meta`，清除后重置索引并重建目录布局；日志清除前释放文件句柄并自动恢复 |
| v0.12 | — | 新增**检查更新 API（2.20）**：`GET /music/update` 读状态，`POST /music/update/{check,download,apply}` 检查 GitHub Releases / 分片下载本平台安装包 / 解压覆盖并重启（`confirm: "UPDATE"`）；版本号由构建注入 `RADMUSIC_VERSION`；新增**归档解压**（inflate / zip / tar.gz，逐条目 CRC-32 校验 + zip slip 防护）；安装包与替换脚本存放于**可执行文件同级** `updates/`，数据根与设置不受影响；设置页「关于」新增「版本与更新」卡片 |
