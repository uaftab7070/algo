// ahpv: AL-HARAS passive vision command-line interface.
// Outputs are observation records only: NOT FOR FLIGHT OR ENGAGEMENT CONTROL.
#include "annotations/annotations.hpp"
#include "common/clock.hpp"
#include "common/fsutil.hpp"
#include "common/log.hpp"
#include "common/version.hpp"
#include "frames/pipeline.hpp"
#include "metrics/report.hpp"
#include "metrics/resources.hpp"
#include "platform/platform.hpp"
#include "preview/preview_service.hpp"
#include "recording/verify.hpp"
#include "security/audit.hpp"
#include "security/auth.hpp"
#include "security/config.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <thread>

using namespace ahpv;

namespace {

// Exit statuses (docs/operating-guide.md).
constexpr int kOk = 0, kCheckFailed = 1, kUsage = 2, kCameraLost = 3, kInternal = 5, kUnauthorized = 6;

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Unauthorized : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Args {
public:
    Args(int argc, char** argv, const std::set<std::string>& flags) {
        for (int i = 0; i < argc; ++i) {
            const std::string a = argv[i];
            if (a.rfind("--", 0) == 0) {
                const std::string key = a.substr(2);
                if (flags.count(key)) {
                    options_[key] = "true";
                } else {
                    if (i + 1 >= argc) throw UsageError("--" + key + " requires a value");
                    options_[key] = argv[++i];
                }
            } else {
                positional_.push_back(a);
            }
        }
    }
    std::string get(const std::string& key, const std::string& fallback = "") {
        used_.insert(key);
        const auto it = options_.find(key);
        return it == options_.end() ? fallback : it->second;
    }
    std::string require(const std::string& key) {
        const std::string value = get(key);
        if (value.empty()) throw UsageError("--" + key + " is required");
        return value;
    }
    bool flag(const std::string& key) { return get(key) == "true"; }
    std::size_t count() const { return positional_.size(); }
    std::string at(std::size_t i, const char* what) const {
        if (i >= positional_.size()) throw UsageError(std::string("missing argument: ") + what);
        return positional_[i];
    }
    void finish() const {
        // --token-file is consumed by authorize(), which may run after finish().
        for (const auto& [key, value] : options_)
            if (!used_.count(key) && key != "token-file") throw UsageError("unknown option --" + key);
    }

private:
    std::map<std::string, std::string> options_;
    std::vector<std::string> positional_;
    std::set<std::string> used_;
};

void print(const json::Value& value) { std::cout << json::dump(value) << std::endl; }

std::uint64_t parse_count(const std::string& text, const char* what) {
    if (text.empty() || text.size() > 12 || text.find_first_not_of("0123456789") != std::string::npos)
        throw UsageError(std::string(what) + " must be a non-negative integer");
    return std::stoull(text);
}

Config configure(Args& args) {
    Config config = load_config(args.require("config"));
    Logger::instance().set_level(config.log_level);
    if (!config.log_file.empty()) Logger::instance().set_file(config.log_file);
    return config;
}

// Authenticates the token and checks the permission; every outcome is audited.
Principal authorize(const Config& config, AuditLog& audit, Args& args, Permission permission, const std::string& action) {
    AuthStore store;
    try {
        store = AuthStore::load(config.auth_file);
    } catch (const std::exception& e) {
        audit.record("", action, config.origin, "denied", e.what());
        throw Unauthorized(e.what());
    }
    const auto principal = store.authenticate(read_token(args.get("token-file")));
    if (!principal) {
        audit.record("", action, config.origin, "denied", "missing or invalid token");
        throw Unauthorized("a valid token is required (--token-file FILE or AHPV_TOKEN)");
    }
    if (!role_allows(principal->role, permission)) {
        audit.record(principal->id, action, config.origin, "denied", std::string("role '") + principal->role + "' lacks " + permission_name(permission));
        throw Unauthorized("principal '" + principal->id + "' (role " + principal->role + ") may not " + permission_name(permission));
    }
    audit.record(principal->id, action, config.origin, "allowed", "");
    return *principal;
}

json::Value recover_all(SessionStore& store) {
    json::Array results;
    for (const auto& id : store.list()) {
        try {
            const json::Value manifest = json::parse(read_file(join_path(store.directory(id), "session.json")));
            if (manifest.at("status").as_string() != "recording") continue;
            json::Value r = recover_session(store.directory(id));
            log_warn("REC-040", "interrupted session recovered", r);
            results.push_back(std::move(r));
        } catch (const std::exception& e) {
            log_error("REC-041", std::string("session could not be inspected for recovery: ") + e.what(), json::Object{{"session_id", id}});
            results.push_back(json::Object{{"session_id", id}, {"recovered", false}, {"error", e.what()}});
        }
    }
    return json::Value(std::move(results));
}

int cmd_run(Args& args) {
    Config config = configure(args);
    PipelineOptions options;
    options.provenance.purpose = args.get("purpose");
    options.provenance.partition = args.get("partition");
    options.provenance.owner = args.get("owner", "unspecified");
    options.provenance.permission_record = args.get("permission-record", "unspecified");
    options.provenance.place_category = args.get("place-category", "unspecified");
    options.provenance.scene_label = args.get("scene-label", "unspecified");
    options.fault_spec = args.get("fault-injection");
    options.max_frames = parse_count(args.get("max-frames", "0"), "--max-frames");
    options.duration_s = static_cast<double>(parse_count(args.get("duration-s", "0"), "--duration-s"));
    options.inventory_path = args.get("inventory");
    const bool exit_on_end = args.flag("exit-on-end");
    args.finish();
    if (config.recording_enabled) options.provenance.validate();
    make_directories(config.data_root);
    AuditLog audit(config.audit_log);
    const Principal operator_principal = authorize(config, audit, args, Permission::Record, "capture.run");
    if (config.recording_enabled) {
        SessionStore store(config.data_root);
        const json::Value recovered = recover_all(store);
        if (!recovered.as_array().empty()) print(json::Object{{"startup_recovery", recovered}});
    }
    options.config = config;
    Pipeline pipeline(options);
    try {
        pipeline.open();
    } catch (const std::exception& e) {
        std::cerr << "error: camera/source initialization failed: " << e.what() << "\n";
        print(json::Object{{"usage_notice", kUsageNotice}, {"state", "initialization_failed"}, {"error", e.what()}});
        return kCameraLost;
    }
    std::unique_ptr<PreviewService> service;
    std::unique_ptr<HttpServer> server;
    if (config.preview_enabled) {
        service = std::make_unique<PreviewService>(pipeline.config(), AuthStore::load(config.auth_file), audit, &pipeline);
        server = std::make_unique<HttpServer>(config.preview_bind, config.preview_port, [&](const HttpRequest& r) { return service->handle(r); });
        server->start();
        service->set_port(server->port());
    }
    // APP-001 is the readiness marker used by tests/hardware/boot_cycle_check.sh.
    std::string boot_id;
    try {
        boot_id = read_file("/proc/sys/kernel/random/boot_id", 128);
        while (!boot_id.empty() && boot_id.back() == '\n') boot_id.pop_back();
    } catch (const std::exception&) {
    }
    log_info("APP-001", "capture started", json::Object{{"operator", operator_principal.id},
                                                        {"boot_id", boot_id.empty() ? json::Value(nullptr) : json::Value(boot_id)}});
    const int status = pipeline.run(g_stop);
    print(json::Object{{"usage_notice", kUsageNotice}, {"final_state", service_state_name(pipeline.state())},
                       {"exit_status", status}, {"sessions", [&] {
                           json::Array ids;
                           for (const auto& id : pipeline.session_ids()) ids.emplace_back(id);
                           return json::Value(std::move(ids));
                       }()}});
    if (server && !exit_on_end && !g_stop) {
        // Remain in a read-only state so the operator can see why capture ended.
        log_info("APP-002", "capture ended; preview remains available read-only until stopped");
        while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (server) server->stop();
    return status;
}

int cmd_serve(Args& args) {
    Config config = configure(args);
    args.finish();
    AuditLog audit(config.audit_log);
    PreviewService service(config, AuthStore::load(config.auth_file), audit, nullptr);
    HttpServer server(config.preview_bind, config.preview_port, [&](const HttpRequest& r) { return service.handle(r); });
    server.start();
    service.set_port(server.port());
    print(json::Object{{"usage_notice", kUsageNotice}, {"mode", "review_only"},
                       {"url", "http://" + config.preview_bind + ":" + std::to_string(server.port()) + "/"}});
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    server.stop();
    return kOk;
}

std::string session_dir(const Config& config, const std::string& id) { return SessionStore(config.data_root).directory(id); }

int cmd_annotate(Args& args) {
    const std::string action = args.at(1, "annotate action (list|add|revise|delete|review|history)");
    Config config = configure(args);
    const std::string id = args.at(2, "session ID");
    const SessionData data = load_session(session_dir(config, id));
    AnnotationStore store(data, config.require_independent_review);
    if (action == "list" || action == "history") {
        const bool deleted = args.flag("include-deleted");
        const std::string annotation = action == "history" ? args.require("id") : "";
        args.finish();
        print(action == "list" ? store.list(deleted) : store.history(annotation));
        return kOk;
    }
    AuditLog audit(config.audit_log);
    const Permission needed = action == "review" ? Permission::Review : Permission::Annotate;
    json::Value body;
    const auto region = [&](const std::string& text) {
        json::Array values;
        std::size_t start = 0;
        for (int i = 0; i < 4; ++i) {
            const auto comma = text.find(',', start);
            const std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (part.empty() || part.find_first_not_of("0123456789") != std::string::npos || part.size() > 6)
                throw UsageError("--region must be x,y,width,height (non-negative integers)");
            values.emplace_back(static_cast<std::int64_t>(std::stoll(part)));
            if (comma == std::string::npos) {
                if (i != 3) throw UsageError("--region must have four values");
                break;
            }
            start = comma + 1;
            if (i == 3) throw UsageError("--region must have four values");
        }
        return json::Value(std::move(values));
    };
    std::string annotation_id;
    if (action == "add") {
        body.set("frame_id", static_cast<std::int64_t>(parse_count(args.require("frame"), "--frame")));
        body.set("region_xywh_pixels", region(args.require("region")));
        if (!args.get("notes").empty()) body.set("notes", args.get("notes"));
    } else if (action == "revise" || action == "delete" || action == "review") {
        annotation_id = args.require("id");
        body.set("expected_revision", static_cast<std::int64_t>(parse_count(args.require("expected-revision"), "--expected-revision")));
        if (action == "revise") {
            if (!args.get("region").empty()) body.set("region_xywh_pixels", region(args.get("region")));
            if (!args.get("notes").empty()) body.set("notes", args.get("notes"));
        } else if (action == "delete") {
            body.set("reason", args.get("reason"));
        } else {
            body.set("review_status", args.require("status"));
            if (!args.get("comment").empty()) body.set("comment", args.get("comment"));
        }
    } else {
        throw UsageError("unknown annotate action '" + action + "'");
    }
    const Principal principal = authorize(config, audit, args, needed, "annotation." + action);
    args.finish();
    const std::string resource = "session/" + id + "/annotations" + (annotation_id.empty() ? "" : "/" + annotation_id);
    try {
        json::Value record = action == "add" ? store.create(principal, body)
                             : action == "revise" ? store.revise(principal, annotation_id, body)
                             : action == "delete" ? store.remove(principal, annotation_id, body)
                                                  : store.review(principal, annotation_id, body);
        audit.record(principal.id, "annotation." + action, resource, "allowed", "", json::Object{{"revision", record.at("revision")}});
        print(record);
        return kOk;
    } catch (const AnnotationError& e) {
        audit.record(principal.id, "annotation." + action, resource, "failed", e.what());
        std::cerr << "error: " << e.what() << "\n";
        print(json::Object{{"error", e.code}, {"message", e.what()}});
        return kCheckFailed;
    }
}

int cmd_diag(Args& args) {
    Config config = configure(args);
    args.finish();
    json::Value d;
    d.set("usage_notice", kUsageNotice);
    d.set("software", json::Object{{"name", kSoftwareName}, {"version", kVersion}, {"build_id", kBuildId}, {"executable_sha256", executable_sha256()}});
    d.set("platform", platform_summary());
    d.set("configuration", config.to_json());
    ResourceSampler sampler(config.data_root);
    d.set("resources", sampler.sample());
    json::Value sessions = json::Object{};
    if (is_directory(join_path(config.data_root, "sessions"))) {
        SessionStore store(config.data_root);
        std::map<std::string, std::int64_t> by_status;
        for (const auto& id : store.list()) {
            try {
                ++by_status[json::parse(read_file(join_path(store.directory(id), "session.json"))).at("status").as_string()];
            } catch (const std::exception&) {
                ++by_status["unreadable"];
            }
        }
        for (const auto& [status, count] : by_status) sessions.set(status, count);
    }
    d.set("sessions_by_status", sessions);
    d.set("declared_listeners", config.preview_enabled ? json::Value(json::Array{json::Object{
        {"service", "preview"}, {"protocol", "tcp"}, {"bind_address", config.preview_bind}, {"port", config.preview_port}}})
                                                       : json::Value(json::Array{}));
    d.set("observed_listening_sockets", listening_sockets());
    json::Value auth;
    try {
        AuthStore::load(config.auth_file);
        auth.set("status", "ok");
    } catch (const std::exception& e) {
        auth.set("status", "error");
        auth.set("message", e.what());
    }
    d.set("auth_file", auth);
    try {
        d.set("audit_chain", path_exists(config.audit_log) ? verify_audit_chain(config.audit_log) : json::Value(json::Object{{"ok", true}, {"records", 0}}));
    } catch (const std::exception& e) {
        d.set("audit_chain", json::Object{{"ok", false}, {"error", e.what()}});
    }
    d.set("control_interfaces", "none: this build contains no vehicle, serial, MAVLink, PWM, SBUS, CAN or actuator output");
    print(d);
    return kOk;
}

void usage() {
    std::cerr <<
        "ahpv " << kVersion << " — AL-HARAS passive vision (NOT FOR FLIGHT OR ENGAGEMENT CONTROL)\n\n"
        "  ahpv version\n"
        "  ahpv inventory [--out FILE]\n"
        "  ahpv config check --config FILE\n"
        "  ahpv config set --config FILE --token-file T KEY VALUE\n"
        "  ahpv auth add --config FILE --principal ID --role viewer|annotator|reviewer|operator\n"
        "  ahpv run --config FILE --token-file T --purpose TEXT --partition development|acceptance\n"
        "           [--owner --permission-record --place-category --scene-label TEXT]\n"
        "           [--max-frames N] [--duration-s N] [--fault-injection SPEC] [--inventory FILE] [--exit-on-end]\n"
        "  ahpv serve --config FILE\n"
        "  ahpv sessions --config FILE\n"
        "  ahpv verify --config FILE SESSION [--no-crc]\n"
        "  ahpv recover --config FILE --token-file T [SESSION]\n"
        "  ahpv report --config FILE SESSION [--contact-sheet N]\n"
        "  ahpv still --config FILE SESSION FRAME_ID --out FILE.bmp\n"
        "  ahpv contact-sheet --config FILE SESSION --out DIR [--count N]\n"
        "  ahpv annotate list|history|add|revise|delete|review --config FILE SESSION [options]\n"
        "  ahpv diag --config FILE\n"
        "  ahpv audit-verify --config FILE\n";
}

int dispatch(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return kUsage;
    }
    const std::string command = argv[1];
    Args args(argc - 1, argv + 1, {"exit-on-end", "no-crc", "include-deleted"});
    if (command == "version") {
        print(json::Object{{"name", kSoftwareName}, {"version", kVersion}, {"build_id", kBuildId},
                           {"executable_sha256", executable_sha256()}, {"usage_notice", kUsageNotice}});
        return kOk;
    }
    if (command == "inventory") {
        const std::string out = args.get("out");
        args.finish();
        const json::Value inventory = platform_inventory();
        if (out.empty()) print(inventory);
        else write_file_atomic(out, json::dump(inventory) + "\n", 0644);
        return kOk;
    }
    if (command == "config") {
        const std::string action = args.at(1, "config action (check|set)");
        if (action == "check") {
            // Validation only: no log file, data directory or audit log is touched.
            const Config config = load_config(args.require("config"));
            args.finish();
            print(json::Object{{"valid", true}, {"effective", config.to_json()}});
            return kOk;
        }
        if (action == "set") {
            const std::string path = args.require("config");
            const std::string key = args.at(2, "KEY"), value = args.at(3, "VALUE");
            const Config current = load_config(path);
            AuditLog audit(current.audit_log);
            const Principal principal = authorize(current, audit, args, Permission::Configure, "config.set " + key);
            args.finish();
            const std::string old_text = read_file(path);
            std::string new_text;
            try {
                new_text = set_config_value(old_text, key, value);
            } catch (const ConfigError& e) {
                audit.record(principal.id, "config.set " + key, path, "failed", e.what());
                throw;
            }
            const std::string backup = path + ".bak-" + compact_utc(wall_ns());
            write_file_atomic(backup, old_text, 0640);  // Reversible change record (DAT-004).
            write_file_atomic(path, new_text, 0640);
            audit.record(principal.id, "config.set " + key, path, "allowed", "applied",
                         json::Object{{"key", key}, {"value", value}, {"previous_version", backup}});
            print(json::Object{{"updated", true}, {"key", key}, {"previous_version", backup},
                               {"note", "takes effect when the service is next started"}});
            return kOk;
        }
        throw UsageError("unknown config action '" + action + "'");
    }
    if (command == "auth") {
        if (args.at(1, "auth action (add)") != "add") throw UsageError("unknown auth action");
        const Config config = configure(args);
        const std::string principal = args.require("principal"), role = args.require("role");
        args.finish();
        const std::string token = AuthStore::add_principal(config.auth_file, principal, role);
        AuditLog(config.audit_log).record("local-administrator", "auth.add " + principal, config.auth_file, "allowed", "role " + role);
        print(json::Object{{"principal", principal}, {"role", role}, {"token", token},
                           {"note", "Shown once. Store it in a file readable only by the intended user (chmod 600)."}});
        return kOk;
    }
    if (command == "run") return cmd_run(args);
    if (command == "serve") return cmd_serve(args);
    if (command == "sessions") {
        const Config config = configure(args);
        args.finish();
        json::Array items;
        SessionStore store(config.data_root);
        for (const auto& id : store.list()) {
            json::Value item;
            item.set("session_id", id);
            try {
                const json::Value m = json::parse(read_file(join_path(store.directory(id), "session.json")));
                item.set("status", m.at("status"));
                item.set("created_at", m.at("created_at"));
                item.set("run_id", m.at("run_id"));
                item.set("segment_index", m.at("segment_index"));
            } catch (const std::exception& e) {
                item.set("status", "unreadable");
                item.set("error", e.what());
            }
            items.push_back(std::move(item));
        }
        print(json::Object{{"sessions", json::Value(std::move(items))}});
        return kOk;
    }
    if (command == "verify") {
        const Config config = configure(args);
        VerifyOptions options;
        options.check_payload_crc = !args.flag("no-crc");
        const std::string id = args.at(1, "session ID");
        args.finish();
        const json::Value result = verify_session(session_dir(config, id), options);
        print(result);
        return result.at("ok").as_bool() ? kOk : kCheckFailed;
    }
    if (command == "recover") {
        const Config config = configure(args);
        AuditLog audit(config.audit_log);
        authorize(config, audit, args, Permission::Record, "session.recover");
        const std::string id = args.count() > 1 ? args.at(1, "session ID") : "";
        args.finish();
        SessionStore store(config.data_root);
        print(id.empty() ? json::Object{{"recovered", recover_all(store)}} : recover_session(store.directory(id)));
        return kOk;
    }
    if (command == "report") {
        const Config config = configure(args);
        const std::string id = args.at(1, "session ID");
        const int sheet = static_cast<int>(parse_count(args.get("contact-sheet", "9"), "--contact-sheet"));
        args.finish();
        const SessionData data = load_session(session_dir(config, id));
        const json::Value verification = verify_session(data.dir);
        const json::Value annotations = AnnotationStore(data, config.require_independent_review).list(true);
        const json::Value report = build_report(data, verification, annotations);
        print(write_report_bundle(data, report, annotations, sheet));
        return kOk;
    }
    if (command == "still") {
        const Config config = configure(args);
        const std::string id = args.at(1, "session ID");
        const std::uint64_t frame = parse_count(args.at(2, "frame ID"), "frame ID");
        const std::string out = args.require("out");
        args.finish();
        print(export_still(load_session(session_dir(config, id)), frame, out, 4096));
        return kOk;
    }
    if (command == "contact-sheet") {
        const Config config = configure(args);
        const std::string id = args.at(1, "session ID");
        const std::string out = args.require("out");
        const int count = static_cast<int>(parse_count(args.get("count", "9"), "--count"));
        args.finish();
        print(export_contact_sheet(load_session(session_dir(config, id)), out, count, 320));
        return kOk;
    }
    if (command == "annotate") return cmd_annotate(args);
    if (command == "diag") return cmd_diag(args);
    if (command == "audit-verify") {
        const Config config = configure(args);
        args.finish();
        const json::Value result = verify_audit_chain(config.audit_log);
        print(result);
        return result.at("ok").as_bool() ? kOk : kCheckFailed;
    }
    usage();
    return kUsage;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    try {
        return dispatch(argc, argv);
    } catch (const UsageError& e) {
        std::cerr << "usage error: " << e.what() << "\n";
        return kUsage;
    } catch (const ConfigError& e) {
        std::cerr << "configuration error: " << e.what() << "\n";
        log_error("CFG-001", e.what());
        return kUsage;
    } catch (const Unauthorized& e) {
        std::cerr << "unauthorized: " << e.what() << "\n";
        return kUnauthorized;
    } catch (const std::invalid_argument& e) {
        std::cerr << "error: " << e.what() << "\n";
        return kUsage;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        log_error("APP-090", e.what());
        return kInternal;
    }
}
