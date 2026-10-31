#include "preview/preview_service.hpp"

#include "annotations/annotations.hpp"
#include "common/clock.hpp"
#include "common/fsutil.hpp"
#include "common/hash.hpp"
#include "common/log.hpp"
#include "common/version.hpp"
#include "metrics/images.hpp"
#include "metrics/report.hpp"
#include "preview/ui_assets.hpp"

#include <sstream>

namespace ahpv {
namespace {

HttpResponse json_response(int status, const json::Value& body) {
    HttpResponse r;
    r.status = status;
    r.body = json::dump(body);
    return r;
}

HttpResponse error(int status, const std::string& code, const std::string& message) {
    return json_response(status, json::Object{{"error", code}, {"message", message}, {"usage_notice", kUsageNotice}});
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::istringstream in(path);
    std::string part;
    while (std::getline(in, part, '/'))
        if (!part.empty()) parts.push_back(part);
    return parts;
}

bool parse_u64(const std::string& text, std::uint64_t& out) {
    if (text.empty() || text.size() > 19 || text.find_first_not_of("0123456789") != std::string::npos) return false;
    out = std::stoull(text);
    return true;
}

}  // namespace

PreviewService::PreviewService(const Config& config, AuthStore auth, AuditLog& audit, const Pipeline* live)
    : config_(config), auth_(std::move(auth)), audit_(audit), live_(live), store_(config.data_root), port_(config.preview_port) {}

std::shared_ptr<SessionData> PreviewService::session(const std::string& id) {
    const std::string dir = store_.directory(id);  // Validates the ID format.
    if (!is_directory(dir)) return nullptr;
    // Cache key changes whenever the manifest or index grows.
    const std::string key = std::to_string(file_size(join_path(dir, "session.json"))) + ":" +
                            std::to_string(file_size(join_path(dir, "frames.jsonl"))) + ":" +
                            std::to_string(file_size(join_path(dir, "events.jsonl")));
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_.find(id);
    if (it != cache_.end() && it->second.first == key) return it->second.second;
    auto data = std::make_shared<SessionData>(load_session(dir));
    if (cache_.size() > 8) cache_.clear();
    cache_[id] = {key, data};
    return data;
}

HttpResponse PreviewService::handle(const HttpRequest& request) {
    // DNS-rebinding defence: only the literal listener address or localhost.
    const std::string host = request.header("host");
    const std::string port = std::to_string(port_);
    if (host != config_.preview_bind + ":" + port && host != "localhost:" + port)
        return error(421, "host_not_allowed", "requests must address " + config_.preview_bind + ":" + port);
    const std::string origin = request.header("origin");
    if (!origin.empty() && origin != "http://" + host) return error(403, "origin_not_allowed", "cross-origin requests are refused");

    if (request.method == "GET" && (request.path == "/" || request.path == "/index.html")) {
        HttpResponse r;
        r.content_type = "text/html; charset=utf-8";
        r.body = ui_index_html();
        return r;
    }
    if (request.method == "GET" && request.path == "/app.js") {
        HttpResponse r;
        r.content_type = "text/javascript; charset=utf-8";
        r.body = ui_app_js();
        return r;
    }
    if (request.method == "GET" && request.path == "/app.css") {
        HttpResponse r;
        r.content_type = "text/css; charset=utf-8";
        r.body = ui_app_css();
        return r;
    }
    if (request.method == "GET" && request.path == "/favicon.ico") {
        HttpResponse r;
        r.status = 204;
        r.content_type = "image/x-icon";
        return r;
    }
    if (request.method == "GET" && request.path == "/api/health")
        return json_response(200, json::Object{{"service", kSoftwareName}, {"version", kVersion}, {"usage_notice", kUsageNotice}});
    if (request.path.rfind("/api/", 0) != 0) return error(404, "not_found", "no such resource");

    const std::string authorization = request.header("authorization");
    const std::string token = authorization.rfind("Bearer ", 0) == 0 ? authorization.substr(7) : std::string();
    const auto principal = auth_.authenticate(token);
    if (!principal) {
        audit_.record("", request.method + " " + request.path, request.path, "denied", token.empty() ? "missing token" : "invalid token");
        return error(401, "unauthorized", "a valid bearer token is required");
    }
    try {
        return route(request, *principal);
    } catch (const AnnotationError& e) {
        return error(e.http_status, e.code, e.what());
    } catch (const std::invalid_argument& e) {
        return error(400, "bad_request", e.what());
    } catch (const std::exception& e) {
        log_error("PRV-091", std::string("request failed: ") + e.what(), json::Object{{"path", request.path}});
        return error(500, "internal_error", e.what());
    }
}

HttpResponse PreviewService::live_frame() const {
    if (!live_) return error(404, "no_live_source", "this service is in review-only mode");
    const auto frame = live_->latest_frame();
    const ServiceState state = live_->state();
    const std::int64_t stale_ns = static_cast<std::int64_t>(config_.source.no_signal_timeout_ms) * 1000000;
    // Never present an old frame as current (SEC-007, AC-003).
    if (!frame || state == ServiceState::CameraLost || state == ServiceState::SourceError || state == ServiceState::NoSignal ||
        mono_ns() - frame->acquisition_mono_ns > stale_ns)
        return json_response(503, json::Object{{"error", "no_current_frame"}, {"state", service_state_name(state)},
                                               {"message", "no frame newer than " + std::to_string(config_.source.no_signal_timeout_ms) + " ms"},
                                               {"usage_notice", kUsageNotice}});
    const RgbImage image = frame_to_rgb(frame->geometry, frame->data.data(), config_.preview_max_width, live_->source_full_range());
    HttpResponse r;
    r.content_type = "image/bmp";
    r.body = encode_bmp(image);
    r.headers = {{"X-AHPV-Frame-Id", std::to_string(frame->frame_id)},
                 {"X-AHPV-Acquisition-Mono-Ns", std::to_string(frame->acquisition_mono_ns)},
                 {"X-AHPV-Acquisition-Wall", iso8601_utc(frame->acquisition_wall_ns)},
                 {"X-AHPV-Age-Ms", std::to_string((mono_ns() - frame->acquisition_mono_ns) / 1000000)},
                 {"X-AHPV-Flags", json::dump(flag_names(frame->flags))},
                 {"X-AHPV-Transform", image.transform},
                 {"X-AHPV-Usage-Notice", kUsageNotice}};
    return r;
}

HttpResponse PreviewService::session_frame(const std::string& id, const std::string& frame_part) {
    std::uint64_t frame_id = 0;
    if (frame_part.size() < 5 || frame_part.substr(frame_part.size() - 4) != ".bmp" ||
        !parse_u64(frame_part.substr(0, frame_part.size() - 4), frame_id))
        return error(404, "not_found", "frame images are /frames/<frame_id>.bmp");
    const auto data = session(id);
    if (!data) return error(404, "not_found", "unknown session");
    const FrameIndexEntry* entry = find_frame(*data, frame_id);
    if (!entry) return error(404, "frame_not_stored", "frame " + std::to_string(frame_id) + " is not stored (dropped, rejected or out of range)");
    const auto payload = read_frame_payload(*data, *entry);
    if (entry->has_crc && crc32(payload.data(), payload.size()) != entry->crc)
        return error(409, "frame_integrity", "frame " + std::to_string(frame_id) + " fails its CRC-32 check and is not displayed");
    const RgbImage image = frame_to_rgb(data->geometry, payload.data(), config_.preview_max_width, session_full_range(*data));
    HttpResponse r;
    r.content_type = "image/bmp";
    r.body = encode_bmp(image);
    r.headers = {{"X-AHPV-Frame-Id", std::to_string(frame_id)},
                 {"X-AHPV-Source-Timestamp-Ns", std::to_string(entry->source_timestamp_ns)},
                 {"X-AHPV-Transform", image.transform},
                 {"X-AHPV-Display-Scale", std::to_string(static_cast<double>(image.width) / data->geometry.width)},
                 {"X-AHPV-Usage-Notice", kUsageNotice}};
    return r;
}

HttpResponse PreviewService::annotation_write(const HttpRequest& request, const Principal& principal, const std::string& id,
                                              const std::string& annotation_id, const std::string& action) {
    const std::string resource = "session/" + id + "/annotations" + (annotation_id.empty() ? "" : "/" + annotation_id);
    const Permission needed = action == "review" ? Permission::Review : Permission::Annotate;
    if (!role_allows(principal.role, needed)) {
        audit_.record(principal.id, "annotation." + action, resource, "denied", std::string("role lacks ") + permission_name(needed));
        return error(403, "forbidden", "role '" + principal.role + "' cannot " + action + " annotations");
    }
    json::Value body;
    try {
        body = json::parse(request.body);
    } catch (const json::ParseError& e) {
        audit_.record(principal.id, "annotation." + action, resource, "failed", e.what());
        return error(400, "invalid_json", e.what());
    }
    const auto data = session(id);
    if (!data) return error(404, "not_found", "unknown session");
    AnnotationStore store(*data, config_.require_independent_review);
    try {
        json::Value record;
        if (action == "create") record = store.create(principal, body);
        else if (action == "revise") record = store.revise(principal, annotation_id, body);
        else if (action == "delete") record = store.remove(principal, annotation_id, body);
        else record = store.review(principal, annotation_id, body);
        audit_.record(principal.id, "annotation." + action, resource + (annotation_id.empty() ? "/" + record.at("annotation_id").as_string() : ""),
                      "allowed", "", json::Object{{"revision", record.at("revision")}});
        return json_response(action == "create" ? 201 : 200, record);
    } catch (const AnnotationError& e) {
        audit_.record(principal.id, "annotation." + action, resource, e.http_status == 403 ? "denied" : "failed", e.what());
        throw;
    }
}

HttpResponse PreviewService::route(const HttpRequest& request, const Principal& principal) {
    const auto parts = split_path(request.path);  // parts[0] == "api"
    const std::string& m = request.method;
    if (parts.size() >= 2 && (parts[1] == "config" || parts[1] == "control" || parts[1] == "capture")) {
        // Explicitly refused and audited: configuration is changed only via the CLI.
        audit_.record(principal.id, m + " " + request.path, request.path, "denied", "no configuration or control interface over HTTP");
        return error(403, "not_available", "configuration and capture control are not available over HTTP");
    }
    if (m == "GET" && parts.size() == 2 && parts[1] == "whoami") {
        json::Array permissions;
        for (Permission p : {Permission::View, Permission::Annotate, Permission::Review, Permission::Record, Permission::Configure})
            if (role_allows(principal.role, p)) permissions.emplace_back(permission_name(p));
        return json_response(200, json::Object{{"principal", principal.id}, {"role", principal.role},
                                               {"permissions", json::Value(std::move(permissions))}, {"usage_notice", kUsageNotice}});
    }
    if (m == "GET" && parts.size() == 2 && parts[1] == "status") {
        json::Value s = live_ ? live_->status() : json::Value(json::Object{{"usage_notice", kUsageNotice}, {"state", "review_only"}});
        s.set("mode", live_ ? "live" : "review_only");
        return json_response(200, s);
    }
    if (m == "GET" && parts.size() == 3 && parts[1] == "live" && parts[2] == "frame.bmp") return live_frame();
    if (parts.size() >= 2 && parts[1] == "sessions") {
        if (m == "GET" && parts.size() == 2) {
            json::Array items;
            for (const auto& id : store_.list()) {
                json::Value item;
                item.set("session_id", id);
                try {
                    const json::Value manifest = json::parse(read_file(join_path(store_.directory(id), "session.json")));
                    item.set("status", manifest.at("status"));
                    item.set("created_at", manifest.at("created_at"));
                    item.set("run_id", manifest.at("run_id"));
                    item.set("segment_index", manifest.at("segment_index"));
                    const json::Value* stats = manifest.find("statistics");
                    item.set("frames_stored", stats && stats->find("frames_stored") ? stats->at("frames_stored") : json::Value(nullptr));
                } catch (const std::exception& e) {
                    item.set("status", "unreadable");
                    item.set("error", e.what());
                }
                items.push_back(std::move(item));
            }
            return json_response(200, json::Object{{"sessions", json::Value(std::move(items))}, {"usage_notice", kUsageNotice}});
        }
        if (parts.size() < 3) return error(404, "not_found", "no such resource");
        const std::string& id = parts[2];
        if (!valid_session_id(id)) return error(400, "invalid_session_id", "invalid session ID");
        if (m == "GET" && parts.size() == 3) {
            const auto data = session(id);
            if (!data) return error(404, "not_found", "unknown session");
            return json_response(200, data->manifest);
        }
        if (m == "GET" && parts.size() == 4 && parts[3] == "frames") {
            const auto data = session(id);
            if (!data) return error(404, "not_found", "unknown session");
            std::uint64_t offset = 0, limit = 1000;
            if (!request.query_param("offset").empty() && !parse_u64(request.query_param("offset"), offset))
                return error(400, "bad_request", "offset must be a non-negative integer");
            if (!request.query_param("limit").empty() && (!parse_u64(request.query_param("limit"), limit) || limit == 0 || limit > 5000))
                return error(400, "bad_request", "limit must be 1..5000");
            json::Array frames;
            for (std::uint64_t i = offset; i < data->frames.size() && i < offset + limit; ++i)
                frames.push_back(frame_index_json(data->frames[i]));
            return json_response(200, json::Object{{"session_id", id}, {"total", static_cast<std::int64_t>(data->frames.size())},
                                                   {"offset", offset}, {"frames", json::Value(std::move(frames))},
                                                   {"width", data->geometry.width}, {"height", data->geometry.height},
                                                   {"index_errors", static_cast<std::int64_t>(data->index_errors.size())}});
        }
        if (m == "GET" && parts.size() == 5 && parts[3] == "frames") return session_frame(id, parts[4]);
        if (parts.size() >= 4 && parts[3] == "annotations") {
            if (parts.size() >= 5 && !valid_identifier(parts[4])) return error(400, "invalid_annotation_id", "invalid annotation ID");
            if (m == "GET" && parts.size() == 4) {
                const auto data = session(id);
                if (!data) return error(404, "not_found", "unknown session");
                return json_response(200, AnnotationStore(*data, config_.require_independent_review).list(request.query_param("include_deleted") == "1"));
            }
            if (m == "GET" && parts.size() == 6 && parts[5] == "history") {
                const auto data = session(id);
                if (!data) return error(404, "not_found", "unknown session");
                return json_response(200, AnnotationStore(*data, config_.require_independent_review).history(parts[4]));
            }
            if (m == "POST" && parts.size() == 4) return annotation_write(request, principal, id, "", "create");
            if (m == "PUT" && parts.size() == 5) return annotation_write(request, principal, id, parts[4], "revise");
            if (m == "POST" && parts.size() == 6 && parts[5] == "delete") return annotation_write(request, principal, id, parts[4], "delete");
            if (m == "POST" && parts.size() == 6 && parts[5] == "review") return annotation_write(request, principal, id, parts[4], "review");
        }
    }
    if (m != "GET") audit_.record(principal.id, m + " " + request.path, request.path, "denied", "no such write endpoint");
    return error(404, "not_found", "no such resource");
}

}  // namespace ahpv
