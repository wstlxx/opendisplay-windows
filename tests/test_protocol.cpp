// Unit tests for the OpenDisplay protocol module (framing, demux, Annex-B
// parsing, SPS dimensions, control messages). Platform-independent.

#include <cstdio>
#include <cstring>
#include <vector>

#include "protocol/annexb.h"
#include "protocol/control.h"
#include "protocol/demux.h"
#include "protocol/framing.h"
#include "protocol/sps.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        ++g_checks;                                                        \
        auto _va = (a);                                                    \
        auto _vb = (b);                                                    \
        if (!((uint64_t)_va == (uint64_t)_vb)) {                           \
            ++g_failures;                                                  \
            std::printf("FAIL %s:%d: %s == %s (%lld == %lld)\n", __FILE__, \
                        __LINE__, #a, #b, (long long)(int64_t)_va, (long long)(int64_t)_vb); \
        }                                                                  \
    } while (0)

using od::FrameDecoder;

std::vector<uint8_t> Bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// 4-byte start code helper.
const uint8_t kStartCode[4] = {0, 0, 0, 1};

void TestFrameEncodeDecode() {
    const std::vector<uint8_t> payload = Bytes("hello world");
    const auto wire = od::EncodeFrame(payload);
    CHECK_EQ(wire.size(), 15u); // 11 payload + 4 header
    CHECK(wire[0] == 0 && wire[1] == 0 && wire[2] == 0 && wire[3] == 11);

    FrameDecoder dec;
    bool valid = true;
    auto frames = dec.Feed(wire.data(), wire.size(), valid);
    CHECK(valid);
    CHECK_EQ(frames.size(), 1u);
    CHECK(std::equal(frames[0].begin(), frames[0].end(), payload.begin()));
}

void TestFrameSplitAcrossReads() {
    const std::string payload(1000, 'x');
    const auto wire = od::EncodeFrame(payload);
    FrameDecoder dec;
    bool valid = true;
    size_t total = 0;
    // Feed one byte at a time: the worst case of split reads.
    for (size_t i = 0; i < wire.size(); ++i) {
        auto frames = dec.Feed(wire.data() + i, 1, valid);
        CHECK(valid);
        for (auto& f : frames) total += f.size();
    }
    CHECK_EQ(total, payload.size());
}

void TestFramePackedTogether() {
    // Two frames in one read, plus a partial third.
    const auto a = od::EncodeFrame(Bytes("aaaa"));
    const auto b = od::EncodeFrame(Bytes("bbbbbbbb"));
    const auto c = od::EncodeFrame(Bytes("cccc"));
    std::vector<uint8_t> blob;
    blob.insert(blob.end(), a.begin(), a.end());
    blob.insert(blob.end(), b.begin(), b.end());
    blob.insert(blob.end(), c.begin(), c.begin() + 5); // partial

    FrameDecoder dec;
    bool valid = true;
    auto frames = dec.Feed(blob.data(), blob.size(), valid);
    CHECK(valid);
    CHECK_EQ(frames.size(), 2u);
    CHECK_EQ(frames[0].size(), 4u);
    CHECK_EQ(frames[1].size(), 8u);
    CHECK_EQ(dec.BufferedBytes(), 5u); // frame C: 4 header bytes + 1 of 4 payload
    frames = dec.Feed(blob.data() + blob.size() - 3, 3, valid);
    CHECK(valid);
    CHECK_EQ(frames.size(), 1u);
    CHECK_EQ(frames[0].size(), 4u);
}

void TestFrameInvalidLength() {
    // Length prefix above the 16 MiB cap -> fatal.
    uint8_t big[4] = {0x01, 0x00, 0x00, 0x00}; // 16 MiB exactly is OK
    FrameDecoder dec;
    bool valid = true;
    auto frames = dec.Feed(big, 4, valid);
    CHECK(valid);
    CHECK_EQ(frames.size(), 0u);

    // ~4 GiB prefix in a fresh decoder -> fatal.
    FrameDecoder dec2;
    uint8_t hostile[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    frames = dec2.Feed(hostile, 4, valid);
    CHECK(!valid);

    // Length 0 is tolerated (harmless empty payload).
    FrameDecoder dec3;
    uint8_t zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    frames = dec3.Feed(zero, 8, valid);
    CHECK(valid);
    CHECK_EQ(frames.size(), 2u);
}

void TestLargeFrame() {
    std::vector<uint8_t> payload(3 * 1024 * 1024, 0xAB);
    const auto wire = od::EncodeFrame(payload);
    FrameDecoder dec;
    bool valid = true;
    // Two reads of uneven sizes.
    auto f1 = dec.Feed(wire.data(), 1000, valid);
    CHECK(valid);
    CHECK_EQ(f1.size(), 0u);
    auto f2 = dec.Feed(wire.data() + 1000, wire.size() - 1000, valid);
    CHECK(valid);
    CHECK_EQ(f2.size(), 1u);
    CHECK_EQ(f2[0].size(), payload.size());
    CHECK(f2[0].data()[payload.size() - 1] == 0xAB);
}

void TestDemux() {
    using od::IsControlJson;
    const auto json = Bytes("{\"type\":\"ping\",\"t\":123}");
    CHECK(IsControlJson(json));

    const std::vector<uint8_t> empty;
    CHECK(!IsControlJson(empty));

    // Starts with '{' but contains NUL (telemetry-prefixed video frame).
    std::vector<uint8_t> videoWithPrefix = Bytes("{\"cap\":1760000000000,\"snd\":1760000000001}");
    videoWithPrefix.insert(videoWithPrefix.end(), kStartCode, kStartCode + 4);
    videoWithPrefix.push_back(0x65);
    CHECK(!IsControlJson(videoWithPrefix));

    // Binary video without prefix.
    std::vector<uint8_t> video(kStartCode, kStartCode + 4);
    video.push_back(0x67);
    CHECK(!IsControlJson(video));

    // Exactly 32768 bytes of JSON must be treated as video (rule 1: < 32768).
    std::vector<uint8_t> huge(32768, 'a');
    huge[0] = '{';
    CHECK(!IsControlJson(huge));

    // 32767 bytes of JSON with no NUL is control.
    std::vector<uint8_t> justUnder(32767, 'a');
    justUnder[0] = '{';
    CHECK(IsControlJson(justUnder));

    // Not starting with '{'.
    const auto notJson = Bytes("[1,2,3]");
    CHECK(!IsControlJson(notJson));
}

void TestAnnexBKeyframe() {
    // Telemetry prefix + SPS + PPS + IDR slice, exactly as a sender emits it.
    std::vector<uint8_t> f;
    const std::vector<uint8_t> telem = Bytes("{\"cap\":1760000000123,\"snd\":1760000000124}");
    f.insert(f.end(), telem.begin(), telem.end());
    f.insert(f.end(), kStartCode, kStartCode + 4);
    f.push_back(0x67); f.push_back(0xAA); f.push_back(0xBB); // fake SPS
    f.insert(f.end(), kStartCode, kStartCode + 4);
    f.push_back(0x68); f.push_back(0xCC); // fake PPS
    f.insert(f.end(), kStartCode, kStartCode + 4);
    f.push_back(0x65); f.push_back(0x88); f.push_back(0x84); // IDR slice
    f.insert(f.end(), kStartCode, kStartCode + 4);
    f.push_back(0x06); f.push_back(0x05); // SEI (must be skipped)

    const auto frame = od::ParseAnnexB(f);
    CHECK_EQ(frame.captureMs, 1760000000123LL);
    CHECK_EQ(frame.sendMs, 1760000000124LL);
    CHECK_EQ(frame.sps.size(), 3u);
    CHECK(frame.sps[0] == 0x67);
    CHECK_EQ(frame.pps.size(), 2u);
    CHECK(frame.pps[0] == 0x68);
    CHECK_EQ(frame.vclNalus.size(), 1u); // SEI dropped
    CHECK(frame.vclNalus[0][0] == 0x65);
    CHECK(frame.IsKeyframe());
    CHECK(!frame.IsEmpty());
}

void TestAnnexBNonKeyframe() {
    std::vector<uint8_t> f;
    f.insert(f.end(), kStartCode, kStartCode + 4);
    f.push_back(0x41); f.push_back(0x9A); f.push_back(0x00); f.push_back(0x10); // P slice
    const auto frame = od::ParseAnnexB(f);
    CHECK(frame.telemetryPrefix.empty());
    CHECK_EQ(frame.captureMs, -1LL);
    CHECK(frame.sps.empty());
    CHECK_EQ(frame.vclNalus.size(), 1u);
    CHECK(!frame.IsKeyframe());
}

void TestAnnexBMalformed() {
    // No start codes at all.
    const auto garbage = Bytes("this is not annex b");
    const auto frame = od::ParseAnnexB(garbage);
    CHECK(frame.IsEmpty());

    // Empty payload.
    const auto none = od::ParseAnnexB({});
    CHECK(none.IsEmpty());

    // Start code with no NALU bytes after it.
    std::vector<uint8_t> only(kStartCode, kStartCode + 4);
    CHECK(od::ParseAnnexB(only).IsEmpty());

    // Empty NALU between two start codes is skipped.
    std::vector<uint8_t> e;
    e.insert(e.end(), kStartCode, kStartCode + 4);
    e.insert(e.end(), kStartCode, kStartCode + 4);
    e.push_back(0x41); e.push_back(0x9A);
    const auto f = od::ParseAnnexB(e);
    CHECK_EQ(f.vclNalus.size(), 1u);
}

void TestTelemetryTolerance() {
    // Absent fields and unknown fields are tolerated.
    int64_t cap = -2, snd = -2;
    od::ParseTelemetry(Bytes(""), cap, snd);
    CHECK_EQ(cap, -1LL);
    CHECK_EQ(snd, -1LL);
    od::ParseTelemetry(Bytes("{\"cap\":42}"), cap, snd);
    CHECK_EQ(cap, 42LL);
    CHECK_EQ(snd, -1LL);
    od::ParseTelemetry(Bytes("{\"x\":1,\"snd\": 77 ,\"cap\":9}"), cap, snd);
    CHECK_EQ(cap, 9LL);
    CHECK_EQ(snd, 77LL);
}

// Hand-built baseline SPS, 120x68 MBs, no cropping. Byte strings verified
// with an independent Python exp-golomb encoder/decoder round-trip.
// Coded size 1920x1088 (a real encoder would crop to 1080).
void TestSpsDimensions() {
    const uint8_t sps1080[] = {0x67, 0x42, 0x00, 0x1E, 0xAA, 0x40, 0x3C, 0x01, 0x12, 0x00};
    auto dims = od::ParseSpsDimensions(std::span(sps1080, sizeof(sps1080)));
    CHECK(dims.has_value());
    if (dims) {
        CHECK_EQ(dims->width, 1920);
        CHECK_EQ(dims->height, 1088);
    }

    const uint8_t sps360[] = {0x67, 0x42, 0x00, 0x1E, 0xAA, 0x40, 0x50, 0x17, 0x80};
    auto dims2 = od::ParseSpsDimensions(std::span(sps360, sizeof(sps360)));
    CHECK(dims2.has_value());
    if (dims2) {
        CHECK_EQ(dims2->width, 640);
        CHECK_EQ(dims2->height, 368);
    }

    // Real-world High-profile SPS (profile 100, level 4.0) from an actual
    // capture file; FFmpeg reports 640x480 for it.
    const uint8_t spsHigh[] = {0x67, 0x64, 0x00, 0x28, 0xAC, 0xCC, 0x60, 0x28,
                               0x0F, 0x64};
    auto dims3 = od::ParseSpsDimensions(std::span(spsHigh, sizeof(spsHigh)));
    CHECK(dims3.has_value());
    if (dims3) {
        CHECK_EQ(dims3->width, 640);
        CHECK_EQ(dims3->height, 480);
    }

    // Malformed / too short.
    CHECK(!od::ParseSpsDimensions(std::span(sps1080, 2)).has_value());
    const uint8_t bad[] = {0x67, 0xFF, 0xFF, 0xFF};
    CHECK(!od::ParseSpsDimensions(std::span(bad, sizeof(bad))).has_value());
}

void TestControlOutgoing() {
    od::HelloInfo info;
    info.pixelsWide = 1920;
    info.pixelsHigh = 1080;
    info.scale = 1.0;
    info.id = "test-uuid";
    const auto hello = od::BuildHello(info);
    CHECK(hello.find("\"type\":\"hello\"") != std::string::npos);
    CHECK(hello.find("\"pixelsWide\":1920") != std::string::npos);
    CHECK(hello.find("\"pv\":3") != std::string::npos);

    // hello must pass the demux rule (it is what we send to the sender).
    const auto hb = Bytes(hello);
    CHECK(od::IsControlJson(hb));

    const auto ping = od::BuildPing(1760000000000LL);
    CHECK(ping == "{\"type\":\"ping\",\"t\":1760000000000}");
}

void TestControlIncoming() {
    const auto parse = [](const std::string& s) {
        return od::ParseControlMessage(Bytes(s));
    };

    auto pong = parse("{\"type\":\"pong\",\"t\":1760000000000,\"mt\":1760000000005}");
    CHECK(pong.has_value());
    if (pong) {
        CHECK(pong->type == od::ControlType::Pong);
        CHECK(pong->hasT && pong->hasMt);
        CHECK_EQ((long long)pong->t, 1760000000000LL);
        CHECK_EQ((long long)pong->mt, 1760000000005LL);
    }

    auto welcome = parse("{\"type\":\"welcome\",\"pv\":3,\"min\":1}");
    CHECK(welcome && welcome->type == od::ControlType::Welcome);
    if (welcome) {
        CHECK_EQ(welcome->welcomePv, 3);
        CHECK_EQ(welcome->welcomeMin, 1);
    }

    auto sc = parse("{\"type\":\"streamConfig\",\"codec\":\"h264\",\"width\":1920,\"height\":1080,\"framesPerSecond\":60}");
    CHECK(sc && sc->type == od::ControlType::StreamConfig);
    if (sc) {
        CHECK(sc->codec == "h264");
        CHECK_EQ(sc->width, 1920);
        CHECK_EQ(sc->height, 1080);
        CHECK_EQ(sc->fps, 60);
    }

    auto ur = parse("{\"type\":\"updateRequired\",\"target\":\"windows\",\"store\":\"https://example.com\",\"message\":\"update\"}");
    CHECK(ur && ur->type == od::ControlType::UpdateRequired);
    if (ur) CHECK(ur->message == "update");

    // Unknown type -> Unknown, not an error.
    auto unk = parse("{\"type\":\"wibble\",\"x\":1}");
    CHECK(unk && unk->type == od::ControlType::Unknown);
    if (unk) CHECK(unk->typeString == "wibble");

    // No type / not JSON -> nullopt.
    CHECK(!parse("{\"nope\":1}").has_value());
    CHECK(!parse("not json at all").has_value());

    // Sender ping with health counters parses, all fields optional.
    auto sping = parse("{\"type\":\"ping\",\"drops\":0,\"pending\":0,\"capFps\":59.9}");
    CHECK(sping && sping->type == od::ControlType::Ping);

    // Cursor messages parse (content ignored by the app in initial scope).
    CHECK(parse("{\"type\":\"cursor\",\"x\":0.5,\"y\":0.5,\"v\":1}")->type == od::ControlType::Cursor);
    CHECK(parse("{\"type\":\"cursorImg\",\"nw\":0.01,\"nh\":0.01,\"ax\":0,\"ay\":0,\"png\":\"AAAA\"}")->type == od::ControlType::CursorImg);
}

void TestEndToEndPipeline() {
    // Simulate the wire: sender sends welcome, then video keyframe, then a
    // control ping, all packed into one byte blob the way TCP delivers them.
    std::vector<uint8_t> blob;
    const auto autoAppend = [&](const std::string& s) {
        auto f = od::EncodeFrame(Bytes(s));
        blob.insert(blob.end(), f.begin(), f.end());
    };
    autoAppend("{\"type\":\"welcome\",\"pv\":3,\"min\":1}");

    std::vector<uint8_t> kf;
    const uint8_t sps[] = {0x67, 0x42, 0x00, 0x1E, 0xAA, 0x40, 0x3C, 0x01, 0x12, 0x00};
    const std::vector<uint8_t> telem = Bytes("{\"cap\":1760000001000,\"snd\":1760000001001}");
    kf.insert(kf.end(), telem.begin(), telem.end());
    kf.insert(kf.end(), kStartCode, kStartCode + 4);
    kf.insert(kf.end(), sps, sps + sizeof(sps));
    kf.insert(kf.end(), kStartCode, kStartCode + 4);
    kf.push_back(0x68); kf.push_back(0x85);
    kf.insert(kf.end(), kStartCode, kStartCode + 4);
    kf.push_back(0x65); kf.push_back(0x88); kf.push_back(0x84); kf.push_back(0x20);
    auto vframe = od::EncodeFrame(kf);
    blob.insert(blob.end(), vframe.begin(), vframe.end());

    autoAppend("{\"type\":\"ping\",\"pending\":0}");

    FrameDecoder dec;
    bool valid = true;
    auto frames = dec.Feed(blob.data(), blob.size(), valid);
    CHECK(valid);
    CHECK_EQ(frames.size(), 3u);

    // Frame 1: welcome
    CHECK(od::IsControlJson(frames[0]));
    auto welcome = od::ParseControlMessage(frames[0]);
    CHECK(welcome && welcome->type == od::ControlType::Welcome);

    // Frame 2: video keyframe with a real SPS
    CHECK(!od::IsControlJson(frames[1]));
    const auto video = od::ParseAnnexB(frames[1]);
    CHECK(video.IsKeyframe());
    auto dims = od::ParseSpsDimensions(video.sps);
    CHECK(dims && dims->width == 1920 && dims->height == 1088);

    // Frame 3: sender ping
    CHECK(od::IsControlJson(frames[2]));
    auto ping = od::ParseControlMessage(frames[2]);
    CHECK(ping && ping->type == od::ControlType::Ping);
}

} // namespace

int main() {
    TestFrameEncodeDecode();
    TestFrameSplitAcrossReads();
    TestFramePackedTogether();
    TestFrameInvalidLength();
    TestLargeFrame();
    TestDemux();
    TestAnnexBKeyframe();
    TestAnnexBNonKeyframe();
    TestAnnexBMalformed();
    TestTelemetryTolerance();
    TestSpsDimensions();
    TestControlOutgoing();
    TestControlIncoming();
    TestEndToEndPipeline();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
