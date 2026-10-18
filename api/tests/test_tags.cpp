// Rad music API - 本地标签解析 / 音频流探测测试（零依赖）
//
// 覆盖：容器判定（扩展名大小写/未知）、ID3v2.3 文本编码（UTF-8/UTF-16/Latin-1）与
//       APIC 封面、FLAC STREAMINFO + Vorbis Comment + PICTURE、WAV fmt/INFO、
//       M4A(MP4) mvhd/stsd/ilst、APE 尾部标签、Ogg Vorbis / Opus 探测、
//       quality_from_stream 归一化、文件名模板回退、错误码与非致命回退。
//
// 说明：仓库内没有真实音频文件，所有用例都用 std::ofstream 在 build/tmp_tags/ 下
//       构造字节级最小合成文件；每个用例使用自己的子目录并在结束时 remove_all。
#include "radmusic/tags.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using radmusic::AudioContainer;
using radmusic::AudioStreamInfo;
using radmusic::Quality;
using radmusic::QualityLevel;
using radmusic::TagData;
using Bytes = std::vector<uint8_t>;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- 临时目录

fs::path case_dir(const std::string& name) {
    const fs::path p = fs::path("build") / "tmp_tags" / name;
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

void cleanup(const fs::path& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
}

bool write_file(const fs::path& p, const Bytes& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!data.empty()) {
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    f.flush();
    return f.good();
}

// ---------------------------------------------------------------- 字节构造

void put(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
void put(Bytes& b, const char* s) {
    while (*s != '\0') b.push_back(static_cast<uint8_t>(*s++));
}
void put_u8(Bytes& b, uint8_t v) { b.push_back(v); }
void put_u16be(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}
void put_u16le(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void put_u24be(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}
void put_u32be(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}
void put_u32le(Bytes& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
void put_u64be(Bytes& b, uint64_t v) {
    put_u32be(b, static_cast<uint32_t>(v >> 32));
    put_u32be(b, static_cast<uint32_t>(v & 0xFFFFFFFFu));
}
void put_u64le(Bytes& b, uint64_t v) {
    put_u32le(b, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    put_u32le(b, static_cast<uint32_t>(v >> 32));
}
void pad(Bytes& b, size_t n, uint8_t v = 0) { b.insert(b.end(), n, v); }

void append(Bytes& b, const Bytes& o) { b.insert(b.end(), o.begin(), o.end()); }

Bytes syncsafe(uint32_t v) {
    Bytes out;
    out.push_back(static_cast<uint8_t>((v >> 21) & 0x7F));
    out.push_back(static_cast<uint8_t>((v >> 14) & 0x7F));
    out.push_back(static_cast<uint8_t>((v >> 7) & 0x7F));
    out.push_back(static_cast<uint8_t>(v & 0x7F));
    return out;
}

// ID3v2.3/2.4 帧：id(4) + size(4) + flags(2) + 内容
void id3_frame(Bytes& out, const char* id, const Bytes& content) {
    put(out, std::string(id, 4));
    put_u32be(out, static_cast<uint32_t>(content.size()));
    put_u16be(out, 0);
    append(out, content);
}

Bytes latin1(const std::string& s) { return Bytes(s.begin(), s.end()); }

Bytes utf16le_bom(const std::string& s) {
    Bytes out;
    out.push_back(0xFF);
    out.push_back(0xFE);
    for (unsigned char c : s) {
        out.push_back(static_cast<uint8_t>(c));
        out.push_back(0x00);
    }
    return out;
}

const uint8_t kFakeJpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01};

// MP4 box 类型（避免 "\xA9a..." 被当成一个十六进制转义）
const char kNam[4] = {static_cast<char>(0xA9), 'n', 'a', 'm'};
const char kART[4] = {static_cast<char>(0xA9), 'A', 'R', 'T'};
const char kAlb[4] = {static_cast<char>(0xA9), 'a', 'l', 'b'};

}  // namespace

// ================================================================ 容器判定

TEST_CASE(test_tags_container_extension) {
    using radmusic::audio_container_from_extension;
    using radmusic::audio_container_id;
    using radmusic::audio_container_supported;

    CHECK_EQ(audio_container_from_extension("D:/music/a.MP3"), AudioContainer::Mp3);
    CHECK_EQ(audio_container_from_extension("a.mp3"), AudioContainer::Mp3);
    CHECK_EQ(audio_container_from_extension("A.FLAC"), AudioContainer::Flac);
    CHECK_EQ(audio_container_from_extension("x/y/z.M4a"), AudioContainer::M4a);
    CHECK_EQ(audio_container_from_extension("song.WAV"), AudioContainer::Wav);
    CHECK_EQ(audio_container_from_extension("song.ape"), AudioContainer::Ape);
    CHECK_EQ(audio_container_from_extension("song.ogg"), AudioContainer::Ogg);
    CHECK_EQ(audio_container_from_extension("song.OPUS"), AudioContainer::Opus);
    CHECK_EQ(audio_container_from_extension("song.aac"), AudioContainer::Aac);

    // 未知扩展名 / 无扩展名 / 目录名里带点但文件无扩展名
    CHECK_EQ(audio_container_from_extension("note.txt"), AudioContainer::Unknown);
    CHECK_EQ(audio_container_from_extension("noext"), AudioContainer::Unknown);
    CHECK_EQ(audio_container_from_extension("dir.mp3/file"), AudioContainer::Unknown);

    CHECK_EQ(audio_container_supported(AudioContainer::Unknown), false);
    CHECK_EQ(audio_container_supported(AudioContainer::Mp3), true);
    CHECK_EQ(audio_container_supported(AudioContainer::Flac), true);
    CHECK_EQ(audio_container_supported(AudioContainer::M4a), true);
    CHECK_EQ(audio_container_supported(AudioContainer::Wav), true);
    CHECK_EQ(audio_container_supported(AudioContainer::Ape), true);
    CHECK_EQ(audio_container_supported(AudioContainer::Ogg), true);
    CHECK_EQ(audio_container_supported(AudioContainer::Opus), true);
    CHECK_EQ(audio_container_supported(AudioContainer::Aac), true);

    CHECK_EQ(std::string(audio_container_id(AudioContainer::Mp3)), std::string("mp3"));
    CHECK_EQ(std::string(audio_container_id(AudioContainer::Flac)), std::string("flac"));
    CHECK_EQ(std::string(audio_container_id(AudioContainer::Unknown)), std::string("unknown"));
}

// ================================================================ MP3 / ID3v2

TEST_CASE(test_tags_mp3_id3v2) {
    const fs::path dir = case_dir("mp3");
    const fs::path file = dir / "song.mp3";

    Bytes frames;
    {
        Bytes c;
        put_u8(c, 3);  // UTF-8
        put(c, "Synth Title");
        id3_frame(frames, "TIT2", c);
    }
    {
        Bytes c;
        put_u8(c, 3);
        put(c, "Synth Artist");
        id3_frame(frames, "TPE1", c);
    }
    {
        Bytes c;
        put_u8(c, 1);  // UTF-16 with BOM
        append(c, utf16le_bom("Synth Album"));
        id3_frame(frames, "TALB", c);
    }
    {
        Bytes c;
        put_u8(c, 0);  // ISO-8859-1
        put(c, "3/12");
        id3_frame(frames, "TRCK", c);
    }
    {
        Bytes c;
        put_u8(c, 0);
        put(c, "(17)");  // 数字流派引用 → Rock
        id3_frame(frames, "TCON", c);
    }
    {
        Bytes c;
        put_u8(c, 0);
        put(c, "image/jpeg");
        put_u8(c, 0);  // MIME 结束
        put_u8(c, 3);  // front cover
        put(c, "cover");
        put_u8(c, 0);  // 描述结束
        c.insert(c.end(), std::begin(kFakeJpeg), std::end(kFakeJpeg));
        id3_frame(frames, "APIC", c);
    }

    Bytes mp3;
    put(mp3, "ID3");
    put_u8(mp3, 3);  // v2.3
    put_u8(mp3, 0);
    put_u8(mp3, 0);  // flags：无 unsynchronisation
    append(mp3, syncsafe(static_cast<uint32_t>(frames.size() + 16)));
    append(mp3, frames);
    pad(mp3, 16);  // padding
    const size_t audio_offset = mp3.size();

    // 80 个 MPEG1 Layer III / 128 kbps / 44.1 kHz / 立体声帧（帧长 417）
    for (int i = 0; i < 80; ++i) {
        put_u8(mp3, 0xFF);
        put_u8(mp3, 0xFB);
        put_u8(mp3, 0x90);
        put_u8(mp3, 0x00);
        pad(mp3, 417 - 4);
    }
    pad(mp3, 64);

    CHECK(write_file(file, mp3));
    CHECK_EQ(audio_offset, static_cast<size_t>(10 + frames.size() + 16));

    auto r = radmusic::read_tags(file.string());
    CHECK(r.ok());
    if (r.ok()) {
        const TagData& t = r.value();
        CHECK_EQ(t.title, std::string("Synth Title"));
        CHECK_EQ(t.artist, std::string("Synth Artist"));
        CHECK_EQ(t.album, std::string("Synth Album"));  // UTF-16 BOM
        CHECK_EQ(t.track_no, 3);
        CHECK_EQ(t.genre, std::string("Rock"));
        CHECK_EQ(t.tag_source, std::string("id3v2"));
        CHECK_EQ(t.cover.size(), sizeof(kFakeJpeg));
        CHECK_EQ(t.cover_mime, std::string("image/jpeg"));
        // 80 * 417 + 64 = 33424 字节 @128kbps → 2089ms → 2 秒
        CHECK_EQ(t.duration_seconds, 2);
    }

    auto s = radmusic::probe_audio(file.string());
    CHECK(s.ok());
    if (s.ok()) {
        const AudioStreamInfo& i = s.value();
        CHECK_EQ(i.codec, std::string("mp3"));
        CHECK_EQ(i.lossless, false);
        CHECK(i.bitrate.has_value());
        if (i.bitrate) CHECK_EQ(*i.bitrate, 128);
        CHECK(i.sample_rate.has_value());
        if (i.sample_rate) CHECK_EQ(*i.sample_rate, 44100);
        CHECK(i.channels.has_value());
        if (i.channels) CHECK_EQ(*i.channels, 2);
        CHECK(i.bit_depth.has_value() == false);
        CHECK_EQ(i.file_size, static_cast<long long>(mp3.size()));
        CHECK_EQ(i.duration_ms, static_cast<long long>(2089));
    }
    cleanup(dir);
}

// ================================================================ FLAC

TEST_CASE(test_tags_flac_streaminfo_vorbis) {
    const fs::path dir = case_dir("flac");
    const fs::path file = dir / "song.flac";

    Bytes flac;
    put(flac, "fLaC");

    // ---- STREAMINFO（34 字节）：44.1kHz / 2ch / 16bit / 88200 samples = 2s
    {
        Bytes body;
        put_u16be(body, 4096);  // min block
        put_u16be(body, 4096);  // max block
        put_u24be(body, 0);     // min frame
        put_u24be(body, 0);     // max frame
        const uint64_t packed = (static_cast<uint64_t>(44100) << 44) |
                                (static_cast<uint64_t>(1) << 41) |   // channels-1
                                (static_cast<uint64_t>(15) << 36) |  // bit depth-1
                                static_cast<uint64_t>(88200);        // total samples
        put_u64be(body, packed);
        pad(body, 16);  // MD5
        CHECK_EQ(body.size(), static_cast<size_t>(34));
        put_u8(flac, 0x00);  // 非最后一块 + STREAMINFO
        put_u24be(flac, static_cast<uint32_t>(body.size()));
        append(flac, body);
    }

    // ---- VORBIS_COMMENT
    {
        Bytes vc;
        const std::string vendor = "radmusic-test";
        put_u32le(vc, static_cast<uint32_t>(vendor.size()));
        put(vc, vendor);
        const char* entries[] = {"TITLE=Flac Title", "ARTIST=Flac Artist", "TRACKNUMBER=7/12",
                                 "GENRE=Jazz", "ALBUM=Flac Album"};
        put_u32le(vc, 5);
        for (const char* e : entries) {
            const std::string s = e;
            put_u32le(vc, static_cast<uint32_t>(s.size()));
            put(vc, s);
        }
        put_u8(flac, 0x04);  // 非最后一块 + VORBIS_COMMENT
        put_u24be(flac, static_cast<uint32_t>(vc.size()));
        append(flac, vc);
    }

    // ---- PICTURE（最后一块）
    {
        Bytes pic;
        put_u32be(pic, 3);  // front cover
        const std::string mime = "image/png";
        put_u32be(pic, static_cast<uint32_t>(mime.size()));
        put(pic, mime);
        put_u32be(pic, 0);   // 描述长度
        put_u32be(pic, 16);  // width
        put_u32be(pic, 16);  // height
        put_u32be(pic, 24);  // depth
        put_u32be(pic, 0);   // colors
        const Bytes png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00};
        put_u32be(pic, static_cast<uint32_t>(png.size()));
        append(pic, png);
        put_u8(flac, 0x80 | 0x06);  // 最后一块 + PICTURE
        put_u24be(flac, static_cast<uint32_t>(pic.size()));
        append(flac, pic);
    }

    CHECK(write_file(file, flac));

    auto s = radmusic::probe_audio(file.string());
    CHECK(s.ok());
    if (s.ok()) {
        const AudioStreamInfo& i = s.value();
        CHECK_EQ(i.codec, std::string("flac"));
        CHECK_EQ(i.lossless, true);
        CHECK(i.sample_rate.has_value());
        if (i.sample_rate) CHECK_EQ(*i.sample_rate, 44100);
        CHECK(i.bit_depth.has_value());
        if (i.bit_depth) CHECK_EQ(*i.bit_depth, 16);
        CHECK(i.channels.has_value());
        if (i.channels) CHECK_EQ(*i.channels, 2);
        CHECK_EQ(i.duration_ms, static_cast<long long>(2000));
    }

    auto r = radmusic::read_tags(file.string());
    CHECK(r.ok());
    if (r.ok()) {
        const TagData& t = r.value();
        CHECK_EQ(t.title, std::string("Flac Title"));
        CHECK_EQ(t.artist, std::string("Flac Artist"));
        CHECK_EQ(t.album, std::string("Flac Album"));
        CHECK_EQ(t.track_no, 7);
        CHECK_EQ(t.genre, std::string("Jazz"));
        CHECK_EQ(t.duration_seconds, 2);
        CHECK_EQ(t.tag_source, std::string("vorbis"));
        CHECK_EQ(t.cover.size(), static_cast<size_t>(10));
        CHECK_EQ(t.cover_mime, std::string("image/png"));
    }
    cleanup(dir);
}

// ================================================================ WAV

TEST_CASE(test_tags_wav_info) {
    const fs::path dir = case_dir("wav");
    const fs::path file = dir / "song.wav";

    const int sample_rate = 8000;
    const int channels = 1;
    const int bits = 16;
    const int byte_rate = sample_rate * channels * bits / 8;  // 16000
    const int data_bytes = byte_rate;                         // 1 秒

    Bytes wav;
    put(wav, "RIFF");
    put_u32le(wav, 0);  // 稍后回填
    put(wav, "WAVE");
    put(wav, "fmt ");
    put_u32le(wav, 16);
    put_u16le(wav, 1);  // PCM
    put_u16le(wav, static_cast<uint16_t>(channels));
    put_u32le(wav, static_cast<uint32_t>(sample_rate));
    put_u32le(wav, static_cast<uint32_t>(byte_rate));
    put_u16le(wav, static_cast<uint16_t>(channels * bits / 8));  // block align
    put_u16le(wav, static_cast<uint16_t>(bits));
    {
        Bytes info;
        put(info, "INFO");
        const std::string title = "Wav Title";
        put(info, "INAM");
        put_u32le(info, static_cast<uint32_t>(title.size() + 1));
        put(info, title);
        put_u8(info, 0);
        const std::string artist = "Wav Artist";
        put(info, "IART");
        put_u32le(info, static_cast<uint32_t>(artist.size() + 1));
        put(info, artist);
        put_u8(info, 0);
        put_u8(info, 0);  // 子块偶数对齐
        CHECK_EQ(info.size() % 2, static_cast<size_t>(0));
        put(wav, "LIST");
        put_u32le(wav, static_cast<uint32_t>(info.size()));
        append(wav, info);
    }
    put(wav, "data");
    put_u32le(wav, static_cast<uint32_t>(data_bytes));
    pad(wav, static_cast<size_t>(data_bytes));

    const uint32_t riff_size = static_cast<uint32_t>(wav.size() - 8);
    wav[4] = static_cast<uint8_t>(riff_size & 0xFF);
    wav[5] = static_cast<uint8_t>((riff_size >> 8) & 0xFF);
    wav[6] = static_cast<uint8_t>((riff_size >> 16) & 0xFF);
    wav[7] = static_cast<uint8_t>((riff_size >> 24) & 0xFF);

    CHECK(write_file(file, wav));

    auto r = radmusic::read_tags(file.string());
    CHECK(r.ok());
    if (r.ok()) {
        CHECK_EQ(r.value().title, std::string("Wav Title"));
        CHECK_EQ(r.value().artist, std::string("Wav Artist"));
        CHECK_EQ(r.value().tag_source, std::string("wav"));
        CHECK_EQ(r.value().duration_seconds, 1);
    }

    auto s = radmusic::probe_audio(file.string());
    CHECK(s.ok());
    if (s.ok()) {
        const AudioStreamInfo& i = s.value();
        CHECK_EQ(i.codec, std::string("pcm"));
        CHECK_EQ(i.lossless, true);
        CHECK(i.sample_rate.has_value());
        if (i.sample_rate) CHECK_EQ(*i.sample_rate, sample_rate);
        CHECK(i.bit_depth.has_value());
        if (i.bit_depth) CHECK_EQ(*i.bit_depth, bits);
        CHECK(i.channels.has_value());
        if (i.channels) CHECK_EQ(*i.channels, channels);
        CHECK(i.bitrate.has_value());
        if (i.bitrate) CHECK_EQ(*i.bitrate, byte_rate * 8 / 1000);
        CHECK_EQ(i.duration_ms, static_cast<long long>(1000));
    }
    cleanup(dir);
}

// ================================================================ M4A / MP4

namespace {

Bytes mp4_box(const char* type, const Bytes& content) {
    Bytes out;
    put_u32be(out, static_cast<uint32_t>(content.size() + 8));
    put(out, std::string(type, 4));
    append(out, content);
    return out;
}

Bytes mp4_data(uint32_t type_flag, const Bytes& payload) {
    Bytes c;
    put_u32be(c, type_flag);
    put_u32be(c, 0);  // locale
    append(c, payload);
    return mp4_box("data", c);
}

}  // namespace

TEST_CASE(test_tags_m4a_ilst) {
    const fs::path dir = case_dir("m4a");
    const fs::path file = dir / "song.m4a";

    Bytes ftyp;
    put(ftyp, "M4A ");
    put_u32be(ftyp, 0);
    put(ftyp, "M4A ");
    put(ftyp, "isom");

    Bytes moov;
    {
        // mvhd（version 0）：timescale 1000，duration 3000 → 3 秒
        Bytes mvhd;
        put_u32be(mvhd, 0);  // version + flags
        put_u32be(mvhd, 0);  // creation
        put_u32be(mvhd, 0);  // modification
        put_u32be(mvhd, 1000);
        put_u32be(mvhd, 3000);
        pad(mvhd, 80);
        const Bytes box = mp4_box("mvhd", mvhd);
        append(moov, box);
    }
    {
        // stsd：一条 mp4a(AAC) entry，44.1kHz / 2ch / 16bit
        Bytes entry;
        put(entry, "mp4a");
        pad(entry, 6);        // reserved
        put_u16be(entry, 1);  // data reference index
        put_u16be(entry, 0);  // version
        put_u16be(entry, 0);  // revision
        put_u32be(entry, 0);  // vendor
        put_u16be(entry, 2);  // channels
        put_u16be(entry, 16); // sample size
        put_u16be(entry, 0);  // pre-defined
        put_u16be(entry, 0);  // reserved
        put_u32be(entry, static_cast<uint32_t>(44100) << 16);  // sample rate
        Bytes esds;
        put_u32be(esds, 0);
        const Bytes esds_box = mp4_box("esds", esds);
        append(entry, esds_box);

        Bytes stsd;
        put_u32be(stsd, 0);  // version + flags
        put_u32be(stsd, 1);  // entry count
        put_u32be(stsd, static_cast<uint32_t>(entry.size() + 4));  // entry box size = type + 内容 + size
        append(stsd, entry);

        const Bytes stbl = mp4_box("stbl", mp4_box("stsd", stsd));
        const Bytes minf = mp4_box("minf", stbl);
        const Bytes mdia = mp4_box("mdia", minf);
        append(moov, mp4_box("trak", mdia));
    }
    {
        Bytes ilst;
        auto add_item = [&ilst](const char* type, const Bytes& data_box) {
            append(ilst, mp4_box(type, data_box));
        };
        add_item(kNam, mp4_data(1, latin1("M4A Title")));
        add_item(kART, mp4_data(1, latin1("M4A Artist")));
        add_item(kAlb, mp4_data(1, latin1("M4A Album")));
        {
            Bytes trkn;
            put_u16be(trkn, 0);
            put_u16be(trkn, 7);   // track 7
            put_u16be(trkn, 12);  // of 12
            put_u16be(trkn, 0);
            add_item("trkn", mp4_data(0, trkn));
        }
        {
            const Bytes payload(std::begin(kFakeJpeg), std::end(kFakeJpeg));
            add_item("covr", mp4_data(13, payload));  // 13 = JPEG
        }

        Bytes meta;
        put_u32be(meta, 0);  // meta 是 full box
        append(meta, mp4_box("ilst", ilst));
        const Bytes udta = mp4_box("udta", mp4_box("meta", meta));
        append(moov, udta);
    }

    Bytes mp4;
    append(mp4, mp4_box("ftyp", ftyp));
    append(mp4, mp4_box("moov", moov));
    pad(mp4, 32);

    CHECK(write_file(file, mp4));

    auto r = radmusic::read_tags(file.string());
    CHECK(r.ok());
    if (r.ok()) {
        const TagData& t = r.value();
        CHECK_EQ(t.title, std::string("M4A Title"));
        CHECK_EQ(t.artist, std::string("M4A Artist"));
        CHECK_EQ(t.album, std::string("M4A Album"));
        CHECK_EQ(t.track_no, 7);
        CHECK_EQ(t.tag_source, std::string("mp4"));
        CHECK_EQ(t.cover.size(), sizeof(kFakeJpeg));
        CHECK_EQ(t.cover_mime, std::string("image/jpeg"));
        CHECK_EQ(t.duration_seconds, 3);
    }

    auto s = radmusic::probe_audio(file.string());
    CHECK(s.ok());
    if (s.ok()) {
        const AudioStreamInfo& i = s.value();
        CHECK_EQ(i.codec, std::string("aac"));
        CHECK_EQ(i.lossless, false);
        CHECK_EQ(i.duration_ms, static_cast<long long>(3000));
        CHECK(i.sample_rate.has_value());
        if (i.sample_rate) CHECK_EQ(*i.sample_rate, 44100);
        CHECK(i.channels.has_value());
        if (i.channels) CHECK_EQ(*i.channels, 2);
    }
    cleanup(dir);
}

// ================================================================ APE

TEST_CASE(test_tags_ape_footer) {
    const fs::path dir = case_dir("ape");
    const fs::path file = dir / "song.ape";

    Bytes ape;
    put(ape, "MAC ");
    put_u16le(ape, 3990);  // version（3.98+ 描述符布局）
    put_u16le(ape, 0);     // padding
    put_u32le(ape, 0);     // descriptor bytes
    put_u32le(ape, 24);    // header bytes
    put_u32le(ape, 0);     // seektable bytes
    put_u32le(ape, 0);     // header data bytes
    put_u32le(ape, 0);     // frame data bytes
    put_u32le(ape, 0);
    put_u32le(ape, 0);
    pad(ape, 16);           // MD5
    put_u16le(ape, 2000);   // compression level
    put_u16le(ape, 0);      // format flags
    put_u32le(ape, 44100);  // blocks per frame
    put_u32le(ape, 0);      // final frame blocks
    put_u32le(ape, 2);      // total frames → 44100 samples = 1s
    put_u16le(ape, 16);     // bits per sample
    put_u16le(ape, 2);      // channels
    put_u32le(ape, 44100);  // sample rate
    CHECK_EQ(ape.size(), static_cast<size_t>(76));
    pad(ape, 256, 0x11);  // 假音频数据

    Bytes items;
    auto add_text = [&items](const char* key, const std::string& value) {
        put_u32le(items, static_cast<uint32_t>(value.size()));
        put_u32le(items, 0);  // flags：文本
        put(items, key);
        put_u8(items, 0);
        put(items, value);
    };
    add_text("Title", "Ape Title");
    add_text("Artist", "Ape Artist");
    add_text("Album", "Ape Album");
    add_text("Track", "5/9");
    add_text("Genre", "Blues");
    {
        Bytes cover;
        put(cover, "front.jpg");
        put_u8(cover, 0);
        cover.insert(cover.end(), std::begin(kFakeJpeg), std::end(kFakeJpeg));
        put_u32le(items, static_cast<uint32_t>(cover.size()));
        put_u32le(items, 2);  // flags：二进制
        put(items, "Cover Art (Front)");
        put_u8(items, 0);
        append(items, cover);
    }
    const uint32_t tag_size = static_cast<uint32_t>(items.size() + 32);
    append(ape, items);
    put(ape, "APETAGEX");
    put_u32le(ape, 2000);      // version
    put_u32le(ape, tag_size);  // 含 footer、不含 header
    put_u32le(ape, 6);         // item count
    put_u32le(ape, 0);         // flags
    pad(ape, 8);

    CHECK(write_file(file, ape));

    auto r = radmusic::read_tags(file.string());
    CHECK(r.ok());
    if (r.ok()) {
        const TagData& t = r.value();
        CHECK_EQ(t.title, std::string("Ape Title"));
        CHECK_EQ(t.artist, std::string("Ape Artist"));
        CHECK_EQ(t.album, std::string("Ape Album"));
        CHECK_EQ(t.track_no, 5);
        CHECK_EQ(t.genre, std::string("Blues"));
        CHECK_EQ(t.tag_source, std::string("ape"));
        CHECK_EQ(t.cover.size(), sizeof(kFakeJpeg));
        CHECK_EQ(t.cover_mime, std::string("image/jpeg"));
        CHECK_EQ(t.duration_seconds, 1);
    }

    auto s = radmusic::probe_audio(file.string());
    CHECK(s.ok());
    if (s.ok()) {
        const AudioStreamInfo& i = s.value();
        CHECK_EQ(i.codec, std::string("ape"));
        CHECK_EQ(i.lossless, true);
        CHECK(i.bit_depth.has_value());
        if (i.bit_depth) CHECK_EQ(*i.bit_depth, 16);
        CHECK_EQ(i.duration_ms, static_cast<long long>(1000));
    }
    cleanup(dir);
}

// ================================================================ Ogg / Opus

TEST_CASE(test_tags_ogg_opus_probe) {
    const fs::path dir = case_dir("ogg");

    // ---- Ogg Vorbis：identification header + 末页 granule = 3s
    {
        const fs::path file = dir / "song.ogg";
        Bytes payload;
        put_u8(payload, 1);
        put(payload, "vorbis");
        put_u32le(payload, 0);       // version
        put_u8(payload, 2);          // channels
        put_u32le(payload, 44100);   // sample rate
        put_u32le(payload, 0);       // bitrate max
        put_u32le(payload, 128000);  // bitrate nominal
        put_u32le(payload, 0);       // bitrate min
        put_u8(payload, 0xB8);       // blocksizes
        put_u8(payload, 1);          // framing
        CHECK_EQ(payload.size(), static_cast<size_t>(30));

        Bytes ogg;
        put(ogg, "OggS");
        put_u8(ogg, 0);
        put_u8(ogg, 0x02);  // BOS
        put_u64le(ogg, 0);
        put_u32le(ogg, 1);
        put_u32le(ogg, 0);
        put_u32le(ogg, 0);
        put_u8(ogg, 1);  // 1 segment
        put_u8(ogg, static_cast<uint8_t>(payload.size()));
        append(ogg, payload);
        // 末页：granule = 44100 * 3
        put(ogg, "OggS");
        put_u8(ogg, 0);
        put_u8(ogg, 0x04);  // EOS
        put_u64le(ogg, static_cast<uint64_t>(44100) * 3);
        put_u32le(ogg, 1);
        put_u32le(ogg, 1);
        put_u32le(ogg, 0);
        put_u8(ogg, 0);
        CHECK(write_file(file, ogg));

        auto s = radmusic::probe_audio(file.string());
        CHECK(s.ok());
        if (s.ok()) {
            const AudioStreamInfo& i = s.value();
            CHECK_EQ(i.codec, std::string("vorbis"));
            CHECK_EQ(i.lossless, false);
            CHECK(i.sample_rate.has_value());
            if (i.sample_rate) CHECK_EQ(*i.sample_rate, 44100);
            CHECK(i.channels.has_value());
            if (i.channels) CHECK_EQ(*i.channels, 2);
            CHECK(i.bitrate.has_value());
            if (i.bitrate) CHECK_EQ(*i.bitrate, 128);
            CHECK_EQ(i.duration_ms, static_cast<long long>(3000));
        }

        // 契约：Ogg/Opus 标签解析可以返回 TagParseFailed
        auto r = radmusic::read_tags(file.string());
        CHECK_FALSE(r.ok());
        if (!r.ok()) CHECK_EQ(r.status().code, radmusic::ErrorCode::TagParseFailed);
    }

    // ---- Ogg Opus
    {
        const fs::path file = dir / "song.opus";
        Bytes payload;
        put(payload, "OpusHead");
        put_u8(payload, 1);       // version
        put_u8(payload, 2);       // channels
        put_u16le(payload, 312);  // pre-skip
        put_u32le(payload, 48000);
        put_u16le(payload, 0);  // gain
        put_u8(payload, 0);     // mapping family
        CHECK_EQ(payload.size(), static_cast<size_t>(19));

        Bytes ogg;
        put(ogg, "OggS");
        put_u8(ogg, 0);
        put_u8(ogg, 0x02);
        put_u64le(ogg, 0);
        put_u32le(ogg, 2);
        put_u32le(ogg, 0);
        put_u32le(ogg, 0);
        put_u8(ogg, 1);
        put_u8(ogg, static_cast<uint8_t>(payload.size()));
        append(ogg, payload);
        CHECK(write_file(file, ogg));

        auto s = radmusic::probe_audio(file.string());
        CHECK(s.ok());
        if (s.ok()) {
            CHECK_EQ(s.value().codec, std::string("opus"));
            CHECK(s.value().sample_rate.has_value());
            if (s.value().sample_rate) CHECK_EQ(*s.value().sample_rate, 48000);
            CHECK(s.value().channels.has_value());
            if (s.value().channels) CHECK_EQ(*s.value().channels, 2);
        }
        auto r = radmusic::read_tags(file.string());
        CHECK_FALSE(r.ok());
        if (!r.ok()) CHECK_EQ(r.status().code, radmusic::ErrorCode::TagParseFailed);
    }
    cleanup(dir);
}

// ================================================================ quality 归一化

TEST_CASE(test_tags_quality_from_stream) {
    {
        AudioStreamInfo info;
        info.codec = "flac";
        info.lossless = true;
        info.sample_rate = 44100;
        info.bit_depth = 16;
        info.channels = 2;
        const Quality q = radmusic::quality_from_stream(info);
        CHECK_EQ(q.level, QualityLevel::Lossless);
        CHECK_EQ(q.codec, std::string("flac"));
        CHECK_EQ(q.lossless, true);
        CHECK(q.sample_rate.has_value());
        if (q.sample_rate) CHECK_EQ(*q.sample_rate, 44100);
        CHECK(q.bit_depth.has_value());
        if (q.bit_depth) CHECK_EQ(*q.bit_depth, 16);
        CHECK(q.channels.has_value());
        if (q.channels) CHECK_EQ(*q.channels, 2);
        CHECK(q.quality_id.empty());  // 本地音质省略 quality_id
    }
    {
        AudioStreamInfo info;
        info.codec = "flac";
        info.lossless = true;
        info.sample_rate = 96000;
        info.bit_depth = 24;
        CHECK_EQ(radmusic::quality_from_stream(info).level, QualityLevel::HiRes);
    }
    {
        AudioStreamInfo info;
        info.codec = "mp3";
        info.lossless = false;
        info.bitrate = 320;
        info.sample_rate = 44100;
        const Quality q = radmusic::quality_from_stream(info);
        CHECK_EQ(q.level, QualityLevel::High);
        CHECK_EQ(q.lossless, false);
        CHECK(q.bit_depth.has_value() == false);
    }
    {
        AudioStreamInfo info;
        info.codec = "mp3";
        info.lossless = false;
        info.bitrate = 96;
        CHECK_EQ(radmusic::quality_from_stream(info).level, QualityLevel::Low);
    }
    {
        // 无损但采样率/位深缺失：仍按 Lossless
        AudioStreamInfo info;
        info.codec = "alac";
        info.lossless = true;
        info.bit_depth = 16;
        CHECK_EQ(radmusic::quality_from_stream(info).level, QualityLevel::Lossless);
    }
}

// ================================================================ 文件名回退

TEST_CASE(test_tags_from_filename) {
    {
        const TagData t = radmusic::tags_from_filename("D:/x/Artist - Title.mp3");
        CHECK_EQ(t.artist, std::string("Artist"));
        CHECK_EQ(t.title, std::string("Title"));
        CHECK_EQ(t.tag_source, std::string("filename"));
        CHECK_EQ(t.track_no, 0);
        CHECK_EQ(t.duration_seconds, 0);
        CHECK(t.cover.empty());
        CHECK(t.lyrics_text.empty());
    }
    {
        // 无分隔符：整串作为 title
        const TagData t = radmusic::tags_from_filename("D:/x/Solo Song.flac");
        CHECK_EQ(t.title, std::string("Solo Song"));
        CHECK(t.artist.empty());
        CHECK_EQ(t.tag_source, std::string("filename"));
    }
    {
        // Windows 路径分隔符 + 扩展名前有多个点
        const TagData t = radmusic::tags_from_filename("C:\\music\\Some Artist - Some Title.v2.mp3");
        CHECK_EQ(t.artist, std::string("Some Artist"));
        CHECK_EQ(t.title, std::string("Some Title.v2"));
    }
    {
        // 反向模板
        const TagData t = radmusic::tags_from_filename("D:/x/Title - Artist.ogg", "{title} - {artist}");
        CHECK_EQ(t.title, std::string("Title"));
        CHECK_EQ(t.artist, std::string("Artist"));
    }
    {
        // 只含 {title}
        const TagData t = radmusic::tags_from_filename("D:/x/Only Title.wav", "{title}");
        CHECK_EQ(t.title, std::string("Only Title"));
        CHECK(t.artist.empty());
    }
    {
        // 带序号前缀：只按第一个分隔符切分，其余留在 title 里
        const TagData t = radmusic::tags_from_filename("D:/x/01 - Artist - Title.mp3");
        CHECK_EQ(t.artist, std::string("01"));
        CHECK_EQ(t.title, std::string("Artist - Title"));
    }
    {
        // 显式传入默认模板，行为一致
        const TagData t =
            radmusic::tags_from_filename("D:/x/Artist - Title.mp3", "{artist} - {title}");
        CHECK_EQ(t.artist, std::string("Artist"));
        CHECK_EQ(t.title, std::string("Title"));
    }
}

// ================================================================ 错误路径

TEST_CASE(test_tags_errors) {
    const fs::path dir = case_dir("errors");

    // 不存在的文件 → FileNotFound
    {
        const fs::path missing = dir / "missing.mp3";
        auto r = radmusic::read_tags(missing.string());
        CHECK_FALSE(r.ok());
        if (!r.ok()) CHECK_EQ(r.status().code, radmusic::ErrorCode::FileNotFound);
        auto s = radmusic::probe_audio(missing.string());
        CHECK_FALSE(s.ok());
        if (!s.ok()) CHECK_EQ(s.status().code, radmusic::ErrorCode::FileNotFound);
    }

    // 扩展名不支持 → UnsupportedFormat
    {
        const fs::path txt = dir / "note.txt";
        Bytes data;
        put(data, "hello rad music");
        CHECK(write_file(txt, data));
        auto r = radmusic::read_tags(txt.string());
        CHECK_FALSE(r.ok());
        if (!r.ok()) CHECK_EQ(r.status().code, radmusic::ErrorCode::UnsupportedFormat);
        auto s = radmusic::probe_audio(txt.string());
        CHECK_FALSE(s.ok());
        if (!s.ok()) CHECK_EQ(s.status().code, radmusic::ErrorCode::UnsupportedFormat);
        CHECK_EQ(radmusic::audio_container_supported(
                     radmusic::audio_container_from_extension(txt.string())),
                 false);
    }

    // 容器受支持但内容损坏 → TagParseFailed（非致命）
    {
        const fs::path broken = dir / "broken.flac";
        Bytes data;
        put(data, "fLaC");
        pad(data, 8);
        CHECK(write_file(broken, data));
        auto r = radmusic::read_tags(broken.string());
        CHECK_FALSE(r.ok());
        if (!r.ok()) CHECK_EQ(r.status().code, radmusic::ErrorCode::TagParseFailed);
        auto s = radmusic::probe_audio(broken.string());
        CHECK_FALSE(s.ok());
        if (!s.ok()) CHECK_EQ(s.status().code, radmusic::ErrorCode::TagParseFailed);
    }

    // 受支持容器但完全没有标签 → TagParseFailed（调用方回退文件名），流信息仍可探测
    {
        const fs::path bare = dir / "bare.mp3";
        Bytes data;
        put_u8(data, 0xFF);
        put_u8(data, 0xFB);
        put_u8(data, 0x90);
        put_u8(data, 0x00);
        pad(data, 417 - 4);
        CHECK(write_file(bare, data));
        auto r = radmusic::read_tags(bare.string());
        CHECK_FALSE(r.ok());
        if (!r.ok()) CHECK_EQ(r.status().code, radmusic::ErrorCode::TagParseFailed);
        auto s = radmusic::probe_audio(bare.string());
        CHECK(s.ok());
        if (s.ok()) {
            CHECK_EQ(s.value().codec, std::string("mp3"));
            CHECK_EQ(s.value().duration_ms, static_cast<long long>(417 * 8 / 128));
        }
    }

    // 空文件：按扩展名判定为 mp3 → 失败但不抛异常
    {
        const fs::path empty = dir / "empty.mp3";
        CHECK(write_file(empty, Bytes{}));
        auto r = radmusic::read_tags(empty.string());
        CHECK_FALSE(r.ok());
        if (!r.ok()) CHECK_EQ(r.status().code, radmusic::ErrorCode::TagParseFailed);
    }

    // 目录冒充音频文件：不崩溃，返回错误
    {
        const fs::path sub = dir / "sub.flac";
        std::error_code ec;
        fs::create_directories(sub, ec);
        auto r = radmusic::read_tags(sub.string());
        CHECK_FALSE(r.ok());
    }

    cleanup(dir);
}
