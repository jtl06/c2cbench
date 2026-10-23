// c2cbench: core-to-core latency, per cache line.
//
// Tools like c2clat and core-to-core-latency time one shared cache line per
// core pair. On mesh and ring CPUs that line moves owner -> home slice ->
// requester, and its home comes from an undocumented address hash, so the
// "latency between two cores" depends on which line you happened to get.
// c2cbench times many lines per pair and reports the best, median and worst.
//
//   c2cbench                 all pairs of physical cores, median matrix
//   c2cbench -m rw -c 0-7 --csv   handshake mode, CPUs 0-7, one CSV row per pair
//   c2cbench -p | gnuplot -p      heatmap of the median
//
// Modes:
//   cas  both cores CAS one line in turn (lock handoff)
//   rw   core a writes line x and b reads it, then b writes line y and a reads
//        it (one writer per line, like an SP/SC queue's indices)
// Reported per pair: one-way handoff in ns (half a round trip), over the lines.
//
// Linux, x86-64 or AArch64, C++17. Build: g++ -O2 -std=c++17 -pthread c2cbench.cpp -o c2cbench
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
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

namespace {

using Clock = std::chrono::steady_clock;

inline void relax() {
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

void pin(int cpu) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof s, &s) != 0)
        throw std::runtime_error("cannot pin to CPU " + std::to_string(cpu));
}

std::string slurp(const std::string &p) {
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    return s;
}

// CPUs we may use; with physical_only, one per core (the lowest sibling).
std::vector<int> usable_cpus(bool physical_only) {
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

// "0-3,8,10-11" -> {0,1,2,3,8,10,11}
std::vector<int> parse_cpus(const std::string &spec) {
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

// Candidate lines: one per 128 B block (so the adjacent-line prefetcher never
// pairs two of them), spread over a hugepage-backed region.
std::vector<std::atomic<uint64_t> *> make_lines(size_t n, uint64_t seed) {
    const size_t bytes = size_t(32) << 20, huge = size_t(2) << 20;
    void *raw = mmap(nullptr, bytes + huge, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        throw std::runtime_error("mmap failed");
    auto *base = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(raw) + huge - 1) & ~uintptr_t(huge - 1));
    madvise(base, bytes, MADV_HUGEPAGE);
    for (size_t i = 0; i < bytes; i += 4096)
        base[i] = 0;
    mlock(base, bytes); // best effort; keeps the lines at the same physical address
    std::mt19937_64 rng(seed);
    std::set<size_t> blocks;
    while (blocks.size() < n)
        blocks.insert(rng() % (bytes / 128));
    std::vector<std::atomic<uint64_t> *> out;
    for (size_t b : blocks)
        out.push_back(new (base + b * 128) std::atomic<uint64_t>(0));
    return out;
}

struct Options {
    std::string mode = "cas";
    size_t lines = 64;
    unsigned rounds = 200, passes = 3, settle_ms = 20;
    uint64_t seed = 1;
};

// Median one-way handoff (ns) for every candidate between CPUs a and b. A
// partner thread on b serves one job (a line, or a pair of lines) at a time.
std::vector<double> measure_pair(int a, int b, const std::vector<std::atomic<uint64_t> *> &lines, const Options &o) {
    const bool rw = o.mode == "rw";
    const size_t jobs = rw ? lines.size() / 2 : lines.size();
    struct alignas(128) Ctl {
        std::atomic<uint64_t> seq{0};
        std::atomic<size_t> job{0};
        std::atomic<bool> quit{false};
    } go;
    struct alignas(128) Ack {
        std::atomic<uint64_t> seq{0};
    } ack;
    auto x_of = [&](size_t j) { return rw ? lines[2 * j] : lines[j]; };
    auto y_of = [&](size_t j) { return rw ? lines[2 * j + 1] : lines[j]; };
    const unsigned total = o.rounds + 16; // 16 untimed warm-up round trips

    std::thread partner([&] {
        pin(b);
        uint64_t seen = 0;
        for (;;) {
            while (go.seq.load(std::memory_order_acquire) == seen)
                relax();
            seen = go.seq.load(std::memory_order_acquire);
            if (go.quit.load(std::memory_order_acquire))
                return;
            auto *x = x_of(go.job.load(std::memory_order_relaxed)), *y = y_of(go.job.load(std::memory_order_relaxed));
            ack.seq.store(seen, std::memory_order_release);
            for (uint64_t r = 0; r < total; ++r) {
                if (rw) { // wait for a's write to x, answer on y
                    while (x->load(std::memory_order_acquire) != r + 1)
                        relax();
                    y->store(r + 1, std::memory_order_release);
                } else {  // wait for a's CAS, answer with our own
                    uint64_t want = 2 * r + 1;
                    while (x->load(std::memory_order_acquire) != want)
                        relax();
                    x->compare_exchange_strong(want, want + 1, std::memory_order_acq_rel);
                }
            }
        }
    });
    pin(a);
    // Spin both cores first so idle cores are not timed before they ramp up.
    for (auto end = Clock::now() + std::chrono::milliseconds(o.settle_ms); Clock::now() < end;)
        relax();

    std::vector<std::vector<double>> ns(jobs);
    std::vector<size_t> order(jobs);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937_64 rng(o.seed + unsigned(a) * 1000 + unsigned(b));
    uint64_t seq = 0;
    for (unsigned pass = 0; pass < o.passes; ++pass) {
        std::shuffle(order.begin(), order.end(), rng);
        for (size_t j : order) {
            auto *x = x_of(j), *y = y_of(j);
            x->store(0, std::memory_order_relaxed);
            y->store(0, std::memory_order_relaxed);
            go.job.store(j, std::memory_order_relaxed);
            go.seq.store(++seq, std::memory_order_release);
            while (ack.seq.load(std::memory_order_acquire) != seq)
                relax();
            Clock::time_point t0;
            for (uint64_t r = 0; r < total; ++r) {
                if (r == 16)
                    t0 = Clock::now();
                if (rw) {
                    x->store(r + 1, std::memory_order_release);
                    while (y->load(std::memory_order_acquire) != r + 1)
                        relax();
                } else {
                    uint64_t want = 2 * r;
                    x->compare_exchange_strong(want, want + 1, std::memory_order_acq_rel);
                    while (x->load(std::memory_order_acquire) != 2 * r + 2)
                        relax();
                }
            }
            double rt = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / o.rounds;
            ns[j].push_back(rt / 2);
        }
    }
    go.quit.store(true, std::memory_order_release);
    go.seq.store(++seq, std::memory_order_release);
    partner.join();

    std::vector<double> med;
    for (auto &v : ns) {
        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
        med.push_back(v[v.size() / 2]);
    }
    return med;
}

double quantile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
}

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: c2cbench [-m cas|rw] [-c CPUS] [-l LINES] [-r ROUNDS] [-n PASSES] [-t] [--csv | -p] [-s SEED]\n"
                 "  -m      cas: both cores CAS one line (default); rw: one writer per line\n"
                 "  -c      CPUs to use, e.g. 0-7,16 (default: one CPU per physical core)\n"
                 "  -l      cache lines measured per pair (default 64)\n"
                 "  -r, -n  round trips per line per pass (200), passes (3)\n"
                 "  -t      include SMT siblings (with the default CPU list)\n"
                 "  --csv   one row per pair: a,b,mode,best,p10,median,p90,worst (ns)\n"
                 "  -p      gnuplot script: heatmaps of the median and the best line\n"
                 "Default output: matrix of the median line's one-way handoff (ns), then\n"
                 "a summary of how much the line (its home slice) changes each pair.\n");
    std::exit(2);
}

} // namespace

int main(int argc, char **argv) try {
    Options o;
    std::string cpus_spec, out = "matrix";
    bool smt = false;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto val = [&] {
            if (i + 1 >= argc)
                usage();
            return std::string(argv[++i]);
        };
        if (k == "-m") o.mode = val();
        else if (k == "-c") cpus_spec = val();
        else if (k == "-l") o.lines = std::stoul(val());
        else if (k == "-r") o.rounds = unsigned(std::stoul(val()));
        else if (k == "-n") o.passes = unsigned(std::stoul(val()));
        else if (k == "-s") o.seed = std::stoull(val());
        else if (k == "-t") smt = true;
        else if (k == "--csv") out = "csv";
        else if (k == "-p") out = "plot";
        else usage();
    }
    if ((o.mode != "cas" && o.mode != "rw") || o.lines < 2 || o.rounds == 0 || o.passes == 0)
        usage();

    std::vector<int> cpus = cpus_spec.empty() ? usable_cpus(!smt) : parse_cpus(cpus_spec);
    if (cpus.size() < 2)
        throw std::runtime_error("need at least two CPUs");
    auto lines = make_lines(o.lines, o.seed);

    struct Row {
        int a, b;
        double best, p10, med, p90, worst;
    };
    std::vector<Row> rows;
    if (out == "csv")
        std::printf("a,b,mode,best_ns,p10_ns,median_ns,p90_ns,worst_ns\n");
    for (size_t i = 0; i < cpus.size(); ++i)
        for (size_t j = i + 1; j < cpus.size(); ++j) {
            std::vector<double> v;
            std::thread([&] { v = measure_pair(cpus[i], cpus[j], lines, o); }).join();
            Row r{cpus[i], cpus[j], quantile(v, 0), quantile(v, 0.1), quantile(v, 0.5), quantile(v, 0.9), quantile(v, 1)};
            rows.push_back(r);
            if (out == "csv") {
                std::printf("%d,%d,%s,%.1f,%.1f,%.1f,%.1f,%.1f\n", r.a, r.b, o.mode.c_str(), r.best, r.p10, r.med, r.p90,
                            r.worst);
                std::fflush(stdout);
            }
        }

    auto cell = [&](int a, int b, double Row::*f) {
        for (auto &r : rows)
            if ((r.a == a && r.b == b) || (r.a == b && r.b == a))
                return r.*f;
        return 0.0;
    };
    if (out == "plot") {
        for (auto [title, f] : {std::pair{"median line", &Row::med}, std::pair{"best line", &Row::best}}) {
            std::printf("set title \"c2cbench %s: %s one-way handoff (ns)\"\nset xlabel \"CPU\"\nset ylabel \"CPU\"\n"
                        "set palette defined (0 '#1a9850', 1 '#fee08b', 2 '#d73027')\nplot '-' matrix rowheaders "
                        "columnheaders with image notitle\n",
                        o.mode.c_str(), title);
            std::printf("x");
            for (int c : cpus)
                std::printf(" %d", c);
            std::printf("\n");
            for (int a : cpus) {
                std::printf("%d", a);
                for (int b : cpus)
                    a == b ? std::printf(" NaN") : std::printf(" %.1f", cell(a, b, f));
                std::printf("\n");
            }
            std::printf("e\npause mouse close\n");
        }
    } else if (out == "matrix") {
        std::printf("c2cbench %s: median line, one-way handoff ns (%zu lines per pair)\n     ", o.mode.c_str(), lines.size());
        for (int c : cpus)
            std::printf("%5d", c);
        std::printf("\n");
        for (int a : cpus) {
            std::printf("%5d", a);
            for (int b : cpus)
                a == b ? std::printf("%5s", "") : std::printf("%5.0f", cell(a, b, &Row::med));
            std::printf("\n");
        }
    }
    if (out != "csv") {
        std::vector<double> spread, gain;
        for (auto &r : rows) {
            spread.push_back(r.worst - r.best);
            gain.push_back((r.med - r.best) / r.med);
        }
        std::fprintf(out == "plot" ? stderr : stdout,
                     "\nper pair, over %zu lines: worst - best = %.1f ns (median over pairs), up to %.1f ns;\n"
                     "best line vs median line: %.0f%% faster (median over pairs), up to %.0f%%\n",
                     lines.size(), quantile(spread, 0.5), quantile(spread, 1), 100 * quantile(gain, 0.5), 100 * quantile(gain, 1));
    }
    return 0;
} catch (const std::exception &e) {
    std::fprintf(stderr, "c2cbench: %s\n", e.what());
    return 1;
}
