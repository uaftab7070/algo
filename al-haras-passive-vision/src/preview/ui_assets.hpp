#pragma once
// Static browser UI, compiled into the binary so the board needs no web root.
#include <string>

namespace ahpv {
const std::string& ui_index_html();
const std::string& ui_app_js();
const std::string& ui_app_css();
}  // namespace ahpv
