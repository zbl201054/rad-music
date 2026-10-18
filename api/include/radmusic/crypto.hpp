// Rad music API - 加密/摘要原语（零依赖，C++17）
// 提供：SHA-256、MD5（兼容旧 sec.json）、HMAC-SHA256、PBKDF2-HMAC-SHA256、
//       Base64、常量时间比较、AES-256-GCM（1.1.1 / 1.4 建议的加密方案）。
// 约定：所有函数不抛异常；失败通过 ok 出参或返回值表达。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace radmusic::crypto {

using Bytes = std::vector<uint8_t>;
using Sha256Digest = std::array<uint8_t, 32>;
using Md5Digest = std::array<uint8_t, 16>;

std::string to_hex(const uint8_t* data, size_t len);
Bytes from_hex(std::string_view hex, bool* ok = nullptr);

// ---------- 增量摘要 ----------
class Sha256 {
public:
    Sha256();
    void update(const uint8_t* data, size_t len);
    void update(std::string_view data);
    Sha256Digest final();          // 只能调用一次；再次调用返回上次结果
    std::string hex();             // 便捷：final() + to_hex

private:
    void transform(const uint8_t* block);
    uint32_t h_[8];
    uint8_t buffer_[64];
    size_t buffered_;
    uint64_t total_bits_;
    bool finalized_;
    Sha256Digest last_;
};

class Md5 {
public:
    Md5();
    void update(const uint8_t* data, size_t len);
    void update(std::string_view data);
    Md5Digest final();
    std::string hex();

private:
    void transform(const uint8_t* block);
    uint32_t h_[4];
    uint8_t buffer_[64];
    size_t buffered_;
    uint64_t total_bits_;
    bool finalized_;
    Md5Digest last_;
};

// ---------- 一次性摘要 ----------
Sha256Digest sha256(const uint8_t* data, size_t len);
Sha256Digest sha256(std::string_view data);
std::string sha256_hex(std::string_view data);

Md5Digest md5(const uint8_t* data, size_t len);
Md5Digest md5(std::string_view data);
std::string md5_hex(std::string_view data);

// 文件摘要（分块读取，适合大文件）；失败时 ok = false
std::string sha256_file_hex(const std::string& path, bool* ok = nullptr);
std::string md5_file_hex(const std::string& path, bool* ok = nullptr);

// ---------- HMAC / PBKDF2 ----------
Sha256Digest hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len);
Sha256Digest hmac_sha256(std::string_view key, std::string_view msg);

Bytes pbkdf2_hmac_sha256(std::string_view password, const Bytes& salt, uint32_t iterations, size_t dk_len);

// ---------- Base64 ----------
std::string base64_encode(const uint8_t* data, size_t len);
std::string base64_encode(const Bytes& data);
std::string base64_encode(std::string_view text);
bool base64_decode(std::string_view text, Bytes& out);

bool constant_time_equals(const uint8_t* a, const uint8_t* b, size_t len);

// ---------- AES-256-GCM ----------
struct AeadResult {
    bool ok = false;
    Bytes data;            // 解密：明文；加密：密文||tag(16B)
    std::string error;
};

// 加密：输出 密文 || 16 字节 tag；nonce 建议 12 字节（其他长度按 GCM 规范处理）
AeadResult aes256_gcm_encrypt(const std::array<uint8_t, 32>& key, const Bytes& nonce,
                              const Bytes& plaintext, const Bytes& aad = Bytes{});
// 解密：输入 密文 || 16 字节 tag
AeadResult aes256_gcm_decrypt(const std::array<uint8_t, 32>& key, const Bytes& nonce,
                              const Bytes& ciphertext_with_tag, const Bytes& aad = Bytes{});

}  // namespace radmusic::crypto
