// c2cbench harness: CPUs, pinned workers, the lines under test, statistics.
// The benchmarks (cas.hpp, chase.hpp) and other tools build on this.
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <pthread.h>
#include <random>
#include <sched.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <vector>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace c2c {

using Clock = std::chrono::steady_clock;
using Lines = std::vector<std::atomic<uint64_t> *>;

inline void relax() {
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

inline void pin(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof s, &s) != 0)
        throw std::runtime_error("cannot pin to CPU " + std::to_string(cpu));
}

inline std::string slurp(const std::string &p) {
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    return s;
}

// CPUs we may use; with physical_only, one per core (the lowest sibling).
inline std::vector<int> usable_cpus(bool physical_only) {
    cpu_set_t s;
    CPU_ZERO(&s);
    sched_getaffinity(0, sizeof s, &s);
    std::vector<int> out;
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        if (!CPU_ISSET(c, &s))
            continue;
        if (physical_only) {
            std::string sib = slurp("/sys/devices/system/cpu/cpu" + std::to_string(c) + "/topology/thread_siblings_list");
            if (!sib.empty() && std::stoi(sib) != c)
                continue;
        }
        out.push_back(c);
    }
    return out;
}

// Size in bytes of cpu's data/unified cache at `level` (sysfs), or 0 if unknown.
inline size_t cache_size(int cpu, int level) {
    for (int i = 0; i < 8; ++i) {
        std::string d = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/index" + std::to_string(i) + "/";
        if (slurp(d + "level") == std::to_string(level) && slurp(d + "type") != "Instruction") {
            std::string v = slurp(d + "size");
            if (v.empty())
                return 0;
            size_t n = std::stoull(v);
            return v.back() == 'K' ? n << 10 : v.back() == 'M' ? n << 20 : n;
        }
    }
    return 0;
}

// "0-3,8,10-11" -> {0,1,2,3,8,10,11}
inline std::vector<int> parse_cpus(const std::string &spec) {
    std::vector<int> out;
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto d = item.find('-');
        int lo = std::stoi(item.substr(0, d)), hi = d == std::string::npos ? lo : std::stoi(item.substr(d + 1));
        for (int c = lo; c <= hi; ++c)
            out.push_back(c);
    }
    return out;
}

// Candidate lines: one per 128 B block, at a random half of it (so the
// adjacent-line prefetcher never pairs two of them, and the lines use every L1
// set), in 32 MiB of explicit 2 MiB hugepages from the reserved pool
// (vm.nr_hugepages): fixed physical pages, so a line keeps its home for the whole
// run, and 16 pages, which the first-level TLB covers (no TLB misses).
inline Lines make_lines(size_t n, uint64_t seed) {
    const size_t bytes = size_t(32) << 20;
    void *raw = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (raw == MAP_FAILED)
        throw std::runtime_error("needs 16 free 2 MiB hugepages (reserve them: sudo sysctl vm.nr_hugepages=16)");
    auto *base = static_cast<uint8_t *>(raw);
    for (size_t i = 0; i < bytes; i += 4096)
        base[i] = 0;
    std::mt19937_64 rng(seed);
    std::set<size_t> blocks;
    while (blocks.size() < n)
        blocks.insert(rng() % (bytes / 128));
    Lines out;
    for (size_t b : blocks)
        out.push_back(new (base + b * 128 + 64 * (rng() & 1)) std::atomic<uint64_t>(0));
    std::shuffle(out.begin(), out.end(), rng);
    return out;
}

// One pinned worker per CPU, started once and spinning while idle, so every
// core stays at its working clock for the whole run (no per-pair warm-up).
class Pool {
  public:
    explicit Pool(const std::vector<int> &cpus) {
        for (int c : cpus)
            slots_[c] = std::make_unique<Slot>();
        for (auto &[c, s] : slots_)
            s->thread = std::thread(run, c, s.get());
    }
    ~Pool() {
        for (auto &[c, s] : slots_) {
            s->quit.store(true, std::memory_order_release);
            s->seq.fetch_add(1, std::memory_order_release);
        }
        for (auto &[c, s] : slots_)
            s->thread.join();
    }
    // Run f on cpu's worker; returns at once.
    void post(int cpu, std::function<void()> f) {
        Slot &s = *slots_.at(cpu);
        s.task = std::move(f);
        s.seq.fetch_add(1, std::memory_order_release);
    }
    void wait(int cpu) {
        Slot &s = *slots_.at(cpu);
        while (s.done.load(std::memory_order_acquire) != s.seq.load(std::memory_order_relaxed))
            relax();
    }

  private:
    struct alignas(128) Slot {
        std::atomic<uint64_t> seq{0}, done{0};
        std::atomic<bool> quit{false};
        std::function<void()> task;
        std::thread thread;
    };
    static void run(int cpu, Slot *s) {
        pin(cpu);
        uint64_t seen = 0;
        for (;;) {
            while (s->seq.load(std::memory_order_acquire) == seen)
                relax();
            seen = s->seq.load(std::memory_order_acquire);
            if (s->quit.load(std::memory_order_acquire))
                return;
            s->task();
            s->done.store(seen, std::memory_order_release);
        }
    }
    std::map<int, std::unique_ptr<Slot>> slots_;
};

inline double median(std::vector<double> v) {
    std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
    return v[v.size() / 2];
}

inline double quantile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
}

} // namespace c2c
