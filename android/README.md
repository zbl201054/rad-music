# Rad Music · Android 端

把同一个 Rad Music 装进手机：**C++17 服务端交叉编译成 arm64 可执行文件，随 APK 一起打包**，
界面仍然是仓库里那套 `web/`（同一份 HTML/JS/CSS，不做第二套 UI），用应用内的 WebView 加载
`127.0.0.1:8765`。所以它是一个装机即用的独立 App，不需要装浏览器、不需要手动开网页。

---

## 两种使用方式

启动后会让你选一次，之后随时可以在**通知栏的「切换服务器」**里改。

| 模式 | 曲库在哪 | 在线音源（网易云 / 哔哩哔哩） | 需要电脑 |
| --- | --- | --- | --- |
| **本机播放**（默认） | 手机内部存储 `…/files/radmusic-data/` | ✓ 可用（内置 Java 插件，见下） | 不需要 |
| **连接电脑** | 电脑上的曲库 | ✓ 完整可用（含登录、无损） | 需要，同一 Wi-Fi |

> 手机上跑不了 Python，所以桌面端的三个 Python 插件在这里换成了**三个内置的 Java 插件**：
> 以线程形式跑在 app 内，监听 `127.0.0.1:8848`（网易云）、`8849`（B 站）、`8850`（推荐），
> 仍然走「HTTP JSON-RPC」这套原有插件协议 —— 所以 C++ 服务端一行都没改。

### 三个插件的覆盖情况

| 插件 | op 数 | 说明 |
| --- | --- | --- |
| 哔哩哔哩 | 17/17 | 搜索、曲目、播放地址、音质、歌词、封面、歌单（收藏夹/UP 投稿/合集）、MV、UP 主搜索与主页、收藏夹列表、登录（Cookie）、设置 |
| 网易云音乐 | 18/20 | 搜索（单曲/歌单/专辑/歌手/MV）、歌单与专辑详情、歌手热歌、MV、歌词、封面、Cookie 登录、设置；**扫码与手机号登录未实现**（需要额外接口与轮询，请用 Cookie 登录） |
| 智能推荐 | 11/11 | 每日推荐、漫游、探索、相似、心情电台、推荐流、偏好读写、反馈、模式列表、设置 |

两处与桌面版的实际差距：

1. **音质**：网易云只走公开接口，最高 **128K 标准**；B 站 64/132/192K 可用，Hi-Res 与杜比需大会员。要无损请用「连接电脑」模式。
2. **推荐打分**：桌面版有一套精细的相关度/新颖度打分，Android 版做了简化（源权重 + 关键词命中 + 去重），
   **模式与返回字段完全一致**，前端推荐页无需区分。

连接电脑时的用法：

1. 电脑上用 `radmusic_desktop.exe`（或原来的 `start_radmusic.bat`）启动；
2. 记下电脑的局域网 IP（`ipconfig` 里的 IPv4 地址）；
3. App 里选「连接电脑」，填 `192.168.x.x:8765`；
4. 如果连不上，先在手机浏览器打开这个地址确认能通 —— 多半是电脑防火墙挡了 8765 端口。

### 内置音源是怎么跑的

```
WebView ──> C++ 服务端(127.0.0.1:8765) ──HTTP JSON-RPC──> Java 插件(8848/8849) ──HTTPS──> 网易云 / B站
                    │                                              │
                    └──── /music/audio 音频流                        └── /media 代理（带 Referer，转发音频流）
```

- 插件清单 `plugin.json` 随 APK 打包（`assets/plugins/`），首次启动解压到 `<曲库>/plugins/`，
  服务端按原逻辑扫描、按 `endpoint` 探活；
- 清单里**没有 `command`**，服务端不会尝试拉起子进程（手机上没有 `python`）；
- 网易云 / B 站的音频 CDN 会校验 Referer，所以播放地址指向插件的 `/media` 端点，
  由插件带 Referer 转发，并透传 `Range`（拖动进度条可用）。


---

## 构建

需要 JDK、Android SDK（platform 34 / build-tools 34 / NDK 26.1）。第一次构建会自动下载 R8。

```bash
cd android
./build_apk.sh                 # 完整构建（含交叉编译 C++ 服务端）
./build_apk.sh --skip-native   # 只改了 Java / 前端时，跳过原生编译，快很多
```

产物：`android/build/radmusic-<版本>-arm64.apk`（约 3.4 MB）

脚本做的事：

1. 用 NDK 的 `aarch64-linux-android24-clang++` 把 `api/src/*.cpp` + `api/tools/radmusic_server.cpp`
   编成 `libradmusic_server.so`（静态链接 libc++，只依赖 libc/libm/libdl，16KB 页对齐）；
2. 把 `web/` 复制进 `assets/`，并写一个 `web_version.txt` 用于判断要不要重新解压；
3. `aapt2` 编资源 → `javac` → `d8`（用独立的最新 R8，build-tools 自带的 8.2 在 JDK 21 上会崩）→ 组装；
4. `zipalign` + `apksigner`（调试签名，首次构建自动生成 `android/keystore/`）。

> APK 里刻意把可执行文件命名成 `lib…so` 并开启 `extractNativeLibs="true"`：
> Android 只允许从只读的 `nativeLibraryDir` 执行原生程序，App 私有目录在 Android 10+ 被 W^X 禁止 exec。

## 安装

```bash
adb install -r android/build/radmusic-0.13.3-arm64.apk
```

或者把 APK 传到手机上点开安装（需要允许「安装未知来源应用」）。

首次启动会请求两个权限：

- **通知**：前台服务常驻通知（Android 13+ 需要），不授予的话后台保活会差一些；
- **音乐和音频**：导入手机里的本地音乐时读取文件用。

---

## 实现要点

| 文件 | 职责 |
| --- | --- |
| `MainActivity.java` | WebView 外壳、模式选择、文件选择、下载、返回键、权限 |
| `LocalServer.java` | 拉起 / 停止原生服务端进程、解压 `assets/web`、端口探活、失败自诊断 |
| `ServerService.java` | 前台服务 + 常驻通知（带「切换服务器」入口），保证后台不被回收 |

几个和桌面端不同的地方：

- **数据目录**：`/data/user/0/com.radmusic.app/files/radmusic-data/`，卸载即清理，不需要存储权限；
- **日志**：`…/files/logs/server.log`，服务端 stdout/stderr 都重定向到这里；
- **服务端参数**：`--root <私有目录> --web <私有目录>/web --port 8765 --no-plugin-autostart --no-log-console`；
- **自更新**：Android 上不装配 `UpdateService`，`/music/update` 会返回「未装配更新服务」，
  版本更新走 APK 安装；
- **「打开所在文件夹」**：Android 没有这个概念，接口返回不支持。

## 已知限制

1. **内置音源的能力边界**：网易云只走公开接口（不登录、无 weapi/eapi 加密），最高 128K 标准音质，
   VIP / 版权受限曲目拿不到地址；B 站可拿 64K / 132K / 192K 三档，Hi-Res 与杜比需要大会员。
   想要无损和登录态，请用「连接电脑」模式（电脑上跑的是完整 Python 插件）。
2. **后台播放**：WebView 的 `<audio>` 在 App 切后台时通常能继续播（已刻意不调 `web.onPause()`），
   但没有 MediaSession，锁屏 / 通知栏没有播放控制，部分 ROM 仍可能在一段时间后冻结进程。
3. **划掉最近任务**会结束本地服务（音乐停止），这是当前的设计。
4. 只提供 **arm64-v8a**（近五年的手机都是这个架构；x86 设备与模拟器暂不支持）。
5. MV 播放能力取决于 WebView 内核，B 站 mp4 正常；部分 HLS 流可能不支持。
6. 插件日志在 `…/files/logs/`（服务端）与 logcat 的 `RadMusicPlugin` tag（Java 插件）。


## 排错

**闪退 / 点按钮后自动退出**：App 会把未捕获异常写进 `…/files/logs/crash.log`，
下次打开时会弹窗把上次的堆栈贴出来 —— 截图即可反馈，不用连电脑。



启动失败时页面会直接显示**进程状态 + 服务端日志末尾**，不用连电脑。常见原因：

| 现象 | 原因 |
| --- | --- |
| 「服务进程：已退出（code=127）」 | 原生库不匹配（装了 x86 设备）或无法执行 |
| 「服务进程：运行中但端口未就绪」 | 数据目录不可写、端口被占用 |
| 页面能开但没声音 | 未授予音频权限、或曲目没有落盘（本机模式需先导入） |
| 连接电脑失败 | 不在同一 Wi-Fi、电脑防火墙、地址端口写错 |
