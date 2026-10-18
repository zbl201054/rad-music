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

// 拉起插件进程（分离、无窗口）。command 为 argv 形式，cwd 为插件目录（相对命令按它解析）
bool spawn_plugin_process(const std::vector<std::string>& command, const std::string& cwd);

}  // namespace radmusic
