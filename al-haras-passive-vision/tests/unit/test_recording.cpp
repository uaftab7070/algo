#include "test_support.hpp"

#include "capture/y4m_source.hpp"
#include "common/fsutil.hpp"
#include "common/version.hpp"
#include "recording/recorder.hpp"
#include "recording/session.hpp"
#include "recording/verify.hpp"

#include <fcntl.h>
#include <set>
#include <unistd.h>

using namespace ahpv;

namespace {

struct Fixture {
    std::string root;
    std::string id;
    std::string dir;
    SourceInfo info;
};

json::Value base_manifest(const std::string& id, const SourceInfo& info) {
    json::Value m;
    m.set("schema_version", kSchemaVersion);
    m.set("usage_notice", kUsageNotice);
    m.set("session_id", id);
    m.set("run_id", "run-test");
    m.set("segment_index", 0);
    m.set("created_at", "2026-10-24T00:00:00.000Z");
    m.set("source", info.to_json());
    m.set("configuration", json::Object{{"text_sha256", "test"}});
    m.set("provenance", json::Object{{"purpose", "unit test"}});
    return m;
}

// Records `frames` frames; finalize=false leaves the session as after a crash.
Fixture record(int frames, bool finalize, RecorderOptions options = {}) {
    Fixture fx;
    fx.root = test::temp_dir("rec");
    const std::string input = test::write_y4m(join_path(fx.root, "in.y4m"), 32, 16, frames);
    Y4mSource source(input, false, 1000, false);
    fx.info = source.info();
    SessionStore store(fx.root);
    fx.id = store.create();
    fx.dir = store.directory(fx.id);
    options.warn_free_bytes = 0;
    options.reserve_free_bytes = 0;
    Recorder recorder(fx.dir, base_manifest(fx.id, fx.info), fx.info, options);
    ContentAnalyzer analyzer(ValidationThresholds{});
    Frame f;
    std::uint64_t id = 0;
    std::uint64_t stored = 0;
    while (source.read(f).status == ReadStatus::Frame) {
        f.frame_id = id++;
        const QualityStats stats = analyzer.analyze(f, true);
        if (recorder.write_frame(f, stats, 1000)) ++stored;
    }
    if (finalize) recorder.finalize("complete", json::Object{{"frames_assigned", stored}, {"frames_stored", stored}});
    return fx;
}

std::string check_status(const json::Value& result, const std::string& name) {
    for (const auto& c : result.at("checks").as_array())
        if (c.at("name").as_string() == name) return c.at("status").as_string();
    return "absent";
}

}  // namespace

TEST_CASE("session store: IDs are unique, well-formed and validated") {
    SessionStore store(test::temp_dir("ids"));
    std::set<std::string> ids;
    for (int i = 0; i < 40; ++i) ids.insert(store.create());
    CHECK_EQ(ids.size(), 40u);
    for (const auto& id : ids) CHECK(valid_session_id(id));
    CHECK_EQ(store.list().size(), 40u);
    for (const char* bad : {"", "s-20261024T000000Z-ABCDEF12", "../etc", "s-20261024T000000Z-1234567", "s-2026102xT000000Z-12345678"})
        CHECK(!valid_session_id(bad));
    CHECK_THROWS(store.directory("../../etc"));
}

TEST_CASE("recorder + verify: clean session passes every check") {
    const Fixture fx = record(12, true);
    const json::Value result = verify_session(fx.dir);
    CHECK(result.at("ok").as_bool());
    CHECK_EQ(result.at("summary").at("frames_stored").as_int(), 12);
    CHECK_EQ(check_status(result, "frame_accounting"), std::string("pass"));
    CHECK_EQ(check_status(result, "payload_crc"), std::string("pass"));
    const SessionData data = load_session(fx.dir);
    CHECK_EQ(data.frames.size(), 12u);
    CHECK(data.index_errors.empty());
    const auto payload = read_frame_payload(data, data.frames[3]);
    CHECK_EQ(payload.size(), data.geometry.frame_bytes());
    CHECK_EQ(data.manifest.at("status").as_string(), std::string("complete"));
    CHECK(data.manifest.at("manifest_revision").as_int() >= 2);
}

TEST_CASE("verify: detects a modified payload byte and a modified index") {
    const Fixture fx = record(6, true);
    const SessionData data = load_session(fx.dir);
    const std::string video = join_path(fx.dir, "video.y4m");
    const int fd = ::open(video.c_str(), O_WRONLY);
    const char byte = 0x55;
    CHECK(::pwrite(fd, &byte, 1, static_cast<off_t>(data.frames[2].offset + 5)) == 1);
    ::close(fd);
    const json::Value result = verify_session(fx.dir);
    CHECK(!result.at("ok").as_bool());
    CHECK_EQ(check_status(result, "payload_crc"), std::string("fail"));
    CHECK_EQ(check_status(result, "checksums_match"), std::string("fail"));

    const Fixture fy = record(6, true);
    std::string index = read_file(join_path(fy.dir, "frames.jsonl"));
    index.erase(index.find('\n') + 1, index.find('\n', index.find('\n') + 1) - index.find('\n'));  // Remove record 1.
    test::write_text(join_path(fy.dir, "frames.jsonl"), index);
    const json::Value second = verify_session(fy.dir);
    CHECK_EQ(check_status(second, "frame_accounting"), std::string("fail"));
}

TEST_CASE("recovery: interrupted session is marked, preserved and verifiable") {
    const Fixture fx = record(9, false);
    const std::int64_t size_before = file_size(join_path(fx.dir, "video.y4m"));
    // Simulate an append cut by power loss.
    AppendFile(join_path(fx.dir, "frames.jsonl")).write("{\"frame_id\":9,\"sour");
    CHECK_EQ(load_session(fx.dir).manifest.at("status").as_string(), std::string("recording"));
    const json::Value before = verify_session(fx.dir);
    CHECK_EQ(check_status(before, "session_finalized"), std::string("fail"));
    const json::Value r = recover_session(fx.dir);
    CHECK(r.at("recovered").as_bool());
    CHECK_EQ(r.at("recovery").at("indexed_frames").as_int(), 9);
    CHECK_EQ(r.at("recovery").at("incomplete_index_lines").as_int(), 1);
    CHECK_EQ(file_size(join_path(fx.dir, "video.y4m")), size_before);  // Data untouched.
    const json::Value after = verify_session(fx.dir);
    CHECK(after.at("ok").as_bool());
    CHECK_EQ(check_status(after, "index_parse"), std::string("warn"));
    CHECK(!recover_session(fx.dir).at("recovered").as_bool());  // Idempotent.
}

TEST_CASE("storage guard: byte limit stops writes and session stays valid") {
    RecorderOptions options;
    options.max_session_bytes = 9000;  // ~10 frames of 32x16 I420 plus index.
    const Fixture fx = record(40, true, options);
    const SessionData data = load_session(fx.dir);
    CHECK(data.frames.size() > 0 && data.frames.size() < 40);
    CHECK_EQ(data.manifest.at("status").as_string(), std::string("stopped_storage"));
    bool stop_event = false;
    for (const auto& e : data.events) stop_event |= e.at("event").as_string() == "recording_stopped";
    CHECK(stop_event);
    CHECK(verify_session(fx.dir).at("ok").as_bool());
}

TEST_CASE("frame index: schema round trip and rejection") {
    FrameIndexEntry e;
    e.frame_id = 7;
    e.has_source_sequence = true;
    e.source_sequence = 9;
    e.acquisition_mono_ns = 123;
    e.interval_ns = 33;
    e.has_crc = true;
    e.crc = 0xdeadbeef;
    e.flags = kFlagLate | kFlagSequenceGap;
    const FrameIndexEntry back = parse_frame_index(frame_index_json(e));
    CHECK_EQ(back.frame_id, 7u);
    CHECK_EQ(back.crc, 0xdeadbeefu);
    CHECK_EQ(back.flags, e.flags);
    json::Value bad = frame_index_json(e);
    bad.set("crc32", "XYZ");
    CHECK_THROWS(parse_frame_index(bad));
    bad = frame_index_json(e);
    bad.set("flags", json::Value(json::Array{"made_up"}));
    CHECK_THROWS(parse_frame_index(bad));
}
