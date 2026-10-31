#include "test_support.hpp"

#include "common/fsutil.hpp"
#include "common/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>

namespace ahpv::test {

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

std::string temp_dir(const std::string& label) {
    const char* root = std::getenv("AHPV_TEST_TMP");
    std::string pattern = std::string(root && *root ? root : "/tmp") + "/ahpv-" + label + "-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (!::mkdtemp(buffer.data())) throw std::runtime_error("mkdtemp failed for " + pattern);
    return buffer.data();
}

void write_text(const std::string& path, const std::string& text) { write_file_atomic(path, text, 0644); }

std::string write_y4m(const std::string& path, int width, int height, int frames, const std::string& extra) {
    std::string data = "YUV4MPEG2 W" + std::to_string(width) + " H" + std::to_string(height) + " F30:1 Ip A1:1 C420jpeg" + extra + "\n";
    const std::size_t luma = static_cast<std::size_t>(width) * height;
    const std::size_t chroma = static_cast<std::size_t>((width + 1) / 2) * ((height + 1) / 2);
    for (int i = 0; i < frames; ++i) {
        data += "FRAME\n";
        for (std::size_t p = 0; p < luma; ++p) data.push_back(static_cast<char>((p * 7 + static_cast<std::size_t>(i) * 13) % 200 + 20));
        data.append(chroma * 2, static_cast<char>(128));
    }
    write_text(path, data);
    return path;
}

}  // namespace ahpv::test

int main(int argc, char** argv) {
    ahpv::Logger::instance().set_level(ahpv::Level::Error);
    ahpv::Logger::instance().set_stderr(std::getenv("AHPV_TEST_LOG") != nullptr);
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0, failed = 0;
    for (const auto& c : ahpv::test::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        try {
            c.body();
            ++passed;
            std::cout << "PASS " << c.name << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "FAIL " << c.name << ": " << e.what() << "\n";
        }
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
