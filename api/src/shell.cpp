// Rad music API - 系统文件管理器实现（见 shell.hpp）
#include "radmusic/shell.hpp"

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // 必须放在 windows.h 之后：它依赖 EXTERN_C / SHSTDAPI 等宏
#endif

namespace radmusic {
namespace {

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return u8.empty() ? std::string() : std::string(reinterpret_cast<const char*>(u8.c_str()));
}

// 统一成「绝对路径 + 原生分隔符」：曲库里存的 location 用 / 分隔，
// 混着给 explorer 传参虽然通常也能用，但路径里有特殊字符时容易出错。
std::string native_absolute(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path out = std::filesystem::absolute(p, ec).lexically_normal().make_preferred();
    return path_to_utf8(out);
}

#ifdef _WIN32
std::wstring to_wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
#endif

}  // namespace

std::string containing_folder(const std::string& path) {
    if (path.empty()) return std::string();
    std::error_code ec;
    const std::filesystem::path target(path);
    if (std::filesystem::is_directory(target, ec)) return native_absolute(target);
    const std::filesystem::path parent = target.parent_path();
    return parent.empty() ? std::string() : native_absolute(parent);
}

Status reveal_in_file_manager(const std::string& path) {
    if (path.empty()) return Status::error(ErrorCode::RequestFailed, "路径为空");

    std::error_code ec;
    const std::filesystem::path target(path);
    const std::filesystem::file_status st = std::filesystem::status(target, ec);
    if (ec || !std::filesystem::exists(st)) {
        return Status::error(ErrorCode::FileNotFound, "路径不存在: " + path);
    }

#ifdef _WIN32
    // explorer 的 /select, 参数要求绝对路径 + 反斜杠分隔
    const std::wstring wide = to_wide(native_absolute(target));
    if (wide.empty()) return Status::error(ErrorCode::RequestFailed, "路径编码转换失败: " + path);

    HINSTANCE result;
    if (std::filesystem::is_directory(st)) {
        result = ShellExecuteW(nullptr, L"explore", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        // 选中文件：\select 只能带一个路径，且路径里的引号要转义
        const std::wstring param = L"/select,\"" + wide + L"\"";
        result = ShellExecuteW(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
    }
    const INT_PTR code = reinterpret_cast<INT_PTR>(result);
    if (code <= 32) {
        return Status::error(ErrorCode::RequestFailed,
                             "打开文件管理器失败（ShellExecute 返回 " + std::to_string(code) + "）");
    }
    return Status::success();
#else
    return Status::error(ErrorCode::RequestFailed, "当前平台暂不支持打开文件管理器");
#endif
}

}  // namespace radmusic
