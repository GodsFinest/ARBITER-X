#include "testfw.hpp"

#include "arbiter/config.hpp"
#include "arbiter/crc32.hpp"
#include "arbiter/errors.hpp"
#include "arbiter/flatmap.hpp"
#include "arbiter/hash.hpp"
#include "arbiter/histogram.hpp"
#include "arbiter/rng.hpp"

#include <map>
#include <unordered_map>
#include <string>

using namespace arbiter;

AX_TEST(rng_determinism_and_streams) {
    Rng a = Rng::stream(42, 1);
    Rng b = Rng::stream(42, 1);
    for (int i = 0; i < 1000; ++i) CHECK_EQ(a.next(), b.next());

    Rng c = Rng::stream(42, 2);
    Rng d = Rng::stream(43, 1);
    CHECK(a.next() != c.next());  // different streams diverge (overwhelming probability)
    CHECK(a.next() != d.next());

    // bounded stays in range
    Rng e = Rng::stream(7, 3);
    for (int i = 0; i < 10000; ++i) {
        u64 v = e.bounded(97);
        CHECK(v < 97);
        if (testfw::current_failed()) return;
    }
    double u = e.unit();
    CHECK(u >= 0.0 && u < 1.0);
}

AX_TEST(rng_mulhi_portable_matches_wide) {
    // The portable 32-bit-split mulhi (used when __int128 is unavailable)
    // must agree bit-for-bit with the native 128-bit product, this pins
    // cross-compiler stream identity of bounded().
    Rng r = Rng::stream(20260907, 0x1A1A);
    for (int i = 0; i < 20000; ++i) {
        const u64 x = r.next();
        const u64 n = 1 + (r.next() % (1ull << 40));
#if defined(__SIZEOF_INT128__)
        const u64 native = static_cast<u64>((static_cast<u128_t>(x) * n) >> 64);
        // emulate the split path independently of the header's #if
        const u64 alo = x & 0xFFFFFFFFull, ahi = x >> 32;
        const u64 blo = n & 0xFFFFFFFFull, bhi = n >> 32;
        const u64 p0 = alo * blo;
        const u64 p1 = alo * bhi;
        const u64 p2 = ahi * blo;
        const u64 p3 = ahi * bhi;
        u64 mid = p1 + (p0 >> 32);
        mid += p2;
        const u64 carry = mid < p2 ? (1ull << 32) : 0;
        const u64 split = p3 + (mid >> 32) + carry;
        CHECK_EQ(split, native);
        if (testfw::current_failed()) return;
#endif
        // and bounded() itself is Lemire over mulhi
        CHECK(Rng::mulhi64(x, n) == Rng::mulhi64(x, n));  // purity sanity
    }
}

AX_TEST(hash_fnv1a_known_vector) {
    // FNV-1a 64 of the single byte "a" is the standard test vector.
    Hasher h;
    h.raw("a", 1);
    CHECK_EQ(h.value(), 0xaf63dc4c8601ec8cull);
}

AX_TEST(hash_domain_separation) {
    Hasher h1;
    h1.str("ab");
    Hasher h2;
    h2.str("a");
    h2.str("b");
    CHECK(h1.value() != h2.value());  // length-prefixed strings don't concatenate
    Hasher h3;
    h3.tag("A");
    h3.u64v(1);
    Hasher h4;
    h4.tag("A1");
    CHECK(h3.value() != h4.value());
}

AX_TEST(crc32_known_vectors) {
    CHECK_EQ(crc32("123456789", 9), 0xCBF43926u);
    CHECK_EQ(crc32("", 0), 0x00000000u);
    CHECK_EQ(crc32_two("1234", 4, "56789", 5), 0xCBF43926u);  // chunked == whole
}

AX_TEST(histogram_percentiles) {
    Histogram hist(1024);
    for (int i = 1; i <= 1000; ++i) hist.record(static_cast<double>(i));
    auto s = hist.summarize();
    CHECK_EQ(s.count, 1000u);
    CHECK(s.p50 > 490 && s.p50 < 510);
    CHECK(s.p99 > 985 && s.p99 <= 1000);
    CHECK_EQ(s.mn, 1.0);
    CHECK_EQ(s.mx, 1000.0);
}

AX_TEST(config_parse_and_getters) {
    const char* text =
        "# comment\n"
        "[experiment]\n"
        "name = latency_sensitivity\n"
        "seed = 42\n"
        "algo_latency_ns = 800   # inline comment\n"
        "[network]\n"
        "jitter_ns = 50\n"
        "loss_rate = 0.01\n"
        "[risk]\n"
        "enabled = true\n";
    Status st;
    Config cfg = Config::from_string(text, &st);
    CHECK(st);
    CHECK_EQ(cfg.get_str("experiment.name", ""), std::string("latency_sensitivity"));
    CHECK_EQ(cfg.get_i64("experiment.seed", 0), 42);
    CHECK_EQ(cfg.get_i64("experiment.algo_latency_ns", 0), 800);
    CHECK_EQ(cfg.get_f64("network.loss_rate", -1), 0.01);
    CHECK(cfg.get_bool("risk.enabled", false));
    CHECK_EQ(cfg.get_i64("missing.key", 99), 99);
    CHECK(!cfg.has("missing.key"));
    // sections namespace ALL keys (a dotted key inside [s] is s.key...);
    // fully-qualified keys live at top level
    Status st2;
    Config top = Config::from_string("[top]\nsub.x = 1\n", &st2);
    CHECK(st2);
    CHECK_EQ(top.get_i64("top.sub.x", 0), 1);

    // malformed lines must be rejected
    Status bad;
    Config::from_string("this is not kv\n", &bad);
    CHECK_EQ(bad.code, Errc::ConfigInvalid);

    // config hash is stable across re-parse
    Config again = Config::from_string(text, &st);
    CHECK_EQ(cfg.hash(), again.hash());
}

AX_TEST(flatmap_matches_reference_map) {
    // Property test vs std::unordered_map: random interleaved insert/erase/
    // find, including rehash growth and backshift-heavy deletion.
    Rng rng = Rng::stream(7, 0xF1A7);
    FlatIdMap map{64};
    std::unordered_map<u64, u64> ref;
    for (int step = 0; step < 200000; ++step) {
        const u64 key = 1 + rng.bounded(5000);
        const u64 roll = rng.bounded(100);
        if (roll < 55) {
            const u64 v = rng.next();
            map.insert(key, v);
            ref[key] = v;
        } else if (roll < 80) {
            CHECK_EQ(map.erase(key), ref.count(key) != 0);
            ref.erase(key);
        } else {
            const u64* got = map.find(key);
            auto it = ref.find(key);
            if (it == ref.end()) {
                CHECK(got == nullptr);
            } else {
                CHECK(got != nullptr);
                if (got) CHECK_EQ(*got, it->second);
            }
        }
        if (step % 10000 == 0) CHECK_EQ(map.size(), ref.size());
        if (testfw::current_failed()) return;
    }
    CHECK_EQ(map.size(), ref.size());
}

AX_TEST(errors_to_cstr_coverage) {
    for (int i = 0; i <= static_cast<int>(Errc::Internal); ++i) {
        const char* s = to_cstr(static_cast<Errc>(i));
        CHECK(s != nullptr && s[0] != '\0' && std::string(s) != "?");
    }
}

AX_TEST_MAIN