#include "test_support.hpp"

#include "annotations/annotations.hpp"
#include "capture/y4m_source.hpp"
#include "common/fsutil.hpp"
#include "common/version.hpp"
#include "metrics/report.hpp"
#include "recording/recorder.hpp"
#include "recording/verify.hpp"
#include "security/audit.hpp"
#include "security/auth.hpp"
#include "security/config.hpp"

#include <sys/stat.h>

using namespace ahpv;

namespace {

std::string config_error(const std::string& text) {
    try {
        parse_config(text, "<test>");
    } catch (const ConfigError& e) {
        return e.what();
    }
    return "";
}

SessionData recorded_session(int frames) {
    const std::string root = test::temp_dir("ann");
    const std::string input = test::write_y4m(join_path(root, "in.y4m"), 40, 30, frames);
    Y4mSource source(input, false, 1000, false);
    SessionStore store(root);
    const std::string id = store.create();
    json::Value m;
    m.set("schema_version", kSchemaVersion);
    m.set("usage_notice", kUsageNotice);
    m.set("session_id", id);
    m.set("source", source.info().to_json());
    m.set("configuration", json::Object{{"text_sha256", "t"}});
    RecorderOptions options;
    options.warn_free_bytes = options.reserve_free_bytes = 0;
    Recorder recorder(store.directory(id), m, source.info(), options);
    Frame f;
    std::uint64_t n = 0;
    while (source.read(f).status == ReadStatus::Frame) {
        f.frame_id = n++;
        recorder.write_frame(f, QualityStats{}, 0);
    }
    recorder.finalize("complete", json::Object{{"frames_assigned", n}});
    return load_session(store.directory(id));
}

const Principal kAnnotator{"ann-a", "annotator"}, kAnnotatorB{"ann-b", "annotator"}, kReviewer{"rev-a", "reviewer"},
    kViewer{"view-a", "viewer"};

int status_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const AnnotationError& e) {
        return e.http_status;
    }
    return 0;
}

}  // namespace

TEST_CASE("config: defaults, nesting and effective values") {
    const Config c = parse_config("data_root: /data\nsource:\n  kind: y4m_file\n  path: \"/x.y4m\"  # comment\n  v4l2:\n    buffers: 6\n"
                                  "preview:\n  enabled: true\n",
                                  "<t>");
    CHECK_EQ(c.data_root, std::string("/data"));
    CHECK_EQ(c.source.path, std::string("/x.y4m"));
    CHECK_EQ(c.source.buffers, 6);
    CHECK(c.preview_enabled);
    CHECK_EQ(c.preview_bind, std::string("127.0.0.1"));
    CHECK_EQ(c.auth_file, std::string("/data/auth.conf"));
    CHECK_EQ(c.to_json().at("pipeline").at("drop_policy").as_string(), std::string("drop_oldest"));
}

TEST_CASE("config: invalid input is rejected with the line number") {
    const std::string base = "source:\n  path: /x\n";
    CHECK(config_error(base + "bogus: 1\n").find("line 3: unknown configuration key 'bogus'") != std::string::npos);
    CHECK(config_error(base + "pipeline:\n  queue_capacity: 0\n").find("line 4") != std::string::npos);
    CHECK(config_error(base + "pipeline:\n  queue_capacity: many\n").find("non-negative integer") != std::string::npos);
    CHECK(config_error(base + "preview:\n  bind_address: 0.0.0.0\n").find("allow_non_loopback") != std::string::npos);
    CHECK(config_error(base + "preview:\n  bind_address: localhost\n").find("IPv4") != std::string::npos);
    CHECK(config_error(base + "recording:\n  enabled: yes\n").find("true or false") != std::string::npos);
    CHECK(config_error("source:\n\tpath: /x\n").find("tabs") != std::string::npos);
    CHECK(config_error("source:\n  - /x\n").find("lists") != std::string::npos);
    CHECK(config_error("source:\n").find("no values") != std::string::npos);
    CHECK(config_error("source:\n  kind: v4l2\n  path: /dev/video0\n").find("width") != std::string::npos);
    CHECK(config_error(base + "storage:\n  warn_free_bytes: 10\n  reserve_free_bytes: 20\n").find("reserve") != std::string::npos);
    // There is no key through which a control or actuator interface could be configured.
    for (const auto& key : known_config_keys())
        for (const char* forbidden : {"mavlink", "serial", "uart", "pwm", "sbus", "can", "gimbal", "actuator", "target", "flight"})
            CHECK(key.find(forbidden) == std::string::npos);
}

TEST_CASE("config set: canonical rewrite validates the result") {
    const std::string text = "# note\nsource:\n  path: /x\npreview:\n  port: 9000\n";
    const std::string updated = set_config_value(text, "preview.port", "9100");
    CHECK_EQ(parse_config(updated, "<t>").preview_port, 9100);
    CHECK_EQ(parse_config(updated, "<t>").source.path, std::string("/x"));
    CHECK_THROWS(set_config_value(text, "preview.port", "80"));
    CHECK_THROWS(set_config_value(text, "flight.mode", "auto"));
    CHECK_THROWS(set_config_value(text, "source.path", "a\"b"));
}

TEST_CASE("auth: tokens, roles and file permission checks") {
    const std::string dir = test::temp_dir("auth");
    const std::string path = join_path(dir, "auth.conf");
    const std::string op = AuthStore::add_principal(path, "op", "operator");
    const std::string an = AuthStore::add_principal(path, "an", "annotator");
    CHECK_THROWS(AuthStore::add_principal(path, "op", "viewer"));
    CHECK_THROWS(AuthStore::add_principal(path, "x", "admin"));
    CHECK_THROWS(AuthStore::add_principal(path, "../x", "viewer"));
    const std::string text = read_file(path);
    CHECK(text.find(op) == std::string::npos);  // Only digests are stored.
    const AuthStore store = AuthStore::load(path);
    CHECK_EQ(store.authenticate(op)->id, std::string("op"));
    CHECK_EQ(store.authenticate(an)->role, std::string("annotator"));
    CHECK(!store.authenticate(op + "x"));
    CHECK(!store.authenticate(""));
    CHECK(role_allows("operator", Permission::Configure));
    CHECK(!role_allows("annotator", Permission::Configure));
    CHECK(!role_allows("viewer", Permission::Annotate));
    CHECK(!role_allows("reviewer", Permission::Annotate));
    CHECK(role_allows("reviewer", Permission::Review));
    CHECK(!role_allows("nobody", Permission::View));
    ::chmod(path.c_str(), 0644);
    CHECK_THROWS(AuthStore::load(path));
}

TEST_CASE("audit log: hash chain detects edits and deletions") {
    const std::string path = join_path(test::temp_dir("audit"), "audit.jsonl");
    {
        AuditLog log(path);
        log.record("op", "config.set a", "cfg", "allowed", "");
        log.record("", "config.set b", "cfg", "denied", "invalid token");
    }
    AuditLog(path).record("op", "x", "y", "allowed", "");  // Reopening continues the chain.
    CHECK(verify_audit_chain(path).at("ok").as_bool());
    CHECK_EQ(verify_audit_chain(path).at("records").as_int(), 3);
    std::string text = read_file(path);
    text.replace(text.find("denied"), 6, "allowd");
    test::write_text(path, text);
    const json::Value result = verify_audit_chain(path);
    CHECK(!result.at("ok").as_bool());
    CHECK_EQ(result.at("first_bad_line").as_int(), 2);  // The edited record fails its own seal.
}

TEST_CASE("annotations: create, revise, review, delete with full history") {
    const SessionData session = recorded_session(5);
    AnnotationStore store(session, true);
    const json::Value created = store.create(kAnnotator, json::parse(R"({"frame_id":2,"region_xywh_pixels":[1,2,10,8],"notes":"bench item"})"));
    const std::string id = created.at("annotation_id").as_string();
    CHECK_EQ(created.at("annotator_id").as_string(), std::string("ann-a"));
    CHECK_EQ(created.at("source_timestamp_ns").as_int(), session.frames[2].source_timestamp_ns);
    CHECK_EQ(created.at("review_status").as_string(), std::string("unreviewed"));
    const json::Value revised = store.revise(kAnnotatorB, id, json::parse(R"({"expected_revision":1,"region_xywh_pixels":[0,0,40,30]})"));
    CHECK_EQ(revised.at("revision").as_int(), 2);
    CHECK_EQ(revised.at("annotator_id").as_string(), std::string("ann-a"));
    CHECK_EQ(revised.at("last_modified_by").as_string(), std::string("ann-b"));
    const json::Value reviewed = store.review(kReviewer, id, json::parse(R"({"expected_revision":2,"review_status":"accepted","comment":"ok"})"));
    CHECK_EQ(reviewed.at("reviewer_id").as_string(), std::string("rev-a"));
    store.remove(kAnnotator, id, json::parse(R"({"expected_revision":3,"reason":"duplicate"})"));
    CHECK(store.list(false).at("annotations").as_array().empty());
    const json::Value all = store.list(true);
    CHECK_EQ(all.at("annotations").as_array().size(), 1u);
    CHECK(all.at("chain_ok").as_bool());
    CHECK_EQ(store.history(id).at("events").as_array().size(), 4u);  // Originals preserved.
}

TEST_CASE("annotations: validation rejects bounds, unknown fields and conflicts") {
    const SessionData session = recorded_session(3);
    AnnotationStore store(session, true);
    const auto create = [&](const char* body) { return status_of([&] { store.create(kAnnotator, json::parse(body)); }); };
    CHECK_EQ(create(R"({"frame_id":0,"region_xywh_pixels":[35,0,6,5]})"), 400);  // x+w > 40
    CHECK_EQ(create(R"({"frame_id":0,"region_xywh_pixels":[-1,0,6,5]})"), 400);
    CHECK_EQ(create(R"({"frame_id":0,"region_xywh_pixels":[0,0,0,5]})"), 400);
    CHECK_EQ(create(R"({"frame_id":0,"region_xywh_pixels":[0,0,1.5,5]})"), 400);
    CHECK_EQ(create(R"({"frame_id":0,"region_xywh_pixels":[0,0,5]})"), 400);
    CHECK_EQ(create(R"({"frame_id":99,"region_xywh_pixels":[0,0,5,5]})"), 400);
    CHECK_EQ(create(R"({"frame_id":0,"region_xywh_pixels":[0,0,5,5],"steering_offset":[1,2]})"), 400);
    CHECK_EQ(create(R"({"frame_id":0,"region_xywh_pixels":[0,0,5,5],"aim_point":[1,2]})"), 400);
    CHECK_EQ(create(R"({"frame_id":0,"region_xywh_pixels":[0,0,5,5],"annotation_type":"target_lock"})"), 400);
    CHECK_EQ(create(R"({"frame_id":0,"region_xywh_pixels":[0,0,5,5],"notes":"bad\u0007bell"})"), 400);
    CHECK_EQ(status_of([&] { store.create(kViewer, json::parse(R"({"frame_id":0,"region_xywh_pixels":[0,0,5,5]})")); }), 403);
    CHECK_EQ(status_of([&] { store.create(kReviewer, json::parse(R"({"frame_id":0,"region_xywh_pixels":[0,0,5,5]})")); }), 403);
    const std::string id = store.create(kAnnotator, json::parse(R"({"frame_id":0,"region_xywh_pixels":[0,0,5,5]})")).at("annotation_id").as_string();
    CHECK_EQ(status_of([&] { store.revise(kAnnotator, id, json::parse(R"({"expected_revision":4,"notes":"x"})")); }), 409);
    CHECK_EQ(status_of([&] { store.review(kAnnotator, id, json::parse(R"({"expected_revision":1,"review_status":"accepted"})")); }), 403);
    CHECK_EQ(status_of([&] { store.review(kReviewer, id, json::parse(R"({"expected_revision":1,"review_status":"great"})")); }), 400);
    CHECK_EQ(status_of([&] { store.revise(kAnnotator, "a-missing", json::parse(R"({"expected_revision":1,"notes":"x"})")); }), 404);
    // Rejected requests leave existing annotations unchanged (AC-022).
    const auto list = store.list(true).at("annotations").as_array();
    CHECK_EQ(list.size(), 1u);
    CHECK_EQ(list[0].at("revision").as_int(), 1);
}

TEST_CASE("annotations: independent review and tamper-evident log") {
    const SessionData session = recorded_session(3);
    AnnotationStore strict(session, true);
    const std::string id = strict.create(kAnnotator, json::parse(R"({"frame_id":1,"region_xywh_pixels":[0,0,5,5]})")).at("annotation_id").as_string();
    const Principal self_review{"ann-a", "reviewer"};
    CHECK_EQ(status_of([&] { strict.review(self_review, id, json::parse(R"({"expected_revision":1,"review_status":"accepted"})")); }), 409);
    const std::string path = join_path(session.dir, "annotations.jsonl");
    std::string text = read_file(path);
    text.replace(text.find("[0,0,5,5]"), 9, "[0,0,6,5]");
    test::write_text(path, text);
    CHECK(!strict.list(true).at("chain_ok").as_bool());
    CHECK_EQ(status_of([&] { strict.create(kAnnotator, json::parse(R"({"frame_id":1,"region_xywh_pixels":[0,0,5,5]})")); }), 409);
}

TEST_CASE("report: CSV formula neutralisation and deterministic section") {
    CHECK_EQ(csv_field("=cmd()"), std::string("'=cmd()"));
    CHECK_EQ(csv_field("a,b"), std::string("\"a,b\""));
    CHECK_EQ(csv_field("say \"hi\""), std::string("\"say \"\"hi\"\"\""));
    CHECK_EQ(csv_field("-1"), std::string("'-1"));
    const SessionData a = recorded_session(6), b = recorded_session(6);
    const json::Value ra = build_report(a, verify_session(a.dir), AnnotationStore(a, true).list(true));
    const json::Value rb = build_report(b, verify_session(b.dir), AnnotationStore(b, true).list(true));
    CHECK(ra.at("deterministic") == rb.at("deterministic"));
    CHECK(ra.at("report_id") != rb.at("report_id"));
    CHECK_EQ(ra.at("usage_notice").as_string(), std::string(kUsageNotice));
    CHECK(summary_csv(ra).find("deterministic.frames.stored,6") != std::string::npos);
}
