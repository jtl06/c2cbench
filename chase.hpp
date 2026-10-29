// chase: the lower CPU writes a pointer chain, the other follows it: one dependent
// read of a line the other core just wrote, per hop. Every chain is made of lines
// no core has loaded before in this run (drawn from a large pool), so no
// prefetcher can have learned them; some recent Arm cores learn chains they have
// seen and fetch ahead. The result is a mean over many fresh lines, not per-line
// values.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "harness.hpp"
#include <cmath>

namespace c2c {

// Chains of kChain lines (16 KB: in every L1), drawn from kPool fresh lines.
const size_t kChain = 256, kPool = 100000, kWarmChains = 4;

// Fresh lines for the chase. Each pair takes the next lines of a shuffled pool;
// when the pool runs out it is reshuffled. A prefetcher that learned "B follows A"
// from an earlier chain then finds B in the current chain with probability about
// kChain / kPool (0.3%), so it cannot help.
class Fresh {
  public:
    Fresh(Lines lines, uint64_t seed) : lines_(std::move(lines)), rng_(seed) {}
    Lines take(size_t n) {
        if (next_ + n > lines_.size())
            std::shuffle(lines_.begin(), lines_.end(), rng_), next_ = 0;
        next_ += n;
        return {lines_.begin() + long(next_ - n), lines_.begin() + long(next_)};
    }

  private:
    Lines lines_;
    std::mt19937_64 rng_;
    size_t next_ = 0;
};

// One chase between a writer core and a reader core. Post reader() to the
// reader's worker; on the writer's worker, link() a chain (each line then sits
// modified in the writer's cache), optionally do more work, then walk() it: the
// reader follows the chain and walk() returns the reader's time in ns. stop()
// ends the reader. Used by measure_chase and by research tools.
class Walker {
  public:
    // Point each line at the next; the last holds a sentinel value.
    static void link(const Lines &ch) {
        for (size_t i = 0; i < ch.size(); ++i)
            ch[i]->store(i + 1 < ch.size() ? reinterpret_cast<uint64_t>(ch[i + 1]) : 1, std::memory_order_relaxed);
    }
    // Writer: have the reader follow ch (already linked) from its first line to its last.
    double walk(const Lines &ch) {
        go_.head.store(reinterpret_cast<uint64_t>(ch[0]), std::memory_order_relaxed);
        go_.hops.store(ch.size() - 1, std::memory_order_relaxed); // ends on the last line's address
        go_.seq.store(++seq_, std::memory_order_release);
        while (ack_.seq.load(std::memory_order_acquire) != seq_)
            relax();
        wrong_ |= ack_.end.load(std::memory_order_relaxed) != reinterpret_cast<uint64_t>(ch.back());
        return ack_.ns.load(std::memory_order_relaxed);
    }
    void stop() {
        go_.quit.store(true, std::memory_order_release);
        go_.seq.store(++seq_, std::memory_order_release);
    }
    // Reader: follow each chain handed over, timing the dependent loads (one clock
    // read around the whole walk: under 1% overhead at 255 hops), until stop().
    void reader() {
        uint64_t seen = 0;
        for (;;) {
            while (go_.seq.load(std::memory_order_acquire) == seen)
                relax();
            seen = go_.seq.load(std::memory_order_acquire);
            if (go_.quit.load(std::memory_order_acquire))
                return;
            uint64_t p = go_.head.load(std::memory_order_relaxed), hops = go_.hops.load(std::memory_order_relaxed);
            auto t0 = Clock::now();
            for (uint64_t i = 0; i < hops; ++i) // each load needs the previous one's value
                p = reinterpret_cast<std::atomic<uint64_t> *>(p)->load(std::memory_order_relaxed);
            auto t1 = Clock::now();
            ack_.end.store(p, std::memory_order_relaxed);
            ack_.ns.store(std::chrono::duration<double, std::nano>(t1 - t0).count(), std::memory_order_relaxed);
            ack_.seq.store(seen, std::memory_order_release);
        }
    }
    // True if any walk ended somewhere other than its chain's last line.
    bool wrong() const { return wrong_; }

  private:
    struct alignas(128) Go {
        std::atomic<uint64_t> seq{0}, head{0}, hops{0};
        std::atomic<bool> quit{false};
    } go_;
    struct alignas(128) Ack {
        std::atomic<uint64_t> seq{0}, end{0};
        std::atomic<double> ns{0};
    } ack_;
    uint64_t seq_ = 0; // writer only
    bool wrong_ = false;
};

// a (the writer) links a chain of fresh lines; b follows it. `lines` per pair, in
// chains of kChain; with `offset`, a stores that many bytes of its own after
// linking each chain, pushing the chain into its L2. Returns ns per hop for each
// timed chain, without chains far above the pair's median (a disturbance: an
// interrupt during its ~15 us); *dropped counts those.
inline std::vector<double> measure_chase(Pool &pool, int a, int b, Fresh &fresh, size_t lines, size_t offset,
                                         size_t *dropped) {
    std::vector<Lines> chains(kWarmChains + std::max<size_t>(1, lines / kChain));
    for (auto &c : chains)
        c = fresh.take(kChain);
    Walker walker;
    std::vector<double> per_hop;
    pool.post(b, [&] { walker.reader(); });
    pool.post(a, [&] {
        std::vector<uint64_t> spill(offset / 8 + 8); // a's own buffer, one store per 64 B line
        for (size_t w = 0; w < chains.size(); ++w) { // the first kWarmChains are untimed
            Walker::link(chains[w]);
            volatile uint64_t *v = spill.data(); // offset: push the chain into a's L2
            for (size_t i = 0; i + 8 <= spill.size() && offset; i += 8)
                v[i] = w;
            double ns = walker.walk(chains[w]);
            if (w >= kWarmChains)
                per_hop.push_back(ns / double(kChain - 1));
        }
        walker.stop();
    });
    pool.wait(a);
    pool.wait(b);
    if (walker.wrong())
        throw std::runtime_error("chase ended on the wrong line");

    const double m = median(per_hop);
    std::vector<double> dev;
    for (double x : per_hop)
        dev.push_back(std::abs(x - m));
    const double cut = m + 5 * std::max(median(dev), 0.5);
    std::vector<double> kept;
    for (double x : per_hop)
        if (x <= cut)
            kept.push_back(x);
    *dropped += per_hop.size() - kept.size();
    return kept;
}

} // namespace c2c
