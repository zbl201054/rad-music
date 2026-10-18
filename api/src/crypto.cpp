// Rad music API - 加密/摘要原语实现（零依赖，C++17，只用标准库）
//
// 覆盖：
//   * SHA-256（增量 + 一次性 + 文件分块摘要）
//   * MD5（增量 + 一次性 + 文件分块摘要；兼容 1.4 旧 sec.json）
//   * HMAC-SHA256（key > 64B 先哈希；ipad/opad）
//   * PBKDF2-HMAC-SHA256（内部复用 HMAC 增量状态，避免每轮重复处理 key）
//   * to_hex / from_hex / Base64 编解码 / 常量时间比较
//   * AES-256-GCM（AES-256 加密 + GHASH 认证，支持任意长度 nonce）
//
// 约定：所有公开函数不抛出异常；失败通过 ok 出参或 AeadResult::ok 表达。
// 所有位运算均在 uint32_t / uint64_t 上进行，避免有符号左移等未定义行为。

#include "radmusic/crypto.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

namespace radmusic::crypto {
namespace {

// ---------- 小工具 ----------

inline uint32_t load_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline void store_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(v & 0xFFu);
}

inline uint32_t load_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void store_le32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline uint32_t load_be64_hi(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
inline uint32_t load_be64_lo(uint64_t v) { return static_cast<uint32_t>(v & 0xFFFFFFFFull); }

inline uint32_t rotr32(uint32_t v, uint32_t n) {
    return (v >> n) | (v << (32u - n));
}

inline uint32_t rotl32(uint32_t v, uint32_t n) {
    return (v << n) | (v >> (32u - n));
}

constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr uint32_t kMd5K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
    0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
    0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
    0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
    0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
    0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
    0xeb86d391u};

constexpr uint32_t kMd5S[64] = {7u, 12u, 17u, 22u, 7u, 12u, 17u, 22u, 7u, 12u, 17u, 22u, 7u, 12u, 17u,
                                22u, 5u, 9u,  14u, 20u, 5u, 9u,  14u, 20u, 5u, 9u,  14u, 20u, 5u, 9u,
                                14u, 20u, 4u, 11u, 16u, 23u, 4u,  11u, 16u, 23u, 4u, 11u, 16u, 23u, 4u,
                                11u, 16u, 23u, 6u, 10u, 15u, 21u, 6u,  10u, 15u, 21u, 6u, 10u, 15u, 21u,
                                6u, 10u, 15u, 21u};

// 分块文件读取大小：64 KiB
constexpr size_t kFileChunk = 64u * 1024u;

bool is_space(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int hex_value(uint8_t c) {
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<int>(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A') + 10;
    return -1;
}

// ---------- AES-256 基础（加密方向足够实现 GCM） ----------

// GF(2^8) 乘法，用于按需生成 S-box（alpha = 0x03）
uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1u) p = static_cast<uint8_t>(p ^ a);
        const uint8_t hi = static_cast<uint8_t>(a & 0x80u);
        a = static_cast<uint8_t>(a << 1);
        if (hi != 0) a = static_cast<uint8_t>(a ^ 0x1Bu);
        b = static_cast<uint8_t>(b >> 1);
    }
    return p;
}

uint8_t gf_inv(uint8_t a) {
    if (a == 0) return 0;
    for (uint32_t x = 1; x < 256u; ++x) {
        if (gf_mul(a, static_cast<uint8_t>(x)) == 1) return static_cast<uint8_t>(x);
    }
    return 0;
}

void compute_aes_sbox(uint8_t out[256]) {
    for (uint32_t x = 0; x < 256u; ++x) {
        const uint8_t inv = gf_inv(static_cast<uint8_t>(x));
        uint32_t r = 0;
        for (uint32_t i = 0; i < 8u; ++i) {
            const uint32_t a = (static_cast<uint32_t>(inv) >> i) & 1u;
            const uint32_t rot =
                (static_cast<uint32_t>(inv) >> ((i + 4) % 8)) & 1u;
            const uint32_t rot2 =
                (static_cast<uint32_t>(inv) >> ((i + 5) % 8)) & 1u;
            const uint32_t rot3 =
                (static_cast<uint32_t>(inv) >> ((i + 6) % 8)) & 1u;
            const uint32_t rot4 =
                (static_cast<uint32_t>(inv) >> ((i + 7) % 8)) & 1u;
            // 仿射变换：b_i = a_i ^ a_{i+4} ^ a_{i+5} ^ a_{i+6} ^ a_{i+7} ^ c_i，c = 0x63
            const uint32_t c = (0x63u >> i) & 1u;
            const uint32_t bit = a ^ rot ^ rot2 ^ rot3 ^ rot4 ^ c;
            r |= (bit << i);
        }
        out[x] = static_cast<uint8_t>(r);
    }
}

// S-box 只计算一次；函数内 static 初始化在 C++11 起是线程安全的。
const uint8_t* aes_sbox() {
    static const std::array<uint8_t, 256> kSbox = [] {
        std::array<uint8_t, 256> table{};
        compute_aes_sbox(table.data());
        return table;
    }();
    return kSbox.data();
}

constexpr uint8_t kRcon[15] = {0x00u, 0x01u, 0x02u, 0x04u, 0x08u, 0x10u, 0x20u, 0x40u,
                               0x80u, 0x1Bu, 0x36u, 0x6Cu, 0xD8u, 0xABu, 0x4Du};

// AES-256：密钥扩展 → 15 个轮密钥（240 字节 = 60 个 big-endian 字）
void aes256_expand_key(const uint8_t key[32], uint8_t rk[240]) {
    const uint8_t* s = aes_sbox();
    uint8_t w[60][4];
    for (uint32_t i = 0; i < 8u; ++i) {
        w[i][0] = key[4u * i];
        w[i][1] = key[4u * i + 1u];
        w[i][2] = key[4u * i + 2u];
        w[i][3] = key[4u * i + 3u];
    }
    for (uint32_t i = 8u; i < 60u; ++i) {
        uint8_t t[4];
        if (i % 8u == 0u) {
            t[0] = static_cast<uint8_t>(s[w[i - 1u][1]] ^ kRcon[i / 8u]);
            t[1] = s[w[i - 1u][2]];
            t[2] = s[w[i - 1u][3]];
            t[3] = s[w[i - 1u][0]];
        } else if (i % 8u == 4u) {
            t[0] = s[w[i - 1u][0]];
            t[1] = s[w[i - 1u][1]];
            t[2] = s[w[i - 1u][2]];
            t[3] = s[w[i - 1u][3]];
        } else {
            t[0] = w[i - 1u][0];
            t[1] = w[i - 1u][1];
            t[2] = w[i - 1u][2];
            t[3] = w[i - 1u][3];
        }
        for (int j = 0; j < 4; ++j) {
            w[i][j] = static_cast<uint8_t>(w[i - 8u][j] ^ t[j]);
        }
    }
    for (uint32_t i = 0; i < 60u; ++i) {
        rk[4u * i] = w[i][0];
        rk[4u * i + 1u] = w[i][1];
        rk[4u * i + 2u] = w[i][2];
        rk[4u * i + 3u] = w[i][3];
    }
}

void add_round_key(uint8_t st[16], const uint8_t* rk) {
    for (int i = 0; i < 16; ++i) st[i] = static_cast<uint8_t>(st[i] ^ rk[i]);
}

void aes_sub_bytes(uint8_t st[16]) {
    const uint8_t* s = aes_sbox();
    for (int i = 0; i < 16; ++i) st[i] = s[st[i]];
}

// 状态按 AES 标准列主序存放：st[4*c + r] = s[r][c]
void aes_shift_rows(uint8_t st[16]) {
    uint8_t t = st[1];
    st[1] = st[5];
    st[5] = st[9];
    st[9] = st[13];
    st[13] = t;

    uint8_t t0 = st[2];
    uint8_t t1 = st[6];
    st[2] = st[10];
    st[6] = st[14];
    st[10] = t0;
    st[14] = t1;

    t = st[15];
    st[15] = st[11];
    st[11] = st[7];
    st[7] = st[3];
    st[3] = t;
}

inline uint8_t xtime(uint8_t x) {
    const uint8_t hi = static_cast<uint8_t>(x & 0x80u);
    uint8_t r = static_cast<uint8_t>(x << 1);
    if (hi != 0) r = static_cast<uint8_t>(r ^ 0x1Bu);
    return r;
}

void aes_mix_columns(uint8_t st[16]) {
    for (int c = 0; c < 4; ++c) {
        uint8_t* p = st + 4 * c;
        const uint8_t a0 = p[0];
        const uint8_t a1 = p[1];
        const uint8_t a2 = p[2];
        const uint8_t a3 = p[3];
        const uint8_t x = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);
        p[0] = static_cast<uint8_t>(a0 ^ x ^ xtime(static_cast<uint8_t>(a0 ^ a1)));
        p[1] = static_cast<uint8_t>(a1 ^ x ^ xtime(static_cast<uint8_t>(a1 ^ a2)));
        p[2] = static_cast<uint8_t>(a2 ^ x ^ xtime(static_cast<uint8_t>(a2 ^ a3)));
        p[3] = static_cast<uint8_t>(a3 ^ x ^ xtime(static_cast<uint8_t>(a3 ^ a0)));
    }
}

// 单块 AES-256 加密（仅加密方向，GCM 的 CTR 只需要它）
void aes256_encrypt_block(const uint8_t rk[240], const uint8_t in[16], uint8_t out[16]) {
    uint8_t st[16];
    std::memcpy(st, in, 16);
    add_round_key(st, rk);
    for (int round = 1; round <= 13; ++round) {
        aes_sub_bytes(st);
        aes_shift_rows(st);
        aes_mix_columns(st);
        add_round_key(st, rk + 16 * round);
    }
    aes_sub_bytes(st);
    aes_shift_rows(st);
    add_round_key(st, rk + 16 * 14);
    std::memcpy(out, st, 16);
}

// ---------- GCM：GHASH over GF(2^128) ----------

// 按 NIST SP 800-38D 的位序实现（bit 0 = MSB of X[0]）
void gf128_mul(const uint8_t x[16], const uint8_t y[16], uint8_t out[16]) {
    uint8_t z[16];
    uint8_t v[16];
    std::memset(z, 0, 16);
    std::memcpy(v, y, 16);
    for (int i = 0; i < 128; ++i) {
        if (((x[i >> 3] >> (7 - (i & 7))) & 1u) != 0) {
            for (int j = 0; j < 16; ++j) z[j] = static_cast<uint8_t>(z[j] ^ v[j]);
        }
        const uint8_t lsb = static_cast<uint8_t>(v[15] & 1u);
        for (int j = 15; j > 0; --j) {
            v[j] = static_cast<uint8_t>((v[j] >> 1) | ((v[j - 1] & 1u) << 7));
        }
        v[0] = static_cast<uint8_t>(v[0] >> 1);
        if (lsb != 0) v[0] = static_cast<uint8_t>(v[0] ^ 0xE1u);
    }
    std::memcpy(out, z, 16);
}

class GHash {
public:
    explicit GHash(const uint8_t h[16]) : y_{} { std::memcpy(h_, h, 16); }

    void update_block(const uint8_t block[16]) {
        for (int i = 0; i < 16; ++i) y_[i] = static_cast<uint8_t>(y_[i] ^ block[i]);
        uint8_t t[16];
        gf128_mul(y_, h_, t);
        std::memcpy(y_, t, 16);
    }

    // 把数据按 16 字节零填充后分块输入
    void update(const uint8_t* data, size_t len) {
        uint8_t block[16];
        size_t off = 0;
        while (off < len) {
            const size_t n = (len - off < 16) ? (len - off) : size_t{16};
            std::memcpy(block, data + off, n);
            if (n < 16) std::memset(block + n, 0, 16 - n);
            update_block(block);
            off += n;
        }
    }

    void digest(uint8_t out[16]) const { std::memcpy(out, y_, 16); }

private:
    uint8_t h_[16];
    uint8_t y_[16];
};

// 注意：GCM 的 CTR 只对 J0 的最后一个 32 位字自增（SP 800-38D 的 inc32），
// 而不是把 J0 当成 128 位整数整体 +1。
void gcm_inc32(const uint8_t in[16], uint8_t out[16]) {
    uint32_t ctr = load_be32(in + 12);
    ctr = ctr + 1u;  // 无符号回绕是定义良好的
    std::memcpy(out, in, 12);
    store_be32(out + 12, ctr);
}

// GHASH(J0, AAD, ciphertext) -> tag（AAD 可为空）
void gcm_tag(const uint8_t rk[240], const uint8_t h[16], const uint8_t j0[16],
             const uint8_t* aad, size_t aad_len, const uint8_t* ct, size_t ct_len,
             uint8_t tag[16]) {
    GHash ghash(h);
    if (aad_len > 0) ghash.update(aad, aad_len);
    if (ct_len > 0) ghash.update(ct, ct_len);

    uint8_t len_block[16];
    const uint64_t aad_bits = static_cast<uint64_t>(aad_len) * 8ull;
    const uint64_t ct_bits = static_cast<uint64_t>(ct_len) * 8ull;
    store_be32(len_block, load_be64_hi(aad_bits));
    store_be32(len_block + 4, load_be64_lo(aad_bits));
    store_be32(len_block + 8, load_be64_hi(ct_bits));
    store_be32(len_block + 12, load_be64_lo(ct_bits));
    ghash.update_block(len_block);

    uint8_t y[16];
    ghash.digest(y);
    uint8_t ej0[16];
    aes256_encrypt_block(rk, j0, ej0);
    for (int i = 0; i < 16; ++i) tag[i] = static_cast<uint8_t>(y[i] ^ ej0[i]);
}

// GCM 的 CTR 部分：密文[i] = 明文[i] ^ E_K(inc32(J0))（counter 32 位回绕）
void gcm_ctr(const uint8_t rk[240], const uint8_t j0[16], const uint8_t* in, size_t len,
             uint8_t* out) {
    uint8_t ctr[16];
    uint8_t ks[16];
    gcm_inc32(j0, ctr);
    size_t off = 0;
    while (off < len) {
        aes256_encrypt_block(rk, ctr, ks);
        const size_t n = (len - off < 16) ? (len - off) : size_t{16};
        for (size_t i = 0; i < n; ++i) out[off + i] = static_cast<uint8_t>(in[off + i] ^ ks[i]);
        off += n;
        if (off < len) gcm_inc32(ctr, ctr);
    }
}

// 计算 J0：12 字节 nonce 走标准路径，其他长度按 GCM 规范用 GHASH
void gcm_j0(const uint8_t h[16], const uint8_t* nonce, size_t nonce_len, uint8_t j0[16]) {
    if (nonce_len == 12u) {
        std::memcpy(j0, nonce, 12);
        j0[12] = 0;
        j0[13] = 0;
        j0[14] = 0;
        j0[15] = 1;
        return;
    }
    GHash ghash(h);
    if (nonce_len > 0) ghash.update(nonce, nonce_len);
    uint8_t len_block[16];
    std::memset(len_block, 0, 16);
    store_be32(len_block + 8, load_be64_hi(static_cast<uint64_t>(nonce_len) * 8ull));
    store_be32(len_block + 12, load_be64_lo(static_cast<uint64_t>(nonce_len) * 8ull));
    ghash.update_block(len_block);
    ghash.digest(j0);
}

}  // namespace

// ---------- hex ----------

std::string to_hex(const uint8_t* data, size_t len) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    if (data == nullptr && len != 0) return out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        out[2 * i] = kDigits[(b >> 4) & 0x0Fu];
        out[2 * i + 1] = kDigits[b & 0x0Fu];
    }
    return out;
}

Bytes from_hex(std::string_view hex, bool* ok) {
    Bytes out;
    if (hex.size() % 2u != 0u) {
        if (ok) *ok = false;
        return out;
    }
    out.reserve(hex.size() / 2u);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_value(static_cast<uint8_t>(hex[i]));
        const int lo = hex_value(static_cast<uint8_t>(hex[i + 1]));
        if (hi < 0 || lo < 0) {
            if (ok) *ok = false;
            return Bytes{};
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    if (ok) *ok = true;
    return out;
}

// ---------- SHA-256 ----------

Sha256::Sha256()
    : h_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu,
         0x1f83d9abu, 0x5be0cd19u},
      buffer_{},
      buffered_(0),
      total_bits_(0),
      finalized_(false),
      last_{} {}

void Sha256::transform(const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = load_be32(block + 4 * i);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr32(w[i - 15], 7u) ^ rotr32(w[i - 15], 18u) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr32(w[i - 2], 17u) ^ rotr32(w[i - 2], 19u) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h_[0];
    uint32_t b = h_[1];
    uint32_t c = h_[2];
    uint32_t d = h_[3];
    uint32_t e = h_[4];
    uint32_t f = h_[5];
    uint32_t g = h_[6];
    uint32_t h = h_[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
        const uint32_t s0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const uint8_t* data, size_t len) {
    if (finalized_ || data == nullptr || len == 0) return;
    total_bits_ += static_cast<uint64_t>(len) * 8ull;

    size_t off = 0;
    if (buffered_ > 0) {
        const size_t need = 64u - buffered_;
        if (len < need) {
            std::memcpy(buffer_ + buffered_, data, len);
            buffered_ += len;
            return;
        }
        std::memcpy(buffer_ + buffered_, data, need);
        transform(buffer_);
        buffered_ = 0;
        off = need;
    }
    while (len - off >= 64u) {
        transform(data + off);
        off += 64u;
    }
    if (off < len) {
        buffered_ = len - off;
        std::memcpy(buffer_, data + off, buffered_);
    }
}

void Sha256::update(std::string_view data) {
    update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

Sha256Digest Sha256::final() {
    if (finalized_) return last_;

    // 消息位长度必须在填充之前取走：填充字节不能计入长度字段
    const uint64_t bits = total_bits_;

    uint8_t pad = 0x80u;
    update(&pad, 1);
    uint8_t zero = 0x00u;
    while (buffered_ != 56u) update(&zero, 1);

    // buffered_ == 56，直接补齐长度字段后做最后一次压缩
    for (int i = 0; i < 8; ++i) {
        buffer_[56 + i] = static_cast<uint8_t>((bits >> (56 - 8 * i)) & 0xFFull);  // 大端
    }
    transform(buffer_);
    buffered_ = 0;

    for (int i = 0; i < 8; ++i) store_be32(last_.data() + 4 * i, h_[i]);
    finalized_ = true;
    return last_;
}

std::string Sha256::hex() {
    const Sha256Digest d = final();
    return to_hex(d.data(), d.size());
}

Sha256Digest sha256(const uint8_t* data, size_t len) {
    Sha256 s;
    s.update(data, len);
    return s.final();
}

Sha256Digest sha256(std::string_view data) {
    Sha256 s;
    s.update(data);
    return s.final();
}

std::string sha256_hex(std::string_view data) {
    const Sha256Digest d = sha256(data);
    return to_hex(d.data(), d.size());
}

// ---------- MD5 ----------

Md5::Md5()
    : h_{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u},
      buffer_{},
      buffered_(0),
      total_bits_(0),
      finalized_(false),
      last_{} {}

void Md5::transform(const uint8_t* block) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = load_le32(block + 4 * i);

    uint32_t a = h_[0];
    uint32_t b = h_[1];
    uint32_t c = h_[2];
    uint32_t d = h_[3];

    for (uint32_t i = 0; i < 64u; ++i) {
        uint32_t f = 0;
        uint32_t g = 0;
        if (i < 16u) {
            f = (b & c) | ((~b) & d);
            g = i;
        } else if (i < 32u) {
            f = (d & b) | ((~d) & c);
            g = (5u * i + 1u) % 16u;
        } else if (i < 48u) {
            f = b ^ c ^ d;
            g = (3u * i + 5u) % 16u;
        } else {
            f = c ^ (b | (~d));
            g = (7u * i) % 16u;
        }
        const uint32_t tmp = d;
        d = c;
        c = b;
        const uint32_t sum = a + f + kMd5K[i] + m[g];
        b = b + rotl32(sum, kMd5S[i]);
        a = tmp;
    }

    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
}

void Md5::update(const uint8_t* data, size_t len) {
    if (finalized_ || data == nullptr || len == 0) return;
    total_bits_ += static_cast<uint64_t>(len) * 8ull;

    size_t off = 0;
    if (buffered_ > 0) {
        const size_t need = 64u - buffered_;
        if (len < need) {
            std::memcpy(buffer_ + buffered_, data, len);
            buffered_ += len;
            return;
        }
        std::memcpy(buffer_ + buffered_, data, need);
        transform(buffer_);
        buffered_ = 0;
        off = need;
    }
    while (len - off >= 64u) {
        transform(data + off);
        off += 64u;
    }
    if (off < len) {
        buffered_ = len - off;
        std::memcpy(buffer_, data + off, buffered_);
    }
}

void Md5::update(std::string_view data) {
    update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

Md5Digest Md5::final() {
    if (finalized_) return last_;

    // 消息位长度必须在填充之前取走
    const uint64_t bits = total_bits_;

    uint8_t pad = 0x80u;
    update(&pad, 1);
    uint8_t zero = 0x00u;
    while (buffered_ != 56u) update(&zero, 1);

    for (int i = 0; i < 8; ++i) {
        buffer_[56 + i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFFull);  // 小端
    }
    transform(buffer_);
    buffered_ = 0;

    for (int i = 0; i < 4; ++i) store_le32(last_.data() + 4 * i, h_[i]);
    finalized_ = true;
    return last_;
}

std::string Md5::hex() {
    const Md5Digest d = final();
    return to_hex(d.data(), d.size());
}

Md5Digest md5(const uint8_t* data, size_t len) {
    Md5 m;
    m.update(data, len);
    return m.final();
}

Md5Digest md5(std::string_view data) {
    Md5 m;
    m.update(data);
    return m.final();
}

std::string md5_hex(std::string_view data) {
    const Md5Digest d = md5(data);
    return to_hex(d.data(), d.size());
}

// ---------- 文件摘要 ----------

std::string sha256_file_hex(const std::string& path, bool* ok) {
    if (ok) *ok = false;
    // path 为 UTF-8 字节：经 std::filesystem::path 打开以走 Windows 宽字符 API，
    // 保证中文等非 ASCII 文件名可读。
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return std::string();

    Sha256 hasher;
    std::vector<uint8_t> buffer(kFileChunk);
    for (;;) {
        in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) hasher.update(buffer.data(), static_cast<size_t>(got));
        if (got < static_cast<std::streamsize>(buffer.size())) {
            if (in.bad()) return std::string();
            break;
        }
    }
    const Sha256Digest d = hasher.final();
    if (ok) *ok = true;
    return to_hex(d.data(), d.size());
}

std::string md5_file_hex(const std::string& path, bool* ok) {
    if (ok) *ok = false;
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return std::string();

    Md5 hasher;
    std::vector<uint8_t> buffer(kFileChunk);
    for (;;) {
        in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) hasher.update(buffer.data(), static_cast<size_t>(got));
        if (got < static_cast<std::streamsize>(buffer.size())) {
            if (in.bad()) return std::string();
            break;
        }
    }
    const Md5Digest d = hasher.final();
    if (ok) *ok = true;
    return to_hex(d.data(), d.size());
}

// ---------- HMAC / PBKDF2 ----------

Sha256Digest hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len) {
    uint8_t k[64];
    std::memset(k, 0, sizeof(k));
    if (key_len > 64u) {
        const Sha256Digest kh = sha256(key, key_len);
        std::memcpy(k, kh.data(), kh.size());
    } else if (key != nullptr && key_len > 0) {
        std::memcpy(k, key, key_len);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (size_t i = 0; i < 64u; ++i) {
        ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36u);
        opad[i] = static_cast<uint8_t>(k[i] ^ 0x5Cu);
    }

    Sha256 inner;
    inner.update(ipad, 64u);
    inner.update(msg, msg_len);
    const Sha256Digest ih = inner.final();

    Sha256 outer;
    outer.update(opad, 64u);
    outer.update(ih.data(), ih.size());
    return outer.final();
}

Sha256Digest hmac_sha256(std::string_view key, std::string_view msg) {
    return hmac_sha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                       reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
}

Bytes pbkdf2_hmac_sha256(std::string_view password, const Bytes& salt, uint32_t iterations,
                         size_t dk_len) {
    Bytes dk;
    if (iterations < 1u || dk_len == 0u) return dk;

    // 准备 HMAC 的 ipad / opad（key > 64B 先哈希）
    uint8_t k[64];
    std::memset(k, 0, sizeof(k));
    if (password.size() > 64u) {
        const Sha256Digest kh =
            sha256(reinterpret_cast<const uint8_t*>(password.data()), password.size());
        std::memcpy(k, kh.data(), kh.size());
    } else if (!password.empty()) {
        std::memcpy(k, password.data(), password.size());
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (size_t i = 0; i < 64u; ++i) {
        ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36u);
        opad[i] = static_cast<uint8_t>(k[i] ^ 0x5Cu);
    }

    dk.reserve(dk_len);
    const size_t blocks = (dk_len + 31u) / 32u;

    for (size_t i = 1; i <= blocks; ++i) {
        const uint8_t index_be[4] = {static_cast<uint8_t>((i >> 24) & 0xFFu),
                                     static_cast<uint8_t>((i >> 16) & 0xFFu),
                                     static_cast<uint8_t>((i >> 8) & 0xFFu),
                                     static_cast<uint8_t>(i & 0xFFu)};
        Sha256Digest prev{};
        uint8_t t[32];
        std::memset(t, 0, sizeof(t));

        for (uint32_t iter = 0; iter < iterations; ++iter) {
            // U_j = HMAC(P, U_{j-1})：内层 = SHA256(ipad || (salt||INT(i) 或 U_{j-1}))
            Sha256 inner;
            inner.update(ipad, 64u);
            if (iter == 0u) {
                inner.update(salt.data(), salt.size());
                inner.update(index_be, 4u);
            } else {
                inner.update(prev.data(), prev.size());
            }
            const Sha256Digest ih = inner.final();

            Sha256 outer;
            outer.update(opad, 64u);
            outer.update(ih.data(), ih.size());
            prev = outer.final();

            if (iter == 0u) {
                std::memcpy(t, prev.data(), 32);
            } else {
                for (size_t j = 0; j < 32u; ++j) t[j] = static_cast<uint8_t>(t[j] ^ prev[j]);
            }
        }

        const size_t need = dk_len - dk.size();
        const size_t take = (need < 32u) ? need : size_t{32};
        for (size_t j = 0; j < take; ++j) dk.push_back(t[j]);
    }

    return dk;
}

// ---------- Base64 ----------

namespace {
const char kBase64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// -1 表示空白（解码时忽略），-2 表示非法字符，-3 表示 '='
int base64_value(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return static_cast<int>(c - 'A');
    if (c >= 'a' && c <= 'z') return static_cast<int>(c - 'a') + 26;
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -3;
    if (is_space(c)) return -1;
    return -2;
}
}  // namespace

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    if (data == nullptr && len != 0) return out;
    out.reserve(((len + 2u) / 3u) * 4u);
    size_t i = 0;
    while (i + 3u <= len) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
        out.push_back(kBase64Table[(v >> 18) & 0x3Fu]);
        out.push_back(kBase64Table[(v >> 12) & 0x3Fu]);
        out.push_back(kBase64Table[(v >> 6) & 0x3Fu]);
        out.push_back(kBase64Table[v & 0x3Fu]);
        i += 3;
    }
    const size_t rem = len - i;
    if (rem == 1u) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kBase64Table[(v >> 18) & 0x3Fu]);
        out.push_back(kBase64Table[(v >> 12) & 0x3Fu]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2u) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kBase64Table[(v >> 18) & 0x3Fu]);
        out.push_back(kBase64Table[(v >> 12) & 0x3Fu]);
        out.push_back(kBase64Table[(v >> 6) & 0x3Fu]);
        out.push_back('=');
    }
    return out;
}

std::string base64_encode(const Bytes& data) {
    return base64_encode(data.data(), data.size());
}

std::string base64_encode(std::string_view text) {
    return base64_encode(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool base64_decode(std::string_view text, Bytes& out) {
    out.clear();

    // 1) 去掉空白，检查字符集与填充位置
    std::string clean;
    clean.reserve(text.size());
    size_t pad = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const int v = base64_value(static_cast<uint8_t>(text[i]));
        if (v == -1) continue;              // 空白
        if (v == -2) return false;          // 非法字符
        if (v == -3) {
            ++pad;
            continue;                       // '=' 只做位置校验，不进入内容
        }
        if (pad > 0) return false;          // '=' 之后不允许再有数据
        clean.push_back(text[i]);
    }
    if (pad > 2u) return false;
    // 只有 '=' 而没有数据字符（例如 "===="）不是合法 Base64
    if (pad > 0u && clean.empty()) return false;
    if (pad > 0u) {
        // 带填充：数据长度 + 填充必须凑成整块，且填充位数与剩余字节数一致
        if ((clean.size() + pad) % 4u != 0u) return false;
        const size_t rem = clean.size() % 4u;
        if (rem != 4u - pad) return false;
    } else {
        // 不带填充：允许 4 的倍数，以及 2/3 个余数字符（JWT 风格）；余 1 个字符非法
        if (clean.size() % 4u == 1u) return false;
    }

    // 2) 解码（把 '=' 视为全 0 位）
    out.reserve(clean.size() / 4u * 3u + 3u);
    uint32_t acc = 0;
    size_t acc_bits = 0;
    for (size_t i = 0; i < clean.size(); ++i) {
        acc = (acc << 6) | static_cast<uint32_t>(base64_value(static_cast<uint8_t>(clean[i])));
        acc_bits += 6u;
        if (acc_bits >= 8u) {
            acc_bits -= 8u;
            out.push_back(static_cast<uint8_t>((acc >> acc_bits) & 0xFFu));
        }
    }

    // 3) 校验被丢弃的低位必须为 0（拒绝 "YR==" 之类的非规范编码）
    if (acc_bits > 0u) {
        const uint32_t mask = (1u << acc_bits) - 1u;
        if ((acc & mask) != 0u) {
            out.clear();
            return false;
        }
    }
    return true;
}

bool constant_time_equals(const uint8_t* a, const uint8_t* b, size_t len) {
    if (len == 0) return true;
    if (a == nullptr || b == nullptr) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

// ---------- AES-256-GCM ----------

AeadResult aes256_gcm_encrypt(const std::array<uint8_t, 32>& key, const Bytes& nonce,
                              const Bytes& plaintext, const Bytes& aad) {
    AeadResult r;
    if (nonce.empty()) {
        r.error = "GCM: nonce 不能为空";
        return r;
    }

    uint8_t rk[240];
    aes256_expand_key(key.data(), rk);

    uint8_t h[16];
    const uint8_t zero[16] = {0};
    aes256_encrypt_block(rk, zero, h);

    uint8_t j0[16];
    gcm_j0(h, nonce.data(), nonce.size(), j0);

    Bytes out(plaintext.size() + 16u);
    if (!plaintext.empty()) {
        gcm_ctr(rk, j0, plaintext.data(), plaintext.size(), out.data());
    }
    gcm_tag(rk, h, j0, aad.data(), aad.size(), out.data(), plaintext.size(),
            out.data() + plaintext.size());

    r.ok = true;
    r.data = std::move(out);
    return r;
}

AeadResult aes256_gcm_decrypt(const std::array<uint8_t, 32>& key, const Bytes& nonce,
                              const Bytes& ciphertext_with_tag, const Bytes& aad) {
    AeadResult r;
    if (nonce.empty()) {
        r.error = "GCM: nonce 不能为空";
        return r;
    }
    if (ciphertext_with_tag.size() < 16u) {
        r.error = "GCM: 输入不足 16 字节认证标签";
        return r;
    }

    const size_t ct_len = ciphertext_with_tag.size() - 16u;
    const uint8_t* ct = ciphertext_with_tag.data();
    const uint8_t* tag = ct + ct_len;

    uint8_t rk[240];
    aes256_expand_key(key.data(), rk);

    uint8_t h[16];
    const uint8_t zero[16] = {0};
    aes256_encrypt_block(rk, zero, h);

    uint8_t j0[16];
    gcm_j0(h, nonce.data(), nonce.size(), j0);

    uint8_t expect[16];
    gcm_tag(rk, h, j0, aad.data(), aad.size(), ct, ct_len, expect);
    if (!constant_time_equals(expect, tag, 16u)) {
        r.ok = false;
        r.error = "GCM: 认证标签校验失败";
        return r;
    }

    Bytes pt(ct_len);
    if (ct_len > 0) gcm_ctr(rk, j0, ct, ct_len, pt.data());

    r.ok = true;
    r.data = std::move(pt);
    return r;
}

}  // namespace radmusic::crypto
