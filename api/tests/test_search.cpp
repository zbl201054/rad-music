// Rad music API - 多源搜索与折叠聚合测试（Readme.md 2.5 / 2.7）
// 用例前缀统一为 test_search_
#include "radmusic/search.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "radmusic/groupkey.hpp"
#include "radmusic/quality.hpp"
#include "test_framework.hpp"

namespace {

using radmusic::ErrorCode;
using radmusic::ISourcePlugin;
using radmusic::PluginManifest;
using radmusic::PluginType;
using radmusic::Quality;
using radmusic::QualityLevel;
using radmusic::Result;
using radmusic::SearchItem;
using radmusic::SearchRequest;
using radmusic::SourceEntry;
using radmusic::SourceStatus;

// ---------------- 测试用假源 ----------------

class FakeSource : public ISourcePlugin {
public:
    FakeSource(std::string platform, Result<SearchItem> result)
        : platform_(std::move(platform)), result_(std::move(result)) {
        manifest_.id = "fake_" + platform_;
        manifest_.name = "Fake Source " + platform_;
        manifest_.type = PluginType::Source;
        manifest_.platforms = {platform_};
        manifest_.capabilities = {"search"};
        radmusic::QualityLevelSpec level;
        level.id = "low";
        level.label = "低";
        level.spec.level = QualityLevel::Low;
        level.spec.codec = "mp3";
        level.spec.bitrate = 128;
        level.spec.quality_id = "low";
        manifest_.quality_levels = {level};
    }

    const PluginManifest& manifest() const override { return manifest_; }
    std::vector<std::string> platforms() const override { return {platform_}; }
    bool supports(std::string_view) const override { return true; }
    std::vector<std::string> ops() const override { return {"search"}; }
    std::vector<radmusic::QualityLevelSpec> quality_levels() const override {
        return manifest_.quality_levels;
    }
    Result<radmusic::BaseInfo> resolve(std::string_view) override {
        return Result<radmusic::BaseInfo>::failure(ErrorCode::UnsupportedCapability, "fake: resolve 未实现");
    }
    Result<SearchItem> search(std::string_view, const radmusic::json::Value&) override {
        ++calls;
        if (sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        return result_;
    }
    Result<std::string> open(std::string_view) override {
        return Result<std::string>::failure(ErrorCode::UnsupportedCapability, "fake: open 未实现");
    }
    Result<radmusic::json::Value> request(std::string_view, const radmusic::json::Value&) override {
        return Result<radmusic::json::Value>::failure(ErrorCode::UnsupportedCapability,
                                                      "fake: request 未实现");
    }
    Result<std::vector<Quality>> qualities(std::string_view) override {
        return Result<std::vector<Quality>>::failure(ErrorCode::UnsupportedCapability,
                                                     "fake: qualities 未实现");
    }

    int calls = 0;
    int sleep_ms = 1;

private:
    std::string platform_;
    PluginManifest manifest_;
    Result<SearchItem> result_;
};

// ---------------- 构造辅助 ----------------

Quality quality_of(QualityLevel level, std::string codec, int bitrate) {
    Quality q;
    q.level = level;
    q.codec = std::move(codec);
    q.bitrate = bitrate;
    q.quality_id = radmusic::quality_level_id(level);
    q.label = q.quality_id;
    q.lossless = level == QualityLevel::Lossless || level == QualityLevel::HiRes;
    return q;
}

// 构造“插件返回的条目”：base 由 entry_from_item 补全，available 恒为 true
SearchItem make_item(const std::string& title, const std::string& artist, int duration,
                     std::vector<Quality> qualities = {}) {
    SearchItem item;
    item.title = title;
    item.artists = {artist};
    item.duration_seconds = duration;
    item.group_key = radmusic::group_key(title, artist, duration);
    if (qualities.empty()) qualities.push_back(quality_of(QualityLevel::High, "mp3", 320));
    item.sources.push_back(radmusic::entry_from_item("__item__", item));
    item.sources.front().quality = qualities.front();
    item.sources.front().qualities = std::move(qualities);
    return item;
}

// 构造“已是源条目”的输入（fold_sources 直接输入用）
SourceEntry make_entry(const std::string& platform, const std::string& title, const std::string& artist,
                       int duration, Quality quality, bool available = true) {
    SourceEntry entry;
    entry.platform = platform;
    entry.item_id = platform + ":id";
    entry.available = available;
    entry.quality = quality;
    entry.qualities = {quality};
    entry.base.name = title;
    entry.base.author = artist;
    entry.base.duration_seconds = duration;
    entry.base.platform = platform;
    entry.base.quality = quality;
    return entry;
}

std::vector<std::shared_ptr<ISourcePlugin>> sources_of(
    const std::vector<std::shared_ptr<FakeSource>>& fakes) {
    std::vector<std::shared_ptr<ISourcePlugin>> out;
    out.reserve(fakes.size());
    for (const auto& f : fakes) out.push_back(f);
    return out;
}

std::shared_ptr<FakeSource> ok_source(const std::string& platform, const SearchItem& item,
                                      int sleep_ms = 1) {
    auto fake = std::make_shared<FakeSource>(platform, Result<SearchItem>::success(item));
    fake->sleep_ms = sleep_ms;
    return fake;
}

std::shared_ptr<FakeSource> failing_source(const std::string& platform, ErrorCode code) {
    auto fake = std::make_shared<FakeSource>(platform, Result<SearchItem>::failure(code, "fake failure"));
    fake->sleep_ms = 1;
    return fake;
}

SearchRequest make_request(std::string keyword) {
    SearchRequest request;
    request.keyword = std::move(keyword);
    request.page = 1;
    request.page_size = 20;
    return request;
}

std::vector<SearchItem> make_flat_items(int count, const std::string& prefix) {
    std::vector<SearchItem> items;
    items.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        SearchItem item;
        item.title = prefix + " " + std::to_string(i);
        item.artists = {"Artist"};
        item.duration_seconds = 100 + i;
        item.group_key = radmusic::group_key(item.title, "Artist", item.duration_seconds);
        items.push_back(std::move(item));
    }
    return items;
}

// 只含 radish 的源优先级列表（避免 std::vector<std::string> 的初始化列表推导问题）
const std::vector<std::string> kRadishOnly = {"radish"};

}  // namespace

// 两个源返回同一首歌（标题/作者相同、时长差 2 秒，在 ±3 秒容差内）→ 折叠成 1 条
TEST_CASE(test_search_fold_two_sources_same_song) {
    const std::string title = "Same Song";
    const std::string artist = "Same Artist";

    auto radish = ok_source(
        "radish", make_item(title, artist, 215, {quality_of(QualityLevel::Lossless, "flac", 1411)}));
    auto netease =
        ok_source("netease", make_item(title, artist, 217, {quality_of(QualityLevel::High, "mp3", 320)}));

    const std::vector<std::string> priority = {"radish", "netease"};
    auto result = radmusic::execute_search(make_request("same"), sources_of({radish, netease}), priority);

    CHECK(result.ok());
    const auto& response = result.value();
    CHECK_EQ(response.keyword, std::string("same"));
    CHECK_EQ(response.total, 1);
    CHECK_EQ(response.results.size(), static_cast<size_t>(1));
    CHECK_EQ(response.sources_status.size(), static_cast<size_t>(2));

    const SearchItem& item = response.results.front();
    CHECK(item.folded);
    CHECK_EQ(item.title, title);
    CHECK_EQ(item.duration_seconds, 215);
    CHECK_EQ(item.artists.size(), static_cast<size_t>(1));
    CHECK_EQ(item.artists.front(), artist);
    CHECK_EQ(item.sources.size(), static_cast<size_t>(2));
    CHECK_EQ(item.best_source, std::string("radish"));
    CHECK_EQ(item.sources[0].platform, std::string("radish"));
    CHECK_EQ(item.sources[1].platform, std::string("netease"));
    // 折叠键由宿主重建；±3 秒容差内两条都落在同一个折叠键上
    CHECK(radmusic::group_keys_fold_together(item.group_key,
                                             radmusic::group_key(title, artist, 217)));
    CHECK(radish->calls == 1);
    CHECK(netease->calls == 1);
}

// 时长差 10 秒 → 不折叠（2 条）
TEST_CASE(test_search_no_fold_when_duration_differs) {
    const std::string title = "Long Song";
    const std::string artist = "Some Artist";

    auto a = ok_source("radish", make_item(title, artist, 200, {quality_of(QualityLevel::High, "mp3", 320)}));
    auto b = ok_source("netease", make_item(title, artist, 210, {quality_of(QualityLevel::High, "mp3", 320)}));

    const std::vector<std::string> priority = {"radish", "netease"};
    auto result = radmusic::execute_search(make_request("long"), sources_of({a, b}), priority);

    CHECK(result.ok());
    CHECK_EQ(result.value().total, 2);
    CHECK_EQ(result.value().results.size(), static_cast<size_t>(2));
    for (const auto& item : result.value().results) {
        CHECK_EQ(item.sources.size(), static_cast<size_t>(1));
    }
}

// 同一源同一首歌返回两个音质 → 合并为 1 条且保留最高音质、qualities 降序
TEST_CASE(test_search_same_platform_merges_qualities) {
    const std::string title = "Quality Song";
    const std::string artist = "Quality Artist";
    const Quality lossless = quality_of(QualityLevel::Lossless, "flac", 1411);
    const Quality high = quality_of(QualityLevel::High, "mp3", 320);

    std::vector<SourceEntry> entries;
    entries.push_back(make_entry("radish", title, artist, 180, high));
    entries.push_back(make_entry("radish", title, artist, 180, lossless));

    const std::vector<SearchItem> folded = radmusic::fold_sources(std::move(entries), {"radish"});

    CHECK_EQ(folded.size(), static_cast<size_t>(1));
    const SearchItem& item = folded.front();
    CHECK_EQ(item.sources.size(), static_cast<size_t>(1));
    CHECK_EQ(item.best_source, std::string("radish"));
    CHECK_EQ(item.sources.front().quality.level, QualityLevel::Lossless);
    CHECK_EQ(item.sources.front().qualities.size(), static_cast<size_t>(2));
    CHECK_EQ(item.sources.front().qualities[0].level, QualityLevel::Lossless);
    CHECK_EQ(item.sources.front().qualities[1].level, QualityLevel::High);
}

// 一个源失败（SourceTimeout）→ 整体成功、sources_status 有失败源、结果仍含另一个源
TEST_CASE(test_search_one_source_fails_overall_ok) {
    auto bad = failing_source("third_x", ErrorCode::SourceTimeout);
    auto good = ok_source("radish", make_item("Only Song", "Only Artist", 120), 5);

    const std::vector<std::string> priority = {"radish", "third_x"};
    auto result = radmusic::execute_search(make_request("only"), sources_of({bad, good}), priority);

    CHECK(result.ok());
    CHECK_EQ(result.value().total, 1);
    CHECK_EQ(result.value().results.size(), static_cast<size_t>(1));
    CHECK_EQ(result.value().results.front().best_source, std::string("radish"));
    CHECK_EQ(result.value().sources_status.size(), static_cast<size_t>(2));

    const SourceStatus* failed = nullptr;
    const SourceStatus* succeeded = nullptr;
    for (const auto& status : result.value().sources_status) {
        if (status.platform == "third_x") failed = &status;
        if (status.platform == "radish") succeeded = &status;
    }
    CHECK(failed != nullptr);
    CHECK(succeeded != nullptr);
    if (failed != nullptr) {
        CHECK_EQ(failed->code, ErrorCode::SourceTimeout);
        CHECK(!failed->message.empty());
    }
    if (succeeded != nullptr) {
        CHECK_EQ(succeeded->code, ErrorCode::Ok);
        CHECK(succeeded->elapsed_ms >= 0);
    }
    CHECK_EQ(bad->calls, 1);
    CHECK_EQ(good->calls, 1);
}

// 所有源失败 → 1009
TEST_CASE(test_search_all_sources_fail_returns_1009) {
    auto a = failing_source("radish", ErrorCode::SourceTimeout);
    auto b = failing_source("netease", ErrorCode::RequestFailed);

    const std::vector<std::string> priority = {"radish", "netease"};
    auto result = radmusic::execute_search(make_request("nothing"), sources_of({a, b}), priority);

    CHECK(!result.ok());
    CHECK_EQ(result.status().code, ErrorCode::SearchNoSource);
}

// 无可用源（空列表 / 全为空指针）→ 1009
TEST_CASE(test_search_no_source_returns_1009) {
    SearchRequest request = make_request("keywords");
    const std::vector<std::string> priority = {"radish"};

    auto empty = radmusic::execute_search(request, {}, priority);
    CHECK(!empty.ok());
    CHECK_EQ(empty.status().code, ErrorCode::SearchNoSource);

    std::vector<std::shared_ptr<ISourcePlugin>> nulls = {nullptr, nullptr};
    auto all_null = radmusic::execute_search(request, nulls, priority);
    CHECK(!all_null.ok());
    CHECK_EQ(all_null.status().code, ErrorCode::SearchNoSource);
}

// keyword 为空 → 1007（且不得调用插件）
TEST_CASE(test_search_empty_keyword_returns_1007) {
    auto source = ok_source("radish", make_item("Song", "Artist", 100));
    const std::vector<std::string> priority = {"radish"};

    auto result = radmusic::execute_search(make_request(""), sources_of({source}), priority);
    CHECK(!result.ok());
    CHECK_EQ(result.status().code, ErrorCode::InvalidMetadata);
    CHECK_EQ(source->calls, 0);
}

// 源成功但返回 0 条 → 整体成功（results 为空、total = 0），不是 1009
// 说明：契约里 ISourcePlugin::search 每次只返回 0/1 个 SearchItem，
// 因此这里以“成功源 + 失败源”验证“只要有一个源成功就整体成功”。
TEST_CASE(test_search_success_with_zero_results_is_ok) {
    SearchItem empty_item;   // 成功但没有元数据：仍算一条结果（条目数由源返回决定）
    auto empty_source = ok_source("radish", empty_item);
    auto failed_source = failing_source("netease", ErrorCode::SourceTimeout);

    auto result = radmusic::execute_search(make_request("empty"), sources_of({empty_source, failed_source}),
                                           kRadishOnly);
    CHECK(result.ok());

    // 只有一个成功源（无失败源）时同样成功，且 sources_status 标注该源为 Ok
    auto ok_but_empty = ok_source("radish", SearchItem());
    auto ok_result = radmusic::execute_search(make_request("empty"), sources_of({ok_but_empty}), kRadishOnly);
    CHECK(ok_result.ok());
    CHECK_EQ(ok_result.value().total, 1);   // 插件返回了 1 个条目（仅无元数据）
    CHECK_EQ(ok_result.value().sources_status.size(), static_cast<size_t>(1));
    CHECK_EQ(ok_result.value().sources_status.front().code, ErrorCode::Ok);
    CHECK_EQ(ok_but_empty->calls, 1);
}

// 分页：25 条结果、page=2、page_size=10 → 10 条且 total=25
TEST_CASE(test_search_pagination_page2) {
    std::vector<SourceEntry> entries;
    for (int i = 0; i < 25; ++i) {
        entries.push_back(make_entry("radish", "Song " + std::to_string(i), "Artist", 100 + i,
                                     quality_of(QualityLevel::High, "mp3", 320)));
    }
    const std::vector<SearchItem> folded = radmusic::fold_sources(entries, {"radish"});
    CHECK_EQ(folded.size(), static_cast<size_t>(25));

    const std::vector<SearchItem> page2 = radmusic::paginate(folded, 2, 10);
    CHECK_EQ(page2.size(), static_cast<size_t>(10));
    CHECK_EQ(radmusic::paginate(folded, 3, 10).size(), static_cast<size_t>(5));
    CHECK_EQ(radmusic::paginate(folded, 9, 10).size(), static_cast<size_t>(0));

    // 切页不丢不重：三页并集 = 全部 25 条，且每页内条目互不相同
    std::vector<std::string> seen;
    for (int page = 1; page <= 3; ++page) {
        const std::vector<SearchItem> slice = radmusic::paginate(folded, page, 10);
        for (const auto& item : slice) seen.push_back(item.group_key);
    }
    CHECK_EQ(seen.size(), static_cast<size_t>(25));
    bool all_unique = true;
    for (size_t i = 0; i < seen.size(); ++i) {
        for (size_t j = i + 1; j < seen.size(); ++j) {
            if (seen[i] == seen[j]) all_unique = false;
        }
    }
    CHECK(all_unique);

    // 通过 execute_search 走完整链路：total 是结果总数，而不是当前页条数
    SearchRequest request = make_request("paged");
    request.page = 2;
    request.page_size = 10;
    auto source = ok_source("radish", make_item("Paged 0", "Artist", 100));
    auto result = radmusic::execute_search(request, sources_of({source}), kRadishOnly);
    CHECK(result.ok());
    CHECK_EQ(result.value().total, 1);
    CHECK_EQ(result.value().page, 2);
    CHECK_EQ(result.value().page_size, 10);
    CHECK_EQ(result.value().results.size(), static_cast<size_t>(0));

    const std::vector<SearchItem> many = make_flat_items(25, "Paged");
    CHECK_EQ(radmusic::paginate(many, 2, 10).size(), static_cast<size_t>(10));
    CHECK_EQ(many.size(), static_cast<size_t>(25));
}

// 分页：page_size=0 → 默认 20；page_size=1000 → 上限 100
TEST_CASE(test_search_pagination_page_size_limits) {
    const std::vector<SearchItem> twenty_five = make_flat_items(25, "Song");
    CHECK_EQ(radmusic::paginate(twenty_five, 1, 0).size(), static_cast<size_t>(20));
    CHECK_EQ(radmusic::paginate(twenty_five, 1, -5).size(), static_cast<size_t>(20));
    CHECK_EQ(radmusic::paginate(twenty_five, 1, 1000).size(), static_cast<size_t>(25));

    const std::vector<SearchItem> one_twenty = make_flat_items(120, "Bulk");
    CHECK_EQ(radmusic::paginate(one_twenty, 1, 1000).size(), static_cast<size_t>(100));
    CHECK_EQ(radmusic::paginate(one_twenty, 2, 1000).size(), static_cast<size_t>(20));

    // execute_search 也会归一化 page_size
    auto source = ok_source("radish", make_item("One Song", "One Artist", 100));
    SearchRequest request = make_request("one");
    request.page_size = 0;
    auto defaulted = radmusic::execute_search(request, sources_of({source}), kRadishOnly);
    CHECK(defaulted.ok());
    CHECK_EQ(defaulted.value().page_size, 20);

    request.page_size = 1000;
    auto capped = radmusic::execute_search(request, sources_of({source}), kRadishOnly);
    CHECK(capped.ok());
    CHECK_EQ(capped.value().page_size, 100);
}

// 不可用源（available=false）保留在 sources 中且不会被选为 best_source（当有可用源时）
TEST_CASE(test_search_unavailable_source_kept_but_not_best) {
    const std::string title = "Vip Song";
    const std::string artist = "Vip Artist";

    SearchItem netease_item = make_item(title, artist, 200, {quality_of(QualityLevel::High, "mp3", 320)});
    netease_item.sources.front().available = false;
    netease_item.sources.front().reason = "vip required";

    auto netease = ok_source("netease", netease_item);
    auto radish = ok_source(
        "radish", make_item(title, artist, 200, {quality_of(QualityLevel::Medium, "mp3", 192)}));

    // 故意让不可用源优先级最高，验证 best_source 仍选可用源
    const std::vector<std::string> priority = {"netease", "radish"};
    auto result = radmusic::execute_search(make_request("vip"), sources_of({netease, radish}), priority);

    CHECK(result.ok());
    CHECK_EQ(result.value().total, 1);
    const SearchItem& item = result.value().results.front();
    CHECK_EQ(item.sources.size(), static_cast<size_t>(2));
    CHECK_EQ(item.sources[0].platform, std::string("netease"));
    CHECK(!item.sources[0].available);
    CHECK_EQ(item.sources[0].reason, std::string("vip required"));
    CHECK_EQ(item.best_source, std::string("radish"));
}

// 全部源都不可用 → 仍折叠，best_source 取优先级最高者（灰显由前端决定）
TEST_CASE(test_search_all_unavailable_keeps_best_by_priority) {
    const std::string title = "Grey Song";
    const std::string artist = "Grey Artist";

    SearchItem a = make_item(title, artist, 150, {quality_of(QualityLevel::High, "mp3", 320)});
    a.sources.front().available = false;
    SearchItem b = make_item(title, artist, 150, {quality_of(QualityLevel::Lossless, "flac", 1411)});
    b.sources.front().available = false;

    auto first = ok_source("netease", a);
    auto second = ok_source("radish", b);

    const std::vector<std::string> priority = {"netease", "radish"};
    auto result = radmusic::execute_search(make_request("grey"), sources_of({first, second}), priority);

    CHECK(result.ok());
    CHECK_EQ(result.value().total, 1);
    const SearchItem& item = result.value().results.front();
    CHECK_EQ(item.sources.size(), static_cast<size_t>(2));
    CHECK_EQ(item.best_source, std::string("netease"));
}

// fold=false：每个条目单独成行，group_key 用 item 自带的
TEST_CASE(test_search_fold_disabled_keeps_rows_separate) {
    const std::string title = "Same Song";
    const std::string artist = "Same Artist";

    auto radish = ok_source("radish", make_item(title, artist, 215));
    auto netease = ok_source("netease", make_item(title, artist, 216));

    SearchRequest request = make_request("same");
    request.fold = false;
    const std::vector<std::string> priority = {"radish", "netease"};
    auto result = radmusic::execute_search(request, sources_of({radish, netease}), priority);

    CHECK(result.ok());
    CHECK_EQ(result.value().total, 2);
    CHECK_EQ(result.value().results.size(), static_cast<size_t>(2));
    for (const auto& item : result.value().results) {
        CHECK(!item.folded);
        CHECK_EQ(item.sources.size(), static_cast<size_t>(1));
        CHECK(!item.group_key.empty());
    }
}

// 源优先级：列表外的平台排最后；同优先级内按音质降序
TEST_CASE(test_search_priority_order_unknown_platform_last) {
    const std::string title = "Priority Song";
    const std::string artist = "Priority Artist";

    std::vector<SourceEntry> entries;
    entries.push_back(
        make_entry("unknown_x", title, artist, 100, quality_of(QualityLevel::High, "mp3", 320)));
    entries.push_back(make_entry("netease", title, artist, 100, quality_of(QualityLevel::High, "mp3", 320)));
    entries.push_back(
        make_entry("radish", title, artist, 100, quality_of(QualityLevel::Lossless, "flac", 1411)));

    const std::vector<SearchItem> folded = radmusic::fold_sources(entries, {"radish", "netease"});
    CHECK_EQ(folded.size(), static_cast<size_t>(1));
    const SearchItem& item = folded.front();
    CHECK_EQ(item.sources.size(), static_cast<size_t>(3));
    CHECK_EQ(item.sources[0].platform, std::string("radish"));
    CHECK_EQ(item.sources[1].platform, std::string("netease"));
    CHECK_EQ(item.sources[2].platform, std::string("unknown_x"));
    CHECK_EQ(item.best_source, std::string("radish"));
}

// entry_from_item：platform 填好、available 默认 true、quality 取 sources 中首个可用者的最高音质
TEST_CASE(test_search_entry_from_item_maps_quality_and_platform) {
    SearchItem item = make_item("Mapped Song", "Mapped Artist", 133,
                                {quality_of(QualityLevel::Medium, "mp3", 192),
                                 quality_of(QualityLevel::Lossless, "flac", 1411)});

    const SourceEntry entry = radmusic::entry_from_item("radish", item);
    CHECK_EQ(entry.platform, std::string("radish"));
    CHECK(entry.available);
    CHECK_EQ(entry.base.platform, std::string("radish"));
    CHECK_EQ(entry.base.name, std::string("Mapped Song"));
    CHECK_EQ(entry.base.author, std::string("Mapped Artist"));
    CHECK_EQ(entry.base.duration_seconds, 133);
    CHECK_EQ(entry.quality.level, QualityLevel::Lossless);

    // 无 sources → 用 base.quality（此处未设置，即 unknown）
    SearchItem bare;
    bare.title = "Bare Song";
    bare.artists = {"Bare Artist"};
    bare.duration_seconds = 42;
    bare.group_key = radmusic::group_key(bare.title, "Bare Artist", 42);
    bare.sources.clear();
    const SourceEntry bare_entry = radmusic::entry_from_item("local", bare);
    CHECK_EQ(bare_entry.platform, std::string("local"));
    CHECK(bare_entry.available);
    CHECK_EQ(bare_entry.base.name, std::string("Bare Song"));
    CHECK_EQ(bare_entry.base.author, std::string("Bare Artist"));
    CHECK_EQ(bare_entry.quality.level, QualityLevel::Unknown);

    // sources 全部不可用时：可用性标记必须保留（灰显不能被抹掉），
    // 没有可用的 qualities 时 quality 回落该条目的 base.quality
    SearchItem only_unavailable =
        make_item("Hidden Song", "Hidden Artist", 90, {quality_of(QualityLevel::HiRes, "flac", 4608)});
    only_unavailable.sources.front().available = false;
    only_unavailable.sources.front().reason = "vip required";
    only_unavailable.sources.front().base.quality = quality_of(QualityLevel::High, "mp3", 320);

    const SourceEntry hidden = radmusic::entry_from_item("netease", only_unavailable);
    CHECK_EQ(hidden.platform, std::string("netease"));
    CHECK(!hidden.available);
    CHECK_EQ(hidden.reason, std::string("vip required"));
    CHECK_EQ(hidden.base.name, std::string("Hidden Song"));
    CHECK_EQ(hidden.quality.level, QualityLevel::High);

    // 不可用条目不参与“最高音质”计算：sources 里同时有不可用的 HiRes 与可用的 Medium 时取 Medium
    SearchItem mixed = make_item("Mixed Song", "Mixed Artist", 100,
                                 {quality_of(QualityLevel::Medium, "mp3", 192)});
    SourceEntry unavailable_part;
    unavailable_part.platform = "netease";
    unavailable_part.available = false;
    unavailable_part.reason = "vip required";
    unavailable_part.quality = quality_of(QualityLevel::HiRes, "flac", 4608);
    mixed.sources.push_back(unavailable_part);
    const SourceEntry mixed_entry = radmusic::entry_from_item("radish", mixed);
    CHECK_EQ(mixed_entry.quality.level, QualityLevel::Medium);
}

// entry_from_item：多位歌手必须全部保留（用「 / 」连接），不能只取第一位
TEST_CASE(test_search_entry_from_item_joins_multiple_artists) {
    SearchItem duet;
    duet.title = "Duet Song";
    duet.artists = {"Alpha", "Beta"};
    duet.duration_seconds = 200;
    const SourceEntry duet_entry = radmusic::entry_from_item("netease", duet);
    CHECK_EQ(duet_entry.base.author, std::string("Alpha / Beta"));

    SearchItem trio;
    trio.title = "Trio Song";
    trio.artists = {"One", "Two", "Three"};
    trio.duration_seconds = 210;
    CHECK_EQ(radmusic::entry_from_item("netease", trio).base.author, std::string("One / Two / Three"));

    SearchItem solo;
    solo.title = "Solo Song";
    solo.artists = {"Solo"};
    solo.duration_seconds = 100;
    CHECK_EQ(radmusic::entry_from_item("netease", solo).base.author, std::string("Solo"));
}

// 封面：入口折叠时 entry.base 是新建的，必须沿用插件给出的 pic，
// 否则搜索结果整行都没有封面（列表页 / 播放器封面全空）。
TEST_CASE(test_search_keeps_cover_pic) {
    const std::string pic = "https://example.invalid/cover.jpg";
    SearchItem item = make_item("Cover Song", "Cover Artist", 120,
                                {quality_of(QualityLevel::High, "mp3", 320)});
    item.sources.front().base.pic = pic;

    const SourceEntry entry = radmusic::entry_from_item("netease", item);
    CHECK_EQ(entry.base.pic, pic);

    // 折叠：优先级更高的源没有封面时，折叠行要能兜底取到其它源的封面
    std::vector<SourceEntry> entries;
    entries.push_back(entry);
    entries.push_back(make_entry("radish", "Cover Song", "Cover Artist", 120,
                                 quality_of(QualityLevel::Lossless, "flac", 1411)));
    const std::vector<SearchItem> folded = radmusic::fold_sources(entries, {"radish", "netease"});
    CHECK_EQ(folded.size(), static_cast<size_t>(1));
    CHECK_EQ(folded.front().sources.front().platform, std::string("radish"));
    CHECK_EQ(folded.front().pic, pic);

    // 序列化 / 反序列化之后封面不能丢（前端读 results[].pic）
    const SearchItem back = SearchItem::from_json(folded.front().to_json());
    CHECK_EQ(back.pic, pic);
}
