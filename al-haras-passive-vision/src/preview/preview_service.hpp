#pragma once
// Read-only preview, replay and annotation API (FR-004, FR-005, FR-010).
// There is no endpoint that changes capture, configuration or any device; the
// only writes are annotation records. Every /api route except /api/health
// requires a bearer token, and every write attempt is audited.
#include "frames/pipeline.hpp"
#include "preview/http_server.hpp"
#include "recording/session.hpp"
#include "security/audit.hpp"
#include "security/auth.hpp"
#include "security/config.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ahpv {

class PreviewService {
public:
    PreviewService(const Config& config, AuthStore auth, AuditLog& audit, const Pipeline* live);
    HttpResponse handle(const HttpRequest& request);
    void set_port(int port) { port_ = port; }

private:
    const Config& config_;
    AuthStore auth_;
    AuditLog& audit_;
    const Pipeline* live_;
    SessionStore store_;
    int port_{0};
    std::mutex cache_mutex_;
    std::map<std::string, std::pair<std::string, std::shared_ptr<SessionData>>> cache_;

    std::shared_ptr<SessionData> session(const std::string& id);
    HttpResponse route(const HttpRequest& request, const Principal& principal);
    HttpResponse live_frame() const;
    HttpResponse session_frame(const std::string& id, const std::string& frame_part);
    HttpResponse annotation_write(const HttpRequest& request, const Principal& principal, const std::string& id,
                                  const std::string& annotation_id, const std::string& action);
};

}  // namespace ahpv
