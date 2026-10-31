// Rad music API - 多源搜索与折叠聚合（Readme.md 2.5 / 2.7）
// 零第三方依赖；同步串行调用各播放源插件，不做并行以保持无锁、无线程。
#include "radmusic/search.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

#include "radmusic/groupkey.hpp"
#include "radmusic/json.hpp"
#include "radmusic/quality.hpp"

namespace radmusic {
namespace {

// ---------------- 小工具 ----------------

// ASCII 小写化（排序用；不做 Unicode 折叠，与 normalize_text 的用途区分开）
std::string ascii_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        out.push_back(static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    }
    return out;
}

// 源优先级：靠前优先，不在列表中的排最后（按出现顺序稳定）
int priority_rank(const std::vector<std::string>& priority, const std::string& platform) {
    for (size_t i = 0; i < priority.size(); ++i) {
        if (priority[i] == platform) return static_cast<int>(i);
    }
    return static_cast<int>(priority.size());
}

// 条目的“这首歌”身份：标题 + 主作者 + 时长（时长单独保留以便 ±3 秒容差判定）
SongIdentity reduce_to_song(const SourceEntry& entry) {
    SongIdentity id;
    id.title = entry.base.name;
    id.artist = entry.base.author;
    if (id.title.empty() && !entry.base.album.empty()) id.title = entry.base.album;
    id.duration_seconds = entry.base.duration_seconds;
    return id;
}

// 展示用标题：优先取 base.name，其次回落到 item_id
std::string entry_display_title(const SourceEntry& entry) {
    if (!entry.base.name.empty()) return entry.base.name;
    if (!entry.item_id.empty()) return entry.item_id;
    return std::string();
}

// 作者列表：主作者 + 其余作者（SearchItem.artists 为列表，这里只有一个来源字段）
std::vector<std::string> artists_of(const std::string& primary) {
    std::vector<std::string> out;
    if (!primary.empty()) out.push_back(primary);
    return out;
}

// 多位歌手用「 / 」连接成一个作者串（保留全部歌手，前端可据此识别多歌手）
std::string join_artists(const std::vector<std::string>& artists) {
    std::string out;
    for (size_t i = 0; i < artists.size(); ++i) {
        if (i) out += " / ";
        out += artists[i];
    }
    return out;
}

// 去重（按音质身份）并按 Quality::compare 降序排序
std::vector<Quality> normalize_qualities(std::vector<Quality> qualities) {
    std::vector<Quality> unique;
    unique.reserve(qualities.size());
    for (const Quality& q : qualities) {
        bool seen = false;
        for (const Quality& u : unique) {
            if (u.same_identity(q)) {
                seen = true;
                break;
            }
        }
        if (!seen) unique.push_back(q);
    }
    std::stable_sort(unique.begin(), unique.end(),
                     [](const Quality& a, const Quality& b) { return b.compare(a) < 0; });
    return unique;
}

// 收集该条目表达的全部音质（条目自身 quality + qualities[]）
std::vector<Quality> entry_qualities(const SourceEntry& entry) {
    std::vector<Quality> all = entry.qualities;
    bool contains_self = false;
    for (const Quality& q : all) {
        if (q.same_identity(entry.quality)) {
            contains_self = true;
            break;
        }
    }
    if (!contains_self && entry.quality.is_known()) all.push_back(entry.quality);
    return all;
}

// 同一平台合并：保留音质更高者，qualities 取并集
void merge_platform_entry(SourceEntry& existing, const SourceEntry& incoming) {
    // 可用性取“更可用”的一侧：同平台只要有一条可用就不该被标灰
    if (incoming.available && !existing.available) existing.available = incoming.available;
    if (existing.item_id.empty()) existing.item_id = incoming.item_id;
    if (existing.location.empty()) existing.location = incoming.location;
    if (existing.base.name.empty()) existing.base = incoming.base;

    std::vector<Quality> merged = entry_qualities(existing);
    for (const Quality& q : entry_qualities(incoming)) merged.push_back(q);
    merged = normalize_qualities(std::move(merged));

    if (existing.quality.compare(incoming.quality) < 0) {
        existing.quality = incoming.quality;
        if (existing.reason.empty() || !incoming.available) existing.reason = incoming.reason;
    }
    existing.qualities = std::move(merged);
}

// ---------------- 折叠 ----------------

// 把同一首歌的多个源条目聚合为一条
SearchItem fold_group(std::vector<SourceEntry> group, const std::vector<std::string>& priority) {
    // 1) 同一平台只保留一条（该平台最高音质）
    std::vector<SourceEntry> by_platform;
    for (const SourceEntry& entry : group) {
        bool merged = false;
        for (SourceEntry& kept : by_platform) {
            if (kept.platform == entry.platform) {
                merge_platform_entry(kept, entry);
                merged = true;
                break;
            }
        }
        if (!merged) by_platform.push_back(entry);
    }

    // 2) sources[] 排序：源优先级升序，其次音质降序
    std::stable_sort(by_platform.begin(), by_platform.end(),
                     [&priority](const SourceEntry& a, const SourceEntry& b) {
                         const int ra = priority_rank(priority, a.platform);
                         const int rb = priority_rank(priority, b.platform);
                         if (ra != rb) return ra < rb;
                         return b.quality.compare(a.quality) < 0;
                     });

    // 3) 折叠行字段
    SearchItem item;
    item.folded = true;
    item.sources = by_platform;

    // title：取第一个可用条目的标题（无可用条目则退回首个条目）
    const SourceEntry* first_available = nullptr;
    for (const SourceEntry& entry : by_platform) {
        if (entry.available) {
            first_available = &entry;
            break;
        }
    }
    const SourceEntry& title_ref = first_available ? *first_available : by_platform.front();
    item.title = entry_display_title(title_ref);
    item.artists = artists_of(title_ref.base.author);
    item.album = title_ref.base.album;
    item.duration_seconds = title_ref.base.duration_seconds;
    // 折叠行封面：按源优先级取第一个非空封面（标题来源可能没有封面）
    for (const SourceEntry& entry : by_platform) {
        if (!entry.base.pic.empty()) {
            item.pic = entry.base.pic;
            break;
        }
    }

    // group_key：宿主自行归一化（不受插件给出的键影响）
    item.group_key = group_key(item.title, title_ref.base.author, item.duration_seconds);

    // best_source：可用源里按优先级选（同优先级取音质更高者）；都不适用则取优先级最高者
    const SourceEntry* best = nullptr;
    int best_rank = 0;
    for (const SourceEntry& entry : by_platform) {
        if (!entry.available) continue;
        const int rank = priority_rank(priority, entry.platform);
        if (best == nullptr || rank < best_rank ||
            (rank == best_rank && best->quality.compare(entry.quality) < 0)) {
            best = &entry;
            best_rank = rank;
        }
    }
    if (best == nullptr) {
        for (const SourceEntry& entry : by_platform) {
            if (best == nullptr || priority_rank(priority, entry.platform) < best_rank) {
                best = &entry;
                best_rank = priority_rank(priority, entry.platform);
            }
        }
    }
    if (best != nullptr) item.best_source = best->platform;

    return item;
}

// 排序：标题（ASCII 大小写不敏感）→ 作者 → 时长 → group_key
bool result_less(const SearchItem& a, const SearchItem& b) {
    const std::string ta = ascii_lower(a.title);
    const std::string tb = ascii_lower(b.title);
    if (ta != tb) return ta < tb;
    const std::string aa = ascii_lower(a.artists.empty() ? std::string() : a.artists.front());
    const std::string ab = ascii_lower(b.artists.empty() ? std::string() : b.artists.front());
    if (aa != ab) return aa < ab;
    if (a.duration_seconds != b.duration_seconds) return a.duration_seconds < b.duration_seconds;
    return a.group_key < b.group_key;
}

// ---------------- 筛选 / 排序（v0.11） ----------------

bool platform_requested(const std::vector<std::string>& wanted, const std::string& platform) {
    if (wanted.empty()) return true;
    for (const std::string& item : wanted) {
        if (item == platform) return true;
    }
    return false;
}

int quality_rank_of(QualityLevel level) { return quality_level_rank(level); }

// 条目可用的最高音质等级（available=false 的源不参与）
int item_best_quality_rank(const SearchItem& item) {
    int best = 0;
    for (const SourceEntry& source : item.sources) {
        if (!source.available) continue;
        if (source.quality.is_known()) best = std::max(best, quality_rank_of(source.quality.level));
        for (const Quality& quality : source.qualities) {
            if (quality.is_known()) best = std::max(best, quality_rank_of(quality.level));
        }
    }
    return best;
}

bool item_matches_filters(const SearchItem& item, const SearchRequest& request) {
    bool has_available = false;
    for (const SourceEntry& source : item.sources) {
        if (source.available) {
            has_available = true;
            break;
        }
    }
    if (request.available_only && !has_available) return false;

    if (!request.quality_min.empty()) {
        const int need = quality_rank_of(quality_level_from_id(request.quality_min));
        if (need > 0 && item_best_quality_rank(item) < need) return false;
    }

    if (request.duration_min > 0 || request.duration_max > 0) {
        // 时长未知（0）不参与区间过滤，避免把没有时长元数据的曲目误杀
        const int duration = item.duration_seconds;
        if (duration > 0) {
            if (request.duration_min > 0 && duration < request.duration_min) return false;
            if (request.duration_max > 0 && duration > request.duration_max) return false;
        }
    }
    return true;
}

bool sort_items(std::vector<SearchItem>& items, const std::string& sort) {
    if (sort == "duration") {
        std::stable_sort(items.begin(), items.end(), [](const SearchItem& a, const SearchItem& b) {
            if (a.duration_seconds != b.duration_seconds) return a.duration_seconds < b.duration_seconds;
            return result_less(a, b);
        });
        return true;
    }
    if (sort == "quality") {
        std::stable_sort(items.begin(), items.end(), [](const SearchItem& a, const SearchItem& b) {
            const int qa = item_best_quality_rank(a);
            const int qb = item_best_quality_rank(b);
            if (qa != qb) return qa > qb;
            return result_less(a, b);
        });
        return true;
    }
    if (sort == "artist") {
        std::stable_sort(items.begin(), items.end(), [](const SearchItem& a, const SearchItem& b) {
            const std::string aa = ascii_lower(a.artists.empty() ? std::string() : a.artists.front());
            const std::string ab = ascii_lower(b.artists.empty() ? std::string() : b.artists.front());
            if (aa != ab) return aa < ab;
            return result_less(a, b);
        });
        return true;
    }
    // relevance / title 都走标题优先的稳定排序
    std::stable_sort(items.begin(), items.end(), result_less);
    return true;
}

}  // namespace

std::vector<SearchItem> fold_sources(std::vector<SourceEntry> entries,
                                     const std::vector<std::string>& source_priority) {
    std::vector<SearchItem> out;
    std::vector<std::vector<SourceEntry>> groups;

    // 线性扫描分组：用 groupkey.hpp 的歌曲身份判定（支持 ±3 秒容差），
    // 不能只比 group_key 字符串相等——那样时长差 1~3 秒会被拆成两行。
    for (SourceEntry& entry : entries) {
        const SongIdentity id = reduce_to_song(entry);
        bool merged = false;
        for (size_t g = 0; g < groups.size(); ++g) {
            const SongIdentity existing = reduce_to_song(groups[g].front());
            if (songs_fold_together(existing, id)) {
                groups[g].push_back(entry);
                merged = true;
                break;
            }
        }
        if (!merged) groups.push_back({entry});
    }

    out.reserve(groups.size());
    for (std::vector<SourceEntry>& group : groups) out.push_back(fold_group(std::move(group), source_priority));
    std::stable_sort(out.begin(), out.end(), result_less);
    return out;
}

std::vector<SearchItem> paginate(const std::vector<SearchItem>& items, int page, int page_size) {
    int size = page_size <= 0 ? 20 : page_size;
    if (size > 100) size = 100;
    const int current = page < 1 ? 1 : page;

    const long long total = static_cast<long long>(items.size());
    const long long begin = (static_cast<long long>(current) - 1) * size;
    if (begin >= total) return {};
    const long long end = std::min<long long>(begin + size, total);
    return std::vector<SearchItem>(items.begin() + static_cast<std::ptrdiff_t>(begin),
                                   items.begin() + static_cast<std::ptrdiff_t>(end));
}

SourceEntry entry_from_item(const std::string& platform, const SearchItem& item) {
    SourceEntry entry;
    entry.platform = platform;
    // 曲目标识必须来自插件（后续 request/get_url/get_song 都靠它）；插件没给才回落到折叠键
    entry.item_id = item.group_key;

    // sources[]：优先取第一个可用条目作为该源条目（灰显条目不能顶替可用条目）；
    // 全部不可用时取首个，以保证“不可用源仍然保留在 sources[] 中”。
    const SourceEntry* chosen = nullptr;
    for (const SourceEntry& source : item.sources) {
        if (source.available) {
            chosen = &source;
            break;
        }
    }
    if (chosen == nullptr && !item.sources.empty()) chosen = &item.sources.front();

    if (chosen != nullptr) {
        if (!chosen->item_id.empty()) entry.item_id = chosen->item_id;   // 保留插件的真实 id
        entry.available = chosen->available;   // 保留插件的可用性标记（如 vip required）
        entry.reason = chosen->reason;
        entry.location = chosen->location;
        entry.qualities = chosen->qualities;
    } else {
        entry.available = true;                // 没有 sources 时默认可用
    }

    // quality：取该源可用条目的最高音质 —— 既要看 entries 里的 quality（最高可用音质），
    // 也要看它自带的 qualities[]（列表里的最高项才算数）
    std::vector<Quality> available_qualities;
    for (const SourceEntry& source : item.sources) {
        if (!source.available) continue;
        for (const Quality& q : source.qualities) available_qualities.push_back(q);
        if (source.quality.is_known()) {
            available_qualities.push_back(source.quality);
        } else if (source.base.quality.is_known()) {
            available_qualities.push_back(source.base.quality);
        }
    }
    available_qualities = normalize_qualities(std::move(available_qualities));
    if (!available_qualities.empty()) {
        entry.quality = available_qualities.front();   // 降序，首项即最高
        if (entry.qualities.empty()) entry.qualities = available_qualities;
    }

    entry.base.platform = platform;
    entry.base.name = item.title;
    entry.base.author = join_artists(item.artists);
    entry.base.album = item.album;
    entry.base.duration_seconds = item.duration_seconds;
    // 封面：插件 / 本地曲库给出的 pic 必须沿用，否则搜索结果整行无封面
    // （entry.base 是新构造的，不复制就会丢掉 chosen->base.pic）
    if (chosen != nullptr) entry.base.pic = chosen->base.pic;
    if (entry.base.pic.empty() && !item.pic.empty()) entry.base.pic = item.pic;
    // 无可用音质时回落 base.quality；调用方设置的 base.quality 不被覆盖
    if (!entry.quality.is_known() && chosen != nullptr) entry.quality = chosen->base.quality;
    if (!entry.quality.is_known()) entry.quality = entry.base.quality;
    if (!entry.base.quality.is_known()) entry.base.quality = entry.quality;
    return entry;
}

Result<SearchResponse> execute_search(const SearchRequest& request,
                                      const std::vector<std::shared_ptr<ISourcePlugin>>& sources,
                                      const std::vector<std::string>& source_priority,
                                      const std::vector<SourceEntry>& host_entries,
                                      const std::string& host_platform) {
    if (request.keyword.empty()) {
        return Result<SearchResponse>::failure(ErrorCode::InvalidMetadata, "搜索关键字不能为空");
    }

    // 只调用 request.sources 指定的源；platform 不匹配的源直接跳过
    std::vector<std::shared_ptr<ISourcePlugin>> usable;
    for (const std::shared_ptr<ISourcePlugin>& plugin : sources) {
        if (!plugin) continue;
        const PluginManifest& manifest = plugin->manifest();
        std::string platform = manifest.platforms.empty() ? std::string() : manifest.platforms.front();
        if (platform.empty()) platform = manifest.id;
        if (!platform_requested(request.sources, platform)) continue;
        usable.push_back(plugin);
    }

    const bool host_enabled = !host_platform.empty();
    if (usable.empty() && !host_enabled) {
        if (!request.sources.empty()) {
            std::string names;
            for (const std::string& name : request.sources) {
                if (!names.empty()) names += ", ";
                names += name;
            }
            return Result<SearchResponse>::failure(ErrorCode::SearchNoSource,
                                                   "指定的音源不可用: " + names);
        }
        return Result<SearchResponse>::failure(ErrorCode::SearchNoSource, "没有可用的播放源");
    }

    SearchResponse response;
    response.keyword = request.keyword;
    response.page = request.page < 1 ? 1 : request.page;
    response.page_size = request.page_size <= 0 ? 20 : std::min(request.page_size, 100);

    std::vector<SourceEntry> entries;
    int succeeded_sources = 0;
    int called_sources = 0;

    // 宿主条目（本地曲库）：先放入，参与后续折叠，使本地与在线同曲能合并到一行
    if (host_enabled) {
        SourceStatus status;
        status.platform = host_platform;
        status.result_count = static_cast<int>(host_entries.size());
        response.sources_status.push_back(status);
        ++succeeded_sources;
        entries.insert(entries.end(), host_entries.begin(), host_entries.end());
    }

    // 同步串行调用（不引入线程）；strategy=race 与 fanout 在串行实现下语义一致：
    // 单源失败/超时只记状态，不影响整体。
    for (const std::shared_ptr<ISourcePlugin>& plugin : usable) {
        const PluginManifest& manifest = plugin->manifest();
        std::string platform = manifest.platforms.empty() ? std::string() : manifest.platforms.front();
        if (platform.empty()) platform = manifest.id;

        SourceStatus status;
        status.platform = platform;

        json::Value options = json::Value::object();
        options.set("type", request.type);
        options.set("page", request.page);
        options.set("page_size", request.page_size);
        options.set("timeout_ms", request.timeout_ms);
        options.set("source", platform);

        const auto started = std::chrono::steady_clock::now();
        ++called_sources;
        bool threw = false;
        Result<std::vector<SearchItem>> result =
            Result<std::vector<SearchItem>>::failure(ErrorCode::RequestFailed, "未执行");
        try {
            result = plugin->search_all(request.keyword, options);
        } catch (const std::exception& e) {
            threw = true;
            status.code = ErrorCode::RequestFailed;
            status.message = std::string("插件抛出异常: ") + e.what();
        } catch (...) {
            threw = true;
            status.code = ErrorCode::RequestFailed;
            status.message = "插件抛出未知异常";
        }
        const auto finished = std::chrono::steady_clock::now();
        status.elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();

        if (!threw) {
            if (result.ok()) {
                ++succeeded_sources;
                status.result_count = static_cast<int>(result.value().size());
                for (const SearchItem& item : result.value()) entries.push_back(entry_from_item(platform, item));
            } else {
                status.code = result.status().code;
                status.message = result.status().response_message();
            }
        }
        response.sources_status.push_back(status);
    }

    // 所有源都失败 → 1009（“成功但 0 条”仍是成功，不算 1009）
    if (succeeded_sources == 0) {
        return Result<SearchResponse>::failure(ErrorCode::SearchNoSource,
                                               "全部播放源搜索失败（已调用 " +
                                                   std::to_string(called_sources) + " 个源）");
    }

    std::vector<SearchItem> results;
    if (request.fold) {
        results = fold_sources(std::move(entries), source_priority);
    } else {
        results.reserve(entries.size());
        for (SourceEntry& entry : entries) {
            SearchItem item;
            item.group_key = group_key(entry_display_title(entry), entry.base.author,
                                       entry.base.duration_seconds);
            item.title = entry_display_title(entry);
            item.artists = artists_of(entry.base.author);
            item.album = entry.base.album;
            item.duration_seconds = entry.base.duration_seconds;
            item.pic = entry.base.pic;
            item.folded = false;
            item.best_source = entry.platform;
            item.sources.push_back(entry);
            results.push_back(std::move(item));
        }
    }

    // 筛选 → 排序 → 分页（顺序固定，保证 total 与分页一致）
    std::vector<SearchItem> filtered;
    filtered.reserve(results.size());
    for (SearchItem& item : results) {
        if (request.available_only) {
            // 同时把不可用音源从 sources[] 中移除，避免前端继续显示灰条
            std::vector<SourceEntry> kept;
            for (const SourceEntry& source : item.sources) {
                if (source.available) kept.push_back(source);
            }
            item.sources = std::move(kept);
        }
        if (!item_matches_filters(item, request)) continue;
        filtered.push_back(std::move(item));
    }
    sort_items(filtered, request.sort);

    response.total = static_cast<int>(filtered.size());
    response.results = paginate(filtered, response.page, response.page_size);
    return Result<SearchResponse>::success(std::move(response));
}

}  // namespace radmusic
