// cas: both cores CAS one line in turn (a lock handoff), each retrying until its
// turn, as c2clat -w and core-to-core-latency do. Half a round trip, timed per
// line, so it shows how the line (its home) changes the time.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "harness.hpp"
#include <numeric>

namespace c2c {

// Median one-way handoff (ns) for every line between CPUs a and b, over `passes`
// passes of `rounds` round trips each. a's worker drives, b's worker answers,
// one line at a time, in a new random order every pass.
inline std::vector<double> measure_cas(Pool &pool, int a, int b, const Lines &lines, unsigned rounds, unsigned passes,
                                       uint64_t seed) {
    struct alignas(128) Ctl {
        std::atomic<uint64_t> seq{0};
        std::atomic<size_t> line{0};
        std::atomic<bool> quit{false};
    } go;
    struct alignas(128) Ack {
        std::atomic<uint64_t> seq{0};
    } ack;
    const unsigned total = rounds + 16; // 16 untimed warm-up round trips
    std::vector<std::vector<double>> ns(lines.size());

    pool.post(b, [&] {
        uint64_t seen = 0;
        for (;;) {
            while (go.seq.load(std::memory_order_acquire) == seen)
                relax();
            seen = go.seq.load(std::memory_order_acquire);
            if (go.quit.load(std::memory_order_acquire))
                return;
            auto *x = lines[go.line.load(std::memory_order_relaxed)];
            ack.seq.store(seen, std::memory_order_release);
            for (uint64_t r = 0; r < total; ++r) // retry until a's CAS lands, then ours does
                for (uint64_t want = 2 * r + 1; !x->compare_exchange_strong(want, want + 1, std::memory_order_acq_rel);)
                    want = 2 * r + 1;
        }
    });
    pool.post(a, [&] {
        std::vector<size_t> order(lines.size());
        std::iota(order.begin(), order.end(), 0);
        std::mt19937_64 rng(seed + unsigned(a) * 1000 + unsigned(b));
        uint64_t seq = 0;
        for (unsigned pass = 0; pass < passes; ++pass) {
            std::shuffle(order.begin(), order.end(), rng);
            for (size_t j : order) {
                auto *x = lines[j];
                x->store(0, std::memory_order_relaxed);
                go.line.store(j, std::memory_order_relaxed);
                go.seq.store(++seq, std::memory_order_release);
                while (ack.seq.load(std::memory_order_acquire) != seq)
                    relax();
                Clock::time_point t0;
                for (uint64_t r = 0; r < total; ++r) {
                    if (r == 16)
                        t0 = Clock::now();
                    for (uint64_t want = 2 * r; !x->compare_exchange_strong(want, want + 1, std::memory_order_acq_rel);)
                        want = 2 * r; // retry, as c2clat -w and core-to-core-latency do
                }
                while (x->load(std::memory_order_acquire) != 2 * total) // b's last CAS
                    relax();
                double rt = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / rounds;
                ns[j].push_back(rt / 2);
            }
        }
        go.quit.store(true, std::memory_order_release);
        go.seq.store(++seq, std::memory_order_release);
    });
    pool.wait(a);
    pool.wait(b);

    std::vector<double> med;
    for (auto &v : ns)
        med.push_back(median(v));
    return med;
}

} // namespace c2c
