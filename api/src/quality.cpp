#include "radmusic/quality.hpp"

#include <algorithm>
#include <cctype>

namespace radmusic {
namespace {

std::string lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

std::string int_or_empty(const std::optional<int>& v) {
    return v.has_value() ? std::to_string(*v) : std::string();
}

}  // namespace

const char* quality_level_id(QualityLevel level) {
    switch (level) {
        case QualityLevel::HiRes: return "hires";
        case QualityLevel::Lossless: return "lossless";
        case QualityLevel::High: return "high";
        case QualityLevel::Medium: return "medium";
        case QualityLevel::Low: return "low";
        default: return "unknown";
    }
}

QualityLevel quality_level_from_id(std::string_view id) {
    const std::string s = lower(id);
    if (s == "hires" || s == "hi-res" || s == "hi_res") return QualityLevel::HiRes;
    if (s == "lossless") return QualityLevel::Lossless;
    if (s == "high") return QualityLevel::High;
    if (s == "medium") return QualityLevel::Medium;
    if (s == "low") return QualityLevel::Low;
    return QualityLevel::Unknown;
}

int quality_level_rank(QualityLevel level) { return static_cast<int>(level); }

QualityLevel infer_quality_level(bool lossless,
                                 std::optional<int> sample_rate,
                                 std::optional<int> bit_depth,
                                 std::optional<int> bitrate) {
    if (lossless) {
        const bool hi_res = (sample_rate.has_value() && *sample_rate > 48000) ||
                            (bit_depth.has_value() && *bit_depth > 16);
        return hi_res ? QualityLevel::HiRes : QualityLevel::Lossless;
    }
    if (bitrate.has_value()) {
        if (*bitrate >= 256) return QualityLevel::High;
        if (*bitrate >= 160) return QualityLevel::Medium;
        return QualityLevel::Low;
    }
    return QualityLevel::Unknown;
}

int Quality::compare(const Quality& other) const {
    const int ra = quality_level_rank(level);
    const int rb = quality_level_rank(other.level);
    if (ra != rb) return ra < rb ? -1 : 1;

    const int sa = sample_rate.value_or(0);
    const int sb = other.sample_rate.value_or(0);
    if (sa != sb) return sa < sb ? -1 : 1;

    const int ba = bit_depth.value_or(0);
    const int bb = other.bit_depth.value_or(0);
    if (ba != bb) return ba < bb ? -1 : 1;

    const int bra = bitrate.value_or(0);
    const int brb = other.bitrate.value_or(0);
    if (bra != brb) return bra < brb ? -1 : 1;

    // 无损优先；同无损性时按 codec 字典序保证稳定
    if (lossless != other.lossless) return lossless ? 1 : -1;
    if (codec != other.codec) return codec < other.codec ? -1 : 1;
    return 0;
}

bool Quality::same_identity(const Quality& other) const {
    if (level != other.level) return false;
    if (lossless != other.lossless) return false;
    if (codec != other.codec) return false;
    if (bitrate.value_or(0) != other.bitrate.value_or(0)) return false;
    if (sample_rate.value_or(0) != other.sample_rate.value_or(0)) return false;
    if (bit_depth.value_or(0) != other.bit_depth.value_or(0)) return false;
    return true;
}

std::string Quality::key_suffix() const {
    if (!quality_id.empty()) return quality_id;
    std::string out = quality_level_id(level);
    if (!codec.empty()) {
        out += "-";
        out += codec;
    }
    if (bitrate.has_value()) {
        out += "-";
        out += std::to_string(*bitrate);
    }
    return out;
}

json::Value Quality::to_json() const {
    json::Value v = json::Value::object();
    v.set("level", std::string(quality_level_id(level)));
    if (!codec.empty()) v.set("codec", codec);
    v.set("lossless", lossless);
    if (bitrate.has_value()) v.set("bitrate", *bitrate);
    if (sample_rate.has_value()) v.set("sample_rate", *sample_rate);
    if (bit_depth.has_value()) v.set("bit_depth", *bit_depth);
    if (channels.has_value()) v.set("channels", *channels);
    if (!quality_id.empty()) v.set("quality_id", quality_id);
    if (!label.empty()) v.set("label", label);
    return v;
}

Quality Quality::from_json(const json::Value& v) {
    Quality q;
    if (!v.is_object()) return q;
    q.level = quality_level_from_id(v.get_string("level", "unknown"));
    q.codec = v.get_string("codec");
    q.lossless = v.get_bool("lossless", false);
    if (const json::Value* b = v.find("bitrate"); b && b->is_number()) q.bitrate = static_cast<int>(b->as_int());
    if (const json::Value* b = v.find("sample_rate"); b && b->is_number()) q.sample_rate = static_cast<int>(b->as_int());
    if (const json::Value* b = v.find("bit_depth"); b && b->is_number()) q.bit_depth = static_cast<int>(b->as_int());
    if (const json::Value* b = v.find("channels"); b && b->is_number()) q.channels = static_cast<int>(b->as_int());
    q.quality_id = v.get_string("quality_id");
    q.label = v.get_string("label");
    if (q.codec.empty() && !v.contains("codec")) q.codec.clear();
    return q;
}

json::Value QualityLevelSpec::to_json() const {
    json::Value v = spec.to_json();
    v.set("id", id);
    if (!label.empty()) v.set("label", label);
    return v;
}

QualityLevelSpec QualityLevelSpec::from_json(const json::Value& v) {
    QualityLevelSpec spec;
    if (!v.is_object()) return spec;
    spec.id = v.get_string("id");
    spec.label = v.get_string("label");
    Quality q = Quality::from_json(v);
    q.quality_id = spec.id;
    if (q.label.empty()) q.label = spec.label;
    if (!q.is_known()) {
        // 清单没写 level 时，按 codec/码率推断，保持宽容
        const bool lossless = q.lossless || q.codec == "flac" || q.codec == "alac" || q.codec == "pcm";
        q.lossless = lossless;
        q.level = infer_quality_level(lossless, q.sample_rate, q.bit_depth, q.bitrate);
    }
    spec.spec = q;
    return spec;
}

bool QualityLevelSpec::validate_all(const std::vector<QualityLevelSpec>& specs, std::string* error) {
    for (size_t i = 0; i < specs.size(); ++i) {
        const auto& s = specs[i];
        if (s.id.empty()) {
            if (error) *error = "quality_levels[" + std::to_string(i) + "].id 为空";
            return false;
        }
        if (!s.spec.is_known()) {
            if (error) *error = "quality_levels[" + s.id + "].level 非法或缺失";
            return false;
        }
        for (size_t j = i + 1; j < specs.size(); ++j) {
            if (specs[j].id == s.id) {
                if (error) *error = "quality_levels 存在重复 id: " + s.id;
                return false;
            }
        }
    }
    return true;
}

std::string quality_key(const std::string& platform, const Quality& quality) {
    return platform + "." + quality.key_suffix();
}

std::optional<Quality> resolve_quality(const std::vector<QualityLevelSpec>& specs, std::string_view quality_id) {
    for (const auto& s : specs) {
        if (s.id == quality_id) {
            Quality q = s.spec;
            q.quality_id = s.id;
            if (q.label.empty()) q.label = s.label;
            return q;
        }
    }
    return std::nullopt;
}

std::optional<Quality> pick_quality(const std::vector<QualityLevelSpec>& specs,
                                    const std::vector<std::string>& available_ids,
                                    std::string_view request) {
    const std::string req = lower(request);

    // 1) 平台 quality_id 精确匹配
    for (const auto& s : specs) {
        if (!available_ids.empty() &&
            std::find(available_ids.begin(), available_ids.end(), s.id) == available_ids.end()) {
            continue;
        }
        if (lower(s.id) == req) return resolve_quality(specs, s.id);
    }

    // 2) 归一化 level：取该等级中规格最高的一条
    const QualityLevel wanted = quality_level_from_id(req);
    if (wanted != QualityLevel::Unknown) {
        std::optional<Quality> best;
        for (const auto& s : specs) {
            if (s.spec.level != wanted) continue;
            if (!available_ids.empty() &&
                std::find(available_ids.begin(), available_ids.end(), s.id) == available_ids.end()) {
                continue;
            }
            if (!best.has_value() || best->compare(s.spec) < 0) best = resolve_quality(specs, s.id);
        }
        if (best.has_value()) return best;
    }

    // 3) auto：取最高可用
    if (req == "auto" || req.empty()) {
        std::optional<Quality> best;
        for (const auto& s : specs) {
            if (!available_ids.empty() &&
                std::find(available_ids.begin(), available_ids.end(), s.id) == available_ids.end()) {
                continue;
            }
            if (!best.has_value() || best->compare(s.spec) < 0) best = resolve_quality(specs, s.id);
        }
        return best;
    }

    return std::nullopt;
}

}  // namespace radmusic
