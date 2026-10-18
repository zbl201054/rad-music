#include "radmusic/decrypt.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <sstream>
#include <vector>

#include "radmusic/crypto.hpp"

namespace radmusic {
namespace {

Result<crypto::Bytes> read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return Result<crypto::Bytes>::failure(ErrorCode::FileNotFound, "无法读取文件: " + path.u8string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    return Result<crypto::Bytes>::success(crypto::Bytes(text.begin(), text.end()));
}

Status write_all(const std::filesystem::path& path, const crypto::Bytes& data) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return Status(ErrorCode::RequestFailed, "无法写入文件: " + path.u8string());
    if (!data.empty()) out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out.good()) return Status(ErrorCode::RequestFailed, "写入失败: " + path.u8string());
    return Status::success();
}

long long now_seconds() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

json::Value SecInfo::to_json() const {
    json::Value v = json::Value::object();
    v.set("version", static_cast<long long>(version));
    v.set("alg", alg);
    v.set("kdf", kdf);
    v.set("salt", salt_b64);
    v.set("iter", static_cast<long long>(iter));
    v.set("verifier", verifier);
    v.set("nonce", nonce_b64);
    if (!sha_plain.empty()) v.set("sha_plain", sha_plain);
    v.set("has_tag", has_tag);
    return v;
}

SecInfo SecInfo::from_json(const json::Value& v) {
    SecInfo s;
    if (!v.is_object()) return s;
    s.version = static_cast<int>(v.get_int("version", 2));
    s.alg = v.get_string("alg", "AES-256-GCM");
    s.kdf = v.get_string("kdf", "PBKDF2-HMAC-SHA256");
    s.salt_b64 = v.get_string("salt");
    s.iter = static_cast<uint32_t>(v.get_int("iter", 200000));
    s.verifier = v.get_string("verifier");
    s.nonce_b64 = v.get_string("nonce");
    s.sha_plain = v.get_string("sha_plain");
    s.has_tag = v.get_bool("has_tag", true);
    // 旧格式没有 version/kdf：有 md5 或 无 salt 时按 v1 处理
    if (!v.contains("version") && !v.contains("kdf")) s.version = 1;
    return s;
}

json::Value UsecInfo::to_json() const {
    json::Value v = json::Value::object();
    v.set("usec", usec);
    v.set("updated_at", updated_at);
    return v;
}

UsecInfo UsecInfo::from_json(const json::Value& v) {
    UsecInfo u;
    if (!v.is_object()) return u;
    u.usec = v.get_string("usec");
    u.updated_at = v.get_int("updated_at", 0);
    return u;
}

Result<UsecInfo> Decryptor::load_usec() const {
    const auto file = paths_.usec_file();
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return Result<UsecInfo>::failure(ErrorCode::UsecError, "usec.json 不存在");
    }
    auto text = read_text_file(file);
    if (!text.ok()) return Result<UsecInfo>(text.status());
    json::ParseError err;
    json::Value parsed = json::Value::parse(text.value(), &err);
    if (!parsed.is_object()) {
        return Result<UsecInfo>::failure(ErrorCode::UsecError, "usec.json 解析失败: " + err.message);
    }
    UsecInfo info = UsecInfo::from_json(parsed);
    if (info.usec.empty()) {
        return Result<UsecInfo>::failure(ErrorCode::UsecError, "usec.json 中密钥为空");
    }
    return Result<UsecInfo>::success(info);
}

Status Decryptor::save_usec(const UsecInfo& info) const {
    UsecInfo copy = info;
    if (copy.updated_at == 0) copy.updated_at = now_seconds();
    return write_text_file_atomic(paths_.root, paths_.usec_file(), copy.to_json().dump(2));
}

Result<SecInfo> Decryptor::load_sec(std::string_view sha) const {
    const auto file = paths_.sec_file(std::string(sha));
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return Result<SecInfo>::failure(ErrorCode::FileNotFound, "sec.json 不存在: " + file.u8string());
    }
    auto text = read_text_file(file);
    if (!text.ok()) return Result<SecInfo>(text.status());
    json::ParseError err;
    json::Value parsed = json::Value::parse(text.value(), &err);
    if (!parsed.is_object()) {
        return Result<SecInfo>::failure(ErrorCode::InvalidMetadata, "sec.json 解析失败: " + err.message);
    }
    SecInfo sec = SecInfo::from_json(parsed);
    if (sec.verifier.empty()) {
        return Result<SecInfo>::failure(ErrorCode::InvalidMetadata, "sec.json 缺少 verifier");
    }
    return Result<SecInfo>::success(sec);
}

Status Decryptor::save_sec(std::string_view sha, const SecInfo& sec) const {
    const auto dir = paths_.track_dir(std::string(sha));
    return write_text_file_atomic(dir, paths_.sec_file(std::string(sha)), sec.to_json().dump(2));
}

std::string Decryptor::password_md5(std::string_view password) {
    return crypto::md5_hex(password);
}

std::array<uint8_t, 32> Decryptor::derive_key(const SecInfo& sec, std::string_view password) {
    std::array<uint8_t, 32> key{};
    crypto::Bytes salt;
    if (!sec.salt_b64.empty()) {
        crypto::base64_decode(sec.salt_b64, salt);
    }
    if (sec.version >= 2 || !salt.empty()) {
        const crypto::Bytes dk =
            crypto::pbkdf2_hmac_sha256(password, salt, sec.iter == 0 ? 1u : sec.iter, 32u);
        for (size_t i = 0; i < 32 && i < dk.size(); ++i) key[i] = dk[i];
        return key;
    }
    // v1 且无 salt：退化为 SHA-256(口令)
    const crypto::Sha256Digest d = crypto::sha256(password);
    for (size_t i = 0; i < 32; ++i) key[i] = d[i];
    return key;
}

Status Decryptor::verify_password(const SecInfo& sec, std::string_view password) const {
    if (password.empty()) return Status(ErrorCode::UsecError, "密钥为空");
    if (sec.version <= 1) {
        // 旧格式：MD5(口令) 与 sec.json 的 verifier 比较（常量时间）
        const std::string want = sec.verifier;
        const std::string got = password_md5(password);
        if (want.size() != got.size()) return Status(ErrorCode::UsecError, "密钥校验失败（usec error）");
        if (!crypto::constant_time_equals(reinterpret_cast<const uint8_t*>(want.data()),
                                          reinterpret_cast<const uint8_t*>(got.data()), got.size())) {
            return Status(ErrorCode::UsecError, "密钥校验失败（usec error）");
        }
        return Status::success();
    }

    // v2：用 PBKDF2 派生后与 verifier（hex）比较
    crypto::Bytes salt;
    if (!sec.salt_b64.empty() && !crypto::base64_decode(sec.salt_b64, salt)) {
        return Status(ErrorCode::InvalidMetadata, "sec.json 的 salt 不是合法 Base64");
    }
    const crypto::Bytes dk =
        crypto::pbkdf2_hmac_sha256(password, salt, sec.iter == 0 ? 1u : sec.iter, 32u);
    const std::string got = crypto::to_hex(dk.data(), dk.size());
    bool ok = false;
    crypto::Bytes want = crypto::from_hex(sec.verifier, &ok);
    if (!ok || want.size() != dk.size()) {
        // verifier 可能是别的长度/格式：退化为字符串比较（仍常量时间）
        if (sec.verifier.size() != got.size()) return Status(ErrorCode::UsecError, "密钥校验失败（usec error）");
        if (!crypto::constant_time_equals(reinterpret_cast<const uint8_t*>(sec.verifier.data()),
                                          reinterpret_cast<const uint8_t*>(got.data()), got.size())) {
            return Status(ErrorCode::UsecError, "密钥校验失败（usec error）");
        }
        return Status::success();
    }
    if (!crypto::constant_time_equals(want.data(), dk.data(), dk.size())) {
        return Status(ErrorCode::UsecError, "密钥校验失败（usec error）");
    }
    return Status::success();
}

Result<std::string> Decryptor::decrypt_file(const SecInfo& sec, std::string_view password,
                                            const std::filesystem::path& in,
                                            const std::filesystem::path& out) const {
    Status verified = verify_password(sec, password);
    if (!verified.ok()) return Result<std::string>(verified);

    auto cipher = read_all(in);
    if (!cipher.ok()) return Result<std::string>(cipher.status());

    if (sec.alg != "AES-256-GCM") {
        return Result<std::string>::failure(ErrorCode::DecryptFailed, "不支持的加密算法: " + sec.alg);
    }

    crypto::Bytes nonce;
    if (sec.nonce_b64.empty() || !crypto::base64_decode(sec.nonce_b64, nonce)) {
        return Result<std::string>::failure(ErrorCode::DecryptFailed, "sec.json 缺少合法 nonce");
    }

    const auto key = derive_key(sec, password);
    const crypto::AeadResult plain = crypto::aes256_gcm_decrypt(key, nonce, cipher.value());
    if (!plain.ok) {
        return Result<std::string>::failure(ErrorCode::DecryptFailed,
                                           plain.error.empty() ? "解密或认证失败" : plain.error);
    }

    if (!sec.sha_plain.empty()) {
        const std::string got = crypto::sha256_hex(
            std::string_view(reinterpret_cast<const char*>(plain.data.data()), plain.data.size()));
        if (got != sec.sha_plain) {
            return Result<std::string>::failure(ErrorCode::DecryptFailed,
                                               "解密后明文 sha 与 sec.json 的 sha_plain 不一致");
        }
    }

    Status w = write_all(out, plain.data);
    if (!w.ok()) return Result<std::string>(w);
    return Result<std::string>::success(out.u8string());
}

Status Decryptor::encrypt_file(const std::filesystem::path& in, const std::filesystem::path& out,
                               std::string_view password, uint32_t iterations,
                               SecInfo* sec_out) const {
    if (password.empty()) return Status(ErrorCode::UsecError, "密钥为空");
    auto data = read_all(in);
    if (!data.ok()) return data.status();

    SecInfo sec;
    sec.version = 2;
    sec.alg = "AES-256-GCM";
    sec.kdf = "PBKDF2-HMAC-SHA256";
    sec.iter = iterations == 0 ? 1u : iterations;
    sec.has_tag = true;

    crypto::Bytes salt(16);
    for (size_t i = 0; i < salt.size(); ++i) {
        salt[i] = static_cast<uint8_t>((i * 131u + 17u + static_cast<unsigned>(sec.iter)) & 0xFFu);
    }
    sec.salt_b64 = crypto::base64_encode(salt);

    crypto::Bytes nonce(12);
    for (size_t i = 0; i < nonce.size(); ++i) {
        nonce[i] = static_cast<uint8_t>((i * 37u + 91u + static_cast<unsigned>(data.value().size())) & 0xFFu);
    }
    sec.nonce_b64 = crypto::base64_encode(nonce);

    const crypto::Bytes dk = crypto::pbkdf2_hmac_sha256(password, salt, sec.iter, 32u);
    sec.verifier = crypto::to_hex(dk.data(), dk.size());
    sec.sha_plain = crypto::sha256_hex(
        std::string_view(reinterpret_cast<const char*>(data.value().data()), data.value().size()));

    std::array<uint8_t, 32> key{};
    for (size_t i = 0; i < 32; ++i) key[i] = dk[i];

    const crypto::AeadResult enc = crypto::aes256_gcm_encrypt(key, nonce, data.value());
    if (!enc.ok) return Status(ErrorCode::DecryptFailed, enc.error.empty() ? "加密失败" : enc.error);

    Status w = write_all(out, enc.data);
    if (!w.ok()) return w;
    if (sec_out) *sec_out = sec;
    return Status::success();
}

}  // namespace radmusic
