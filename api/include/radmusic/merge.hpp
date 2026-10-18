// Rad music API - 自定义合并（Readme 1.10 扩展）
//
// 场景：同一首歌因标签/识别误差被拆成多条（歌名多了「(Live)」、歌手写法不同、时长差几秒），
// 在曲库里表现为互不相关的多个 song_key，音质无法堆叠在一起。
// 「合并」把若干 song_key 归一到同一 song_key（= 同一首歌），各条目按 platform+quality 堆叠成音质列表。
//
// 落盘位置：<root>/library/merge.json（与歌单/专辑同级，旧曲库缺省即视为无合并）。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "radmusic/error.hpp"
#include "radmusic/json.hpp"
#include "radmusic/model.hpp"

namespace radmusic {

class LibraryPaths;   // storage.hpp（避免头文件循环依赖）

// 合并组中的一个成员：记录合并前的身份，用于撤销与展示
struct MergeMember {
    std::string song_key;               // 合并前的 song_key
    std::string title;                  // 合并前的歌名
    std::string artist;                 // 合并前的歌手
    int duration_seconds = 0;           // 合并前的时长
    std::vector<std::string> shas;      // 该成员下的条目（撤销时按 sha 定位）

    json::Value to_json() const;
    static MergeMember from_json(const json::Value& v);
};

// 一个合并组：成员统一采用 canonical 身份（决定合并后的 song_key）
struct MergeGroup {
    std::string song_key;               // 合并后的规范 song_key（= song_key(title, artist, duration)）
    std::string title;
    std::string artist;
    int duration_seconds = 0;
    long long created_at = 0;           // epoch 秒
    std::vector<MergeMember> members;

    json::Value to_json() const;
    static MergeGroup from_json(const json::Value& v);
};

class MergeStore {
public:
    // paths 必须比 MergeStore 活得久（与 MetaCache 约定一致）
    explicit MergeStore(const LibraryPaths& paths) : paths_(paths) {}

    Status load();
    Status save() const;

    const std::vector<MergeGroup>& groups() const { return groups_; }
    bool empty() const { return groups_.empty(); }

    // 按 target 或成员的 song_key 命中（传未合并的 key 返回 nullptr）
    const MergeGroup* find(std::string_view song_key) const;
    MergeGroup* find_mutable(std::string_view song_key);
    // song_key → 所属组的规范 song_key；不属于任何组返回 nullopt
    std::optional<std::string> canonical_song_key(std::string_view song_key) const;
    bool is_merged(std::string_view song_key) const;

    // 元数据规范化：把 base 的 name/author/duration 改写为所属组的规范值，
    // 原始值保存到 orig_*（已存在则不覆盖），保证 song_key 可由元数据反推（1029）。
    // 返回是否发生了改写。
    bool normalize(BaseInfo& base) const;
    // 还原：把 orig_* 写回 name/author/duration 并清空（orig 缺失时用 fallback）
    static void restore(BaseInfo& base, std::string_view fallback_title = std::string_view(),
                        std::string_view fallback_artist = std::string_view(), int fallback_duration = -1);

    // 新增或更新（按 song_key 去重；成员的 shas 做并集）
    Status add_or_update(const MergeGroup& group);
    // 整组删除（target 或成员 key 均可）；返回是否删到
    bool remove(std::string_view song_key);
    // 移出指定成员；成员清空后整组删除
    bool remove_member(std::string_view song_key, std::string_view member_song_key);

    json::Value to_json() const;

private:
    const LibraryPaths& paths_;
    std::vector<MergeGroup> groups_;
};

// 由元数据推导 song_key（与 MetaCache::path_for 的 song_key 推导保持一致）
std::string song_key_of_base(const BaseInfo& base);

}  // namespace radmusic
