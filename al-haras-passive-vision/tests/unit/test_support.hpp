#pragma once
// Minimal self-registering test harness (no third-party dependency on the board).
#include <cstdint>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ahpv::test {

struct Case {
    const char* name;
    std::function<void()> body;
};
std::vector<Case>& registry();

struct Register {
    Register(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A fresh, empty directory under $AHPV_TEST_TMP (default /tmp).
std::string temp_dir(const std::string& label);
void write_text(const std::string& path, const std::string& text);
// Synthetic I420 Y4M with `frames` frames whose luma encodes the frame index.
std::string write_y4m(const std::string& path, int width, int height, int frames, const std::string& extra = "");

}  // namespace ahpv::test

#define AHPV_CAT2(a, b) a##b
#define AHPV_CAT(a, b) AHPV_CAT2(a, b)
#define TEST_CASE(name) \
    static void AHPV_CAT(test_fn_, __LINE__)(); \
    static ::ahpv::test::Register AHPV_CAT(test_reg_, __LINE__)(name, AHPV_CAT(test_fn_, __LINE__)); \
    static void AHPV_CAT(test_fn_, __LINE__)()

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::ostringstream m_; \
            m_ << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed"; \
            throw ::ahpv::test::Failure(m_.str()); \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        const auto a_ = (a); /* copies: operands may reference temporaries */ \
        const auto b_ = (b); \
        if (!(a_ == b_)) { \
            std::ostringstream m_; \
            m_ << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #a ", " #b ") failed: [" << a_ << "] vs [" << b_ << "]"; \
            throw ::ahpv::test::Failure(m_.str()); \
        } \
    } while (0)

#define CHECK_THROWS(expr) \
    do { \
        bool thrown_ = false; \
        try { \
            (void)(expr); \
        } catch (...) { \
            thrown_ = true; \
        } \
        if (!thrown_) { \
            std::ostringstream m_; \
            m_ << __FILE__ << ":" << __LINE__ << ": expected exception from " #expr; \
            throw ::ahpv::test::Failure(m_.str()); \
        } \
    } while (0)
