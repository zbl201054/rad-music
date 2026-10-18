#include "radmusic/engine.hpp"

#include <algorithm>
#include <filesystem>

#include "radmusic/crypto.hpp"
#include "radmusic/groupkey.hpp"

namespace radmusic {
namespace {

bool looks_like_sha(std::string_view s) {
    if (s.size() != 64) return false;
    for (unsigned char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// key 形式：<sha> / sha:<sha> / online:<platform>:<id> / <platform>:<id>
struct KeyParts {
    std::string sha;
    std::string platform;
    std::string item_id;
    bool online() const { return !platform.empty() && !item_id.empty(); }
};

KeyParts parse_key(std::string_view key) {
    KeyParts parts;
    std::string s(key);
    if (s.rfind("sha:", 0) == 0) {
        parts.sha = s.substr(4);
        return parts;
    }
    if (s.rfind("online:", 0) == 0) {
        const std::string rest = s.substr(7);
        const size_t pos = rest.find(':');
        if (pos != std::string::npos) {
            parts.platform = rest.substr(0, pos);
            parts.item_id = rest.substr(pos + 1);
        } else {
            parts.platform = rest;
        }
        return parts;
    }
    if (looks_like_sha(s)) {
        parts.sha = s;
        return parts;
    }
    const size_t pos = s.find(':');
    if (pos != std::string::npos) {
        parts.platform = s.substr(0, pos);
        parts.item_id = s.substr(pos + 1);
    }
    return parts;
}

}  // namespace

CoreEngine::CoreEngine(const LibraryPaths& paths, PluginRegistry& plugins, EngineOptions options)
    : paths_(paths), plugins_(plugins), options_(options), decryptor_(paths), meta_(paths) {
    sha_provider = [](const std::string& path) -> Result<std::string> {
        bool ok = false;
        const std::string hex = crypto::sha256_file_hex(path, &ok);
        if (!ok) return Result<std::string>::failure(ErrorCode::FileNotFound, "无法计算 sha: " + path);
        return Result<std::string>::success(hex);
    };
    file_exists = [](const std::string& path) {
        std::error_code ec;
        return std::filesystem::exists(path, ec);
    };
    copy_file = [](const std::string& from, const std::string& to) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(to).parent_path(), ec);
        std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
        return !ec;
    };
}

CoreEngine::~CoreEngine() = default;

std::optional<BaseInfo> CoreEngine::find_base(std::string_view key) {
    const KeyParts parts = parse_key(key);
    std::optional<MetaPath> path;
    if (!parts.sha.empty()) {
        path = meta_.find_by_sha(parts.sha);
    } else if (parts.online()) {
        path = meta_.find_by_platform_id(parts.platform, parts.item_id);
    }
    if (!path.has_value()) return std::nullopt;
    auto base = meta_.read(*path);
    if (!base.ok()) return std::nullopt;
    return base.value();
}

Result<BaseInfo> CoreEngine::resolve_base(std::string_view key) {
    if (auto found = find_base(key); found.has_value()) {
        return Result<BaseInfo>::success(*found);
    }
    // 1.3 规则 5：base.json 在缓存内，缺失时必须重建
    if (!options_.auto_rebuild_base || !rebuild_base) {
        return Result<BaseInfo>::failure(ErrorCode::BaseRebuildFailed,
                                        "缓存中没有 base.json 且未提供重建能力");
    }
    auto rebuilt = rebuild_base(key);
    if (!rebuilt.ok()) return rebuilt;

    BaseInfo base = rebuilt.value();
    Status valid = base.validate();
    if (!valid.ok()) return Result<BaseInfo>(valid);

    auto stored = meta_.put(base);
    if (!stored.ok()) return Result<BaseInfo>(stored.status());
    return Result<BaseInfo>::success(stored.value().base);
}

Result<LoadedTrack> CoreEngine::load(std::string_view key, std::string_view password) {
    auto base_result = resolve_base(key);
    if (!base_result.ok()) return Result<LoadedTrack>(base_result.status());
    BaseInfo base = base_result.value();

    // a. platform 检查（非 local 必须有插件）
    if (base.platform != "local" && options_.require_platform_plugin) {
        auto plugin = plugins_.source_for_platform(base.platform);
        if (!plugin) {
            return Result<LoadedTrack>::failure(ErrorCode::PlatformError,
                                                "无可用插件适配 platform: " + base.platform);
        }
    }

    LoadedTrack track;
    track.base = base;
    if (auto path = meta_.find_by_sha(base.sha); path.has_value()) track.meta_path = *path;

    std::string play_path = base.location;

    // 1.1 字段规范：location 可以是「相对 base.json 的路径、绝对路径或 http(s) URL」。
    // 导入（1.10）写入的是相对曲库根的 "tracks/<sha>/<file>"，因此相对路径在按当前
    // 工作目录找不到时，再尝试相对曲库根解析（绝对路径与在线 URL 不受影响）。
    if (play_path.rfind("http://", 0) != 0 && play_path.rfind("https://", 0) != 0) {
        const std::filesystem::path given(play_path);
        if (given.is_relative()) {
            const bool found_as_is = file_exists ? file_exists(play_path) : true;
            if (!found_as_is) {
                const std::string rooted = (paths_.root / given).u8string();
                if (!file_exists || file_exists(rooted)) play_path = rooted;
            }
        }
    }

    if (base.platform == "local" || play_path.rfind("http", 0) != 0) {
        if (file_exists && !file_exists(play_path)) {
            return Result<LoadedTrack>::failure(ErrorCode::FileNotFound, "文件不存在: " + play_path);
        }
        // b. sha 校验：不一致只警告（原文）
        if (options_.verify_sha && sha_provider) {
            auto actual = sha_provider(play_path);
            if (actual.ok() && actual.value() != base.sha) {
                track.sha_warning = true;
            }
        }
    } else if (open_online) {
        auto url = open_online(base);
        if (!url.ok()) return Result<LoadedTrack>(url.status());
        play_path = url.value();
    }

    // c. secret = 1：校验口令并解密
    if (base.secret == 1) {
        auto sec = decryptor_.load_sec(base.sha);
        if (!sec.ok()) return Result<LoadedTrack>(sec.status());
        Status verified = decryptor_.verify_password(sec.value(), password);
        if (!verified.ok()) return Result<LoadedTrack>(verified);

        const auto plain_dir = paths_.track_dir(base.sha);
        std::filesystem::path ext = std::filesystem::path(base.location).extension();
        if (ext.empty()) ext = ".bin";
        const auto plain_path = plain_dir / ("plain" + ext.string());
        const auto decrypted = decryptor_.decrypt_file(sec.value(), password, play_path, plain_path);
        if (!decrypted.ok()) return Result<LoadedTrack>(decrypted.status());
        play_path = decrypted.value();
        track.decrypted = true;
    }

    // d. 加载至缓存（1.9：cache/audio/<sha>.<ext>；在线 URL 不做整文件缓存）
    const bool is_local_file = (base.platform == "local") || (play_path.rfind("http", 0) != 0);
    if (options_.cache_audio && is_local_file && copy_file && file_exists && file_exists(play_path)) {
        std::filesystem::path ext = std::filesystem::path(play_path).extension();
        const auto cached = paths_.audio_cache() / (base.sha + (ext.empty() ? std::string(".bin") : ext.string()));
        if (!file_exists(cached.u8string())) {
            copy_file(play_path, cached.u8string());
        }
    }

    track.play_path = play_path;

    // 歌词（1.5）：失败不阻断播放
    if (lyric_provider) {
        auto lyric = lyric_provider(base);
        if (lyric.ok()) track.lyric = lyric.value();
    } else {
        auto lyric = load_lyric(base);
        if (lyric.ok()) track.lyric = lyric.value();
    }

    current_ = track;
    status_.state = PlayState::Stopped;
    status_.position_ms = 0;
    status_.duration_ms = static_cast<long long>(base.duration_seconds) * 1000;
    status_.sha = base.sha;
    status_.platform = base.platform;
    status_.item_id = base.item_id;
    return Result<LoadedTrack>::success(track);
}

Status CoreEngine::start() {
    if (!current_.has_value()) return Status(ErrorCode::NotLoaded, "未加载曲目");
    if (backend_start) {
        Status s = backend_start(current_->play_path);
        if (!s.ok()) return s;
    }
    status_.state = PlayState::Playing;
    push_smtc(PlayState::Playing);
    return Status::success();
}

Status CoreEngine::pause() {
    if (!current_.has_value()) return Status(ErrorCode::NotLoaded, "未加载曲目");
    if (status_.state != PlayState::Playing) {
        return Status(ErrorCode::NotLoaded, "当前不在播放状态（not playing）");
    }
    status_.state = PlayState::Paused;
    push_smtc(PlayState::Paused);
    return Status::success();
}

Status CoreEngine::resume() {
    if (!current_.has_value()) return Status(ErrorCode::NotLoaded, "未加载曲目");
    if (status_.state != PlayState::Paused) {
        return Status(ErrorCode::NotLoaded, "当前不在暂停状态（not paused）");
    }
    status_.state = PlayState::Playing;
    push_smtc(PlayState::Playing);
    return Status::success();
}

Status CoreEngine::stop() {
    if (backend_stop) backend_stop();
    status_.state = PlayState::Stopped;
    status_.position_ms = 0;
    push_smtc(PlayState::Stopped);
    return Status::success();
}

void CoreEngine::reset_session() {
    if (backend_stop) backend_stop();
    const double volume = status_.volume;
    current_.reset();
    status_ = PlayStatus();
    status_.volume = volume;
    push_smtc(PlayState::Stopped);
}

Status CoreEngine::seek(double seconds) {
    if (!current_.has_value()) return Status(ErrorCode::NotLoaded, "未加载曲目");
    double pos = seconds;
    if (pos < 0.0) pos = 0.0;
    const double duration = static_cast<double>(status_.duration_ms) / 1000.0;
    if (duration > 0.0 && pos > duration) pos = duration;
    status_.position_ms = static_cast<long long>(pos * 1000.0);
    push_smtc(status_.state);
    return Status::success();
}

Status CoreEngine::set_volume(double level) {
    if (level < 0.0 || level > 1.0) {
        return Status(ErrorCode::InvalidMetadata, "音量必须在 0.0 – 1.0（invalid argument）");
    }
    status_.volume = level;
    return Status::success();
}

int CoreEngine::active_lyric_line() const {
    if (!current_.has_value()) return -1;
    return find_active_line(current_->lyric, static_cast<int>(status_.position_ms));
}

Status CoreEngine::advance_to(long long position_ms) {
    if (!current_.has_value()) return Status(ErrorCode::NotLoaded, "未加载曲目");
    if (position_ms < 0) position_ms = 0;
    if (status_.duration_ms > 0 && position_ms > status_.duration_ms) position_ms = status_.duration_ms;
    status_.position_ms = position_ms;
    // 1.6：状态与进度必须上报系统媒体控件；上报失败不影响播放
    return push_smtc(status_.state);
}

Status CoreEngine::notify_ended() {
    status_.state = PlayState::Ended;
    if (status_.duration_ms > 0) status_.position_ms = status_.duration_ms;
    push_smtc(PlayState::Ended);
    return Status::success();
}

Status CoreEngine::push_smtc(PlayState state) {
    if (!current_.has_value()) return Status::success();
    Status s = sync_smtc();
    (void)state;
    return s;
}

Status CoreEngine::sync_smtc() {
    if (!smtc_ || !current_.has_value()) return Status::success();

    SmtcMetadata meta;
    meta.title = current_->base.name;
    meta.artist = current_->base.author;
    meta.album = current_->base.album;
    meta.duration_ms = status_.duration_ms;
    meta.cover_path = current_->base.pic;

    // 1.6：元数据在 start 成功后更新；切源/切曲也必须刷新
    Status m = smtc_->update_metadata(meta);
    Status s = smtc_->update_state(status_.state, status_.position_ms, status_.duration_ms);
    Status b = smtc_->set_buttons(false, false, status_.duration_ms > 0);

    if (!m.ok()) return m;
    if (!s.ok()) return s;
    return b;
}

Result<Lyric> CoreEngine::load_lyric(const BaseInfo& base) {
    // 1.5 来源优先级：本地 lyric.lrc → lyric.tr.lrc / lyric.roma.lrc → 播放源插件
    if (!base.sha.empty()) {
        const auto main_file = paths_.lyric_file(base.sha);
        std::error_code ec;
        if (std::filesystem::exists(main_file, ec)) {
            auto text = read_text_file(main_file);
            if (text.ok()) {
                auto parsed = parse_lrc(text.value());
                if (parsed.ok() && !parsed.value().empty()) {
                    Lyric lyric = parsed.value();
                    lyric.sha = base.sha;
                    const auto tr_file = paths_.lyric_file(base.sha, "tr.lrc");
                    if (std::filesystem::exists(tr_file, ec)) {
                        if (auto tr_text = read_text_file(tr_file); tr_text.ok()) {
                            if (auto tr = parse_lrc(tr_text.value()); tr.ok()) {
                                merge_track(lyric, tr.value(), 50, false);
                            }
                        }
                    }
                    const auto roma_file = paths_.lyric_file(base.sha, "roma.lrc");
                    if (std::filesystem::exists(roma_file, ec)) {
                        if (auto roma_text = read_text_file(roma_file); roma_text.ok()) {
                            if (auto roma = parse_lrc(roma_text.value()); roma.ok()) {
                                merge_track(lyric, roma.value(), 50, true);
                            }
                        }
                    }
                    return Result<Lyric>::success(lyric);
                }
            }
        }
    }

    // 在线：request(op = "get_lyric")
    if (auto plugin = plugins_.source_for_platform(base.platform); plugin) {
        json::Value params = json::Value::object();
        if (!base.item_id.empty()) params.set("id", base.item_id);
        if (!base.sha.empty()) params.set("sha", base.sha);
        auto result = plugin->request("get_lyric", params);
        if (result.ok()) {
            const json::Value& value = result.value();
            std::string lrc;
            if (value.is_string()) {
                lrc = value.as_string();
            } else if (value.is_object()) {
                lrc = value.get_string("lrc");
                if (lrc.empty()) lrc = value.get_string("lyric");
                if (lrc.empty()) lrc = value.get_string("text");
            }
            if (!lrc.empty()) {
                auto parsed = parse_lrc(lrc);
                if (parsed.ok()) {
                    Lyric lyric = parsed.value();
                    lyric.sha = base.sha;
                    lyric.source = base.platform;
                    return Result<Lyric>::success(lyric);
                }
            }
        }
    }

    // 1.5 规则 2：歌词失败不得阻断播放（1015 非致命）
    return Result<Lyric>::failure(ErrorCode::LyricUnavailable, "未找到可用歌词");
}

}  // namespace radmusic
