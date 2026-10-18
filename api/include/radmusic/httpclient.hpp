// Rad music API - 网络传输（仅用于下载落盘，零第三方依赖）
//
// Windows 上走 WinHTTP（系统组件，需链接 winhttp.lib），支持 HTTPS 与 Range 断点续传；
// 其他平台不提供实现，调用一律返回 false（由宿主注入 fetch 替代）。
#pragma once

#include <filesystem>
#include <string>

namespace radmusic {

// 是否为 http(s) 地址
bool is_remote_url(const std::string& url);

// 把 url 下载到 out_path：
//   * received 传入已下载字节数（>0 时带 Range 续传），实际进度回写；
//   * total 回写总字节数（服务器未给 Content-Length 时为 -1）；
//   * 失败返回 false，error 填原因（含 "insufficient space" 时上层判定为 1019）。
bool http_fetch_file(const std::string& url, const std::filesystem::path& out_path,
                     long long* received, long long* total, std::string* error);

}  // namespace radmusic
