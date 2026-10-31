#pragma once
// Offline QA reports (FR-011, FR-019, FR-022, DAT-005, DAT-006). Fields under
// "deterministic" depend only on recorded data and must match across replays of
// identical input/build/config (NFR-011); timings live under "nondeterministic".
#include "common/json.hpp"
#include "recording/session.hpp"

#include <string>

namespace ahpv {

json::Value build_report(const SessionData& session, const json::Value& verification, const json::Value& annotations);
std::string frames_csv(const SessionData& session);
std::string summary_csv(const json::Value& report);
std::string annotations_csv(const json::Value& annotations);
std::string csv_field(const std::string& text);  // Quotes and neutralises spreadsheet formulas.

// Writes qa-report.json, summary.csv, frames.csv, annotations.csv and an optional
// contact sheet into a new reports/<report_id>/ directory. Returns the paths.
json::Value write_report_bundle(const SessionData& session, const json::Value& report, const json::Value& annotations,
                                int contact_sheet_frames);

// Evenly spaced stored frames as one BMP plus a JSON index of tile -> frame.
json::Value export_contact_sheet(const SessionData& session, const std::string& out_dir, int count, int tile_width);
json::Value export_still(const SessionData& session, std::uint64_t frame_id, const std::string& out_path, int max_width);
bool session_full_range(const SessionData& session);

}  // namespace ahpv
