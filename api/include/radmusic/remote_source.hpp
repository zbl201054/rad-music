// Rad music API - 外部进程播放源适配器（Readme.md 5.5「加载、权限与沙箱」）
//
// 宿主内置的播放源是编译进 radmusic_server 的 C++ 类；但真实音源（如网易云）通常由
// 更容易访问网络的独立进程实现。本适配器把「通过本地 HTTP JSON-RPC 通信的插件进程」
// 包装成一个普通的 ISourcePlugin，于是搜索 / 播放 / 歌词 / 音质 / 下载等既有链路无需改动。
//
// 协议（插件进程实现）：
//   POST <endpoint>/request        Content-Type: application/json
//   {"op":"search","params":{...}}
//   → {"code":0,"data":{...}}      非 0 时 code 直接使用本项目的错误码
//   GET  <endpoint>/health          → 200 表示插件就绪
//
// 支持的 op：search / get_song / get_url / get_qualities / get_lyric / get_pic
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "radmusic/plugin.hpp"

namespace radmusic {

struct RemoteSourceOptions {
    std::string id = "remote.source";
    std::string name = "远程播放源";
    std::string version = "1.0.0";
    std::vector<std::string> platforms;             // 例：{"netease"}
    std::vector<std::string> capabilities;          // search/get_song/get_url/get_qualities/get_lyric
    std::vector<QualityLevelSpec> quality_levels;   // 插件声明的音质清单
    std::string endpoint = "http://127.0.0.1:8848"; // 插件进程地址
    int timeout_ms = 15000;                         // 单次 RPC 超时（网易云搜索有时较慢）
    PluginSettingsSchema settings_schema;           // 插件自描述的可配置项（设置面板渲染用）
};

// 创建一个把请求转发给插件进程的播放源
std::shared_ptr<ISourcePlugin> make_remote_source(RemoteSourceOptions options);

// 探活：GET <endpoint>/health 是否返回 200。用于启动时等待插件进程就绪。
bool remote_source_ping(const std::string& endpoint, int timeout_ms = 1500);

// 从 endpoint 中解析 host / port（默认 80）与路径前缀
bool parse_endpoint(const std::string& endpoint, std::string* host, uint16_t* port, std::string* path_prefix);

// 拉起插件进程（分离、无窗口）。command 为 argv 形式，cwd 为插件目录（相对命令按它解析）。
// endpoint 非空时会在退出清理阶段先 POST 一次 /shutdown（插件能自己收尾，比硬杀干净），
// 叫不动再 TerminateProcess。拉起的进程会登记下来，宿主退出时由 shutdown_plugin_processes() 一起关掉。
bool spawn_plugin_process(const std::vector<std::string>& command, const std::string& cwd,
                          const std::string& endpoint = std::string());

// ---------------------------------------------------------------- 插件进程生命周期
//
// 插件是独立进程（Python / node），宿主退出时必须主动收尾，否则它们会一直挂着：内存白占、
// 端口（8848 等）不放，用户任务管理器里看到一堆孤儿 python.exe。
//
// 两种来源分开处理（都在本模块内记账，单线程模型下无需加锁）：
//   * 宿主拉起的：spawn_plugin_process() 把进程句柄存下来，退出时 TerminateProcess 兜底；
//   * 启动时认领的（ping 发现端口上已经有健康实例）：拿不到句柄，只能请求它自己退出
//     （POST <endpoint>/shutdown），老版本插件没有这个端点时只能作罢。
//
// 只在「真正退出」的路径上调用：重启（数据迁移 / 手动重启）时不关 —— 新实例会认领这些
// 仍在运行的插件，关了反而会让新实例面对一个死掉的 endpoint。
//
// 实现顺序（grace_ms 是整体预算，不是每个进程的上限）：
//   1. 给所有已知插件（认领来的 + 自己拉起的）各发一次 POST /shutdown，超时很短；
//   2. 在 grace_ms 内统一等它们自己退出，先退的不占后面进程的时间；
//   3. 到点还活着的 TerminateProcess。
// 这样 N 个插件的总耗时不会随 N 线性膨胀，桌面壳的等待窗口才等得起。
void remember_adopted_plugin(const std::string& endpoint);
void shutdown_plugin_processes(int grace_ms = 2000);
// 当前记账中的插件进程数（诊断 / 测试用）
int live_plugin_processes();

}  // namespace radmusic
