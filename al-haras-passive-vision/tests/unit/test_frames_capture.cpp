#include "test_support.hpp"

#include "capture/fault_injection.hpp"
#include "capture/y4m_source.hpp"
#include "common/fsutil.hpp"
#include "frames/bounded_queue.hpp"
#include "frames/frame.hpp"

#include <thread>

using namespace ahpv;

namespace {
Frame frame_at(std::uint64_t seq, std::int64_t ts_ms, bool with_sequence = true) {
    Frame f;
    f.has_source_sequence = with_sequence;
    f.source_sequence = seq;
    f.source_timestamp_ns = ts_ms * 1000000;
    return f;
}

struct Id {
    std::uint64_t id;
};
Id summarize(const Frame& f) { return {f.frame_id}; }
}  // namespace

TEST_CASE("geometry: plane sizes for odd dimensions") {
    FrameGeometry g{5, 3, PixelFormat::I420};
    CHECK_EQ(g.luma_bytes(), 15u);
    CHECK_EQ(g.chroma_plane_bytes(), 6u);  // ceil(5/2) * ceil(3/2)
    CHECK_EQ(g.frame_bytes(), 27u);
    CHECK_EQ((FrameGeometry{4, 4, PixelFormat::Gray8}.frame_bytes()), 16u);
    CHECK_EQ((FrameGeometry{4, 4, PixelFormat::I444}.frame_bytes()), 48u);
}

TEST_CASE("timing analyzer: sequence gaps, late frames and regressions") {
    SequenceTimingAnalyzer a(33333333, 1.5);
    Frame f0 = frame_at(0, 0), f1 = frame_at(1, 33), f2 = frame_at(4, 133), f3 = frame_at(3, 140), f4 = frame_at(5, 300);
    a.observe(f0);
    a.observe(f1);
    a.observe(f2);
    a.observe(f3);
    a.observe(f4);
    CHECK_EQ(f1.flags, 0u);
    CHECK(f2.flags & kFlagSequenceGap);
    CHECK_EQ(f2.missing_before, 2u);
    CHECK(f2.flags & kFlagLate);
    CHECK(f3.flags & kFlagTimestampRegression);  // Sequence went backwards.
    CHECK(f4.flags & kFlagLate);
    // Without source sequence numbers a long interval is the only gap evidence.
    SequenceTimingAnalyzer b(33333333, 1.5);
    Frame g0 = frame_at(0, 0, false), g1 = frame_at(0, 100, false);
    b.observe(g0);
    b.observe(g1);
    CHECK((g1.flags & kFlagSequenceGap) && (g1.flags & kFlagLate));
    // A new source epoch is not compared with the previous one.
    b.mark_discontinuity();
    Frame g2 = frame_at(0, 5, false);
    g2.source_epoch = 1;
    b.observe(g2);
    CHECK(g2.flags == kFlagAfterDiscontinuity);
}

TEST_CASE("content analyzer: quality flags and repeated content") {
    ContentAnalyzer analyzer(ValidationThresholds{});
    Frame flat;
    flat.geometry = {64, 64, PixelFormat::Gray8};
    flat.data.assign(64 * 64, 8);
    analyzer.analyze(flat, true);
    CHECK((flat.flags & kFlagLowContrast) && (flat.flags & kFlagVeryDark));
    Frame again = flat;
    again.flags = 0;
    analyzer.analyze(again, true);
    CHECK(again.flags & kFlagRepeatedContent);
    Frame textured = flat;
    textured.flags = 0;
    for (std::size_t i = 0; i < textured.data.size(); ++i) textured.data[i] = static_cast<std::uint8_t>(i * 37 % 251);
    analyzer.analyze(textured, true);
    CHECK_EQ(textured.flags, 0u);
}

TEST_CASE("bounded queue: drop policies retain dropped frame identities") {
    BoundedQueue<Frame, Id> oldest(2, DropPolicy::DropOldest, &summarize);
    for (std::uint64_t i = 0; i < 5; ++i) {
        Frame f;
        f.frame_id = i;
        oldest.push(std::move(f));
    }
    auto dropped = oldest.take_dropped();
    CHECK_EQ(dropped.size(), 3u);
    CHECK_EQ(dropped[0].id, 0u);
    CHECK_EQ(oldest.pop(std::chrono::milliseconds(1))->frame_id, 3u);
    CHECK_EQ(oldest.drops(), 3u);

    BoundedQueue<Frame, Id> newest(2, DropPolicy::DropNewest, &summarize);
    for (std::uint64_t i = 0; i < 4; ++i) {
        Frame f;
        f.frame_id = i;
        newest.push(std::move(f));
    }
    dropped = newest.take_dropped();
    CHECK_EQ(dropped.size(), 2u);
    CHECK_EQ(dropped[1].id, 3u);
    CHECK_EQ(newest.pop(std::chrono::milliseconds(1))->frame_id, 0u);
    CHECK_EQ(newest.high_water(), 2u);
}

TEST_CASE("bounded queue: blocking push is lossless") {
    BoundedQueue<Frame, Id> queue(1, DropPolicy::DropOldest, &summarize);
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < 50; ++i) {
            Frame f;
            f.frame_id = i;
            queue.push_blocking(std::move(f));
        }
        queue.close();
    });
    std::uint64_t expected = 0;
    while (auto item = queue.pop(std::chrono::milliseconds(500))) CHECK_EQ(item->frame_id, expected++);
    producer.join();
    CHECK_EQ(expected, 50u);
    CHECK_EQ(queue.drops(), 0u);
}

TEST_CASE("y4m header: accepted and rejected forms") {
    const Y4mHeader h = parse_y4m_header("YUV4MPEG2 W64 H48 F30000:1001 Ip A1:1 C420mpeg2 XAHPV_SYNTHETIC=1 XCOLORRANGE=FULL");
    CHECK_EQ(h.geometry.width, 64);
    CHECK(h.geometry.format == PixelFormat::I420);
    CHECK(h.synthetic);
    CHECK_EQ(h.color_range, std::string("FULL"));
    CHECK(parse_y4m_header("YUV4MPEG2 W8 H8 F1:1 Cmono").geometry.format == PixelFormat::Gray8);
    for (const char* bad : {"YUV4MPEG W8 H8 F1:1", "YUV4MPEG2 H8 F1:1", "YUV4MPEG2 W8 H8", "YUV4MPEG2 W8 H8 F0:1",
                            "YUV4MPEG2 W8 H8 F1:1 C422", "YUV4MPEG2 W8 H8 F1:1 It", "YUV4MPEG2 W8 W8 H8 F1:1",
                            "YUV4MPEG2 W99999 H8 F1:1", "YUV4MPEG2 W8 H8 F1:1 Z3", "YUV4MPEG2 W-8 H8 F1:1"})
        CHECK_THROWS(parse_y4m_header(bad));
    CHECK_EQ(format_y4m_header({8, 4, PixelFormat::Gray8}, 25, 1, {"AHPV_SESSION=x"}),
             std::string("YUV4MPEG2 W8 H4 F25:1 Ip A1:1 Cmono XAHPV_SESSION=x\n"));
}

TEST_CASE("y4m source: file replay timestamps, end of stream and truncation") {
    const std::string dir = test::temp_dir("y4m");
    const std::string path = test::write_y4m(join_path(dir, "a.y4m"), 16, 8, 4, " XAHPV_SYNTHETIC=1");
    Y4mSource source(path, false, 1000, false);
    CHECK_EQ(source.info().content_origin, std::string("synthetic_fixture"));
    CHECK_EQ(source.info().timestamp_basis, std::string("file_nominal_frame_rate"));
    Frame f;
    for (int i = 0; i < 4; ++i) {
        CHECK(source.read(f).status == ReadStatus::Frame);
        CHECK_EQ(f.source_sequence, static_cast<std::uint64_t>(i));
        CHECK_EQ(f.source_timestamp_ns, static_cast<std::int64_t>(i) * 1000000000LL / 30);
    }
    CHECK(source.read(f).status == ReadStatus::EndOfStream);

    std::string text = read_file(path);
    test::write_text(join_path(dir, "t.y4m"), text.substr(0, text.size() - 50));
    Y4mSource truncated(join_path(dir, "t.y4m"), false, 1000, false);
    for (int i = 0; i < 3; ++i) CHECK(truncated.read(f).status == ReadStatus::Frame);
    const ReadResult r = truncated.read(f);
    CHECK(r.status == ReadStatus::Error);
    CHECK(r.message.find("truncated frame 3") != std::string::npos);
    CHECK(truncated.read(f).status == ReadStatus::Error);  // Stays failed.

    const auto marker = text.find("FRAME", text.find("FRAME") + 5);
    text[marker + 2] = 'X';
    test::write_text(join_path(dir, "m.y4m"), text);
    Y4mSource malformed(join_path(dir, "m.y4m"), false, 1000, false);
    CHECK(malformed.read(f).status == ReadStatus::Frame);
    CHECK(malformed.read(f).status == ReadStatus::Error);
    test::write_text(join_path(dir, "e.y4m"), "");
    CHECK_THROWS(Y4mSource(join_path(dir, "e.y4m"), false, 1000, false));
    CHECK_THROWS(Y4mSource(join_path(dir, "missing.y4m"), false, 1000, false));
}

TEST_CASE("fault injection: spec parsing and injected behaviour") {
    CHECK_THROWS(parse_fault_spec(""));
    CHECK_THROWS(parse_fault_spec("drop=x"));
    CHECK_THROWS(parse_fault_spec("explode=1"));
    CHECK_THROWS(parse_fault_spec("delay=3"));
    const std::string dir = test::temp_dir("fault");
    const std::string path = test::write_y4m(join_path(dir, "a.y4m"), 16, 8, 8);
    FaultInjectingSource source(std::make_unique<Y4mSource>(path, false, 100, false),
                                parse_fault_spec("drop=1;corrupt=3;disconnect=5;nosignal=6:1"), 100);
    Frame f;
    CHECK(source.read(f).status == ReadStatus::Frame);
    CHECK_EQ(f.source_sequence, 0u);
    CHECK(source.read(f).status == ReadStatus::Frame);
    CHECK_EQ(f.source_sequence, 2u);  // Frame 1 dropped upstream.
    CHECK(source.read(f).status == ReadStatus::Corrupted);
    CHECK(f.flags & kFlagFaultInjected);
    CHECK(source.read(f).status == ReadStatus::Frame);
    CHECK(source.read(f).status == ReadStatus::Disconnected);
    CHECK(source.read(f).status == ReadStatus::Disconnected);
    source.reopen();
    CHECK(source.read(f).status == ReadStatus::Frame);
    CHECK(source.read(f).status == ReadStatus::NoSignal);
    CHECK(source.read(f).status == ReadStatus::Frame);
    CHECK_EQ(source.info().details.at("fault_injection").as_string(), std::string("drop=1;corrupt=3;disconnect=5;nosignal=6:1"));
}

TEST_CASE("timing analyzer: configured decimation is not reported as loss") {
    SequenceTimingAnalyzer a(33333333, 1.5, 3);  // keep 1 of every 3 frames
    Frame f0 = frame_at(0, 0), f1 = frame_at(3, 100), f2 = frame_at(6, 200), f3 = frame_at(12, 400);
    a.observe(f0);
    a.observe(f1);
    a.observe(f2);
    a.observe(f3);
    CHECK_EQ(f1.flags, 0u);
    CHECK_EQ(f2.flags, 0u);
    CHECK(f3.flags & kFlagSequenceGap);  // One kept frame (sequence 9) went missing upstream.
    CHECK_EQ(f3.missing_before, 3u);
    CHECK(f3.flags & kFlagLate);
}
