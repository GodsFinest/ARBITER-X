#include "testfw.hpp"

#include "arbiter/spsc.hpp"

#include <atomic>
#include <stdexcept>
#include <thread>

using namespace arbiter;

AX_TEST(spsc_single_thread_push_pop) {
    SpscRing<u64> q{1024};
    CHECK(q.push(1));
    CHECK(q.push(2));
    u64 v = 0;
    CHECK(q.pop(v));
    CHECK_EQ(v, 1u);
    CHECK(q.pop(v));
    CHECK_EQ(v, 2u);
    CHECK(!q.pop(v));
    // fill to capacity
    for (u64 i = 0; i < 1024; ++i) CHECK(q.push(i));
    CHECK(!q.push(9999));  // full
    for (u64 i = 0; i < 1024; ++i) {
        CHECK(q.pop(v));
        CHECK_EQ(v, i);
    }
    CHECK(!q.pop(v));
}

AX_TEST(spsc_rejects_invalid_capacity) {
    bool rejected_zero = false;
    bool rejected_non_power_of_two = false;
    try {
        SpscRing<u64> invalid{0};
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected_zero = true;
    }
    try {
        SpscRing<u64> invalid{3};
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected_non_power_of_two = true;
    }
    CHECK(rejected_zero);
    CHECK(rejected_non_power_of_two);
}

AX_TEST(spsc_two_threads_transfer_million) {
    SpscRing<u64> q{4096};
    constexpr u64 kN = 1'000'000;
    std::atomic<u64> checksum{0};
    std::atomic<bool> fifo_ok{true};
    std::thread producer([&] {
        for (u64 i = 1; i <= kN; ++i) {
            while (!q.push(i)) {
                // spin; a real system would back off
            }
        }
    });
    std::thread consumer([&] {
        u64 expected = 1, sum = 0, v;
        while (expected <= kN) {
            if (q.pop(v)) {
                if (v != expected) fifo_ok.store(false, std::memory_order_relaxed);
                sum += v;
                ++expected;
            }
        }
        checksum.store(sum, std::memory_order_release);
    });
    producer.join();
    consumer.join();
    CHECK(fifo_ok.load(std::memory_order_relaxed));
    CHECK_EQ(checksum.load(), kN * (kN + 1) / 2);
}

AX_TEST_MAIN