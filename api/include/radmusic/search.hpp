#pragma once
#include <string>
#include <vector>
#include "radmusic/error.hpp"
#include "radmusic/model.hpp"
#include "radmusic/plugin.hpp"

namespace radmusic {

// 源调用结果（由调用方提供实现，便于测试注入假源）
struct SourceSearchOutcome {
    std::string platform;
    ErrorCode code = ErrorCode::Ok;
    std::string message;
    long long elapsed_ms = 0;
    std::vector<SourceEntry> entries;   // 该源返回的条目（可带 base/title/artists）
};

// 折叠：把同一首歌的多个源条目聚合为一条（2.7），并排序
//  - 折叠键：group_key(title, 主作者, duration)，时长容差 ±3 秒
//  - 同一首歌同一平台只保留一条（该平台的最高音质）；不同平台分别保留在 sources[]
//  - 折叠行 title 取第一个可用条目的标题；best_source 取源优先级最高且 available 且音质最高的平台
//  - 不可用源仍然保留在 sources[] 中（灰显由前端决定）
std::vector<SearchItem> fold_sources(std::vector<SourceEntry> entries,
                                     const std::vector<std::string>& source_priority);

// 分页：对折叠后的结果切页（page 从 1 开始，page_size<=0 视为 20，上限 100）
std::vector<SearchItem> paginate(const std::vector<SearchItem>& items, int page, int page_size);

// 编排一次多源搜索：
//  - 只调用 request.sources 中指定的源（空 = 全部已启用）；platform 不匹配的源直接跳过
//  - host_entries：宿主自己产生的条目（如本地曲库搜索结果），参与折叠/筛选/分页；
//    host_platform 非空时会在 sources_status 中补一条状态（前端据此展示"本地 N 条"）
//  - 单源失败/超时 → 记入 sources_status（code 1010 等），整体仍返回成功
//  - 全部源失败或没有可用源 → 返回 1009
//  - fold=true 时按 2.7 折叠，否则每个条目单独成行
//  - 过滤（quality_min / duration_min / duration_max / available_only）与排序（sort）
//    在折叠后、分页前执行，保证 total 与分页计数一致
Result<SearchResponse> execute_search(const SearchRequest& request,
                                      const std::vector<std::shared_ptr<ISourcePlugin>>& sources,
                                      const std::vector<std::string>& source_priority,
                                      const std::vector<SourceEntry>& host_entries = {},
                                      const std::string& host_platform = {});

// 把插件返回的 SearchItem 转成 SourceEntry（补 platform/available/quality）
SourceEntry entry_from_item(const std::string& platform, const SearchItem& item);

}  // namespace radmusic
