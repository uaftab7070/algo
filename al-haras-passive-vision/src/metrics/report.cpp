#include "metrics/report.hpp"

#include "common/clock.hpp"
#include "common/fsutil.hpp"
#include "common/hash.hpp"
#include "common/version.hpp"
#include "metrics/histogram.hpp"
#include "metrics/images.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace ahpv {
namespace {

constexpr std::uint32_t kUnusableFlags = kFlagLowContrast | kFlagVeryDark | kFlagVeryBright | kFlagRepeatedContent | kFlagCorrupted;

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

struct Range {
    double min{0}, max{0}, sum{0};
    std::uint64_t n{0};
    void add(double v) {
        if (n == 0 || v < min) min = v;
        if (n == 0 || v > max) max = v;
        sum += v;
        ++n;
    }
    json::Value json() const {
        return json::Object{{"min", round3(min)}, {"mean", n ? round3(sum / static_cast<double>(n)) : 0.0}, {"max", round3(max)}};
    }
};

json::Value field_or_null(const json::Value& object, const char* key) {
    const json::Value* v = object.find(key);
    return v ? *v : json::Value(nullptr);
}

// Flattens scalars of a JSON object into section/metric/value rows.
void flatten(const json::Value& value, const std::string& prefix, std::vector<std::pair<std::string, std::string>>& rows) {
    if (value.is_object()) {
        for (const auto& [key, child] : value.as_object()) flatten(child, prefix.empty() ? key : prefix + "." + key, rows);
    } else if (value.is_array()) {
        rows.emplace_back(prefix, "array[" + std::to_string(value.as_array().size()) + "]");
    } else if (value.is_string()) {
        rows.emplace_back(prefix, value.as_string());
    } else {
        rows.emplace_back(prefix, json::dump(value));
    }
}

}  // namespace

bool session_full_range(const SessionData& session) {
    const json::Value* effective = session.manifest.at("source").find("effective");
    if (!effective) return false;
    const json::Value* range = effective->find("color_range");
    return range && range->is_string() && range->as_string() == "FULL";
}

std::string csv_field(const std::string& text) {
    std::string value = text;
    // Spreadsheet formula injection guard (SEC-005).
    if (!value.empty() && (value[0] == '=' || value[0] == '+' || value[0] == '-' || value[0] == '@' || value[0] == '\t'))
        value = "'" + value;
    if (value.find_first_of(",\"\n\r") == std::string::npos) return value;
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    return out + "\"";
}

json::Value build_report(const SessionData& session, const json::Value& verification, const json::Value& annotations) {
    const json::Value& m = session.manifest;
    json::Value report;
    report.set("schema_version", kSchemaVersion);
    report.set("usage_notice", kUsageNotice);
    report.set("report_id", "r-" + compact_utc(wall_ns()) + "-" + random_hex(4));
    report.set("generated_at", iso8601_utc(wall_ns()));
    report.set("generator", json::Object{{"name", kSoftwareName}, {"version", kVersion}, {"build_id", kBuildId},
                                         {"executable_sha256", executable_sha256()}});
    report.set("session_id", session.id);
    report.set("run_id", field_or_null(m, "run_id"));
    report.set("segment_index", field_or_null(m, "segment_index"));
    report.set("session_status", m.at("status"));

    // Provenance (DAT-001) and completeness.
    const json::Value provenance = field_or_null(m, "provenance");
    bool complete = provenance.is_object();
    if (complete)
        for (const auto& [key, value] : provenance.as_object())
            if (!value.is_string() || value.as_string().empty() || value.as_string() == "unspecified") complete = false;
    report.set("provenance", provenance);
    report.set("provenance_complete", complete);

    // Benchmark metadata (DAT-006).
    json::Value environment;
    environment.set("recording_platform", field_or_null(m, "platform"));
    environment.set("recording_software", field_or_null(m, "software"));
    environment.set("report_platform", platform_summary());
    environment.set("configuration_sha256", m.at("configuration").at("text_sha256"));
    environment.set("source", m.at("source"));
    environment.set("fault_injection", field_or_null(m, "fault_injection"));
    try {
        environment.set("input_manifest_sha256", sha256_file(join_path(session.dir, "session.json")));
    } catch (const std::exception& error) {
        environment.set("input_manifest_sha256", nullptr);
    }
    const json::Value* sums = m.find("checksums");
    environment.set("video_sha256", sums && sums->find("video.y4m") ? sums->at("video.y4m").at("sha256") : json::Value(nullptr));
    report.set("environment", environment);

    // ---- Deterministic section ----
    json::Value det;
    std::map<std::uint64_t, std::string> status;  // frame_id -> stored|queue_drop|rejected_frame
    std::map<std::uint64_t, std::uint32_t> flags;
    Range luma_mean, luma_std;
    Histogram source_interval;
    std::map<std::string, std::int64_t> flag_counts;
    json::Array upstream_gaps;
    std::uint64_t missing_upstream = 0;
    for (const auto& f : session.frames) {
        status[f.frame_id] = "stored";
        flags[f.frame_id] = f.flags;
        luma_mean.add(f.luma_mean);
        luma_std.add(f.luma_std);
        if (f.interval_ns >= 0) source_interval.add(f.interval_ns);
        for (const auto& name : flag_name_list(f.flags)) ++flag_counts[name];
        if (f.missing_before) {
            missing_upstream += f.missing_before;
            upstream_gaps.push_back(json::Object{{"before_frame_id", f.frame_id}, {"missing_source_frames", f.missing_before}});
        }
    }
    std::map<std::string, std::int64_t> event_counts;
    for (const auto& e : session.events) {
        const std::string type = e.at("event").as_string();
        ++event_counts[type];
        if ((type == "queue_drop" || type == "rejected_frame") && e.find("frame_id"))
            status.emplace(static_cast<std::uint64_t>(e.at("frame_id").as_int()), type);
    }
    const auto count_of = [&](const std::string& type) {
        const auto it = event_counts.find(type);
        return it == event_counts.end() ? std::int64_t{0} : it->second;
    };
    json::Value frames;
    frames.set("stored", static_cast<std::int64_t>(session.frames.size()));
    frames.set("dropped_queue", count_of("queue_drop"));
    frames.set("rejected", count_of("rejected_frame"));
    frames.set("missing_upstream", missing_upstream);
    frames.set("first_frame_id", status.empty() ? json::Value(nullptr) : json::Value(status.begin()->first));
    frames.set("last_frame_id", status.empty() ? json::Value(nullptr) : json::Value(status.rbegin()->first));
    frames.set("accounted_ids", static_cast<std::int64_t>(status.size()));
    det.set("frames", frames);
    json::Value flag_json = json::Object{};
    for (const auto& [name, count] : flag_counts) flag_json.set(name, count);
    det.set("flag_counts", flag_json);
    json::Value event_json = json::Object{};
    for (const auto& [name, count] : event_counts) {
        if (name == "session_finalized" || name == "session_recovered") continue;  // Bookkeeping, not observations.
        event_json.set(name, count);
    }
    det.set("event_counts", event_json);
    det.set("source_interval", source_interval.to_json());
    det.set("timestamp_basis", m.at("source").at("timestamp_basis"));
    det.set("quality", json::Object{{"luma_mean", luma_mean.json()}, {"luma_std", luma_std.json()}});

    // Unusable spans: runs of IDs that are missing or carry an unusable-quality flag.
    json::Array segments;
    std::uint64_t unusable = 0;
    std::optional<std::uint64_t> run_start;
    std::uint64_t run_end = 0;
    std::set<std::string> run_reasons;
    const auto close_run = [&]() {
        if (!run_start) return;
        json::Array reasons;
        for (const auto& r : run_reasons) reasons.emplace_back(r);
        segments.push_back(json::Object{{"first_frame_id", *run_start}, {"last_frame_id", run_end},
                                        {"frames", run_end - *run_start + 1}, {"reasons", json::Value(std::move(reasons))}});
        run_start.reset();
        run_reasons.clear();
    };
    for (const auto& [id, kind] : status) {
        std::set<std::string> reasons;
        if (kind != "stored") reasons.insert(kind == "queue_drop" ? "dropped_by_queue" : "rejected_corrupted");
        else
            for (const auto& name : flag_name_list(flags[id] & kUnusableFlags)) reasons.insert(name);
        if (reasons.empty()) {
            close_run();
            continue;
        }
        ++unusable;
        if (run_start && id != run_end + 1) close_run();
        if (!run_start) run_start = id;
        run_end = id;
        run_reasons.insert(reasons.begin(), reasons.end());
    }
    close_run();
    det.set("unusable_segments", json::Value(std::move(segments)));
    det.set("upstream_gaps", json::Value(std::move(upstream_gaps)));
    det.set("unusable_frame_ids", unusable);
    det.set("usable_fraction", status.empty() ? json::Value(nullptr)
                                              : json::Value(round3(1.0 - static_cast<double>(unusable) / static_cast<double>(status.size()))));
    det.set("quality_rules", "unusable = missing (dropped/rejected) or flagged low_contrast, very_dark, very_bright, repeated_content");

    // Annotation completeness (DAT-005); review agreement is not computed.
    json::Value ann;
    std::map<std::string, std::int64_t> by_status;
    std::set<std::string> annotators, reviewers;
    std::set<std::int64_t> annotated_frames;
    std::int64_t total = 0, independent = 0, reviewed = 0;
    for (const auto& a : annotations.at("annotations").as_array()) {
        if (a.at("deleted").as_bool()) continue;
        ++total;
        ++by_status[a.at("review_status").as_string()];
        annotators.insert(a.at("annotator_id").as_string());
        annotated_frames.insert(a.at("frame_id").as_int());
        if (!a.at("reviewer_id").is_null()) {
            ++reviewed;
            reviewers.insert(a.at("reviewer_id").as_string());
            if (a.at("reviewer_id").as_string() != a.at("annotator_id").as_string()) ++independent;
        }
    }
    json::Value status_json = json::Object{};
    for (const auto& [name, count] : by_status) status_json.set(name, count);
    ann.set("active_annotations", total);
    ann.set("by_review_status", status_json);
    ann.set("frames_annotated", static_cast<std::int64_t>(annotated_frames.size()));
    ann.set("frames_annotated_fraction", session.frames.empty() ? json::Value(nullptr) :
            json::Value(round3(static_cast<double>(annotated_frames.size()) / static_cast<double>(session.frames.size()))));
    ann.set("reviewed", reviewed);
    ann.set("independently_reviewed", independent);
    json::Array a_list, r_list;
    for (const auto& a : annotators) a_list.emplace_back(a);
    for (const auto& r : reviewers) r_list.emplace_back(r);
    ann.set("annotators", json::Value(std::move(a_list)));
    ann.set("reviewers", json::Value(std::move(r_list)));
    ann.set("log_chain_ok", annotations.at("chain_ok"));
    ann.set("inter_annotator_agreement", "not computed (no overlapping independent annotation protocol defined)");
    det.set("annotations", ann);

    json::Array failed;
    for (const auto& c : verification.at("checks").as_array())
        if (c.at("status").as_string() == "fail") failed.emplace_back(c.at("name").as_string());
    det.set("integrity", json::Object{{"verify_ok", verification.at("ok")}, {"failed_checks", json::Value(std::move(failed))}});
    report.set("deterministic", det);

    // ---- Non-deterministic section ----
    json::Value nondet;
    Histogram acquisition, processing;
    for (std::size_t i = 0; i < session.frames.size(); ++i) {
        processing.add(session.frames[i].processing_ns);
        if (i > 0) acquisition.add(session.frames[i].acquisition_mono_ns - session.frames[i - 1].acquisition_mono_ns);
    }
    nondet.set("acquisition_interval", acquisition.to_json());
    nondet.set("processing_time", processing.to_json());
    if (session.frames.size() > 1) {
        const double span = static_cast<double>(session.frames.back().acquisition_mono_ns - session.frames.front().acquisition_mono_ns) / 1e9;
        nondet.set("acquisition_span_s", round3(span));
        nondet.set("effective_fps_measured", span > 0 ? json::Value(round3(static_cast<double>(session.frames.size() - 1) / span)) : json::Value(nullptr));
    }
    nondet.set("first_acquisition_wall", session.frames.empty() ? json::Value(nullptr) : json::Value(iso8601_utc(session.frames.front().acquisition_wall_ns)));
    // Resource samples from metrics.jsonl.
    Range rss, cpu, storage_free, mem_available;
    std::int64_t samples = 0;
    const std::string metrics_path = join_path(session.dir, "metrics.jsonl");
    if (path_exists(metrics_path)) {
        std::istringstream in(read_file(metrics_path, 256u << 20));
        std::string line;
        while (std::getline(in, line)) {
            try {
                const json::Value s = json::parse(line);
                ++samples;
                const auto add = [&](Range& r, const char* key) {
                    const json::Value* v = s.find(key);
                    if (v && v->is_number()) r.add(v->as_number());
                };
                add(rss, "rss_bytes");
                add(cpu, "process_cpu_percent");
                add(storage_free, "storage_free_bytes");
                add(mem_available, "mem_available_bytes");
            } catch (const std::exception&) {
            }
        }
    }
    json::Value resources;
    resources.set("samples", samples);
    resources.set("rss_bytes", rss.n ? rss.json() : json::Value(nullptr));
    resources.set("process_cpu_percent", cpu.n ? cpu.json() : json::Value(nullptr));
    resources.set("storage_free_bytes", storage_free.n ? storage_free.json() : json::Value(nullptr));
    resources.set("mem_available_bytes", mem_available.n ? mem_available.json() : json::Value(nullptr));
    resources.set("note", "Measured on the recording platform named in environment.recording_platform only.");
    nondet.set("resources", resources);
    report.set("nondeterministic", nondet);
    return report;
}

std::string frames_csv(const SessionData& session) {
    std::string out = "frame_id,source_epoch,source_sequence,acquisition_mono_ns,acquisition_wall_ns,source_timestamp_ns,"
                      "interval_ns,missing_before,size,crc32,luma_mean,luma_std,flags,processing_ns\n";
    for (const auto& f : session.frames) {
        std::string flags;
        for (const auto& name : flag_name_list(f.flags)) flags += (flags.empty() ? "" : "|") + name;
        const json::Value row = frame_index_json(f);
        out += std::to_string(f.frame_id) + "," + std::to_string(f.source_epoch) + "," +
               (f.has_source_sequence ? std::to_string(f.source_sequence) : "") + "," + std::to_string(f.acquisition_mono_ns) + "," +
               std::to_string(f.acquisition_wall_ns) + "," + std::to_string(f.source_timestamp_ns) + "," +
               (f.interval_ns >= 0 ? std::to_string(f.interval_ns) : "") + "," + std::to_string(f.missing_before) + "," +
               std::to_string(f.size) + "," + (f.has_crc ? hex32(f.crc) : "") + "," + json::dump(row.at("luma_mean")) + "," +
               json::dump(row.at("luma_std")) + "," + csv_field(flags) + "," + std::to_string(f.processing_ns) + "\n";
    }
    return out;
}

std::string summary_csv(const json::Value& report) {
    std::vector<std::pair<std::string, std::string>> rows;
    flatten(report.at("deterministic"), "deterministic", rows);
    flatten(report.at("nondeterministic"), "nondeterministic", rows);
    std::string out = "metric,value\n";
    out += "usage_notice," + csv_field(kUsageNotice) + "\n";
    out += "session_id," + csv_field(report.at("session_id").as_string()) + "\n";
    out += "report_id," + csv_field(report.at("report_id").as_string()) + "\n";
    for (const auto& [metric, value] : rows) out += csv_field(metric) + "," + csv_field(value) + "\n";
    return out;
}

std::string annotations_csv(const json::Value& annotations) {
    std::string out = "annotation_id,revision,frame_id,source_timestamp_ns,x,y,width,height,annotator_id,review_status,reviewer_id,deleted,notes\n";
    for (const auto& a : annotations.at("annotations").as_array()) {
        const auto& r = a.at("region_xywh_pixels").as_array();
        out += csv_field(a.at("annotation_id").as_string()) + "," + std::to_string(a.at("revision").as_int()) + "," +
               std::to_string(a.at("frame_id").as_int()) + "," + std::to_string(a.at("source_timestamp_ns").as_int()) + "," +
               std::to_string(r[0].as_int()) + "," + std::to_string(r[1].as_int()) + "," + std::to_string(r[2].as_int()) + "," +
               std::to_string(r[3].as_int()) + "," + csv_field(a.at("annotator_id").as_string()) + "," +
               csv_field(a.at("review_status").as_string()) + "," +
               csv_field(a.at("reviewer_id").is_null() ? "" : a.at("reviewer_id").as_string()) + "," +
               (a.at("deleted").as_bool() ? "true" : "false") + "," + csv_field(a.at("notes").as_string()) + "\n";
    }
    return out;
}

json::Value export_still(const SessionData& session, std::uint64_t frame_id, const std::string& out_path, int max_width) {
    const FrameIndexEntry* entry = find_frame(session, frame_id);
    if (!entry) throw std::runtime_error("frame " + std::to_string(frame_id) + " is not stored in session " + session.id);
    const auto payload = read_frame_payload(session, *entry);
    if (entry->has_crc && crc32(payload.data(), payload.size()) != entry->crc)
        throw std::runtime_error("frame " + std::to_string(frame_id) + " fails its CRC-32 check; not exported");
    const RgbImage image = frame_to_rgb(session.geometry, payload.data(), max_width, session_full_range(session));
    write_file_atomic(out_path, encode_bmp(image), 0640);
    return json::Object{{"path", out_path}, {"frame_id", frame_id}, {"source_timestamp_ns", entry->source_timestamp_ns},
                        {"transform", image.transform}, {"usage_notice", kUsageNotice}};
}

json::Value export_contact_sheet(const SessionData& session, const std::string& out_dir, int count, int tile_width) {
    if (session.frames.empty()) throw std::runtime_error("session has no stored frames");
    count = std::max(1, std::min<int>(count, static_cast<int>(session.frames.size())));
    std::vector<RgbImage> tiles;
    json::Array index;
    for (int i = 0; i < count; ++i) {
        const std::size_t pick = count == 1 ? 0 : static_cast<std::size_t>(i) * (session.frames.size() - 1) / static_cast<std::size_t>(count - 1);
        const auto& entry = session.frames[pick];
        const auto payload = read_frame_payload(session, entry);
        const bool crc_ok = !entry.has_crc || crc32(payload.data(), payload.size()) == entry.crc;
        RgbImage tile = frame_to_rgb(session.geometry, payload.data(), tile_width, session_full_range(session));
        if (!crc_ok) std::fill(tile.rgb.begin(), tile.rgb.end(), 255);  // Never show unverified pixels as frames.
        tiles.push_back(std::move(tile));
        index.push_back(json::Object{{"tile", i}, {"frame_id", entry.frame_id}, {"source_timestamp_ns", entry.source_timestamp_ns},
                                     {"flags", flag_names(entry.flags)}, {"crc_ok", crc_ok}});
    }
    const RgbImage sheet = contact_sheet(tiles, std::min(count, 3));
    make_directories(out_dir);
    write_file_atomic(join_path(out_dir, "contact-sheet.bmp"), encode_bmp(sheet), 0640);
    json::Value meta;
    meta.set("usage_notice", kUsageNotice);
    meta.set("session_id", session.id);
    meta.set("columns", std::min(count, 3));
    meta.set("tiles", json::Value(std::move(index)));
    meta.set("transform", sheet.transform);
    write_file_atomic(join_path(out_dir, "contact-sheet.json"), json::dump(meta) + "\n", 0640);
    return meta;
}

json::Value write_report_bundle(const SessionData& session, const json::Value& report, const json::Value& annotations,
                                int contact_sheet_frames) {
    const std::string dir = join_path(join_path(session.dir, "reports"), report.at("report_id").as_string());
    make_directories(parent_directory(dir));
    if (!make_directory_exclusive(dir)) throw std::runtime_error("report directory already exists: " + dir);
    write_file_atomic(join_path(dir, "qa-report.json"), json::dump(report) + "\n", 0640);
    write_file_atomic(join_path(dir, "summary.csv"), summary_csv(report), 0640);
    write_file_atomic(join_path(dir, "frames.csv"), frames_csv(session), 0640);
    write_file_atomic(join_path(dir, "annotations.csv"), annotations_csv(annotations), 0640);
    json::Value out;
    out.set("report_dir", dir);
    out.set("report_id", report.at("report_id"));
    json::Array files{"qa-report.json", "summary.csv", "frames.csv", "annotations.csv"};
    if (contact_sheet_frames > 0 && !session.frames.empty()) {
        export_contact_sheet(session, dir, contact_sheet_frames, 320);
        files.emplace_back("contact-sheet.bmp");
        files.emplace_back("contact-sheet.json");
    }
    // Integrity indicator for the report bundle itself (SEC-004).
    json::Value sums;
    for (const auto& name : files) sums.set(name.as_string(), sha256_file(join_path(dir, name.as_string())));
    write_file_atomic(join_path(dir, "checksums.json"), json::dump(json::Object{{"algorithm", "sha256"}, {"files", sums}}) + "\n", 0640);
    files.emplace_back("checksums.json");
    out.set("files", json::Value(std::move(files)));
    return out;
}

}  // namespace ahpv
