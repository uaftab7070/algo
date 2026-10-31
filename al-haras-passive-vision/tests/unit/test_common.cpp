#include "test_support.hpp"

#include "common/clock.hpp"
#include "common/fsutil.hpp"
#include "common/hash.hpp"
#include "common/json.hpp"
#include "metrics/histogram.hpp"

using namespace ahpv;

TEST_CASE("json: round trip keeps order, int64 precision and escapes") {
    const std::string text = R"({"b":1,"a":[true,false,null],"n":1761312000123456789,"f":0.5,"s":"q\"\\\n\u00e9\ud83d\ude00"})";
    const json::Value v = json::parse(text);
    CHECK_EQ(v.at("n").as_int(), 1761312000123456789LL);
    CHECK_EQ(v.at("s").as_string(), std::string("q\"\\\n\xC3\xA9\xF0\x9F\x98\x80"));
    CHECK_EQ(v.as_object().front().first, std::string("b"));
    CHECK(json::parse(json::dump(v)) == v);
    CHECK_EQ(json::dump(json::Value(2.0)), std::string("2.0"));
}

TEST_CASE("json: rejects malformed and unsafe input") {
    for (const char* bad : {"", "{", "[1,]", "{\"a\":1,\"a\":2}", "NaN", "1 2", "\"\\x\"", "\"a\nb\"", "01", "[1e999]",
                            "\"\\ud800\"", "\"\xC0\xAF\"", "{\"a\" 1}", "tru"})
        CHECK_THROWS(json::parse(bad));
    std::string deep(100, '[');
    deep += std::string(100, ']');
    CHECK_THROWS(json::parse(deep));
}

TEST_CASE("sha256: FIPS 180-4 test vectors") {
    CHECK_EQ(sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    Sha256 incremental;
    const std::string million(1000000, 'a');
    for (std::size_t i = 0; i < million.size(); i += 4097) incremental.update(million.substr(i, 4097));
    CHECK_EQ(incremental.finish_hex(), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST_CASE("crc32: IEEE check value and chaining") {
    CHECK_EQ(crc32("123456789", 9), 0xCBF43926u);
    CHECK_EQ(crc32("6789", 4, crc32("12345", 5)), 0xCBF43926u);
    CHECK_EQ(hex32(0xCBF43926u), std::string("cbf43926"));
}

TEST_CASE("clock: monotonic ordering and UTC formatting") {
    const auto a = mono_ns(), b = mono_ns();
    CHECK(b >= a);
    CHECK_EQ(iso8601_utc(1761264000123000000LL), std::string("2025-10-24T00:00:00.123Z"));
    CHECK_EQ(compact_utc(1761264000123000000LL), std::string("20251024T000000Z"));
}

TEST_CASE("fsutil: atomic write, exclusive mkdir and ranged reads") {
    const std::string dir = test::temp_dir("fs");
    const std::string file = join_path(dir, "a.txt");
    write_file_atomic(file, "hello world");
    CHECK_EQ(read_file(file), std::string("hello world"));
    write_file_atomic(file, "replaced");
    CHECK_EQ(read_file(file), std::string("replaced"));
    CHECK(make_directory_exclusive(join_path(dir, "x")));
    CHECK(!make_directory_exclusive(join_path(dir, "x")));
    const auto range = read_range(file, 2, 3);
    CHECK_EQ(std::string(range.begin(), range.end()), std::string("pla"));
    CHECK_THROWS(read_range(file, 6, 10));
    CHECK_THROWS(read_file(file, 3));
    CHECK(storage_status(dir).available);
}

TEST_CASE("histogram: exact small values, bounded relative error, quantiles") {
    Histogram h;
    for (int i = 1; i <= 100; ++i) h.add(static_cast<std::int64_t>(i) * 1000000);  // 1..100 ms
    CHECK_EQ(h.count(), 100u);
    CHECK_EQ(h.min_ns(), 1000000);
    CHECK_EQ(h.max_ns(), 100000000);
    const double p50 = static_cast<double>(h.quantile_ns(0.5)) / 1e6;
    const double p99 = static_cast<double>(h.quantile_ns(0.99)) / 1e6;
    CHECK(p50 >= 50.0 && p50 <= 50.0 * 1.017);
    CHECK(p99 >= 99.0 && p99 <= 100.0);
    Histogram small;
    small.add(42000);
    CHECK_EQ(small.quantile_ns(0.5), 42000);
    small.add(-5);
    CHECK_EQ(small.count(), 1u);
}
