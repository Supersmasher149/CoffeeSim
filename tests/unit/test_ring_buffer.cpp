#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "espressolab/ring_buffer.hpp"

// Issue #38: the bounded MPSC queue behind the sweep batch runner's tunable
// ring capacity. Covered here in isolation from any sweep/simulation
// machinery so these assertions are about the queue's own contract.
using namespace espressolab;

TEST_CASE("a zero capacity is rejected", "[unit]") {
    REQUIRE_THROWS_AS(BoundedRingBuffer<int>(0), std::invalid_argument);
}

TEST_CASE("capacity bounds how many items can be pending at once", "[unit]") {
    BoundedRingBuffer<int> ring(2);
    ring.push(1);
    ring.push(2);
    REQUIRE(ring.size() == 2);

    std::atomic<bool> third_pushed{false};
    std::thread producer([&] {
        ring.push(3);  // must block: the ring is already at capacity
        third_pushed.store(true);
    });

    // Give the producer thread every chance to run; it must still be
    // blocked, since nothing has been popped yet.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE_FALSE(third_pushed.load());
    REQUIRE(ring.size() == 2);

    int out = 0;
    REQUIRE(ring.pop(out));
    REQUIRE(out == 1);

    producer.join();
    REQUIRE(third_pushed.load());
    REQUIRE(ring.size() == 2);
}

TEST_CASE("multiple producers and one consumer see every value exactly once", "[unit]") {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kPerProducer = 250;
    BoundedRingBuffer<int> ring(8);

    std::atomic<std::size_t> producers_remaining{kProducers};
    std::vector<std::thread> producers;
    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (std::size_t i = 0; i < kPerProducer; ++i) {
                ring.push(static_cast<int>(p * kPerProducer + i));
            }
            // Only the last producer to finish closes the ring, so the
            // consumer never sees end-of-stream while another producer is
            // still pushing.
            if (producers_remaining.fetch_sub(1) == 1) ring.close();
        });
    }

    std::vector<int> drained;
    int value = 0;
    while (ring.pop(value)) drained.push_back(value);

    for (auto& producer : producers) producer.join();

    REQUIRE(drained.size() == kProducers * kPerProducer);
    std::sort(drained.begin(), drained.end());
    for (std::size_t i = 0; i < drained.size(); ++i) {
        REQUIRE(drained[i] == static_cast<int>(i));
    }
}

TEST_CASE("pop returns false only once the buffer is closed and drained", "[unit]") {
    BoundedRingBuffer<int> ring(4);
    ring.push(1);
    ring.push(2);
    ring.close();

    int out = 0;
    REQUIRE(ring.pop(out));
    REQUIRE(out == 1);
    REQUIRE(ring.pop(out));
    REQUIRE(out == 2);
    REQUIRE_FALSE(ring.pop(out));  // drained and closed
}
