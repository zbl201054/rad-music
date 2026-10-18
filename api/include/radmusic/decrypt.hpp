// Rad music API - 解密（对应 Readme.md 1.4：usec.json / sec.json / resecrect.cpp）
//
// 兼容性：
//  * v1（旧）：sec.json 用 MD5(口令) 作校验值（Readme 原文），密钥按 SHA-256(口令) 或 PBKDF2 派生；
//  * v2（建议）：PBKDF2-HMAC-SHA256 派生 + 常量时间校验 + AES-256-GCM（认证失败即失败）。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "radmusic/error.hpp"
#include "radmusic/json.hpp"
#include "radmusic/storage.hpp"

namespace radmusic {

// sec.json（Readme 1.4）
struct SecInfo {
    int version = 2;                  // 1 = 旧 MD5 校验；2 = PBKDF2 + AES-256-GCM
    std::string alg = "AES-256-GCM";
    std::string kdf = "PBKDF2-HMAC-SHA256";
    std::string salt_b64;
    uint32_t iter = 200000;
    std::string verifier;             // v1: MD5(口令) hex；v2: PBKDF2 派生值 hex
    std::string nonce_b64;
    std::string sha_plain;            // 明文 SHA-256（可选）
    bool has_tag = true;              // 密文是否带 16 字节 GCM tag

    json::Value to_json() const;
    static SecInfo from_json(const json::Value& v);
};

// usec.json（Readme 1.4）
struct UsecInfo {
    std::string usec;                 // 用户密钥，或指向会话密钥的引用
    long long updated_at = 0;

    json::Value to_json() const;
    static UsecInfo from_json(const json::Value& v);
};

class Decryptor {
public:
    explicit Decryptor(const LibraryPaths& paths) : paths_(paths) {}

    Result<UsecInfo> load_usec() const;
    Status save_usec(const UsecInfo& info) const;

    Result<SecInfo> load_sec(std::string_view sha) const;
    Status save_sec(std::string_view sha, const SecInfo& sec) const;

    // 口令校验：不符 → 1003 usec error
    Status verify_password(const SecInfo& sec, std::string_view password) const;

    // 解密：in 为密文文件，out 为明文输出。失败 → 1006 decrypt failed / 1003 usec error
    Result<std::string> decrypt_file(const SecInfo& sec, std::string_view password,
                                     const std::filesystem::path& in,
                                     const std::filesystem::path& out) const;

    // 加密（用于导入加密曲目与测试）：同时回填 sec_out
    Status encrypt_file(const std::filesystem::path& in, const std::filesystem::path& out,
                        std::string_view password, uint32_t iterations, SecInfo* sec_out) const;

    // 旧格式校验值
    static std::string password_md5(std::string_view password);
    // 由口令与 sec 派生 32 字节密钥
    static std::array<uint8_t, 32> derive_key(const SecInfo& sec, std::string_view password);

private:
    const LibraryPaths& paths_;
};

}  // namespace radmusic
