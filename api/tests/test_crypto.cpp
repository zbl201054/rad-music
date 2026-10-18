// Rad music API - crypto 原语测试（KAT / 往返 / 边界）
//
// 运行方式（由构建系统编译成功后）：
//   radmusic_tests                     # 全部用例
//   radmusic_tests test_crypto         # 只跑本文件
//
// 说明：
//   * 本文件只依赖 tests/test_framework.hpp 与 include/radmusic/crypto.hpp。
//   * 所有已知答案（KAT）向量都写成字符串常量，便于人工核对。
//   * 文件摘要用例会在当前工作目录写一个临时文件并在结束时删除；
//     若运行环境不可写，该用例会失败——这是测试环境问题，不是实现问题。

#include "radmusic/crypto.hpp"

#include "test_framework.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using radmusic::crypto::Bytes;
using radmusic::crypto::md5_file_hex;
using radmusic::crypto::sha256_file_hex;

namespace {

// ---------- 小工具 ----------

std::string to_hex(const Bytes& b) {
    return radmusic::crypto::to_hex(b.data(), b.size());
}

std::string digest_hex(const radmusic::crypto::Sha256Digest& d) {
    return radmusic::crypto::to_hex(d.data(), d.size());
}

std::string digest_hex(const radmusic::crypto::Md5Digest& d) {
    return radmusic::crypto::to_hex(d.data(), d.size());
}

// 把常量十六进制字符串转成字节；转换失败返回空 vector
Bytes hex_bytes(std::string_view hex) {
    bool ok = false;
    Bytes b = radmusic::crypto::from_hex(hex, &ok);
    if (!ok) return Bytes{};
    return b;
}

Bytes bytes_from(const uint8_t* p, size_t n) {
    Bytes b;
    b.assign(p, p + n);
    return b;
}

// ---------- 已知答案常量 ----------

// HMAC-SHA256 RFC 4231 Test Case 1 的 key：20 个 0x0b
const uint8_t kRfc4231Key1[20] = {0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                                  0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b};

// HMAC-SHA256 RFC 4231 Test Case 6 的 key：131 个 0xaa（长于 64 → 先做 SHA-256）
// 用 vector 构造，避免手写常量数组时数错字节
Bytes rfc4231_key6() {
    return Bytes(131, 0xaa);
}

Bytes rfc4231_msg6() {
    // RFC 4231 Test Case 6 的 data 是 ASCII 文本（54 字节），不是 0xaa 填充
    const std::string text = "Test Using Larger Than Block-Size Key - Hash Key First";
    return Bytes(text.begin(), text.end());
}

// AES-256-GCM 测试 key：全 0（GCM 规范 Test Case 15/16 用的就是 0^256，不是 00..1f）
const uint8_t kGcmKeyBytes[32] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

std::array<uint8_t, 32> gcm_key() {
    std::array<uint8_t, 32> key{};
    for (size_t i = 0; i < 32; ++i) key[i] = kGcmKeyBytes[i];
    return key;
}

Bytes gcm_iv12() {
    return Bytes(12, 0x00);
}

// 文件摘要用例用的临时文件名（写在当前工作目录，用例结束删除）
const char* const kTmpFileName = "radtest_crypto_tmp_9f3a.bin";

}  // namespace

// ======================= SHA-256 =======================

TEST_CASE(test_crypto_sha256_empty) {
    const radmusic::crypto::Sha256Digest d = radmusic::crypto::sha256(std::string_view(""));
    CHECK_EQ(digest_hex(d),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(radmusic::crypto::sha256_hex(""),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

TEST_CASE(test_crypto_sha256_abc) {
    CHECK_EQ(radmusic::crypto::sha256_hex("abc"),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST_CASE(test_crypto_sha256_two_block_vector) {
    CHECK_EQ(radmusic::crypto::sha256_hex(
                 "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TEST_CASE(test_crypto_sha256_incremental_matches_oneshot) {
    const char* msg = "abc";
    const size_t n = std::strlen(msg);
    const std::string once = radmusic::crypto::sha256_hex(std::string_view(msg, n));

    // 每次只喂 1 字节
    radmusic::crypto::Sha256 s;
    for (size_t i = 0; i < n; ++i) {
        s.update(reinterpret_cast<const uint8_t*>(msg) + i, 1);
    }
    CHECK_EQ(s.hex(), once);

    // 空更新不应改变结果
    radmusic::crypto::Sha256 s2;
    s2.update(nullptr, 0);
    s2.update(reinterpret_cast<const uint8_t*>(msg), n);
    s2.update(std::string_view(""));
    CHECK_EQ(s2.hex(), once);

    // final() 可以重复调用，结果稳定
    radmusic::crypto::Sha256 s3;
    s3.update(std::string_view(msg));
    CHECK_EQ(digest_hex(s3.final()), digest_hex(s3.final()));

    // 跨块边界：64 字节消息 / 55、56、64、65 字节长度都走不同的填充分支
    std::string base;
    while (base.size() < 200u) base.push_back(static_cast<char>('A' + (base.size() % 26u)));
    for (size_t len = 0; len <= 200u; ++len) {
        const std::string_view part(base.data(), len);
        radmusic::crypto::Sha256 whole;
        whole.update(part);
        const std::string one = digest_hex(whole.final());

        radmusic::crypto::Sha256 split;
        const size_t cut = len / 3u;
        split.update(part.substr(0, cut));
        split.update(part.substr(cut));
        CHECK_EQ(digest_hex(split.final()), one);
    }
}

TEST_CASE(test_crypto_sha256_file) {
    const std::string content = "abc";
    {
        std::FILE* f = std::fopen(kTmpFileName, "wb");
        CHECK(f != nullptr);
        if (f == nullptr) return;
        const size_t written = std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
        CHECK_EQ(written, content.size());
    }

    bool ok = false;
    const std::string hex = sha256_file_hex(kTmpFileName, &ok);
    CHECK(ok);
    CHECK_EQ(hex, std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    bool md5_ok = false;
    const std::string mhex = md5_file_hex(kTmpFileName, &md5_ok);
    CHECK(md5_ok);
    CHECK_EQ(mhex, std::string("900150983cd24fb0d6963f7d28e17f72"));

    std::remove(kTmpFileName);

    bool missing_ok = true;
    const std::string missing = sha256_file_hex("radtest_crypto_no_such_file_9f3a.bin", &missing_ok);
    CHECK_FALSE(missing_ok);
    CHECK(missing.empty());

    bool missing_md5_ok = true;
    md5_file_hex("radtest_crypto_no_such_file_9f3a.bin", &missing_md5_ok);
    CHECK_FALSE(missing_md5_ok);
}

// ======================= MD5 =======================

TEST_CASE(test_crypto_md5_kat) {
    CHECK_EQ(radmusic::crypto::md5_hex(""),
             std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(radmusic::crypto::md5_hex("abc"),
             std::string("900150983cd24fb0d6963f7d28e17f72"));
    CHECK_EQ(radmusic::crypto::md5_hex("message digest"),
             std::string("f96b697d7cb7938d525a2f31aaf161d0"));
}

TEST_CASE(test_crypto_md5_incremental_matches_oneshot) {
    const char* msg = "message digest";
    const size_t n = std::strlen(msg);
    const std::string once = radmusic::crypto::md5_hex(std::string_view(msg, n));

    radmusic::crypto::Md5 m;
    for (size_t i = 0; i < n; ++i) {
        m.update(reinterpret_cast<const uint8_t*>(msg) + i, 1);
    }
    CHECK_EQ(m.hex(), once);

    // 分两段（与 SHA-256 同样的边界遍历）
    std::string base;
    while (base.size() < 200u) base.push_back(static_cast<char>('a' + (base.size() % 26u)));
    for (size_t len = 0; len <= 200u; ++len) {
        const std::string_view part(base.data(), len);
        radmusic::crypto::Md5 whole;
        whole.update(part);
        const std::string one = digest_hex(whole.final());

        radmusic::crypto::Md5 split;
        split.update(part.substr(0, len / 2u));
        split.update(part.substr(len / 2u));
        CHECK_EQ(digest_hex(split.final()), one);
    }
}

// ======================= HMAC-SHA256 =======================

TEST_CASE(test_crypto_hmac_sha256_rfc4231_case1) {
    const std::string msg = "Hi There";
    const radmusic::crypto::Sha256Digest d = radmusic::crypto::hmac_sha256(
        kRfc4231Key1, sizeof(kRfc4231Key1),
        reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    CHECK_EQ(digest_hex(d),
             std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
}

TEST_CASE(test_crypto_hmac_sha256_rfc4231_case2_jefe) {
    const radmusic::crypto::Sha256Digest d =
        radmusic::crypto::hmac_sha256(std::string_view("Jefe"),
                                      std::string_view("what do ya want for nothing?"));
    CHECK_EQ(digest_hex(d),
             std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}

TEST_CASE(test_crypto_hmac_sha256_long_key) {
    // RFC 4231 Test Case 6：131 字节 key（长于 64 → 先做 SHA-256），50 字节 0xaa 消息
    const Bytes key = rfc4231_key6();
    const Bytes msg = rfc4231_msg6();
    CHECK_EQ(key.size(), size_t{131});
    CHECK_EQ(msg.size(), size_t{54});
    const radmusic::crypto::Sha256Digest d =
        radmusic::crypto::hmac_sha256(key.data(), key.size(), msg.data(), msg.size());
    CHECK_EQ(digest_hex(d),
             std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

TEST_CASE(test_crypto_hmac_sha256_ascii_equivalence) {
    // 字节接口与 string_view 接口必须一致
    const std::string key = "Jefe";
    const std::string msg = "what do ya want for nothing?";
    const radmusic::crypto::Sha256Digest a = radmusic::crypto::hmac_sha256(
        reinterpret_cast<const uint8_t*>(key.data()), key.size(),
        reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    const radmusic::crypto::Sha256Digest b =
        radmusic::crypto::hmac_sha256(std::string_view(key), std::string_view(msg));
    CHECK_EQ(digest_hex(a), digest_hex(b));
    CHECK(radmusic::crypto::constant_time_equals(a.data(), b.data(), a.size()));
}

// ======================= PBKDF2-HMAC-SHA256 =======================

TEST_CASE(test_crypto_pbkdf2_rfc6070_like_iter1) {
    const Bytes salt = hex_bytes("73616c74");  // "salt"
    const Bytes dk = radmusic::crypto::pbkdf2_hmac_sha256("password", salt, 1u, 32u);
    CHECK_EQ(dk.size(), size_t{32});
    CHECK_EQ(to_hex(dk),
             std::string("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
}

TEST_CASE(test_crypto_pbkdf2_iter4096) {
    const Bytes salt = hex_bytes("73616c74");
    const Bytes dk = radmusic::crypto::pbkdf2_hmac_sha256("password", salt, 4096u, 32u);
    CHECK_EQ(dk.size(), size_t{32});
    CHECK_EQ(to_hex(dk),
             std::string("c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
}

TEST_CASE(test_crypto_pbkdf2_multiblock_and_edges) {
    const Bytes salt = hex_bytes("73616c74");

    // 多块输出 = 各块拼接（块 2 = c=1, dkLen=32 的第二个块）
    const Bytes b32 = radmusic::crypto::pbkdf2_hmac_sha256("password", salt, 1u, 32u);
    const Bytes b64 = radmusic::crypto::pbkdf2_hmac_sha256("password", salt, 1u, 64u);
    CHECK_EQ(b64.size(), size_t{64});
    if (b64.size() == 64u && b32.size() == 32u) {
        Bytes head(b64.begin(), b64.begin() + 32);
        CHECK_EQ(to_hex(head), to_hex(b32));
    }
    // dkLen = 40 → 前 32 字节与 dkLen = 32 的结果一致，尾部 8 字节来自第二块
    const Bytes b40 = radmusic::crypto::pbkdf2_hmac_sha256("password", salt, 1u, 40u);
    CHECK_EQ(b40.size(), size_t{40});
    if (b40.size() == 40u && b32.size() == 32u) {
        Bytes head(b40.begin(), b40.begin() + 32);
        CHECK_EQ(to_hex(head), to_hex(b32));
        Bytes tail(b40.begin() + 32, b40.end());
        Bytes expect_tail(b64.begin() + 32, b64.begin() + 40);
        CHECK_EQ(to_hex(tail), to_hex(expect_tail));
    }

    // iterations = 0 与 dkLen = 0 返回空，不抛异常
    CHECK(radmusic::crypto::pbkdf2_hmac_sha256("password", salt, 0u, 32u).empty());
    CHECK(radmusic::crypto::pbkdf2_hmac_sha256("password", salt, 1u, 0u).empty());

    // 空 salt 不应崩溃，且结果稳定
    const Bytes empty_salt;
    const Bytes e1 = radmusic::crypto::pbkdf2_hmac_sha256("password", empty_salt, 2u, 16u);
    CHECK_EQ(e1.size(), size_t{16});
    const Bytes e2 = radmusic::crypto::pbkdf2_hmac_sha256("password", empty_salt, 2u, 16u);
    CHECK_EQ(to_hex(e1), to_hex(e2));

    // 长口令（> 64 字节 → 与 HMAC 的长 key 分支同源）不应崩溃
    const std::string long_pw(100, 'x');
    const Bytes l1 = radmusic::crypto::pbkdf2_hmac_sha256(long_pw, salt, 3u, 32u);
    CHECK_EQ(l1.size(), size_t{32});
}

// ======================= Base64 =======================

TEST_CASE(test_crypto_base64_kat) {
    CHECK_EQ(radmusic::crypto::base64_encode("abc"), std::string("YWJj"));
    CHECK_EQ(radmusic::crypto::base64_encode("a"), std::string("YQ=="));
    CHECK_EQ(radmusic::crypto::base64_encode("ab"), std::string("YWI="));
    CHECK_EQ(radmusic::crypto::base64_encode("abcd"), std::string("YWJjZA=="));
    CHECK_EQ(radmusic::crypto::base64_encode(""), std::string(""));
}

TEST_CASE(test_crypto_base64_roundtrip) {
    // 固定数组，含 0x00 / 0xFF 等边界字节
    const uint8_t raw[19] = {0x00, 0xFF, 0x01, 0xFE, 0x7F, 0x80, 0xAA, 0x55, 0x10, 0x2F,
                             0x40, 0x9C, 0x00, 0x00, 0xFF, 0xFF, 0x61, 0x62, 0x63};
    const Bytes data = bytes_from(raw, sizeof(raw));

    const std::string enc = radmusic::crypto::base64_encode(data);
    // 19 字节 = 6 组 × 3 + 1 剩余 → 24 + 4 = 28 个字符，末尾两个 '='
    CHECK_EQ(enc.size(), size_t{28});
    CHECK_EQ(enc.substr(26), std::string("=="));
    CHECK(enc.find('=') == 26u);  // 第一个 '=' 必须在末尾填充区

    Bytes dec;
    CHECK(radmusic::crypto::base64_decode(enc, dec));
    CHECK_EQ(dec.size(), data.size());
    CHECK(radmusic::crypto::constant_time_equals(dec.data(), data.data(), data.size()));

    // 带换行/空格/制表符的输入应被容忍
    Bytes dec2;
    CHECK(radmusic::crypto::base64_decode("YWJj\nZA==\r\n", dec2));
    CHECK_EQ(to_hex(dec2), std::string("61626364"));

    // 空输入 → 空输出
    Bytes dec3;
    CHECK(radmusic::crypto::base64_decode("", dec3));
    CHECK(dec3.empty());

    // 无填充的规范长度输入也应可解（JWT 风格）
    Bytes dec4;
    CHECK(radmusic::crypto::base64_decode("YWJjZA", dec4));
    CHECK_EQ(to_hex(dec4), std::string("61626364"));
}

TEST_CASE(test_crypto_base64_invalid) {
    Bytes out;
    CHECK_FALSE(radmusic::crypto::base64_decode("YQ=", out));    // 2+1 = 3，长度非法
    CHECK_FALSE(radmusic::crypto::base64_decode("YQ===", out));  // 3 个填充
    CHECK_FALSE(radmusic::crypto::base64_decode("Y!", out));     // 非法字符
    CHECK_FALSE(radmusic::crypto::base64_decode("====", out));   // 只有填充
    CHECK_FALSE(radmusic::crypto::base64_decode("Y", out));      // 1 个字符不成组
    CHECK_FALSE(radmusic::crypto::base64_decode("YWJ=", out));   // 1 个 '=' 但只有 2 个数据字符
    CHECK_FALSE(radmusic::crypto::base64_decode("YR==", out));   // 非规范：被丢弃的低位非 0
    CHECK_FALSE(radmusic::crypto::base64_decode("YWJj=YWJj", out));  // '=' 后又出现数据

    // 反面确认：合法输入必须成功
    CHECK(radmusic::crypto::base64_decode("YWJjZA==", out));
    CHECK_EQ(to_hex(out), std::string("61626364"));
}

// ======================= hex =======================

TEST_CASE(test_crypto_hex_roundtrip_and_case) {
    const uint8_t raw[6] = {0x00, 0x0F, 0xA5, 0xFF, 0x10, 0x7E};
    const Bytes data = bytes_from(raw, sizeof(raw));
    const std::string lower = radmusic::crypto::to_hex(data.data(), data.size());
    CHECK_EQ(lower, std::string("000fa5ff107e"));

    bool ok = false;
    const Bytes back = radmusic::crypto::from_hex(lower, &ok);
    CHECK(ok);
    CHECK_EQ(to_hex(back), lower);

    // 大写同样接受
    const Bytes upper = radmusic::crypto::from_hex("000FA5FF107E", &ok);
    CHECK(ok);
    CHECK_EQ(to_hex(upper), lower);

    // 奇数长度
    const Bytes odd = radmusic::crypto::from_hex("abc", &ok);
    CHECK_FALSE(ok);
    CHECK(odd.empty());

    // 非法字符
    const Bytes bad = radmusic::crypto::from_hex("zz", &ok);
    CHECK_FALSE(ok);
    CHECK(bad.empty());

    // 空输入是合法的空结果
    const Bytes none = radmusic::crypto::from_hex("", &ok);
    CHECK(ok);
    CHECK(none.empty());

    // 不传 ok 指针也不能崩
    CHECK(radmusic::crypto::from_hex("00ff").size() == 2u);
    CHECK(radmusic::crypto::from_hex("0").empty());
}

TEST_CASE(test_crypto_constant_time_equals) {
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[4] = {1, 2, 3, 4};
    const uint8_t c[4] = {1, 2, 3, 5};
    CHECK(radmusic::crypto::constant_time_equals(a, b, 4));
    CHECK_FALSE(radmusic::crypto::constant_time_equals(a, c, 4));
    CHECK(radmusic::crypto::constant_time_equals(a, c, 0));
}

// ======================= AES-256-GCM =======================

TEST_CASE(test_crypto_aes_gcm_nist_empty_plaintext) {
    const std::array<uint8_t, 32> key = gcm_key();
    const Bytes iv = gcm_iv12();
    const Bytes empty;

    const radmusic::crypto::AeadResult enc =
        radmusic::crypto::aes256_gcm_encrypt(key, iv, empty, empty);
    CHECK(enc.ok);
    CHECK(enc.error.empty());
    CHECK_EQ(to_hex(enc.data), std::string("530f8afbc74536b9a963b4f1c4cb738b"));

    const radmusic::crypto::AeadResult dec =
        radmusic::crypto::aes256_gcm_decrypt(key, iv, enc.data, empty);
    CHECK(dec.ok);
    CHECK(dec.data.empty());
}

TEST_CASE(test_crypto_aes_gcm_nist_one_zero_block) {
    const std::array<uint8_t, 32> key = gcm_key();
    const Bytes iv = gcm_iv12();
    const Bytes plaintext(16, 0x00);
    const Bytes empty;

    const radmusic::crypto::AeadResult enc =
        radmusic::crypto::aes256_gcm_encrypt(key, iv, plaintext, empty);
    CHECK(enc.ok);
    CHECK_EQ(enc.data.size(), size_t{32});
    // 密文 || tag
    CHECK_EQ(to_hex(enc.data),
             std::string("cea7403d4d606b6e074ec5d3baf39d18"
                         "d0d1c8a799996bf0265b98b5d48ab919"));

    const radmusic::crypto::AeadResult dec =
        radmusic::crypto::aes256_gcm_decrypt(key, iv, enc.data, empty);
    CHECK(dec.ok);
    CHECK_EQ(dec.data.size(), plaintext.size());
    CHECK(radmusic::crypto::constant_time_equals(dec.data.data(), plaintext.data(),
                                                 plaintext.size()));
}

TEST_CASE(test_crypto_aes_gcm_tag_tamper_detected) {
    const std::array<uint8_t, 32> key = gcm_key();
    const Bytes iv = gcm_iv12();
    const Bytes plaintext(16, 0x00);
    const Bytes empty;

    const radmusic::crypto::AeadResult enc =
        radmusic::crypto::aes256_gcm_encrypt(key, iv, plaintext, empty);
    CHECK(enc.ok);
    if (!enc.ok) return;

    // 翻转 tag 的 1 个 bit
    Bytes bad = enc.data;
    bad[bad.size() - 1u] = static_cast<uint8_t>(bad[bad.size() - 1u] ^ 0x01u);
    const radmusic::crypto::AeadResult dec =
        radmusic::crypto::aes256_gcm_decrypt(key, iv, bad, empty);
    CHECK_FALSE(dec.ok);
    CHECK_FALSE(dec.error.empty());

    // 翻转密文 1 个 bit 也必须失败
    Bytes bad2 = enc.data;
    bad2[0] = static_cast<uint8_t>(bad2[0] ^ 0x80u);
    const radmusic::crypto::AeadResult dec2 =
        radmusic::crypto::aes256_gcm_decrypt(key, iv, bad2, empty);
    CHECK_FALSE(dec2.ok);
    CHECK_FALSE(dec2.error.empty());

    // 输入不足 16 字节
    const Bytes too_short(15, 0x00);
    const radmusic::crypto::AeadResult dec3 =
        radmusic::crypto::aes256_gcm_decrypt(key, iv, too_short, empty);
    CHECK_FALSE(dec3.ok);
    CHECK_FALSE(dec3.error.empty());

    // 空 nonce 必须被拒绝
    const Bytes no_nonce;
    const radmusic::crypto::AeadResult dec4 =
        radmusic::crypto::aes256_gcm_decrypt(key, no_nonce, enc.data, empty);
    CHECK_FALSE(dec4.ok);
    CHECK_FALSE(dec4.error.empty());
}

TEST_CASE(test_crypto_aes_gcm_roundtrip_with_aad_unaligned) {
    const std::array<uint8_t, 32> key = gcm_key();
    const Bytes empty;

    // 100 字节非对齐明文（不是 16 的倍数）
    Bytes plaintext;
    for (int i = 0; i < 100; ++i) plaintext.push_back(static_cast<uint8_t>(i * 7 + 3));

    // 19 字节 AAD
    Bytes aad;
    for (int i = 0; i < 19; ++i) aad.push_back(static_cast<uint8_t>(0xF0 + i));

    const Bytes iv12 = gcm_iv12();
    const radmusic::crypto::AeadResult enc =
        radmusic::crypto::aes256_gcm_encrypt(key, iv12, plaintext, aad);
    CHECK(enc.ok);
    CHECK_EQ(enc.data.size(), plaintext.size() + 16u);

    const radmusic::crypto::AeadResult dec =
        radmusic::crypto::aes256_gcm_decrypt(key, iv12, enc.data, aad);
    CHECK(dec.ok);
    CHECK_EQ(dec.data.size(), plaintext.size());
    CHECK(radmusic::crypto::constant_time_equals(dec.data.data(), plaintext.data(),
                                                 plaintext.size()));

    // AAD 不匹配 → 认证失败
    Bytes wrong_aad = aad;
    wrong_aad[0] = static_cast<uint8_t>(wrong_aad[0] ^ 0x01u);
    const radmusic::crypto::AeadResult dec_bad =
        radmusic::crypto::aes256_gcm_decrypt(key, iv12, enc.data, wrong_aad);
    CHECK_FALSE(dec_bad.ok);
    CHECK_FALSE(dec_bad.error.empty());
}

TEST_CASE(test_crypto_aes_gcm_nonce_lengths) {
    const std::array<uint8_t, 32> key = gcm_key();
    // 8 字节 nonce → 走 GHASH 派生 J0 的非标准路径
    const Bytes iv8 = hex_bytes("0011223344556677");
    CHECK_EQ(iv8.size(), size_t{8});

    Bytes plaintext(37, 0x5A);
    const Bytes aad = hex_bytes("aabbccdd");
    const Bytes empty;

    const radmusic::crypto::AeadResult enc8 =
        radmusic::crypto::aes256_gcm_encrypt(key, iv8, plaintext, aad);
    CHECK(enc8.ok);
    CHECK_EQ(enc8.data.size(), plaintext.size() + 16u);
    const radmusic::crypto::AeadResult dec8 =
        radmusic::crypto::aes256_gcm_decrypt(key, iv8, enc8.data, aad);
    CHECK(dec8.ok);
    CHECK(radmusic::crypto::constant_time_equals(dec8.data.data(), plaintext.data(),
                                                 plaintext.size()));

    // 同一明文在 12 字节 IV 与 8 字节 IV 下必须得到不同密文（J0 不同）
    const radmusic::crypto::AeadResult enc12 =
        radmusic::crypto::aes256_gcm_encrypt(key, gcm_iv12(), plaintext, aad);
    CHECK(enc12.ok);
    CHECK_FALSE(radmusic::crypto::constant_time_equals(
        enc12.data.data(), enc8.data.data(), plaintext.size()));

    // 16 字节 nonce（正好一块）与 1 字节 nonce 都不能崩且可回环
    const Bytes iv16 = hex_bytes("000102030405060708090a0b0c0d0e0f");
    const Bytes iv1 = hex_bytes("42");
    CHECK_EQ(iv16.size(), size_t{16});
    CHECK_EQ(iv1.size(), size_t{1});
    const Bytes ivs[2] = {iv16, iv1};
    for (int k = 0; k < 2; ++k) {
        const radmusic::crypto::AeadResult e =
            radmusic::crypto::aes256_gcm_encrypt(key, ivs[k], plaintext, empty);
        CHECK(e.ok);
        const radmusic::crypto::AeadResult d =
            radmusic::crypto::aes256_gcm_decrypt(key, ivs[k], e.data, empty);
        CHECK(d.ok);
        CHECK(radmusic::crypto::constant_time_equals(d.data.data(), plaintext.data(),
                                                     plaintext.size()));
    }

    // 空 AAD 与显式空 AAD 等价
    const radmusic::crypto::AeadResult a =
        radmusic::crypto::aes256_gcm_encrypt(key, iv8, plaintext);
    const radmusic::crypto::AeadResult b =
        radmusic::crypto::aes256_gcm_encrypt(key, iv8, plaintext, empty);
    CHECK_EQ(to_hex(a.data), to_hex(b.data));
}
