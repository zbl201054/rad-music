// Rad music API - 本地曲目联网识别（1.10 扩展）
//
// 用途：导入/已入库的本地曲目，按「歌名 + 歌手 + 时长」到各播放源搜索匹配，
// 命中后把在线版本信息（match_platform + item_id）与网络海报写回 base.json，
// 使播放器可以把同一首歌的「本地文件版本」与「在线音质版本」放在一起自由切换。
//
// 设计要点：
//  * 只读曲库 + 插件，匹配规则全部本地计算（归一化歌名/歌手/时长容差），不依赖第三方库；
//  * 网络搜索同步串行调用各源（与 search.cpp 一致：无锁、无线程）；
//  * 海报经 httpclient 下载到 cache/tmp，校验魔数后落 covers/<hash>.<ext>（与导入内嵌封面同规则）；
//  * 回写一律走 MetaCache::put（唯一性/防覆写由存储层兜底），并同步 LibraryIndex；
//  * 任何单曲失败都不影响同批其他曲目，结果逐条返回。
#pragma once

#include <string>
#include <vector>

#include "radmusic/json.hpp"
#include "radmusic/plugin.hpp"
#include "radmusic/storage.hpp"

namespace radmusic {

struct IdentifyOptions {
    std::vector<std::string> shas;   // 待识别的本地曲目 sha（按文件内容计）
    std::string source;              // 限定音源 platform（空 = 所有启用的播放源）
    int page_size = 8;               // 每个源搜索返回的候选条数
    bool fetch_pic = true;           // 命中后是否抓取网络海报
    bool force = false;              // 已关联过在线版本的也重新识别
    int max_shas = 50;               // 单次批量上限（防御）

    static IdentifyOptions from_json(const json::Value& v);
};

class IdentifyService {
public:
    IdentifyService(const LibraryPaths& paths, MetaCache& meta, LibraryIndex& index,
                    PluginRegistry& plugins);

    // 请求：{"sha": "..."} 或 {"shas": [...]}，可选 source / fetch_pic / force
    Result<json::Value> identify(const json::Value& request);

private:
    Result<json::Value> identify_one(const std::string& sha, const IdentifyOptions& opt);

    // 在启用源里搜索并挑出最佳匹配；matched=false 时 reason 说明原因
    struct Candidate {
        bool matched = false;
        std::string reason;
        std::string platform;
        std::string item_id;
        std::string name;
        std::string author;
        std::string album;
        int duration_seconds = 0;
        std::string pic_url;
        int score = 0;
        int duration_diff = -1;
        int quality_rank_ = 0;   // 最佳音质等级 rank（平分时优先高音质）
        std::vector<Quality> qualities;
        Quality best_quality;

        int quality_rank() const { return quality_rank_; }
    };
    Candidate find_match(const BaseInfo& base, const IdentifyOptions& opt);

    // 下载海报 → covers/<hash16>.<ext>；成功返回相对路径，失败返回空串
    std::string save_cover_from_url(const std::string& url);

    const LibraryPaths& paths_;
    MetaCache& meta_;
    LibraryIndex& index_;
    PluginRegistry& plugins_;
    int tmp_seq_ = 0;
};

}  // namespace radmusic
