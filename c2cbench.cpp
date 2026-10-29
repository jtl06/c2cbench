// c2cbench: core-to-core latency, per cache line.
//
// Tools like c2clat and core-to-core-latency time one shared cache line per
// core pair. On mesh and ring CPUs that line moves owner -> home slice ->
// requester, and its home comes from an undocumented address hash, so the
// "latency between two cores" depends on which line you happened to get.
// c2cbench times many lines per pair and reports the best, median and worst.
//
//   c2cbench                 all pairs of physical cores, median matrix
//   c2cbench -m chase -c 0-7 --csv   pointer chase, CPUs 0-7, one CSV row per pair
//   c2cbench -p | gnuplot -p      heatmap of the median
//
// Modes (ns per line, one way):
//   cas    both cores CAS one line in turn (a lock handoff), each retrying until
//          its turn, as c2clat -w and core-to-core-latency do: half a round trip
//   chase  the lower CPU writes a pointer chain through some of the lines, the
//          other follows it: one dependent read of a line the other core just
//          wrote, per hop. Chains of different lengths over different lines are
//          timed whole, and a least-squares fit splits them into a fixed overhead
//          plus a cost per line, so coarse timers still give per-line results.
//
// Linux, x86-64 or AArch64, C++17. Build: g++ -O2 -std=c++17 -pthread c2cbench.cpp -o c2cbench
// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
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

// Size in bytes of cpu's data/unified cache at `level` (sysfs), or 0 if unknown.
size_t cache_size(int cpu, int level) {
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
// pairs two of them), in 32 MiB of explicit 2 MiB hugepages from the reserved
// pool (vm.nr_hugepages): fixed physical pages, so a line keeps its home for the
// whole run, and no TLB misses.
std::vector<std::atomic<uint64_t> *> make_lines(size_t n, uint64_t seed) {
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
    std::vector<std::atomic<uint64_t> *> out;
    for (size_t b : blocks)
        out.push_back(new (base + b * 128) std::atomic<uint64_t>(0));
    return out;
}

struct Options {
    std::string mode = "cas";
    size_t lines = 64;
    unsigned rounds = 200, passes = 3, settle_ms = 200; // warm-up once, at the start
    uint64_t seed = 0; // 0: pick a new random seed each run (printed)
    size_t offset = 0; // chase: bytes the writer stores after each chain (pushes it out of L1)
};

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

double median(std::vector<double> v) {
    std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
    return v[v.size() / 2];
}

// cas: median one-way handoff (ns) for every line between CPUs a and b. a's
// worker drives, b's worker answers, one line at a time.
std::vector<double> measure_cas(Pool &pool, int a, int b, const std::vector<std::atomic<uint64_t> *> &lines,
                                const Options &o) {
    struct alignas(128) Ctl {
        std::atomic<uint64_t> seq{0};
        std::atomic<size_t> line{0};
        std::atomic<bool> quit{false};
    } go;
    struct alignas(128) Ack {
        std::atomic<uint64_t> seq{0};
    } ack;
    const unsigned total = o.rounds + 16; // 16 untimed warm-up round trips
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
        std::mt19937_64 rng(o.seed + unsigned(a) * 1000 + unsigned(b));
        uint64_t seq = 0;
        for (unsigned pass = 0; pass < o.passes; ++pass) {
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
                double rt = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / o.rounds;
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

// Least squares: rows of 0/1 indicators (plus a constant column) against times.
// Solves the normal equations with Gaussian elimination.
std::vector<double> fit(const std::vector<std::vector<size_t>> &rows, const std::vector<double> &t, size_t n) {
    const size_t m = n + 1; // unknowns: overhead, then one cost per line
    std::vector<double> A(m * m, 0), y(m, 0);
    for (size_t r = 0; r < rows.size(); ++r) {
        std::vector<size_t> cols{0};
        for (size_t i : rows[r])
            cols.push_back(i + 1);
        for (size_t i : cols) {
            y[i] += t[r];
            for (size_t j : cols)
                A[i * m + j] += 1;
        }
    }
    for (size_t c = 0; c < m; ++c) {
        size_t p = c;
        for (size_t r = c + 1; r < m; ++r)
            if (std::abs(A[r * m + c]) > std::abs(A[p * m + c]))
                p = r;
        if (std::abs(A[p * m + c]) < 1e-9)
            throw std::runtime_error("chase fit is singular (too few windows)");
        for (size_t j = 0; j < m; ++j)
            std::swap(A[c * m + j], A[p * m + j]);
        std::swap(y[c], y[p]);
        for (size_t r = 0; r < m; ++r)
            if (r != c) {
                double f = A[r * m + c] / A[c * m + c];
                for (size_t j = c; j < m; ++j)
                    A[r * m + j] -= f * A[c * m + j];
                y[r] -= f * y[c];
            }
    }
    std::vector<double> x(n);
    for (size_t i = 0; i < n; ++i)
        x[i] = y[i + 1] / A[(i + 1) * m + i + 1];
    return x;
}

// chase: a (the writer) stores a pointer chain through a window of the lines,
// so each sits modified in a's cache; b follows it, timing the dependent loads.
// Window sizes run from 1/8 to 1/2 of the lines; each window is timed o.passes
// times (median), and the fit splits window times into overhead + cost per line.
std::vector<double> measure_chase(Pool &pool, int a, int b, const std::vector<std::atomic<uint64_t> *> &lines,
                                  const Options &o) {
    const size_t n = lines.size();
    std::mt19937_64 rng(o.seed + unsigned(a) * 1000 + unsigned(b));
    std::vector<std::vector<size_t>> windows(std::max<size_t>(o.rounds, 4 * n)); // enough rows to fit n costs
    for (size_t w = 0; w < windows.size(); ++w) {
        size_t k = std::max<size_t>(2, n * (1 + w % 4) / 8);
        std::vector<size_t> all(n);
        std::iota(all.begin(), all.end(), 0);
        std::shuffle(all.begin(), all.end(), rng);
        windows[w].assign(all.begin(), all.begin() + long(k)); // also the chain order
    }
    struct alignas(128) Ctl {
        std::atomic<uint64_t> seq{0};
        std::atomic<uint64_t> head{0}, hops{0};
        std::atomic<bool> quit{false};
    } go;
    struct alignas(128) Ack {
        std::atomic<uint64_t> seq{0}, end{0};
        std::atomic<double> ns{0};
    } ack;
    std::vector<std::vector<double>> ns(windows.size());
    bool wrong = false;

    pool.post(b, [&] {
        uint64_t seen = 0;
        for (;;) {
            while (go.seq.load(std::memory_order_acquire) == seen)
                relax();
            seen = go.seq.load(std::memory_order_acquire);
            if (go.quit.load(std::memory_order_acquire))
                return;
            uint64_t p = go.head.load(std::memory_order_relaxed), hops = go.hops.load(std::memory_order_relaxed);
            auto t0 = Clock::now();
            for (uint64_t i = 0; i < hops; ++i) // each load needs the previous one's value
                p = reinterpret_cast<std::atomic<uint64_t> *>(p)->load(std::memory_order_relaxed);
            auto t1 = Clock::now();
            ack.end.store(p, std::memory_order_relaxed);
            ack.ns.store(std::chrono::duration<double, std::nano>(t1 - t0).count(), std::memory_order_relaxed);
            ack.seq.store(seen, std::memory_order_release);
        }
    });
    pool.post(a, [&] {
        std::vector<size_t> order(windows.size());
        std::iota(order.begin(), order.end(), 0);
        std::vector<uint64_t> spill(o.offset / 8 + 8); // a's own buffer, one store per 64 B line
        uint64_t seq = 0;
        for (unsigned pass = 0; pass < o.passes + 1; ++pass) { // pass 0 is an untimed warm-up
            std::shuffle(order.begin(), order.end(), rng);
            for (size_t w : order) {
                auto &win = windows[w];
                for (size_t i = 0; i < win.size(); ++i) // the last line points to a sentinel value
                    lines[win[i]]->store(i + 1 < win.size() ? reinterpret_cast<uint64_t>(lines[win[i + 1]]) : 1,
                                         std::memory_order_relaxed);
                volatile uint64_t *v = spill.data(); // --offset: push the chain into a's L2
                for (size_t i = 0; i + 8 <= spill.size() && o.offset; i += 8)
                    v[i] = seq;
                go.head.store(reinterpret_cast<uint64_t>(lines[win[0]]), std::memory_order_relaxed);
                go.hops.store(win.size() - 1, std::memory_order_relaxed); // ends on the last line's address
                go.seq.store(++seq, std::memory_order_release);
                while (ack.seq.load(std::memory_order_acquire) != seq)
                    relax();
                wrong |= ack.end.load(std::memory_order_relaxed) != reinterpret_cast<uint64_t>(lines[win.back()]);
                if (pass > 0)
                    ns[w].push_back(ack.ns.load(std::memory_order_relaxed));
            }
        }
        go.quit.store(true, std::memory_order_release);
        go.seq.store(++seq, std::memory_order_release);
    });
    pool.wait(a);
    pool.wait(b);
    if (wrong)
        throw std::runtime_error("chase ended on the wrong line");

    // A window of k lines makes k-1 hops: it loads every line but the last
    // (which only holds the end marker).
    std::vector<std::vector<size_t>> rows;
    std::vector<double> t;
    for (size_t w = 0; w < windows.size(); ++w) {
        rows.emplace_back(windows[w].begin(), windows[w].end() - 1);
        t.push_back(median(ns[w]));
    }
    return fit(rows, t, n);
}

double quantile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
}

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: c2cbench [-m cas|chase] [-c CPUS] [-l LINES] [-r ROUNDS] [-n PASSES] [-t] [--csv | -p] [-s SEED]\n"
                 "  -m      cas: both cores CAS one line in turn (default)\n"
                 "          chase: the lower CPU writes a pointer chain, the other follows it\n"
                 "  -c      CPUs to use, e.g. 0-7,16 (default: one CPU per physical core)\n"
                 "  -l      cache lines measured per pair (default 64)\n"
                 "  -r, -n  cas: round trips per line per pass (200), passes (3)\n"
                 "          chase: chain windows (200, at least 4 per line), times each is timed (3)\n"
                 "  --offset SIZE  chase: the writer stores SIZE bytes of its own after each chain,\n"
                 "          pushing the chain from its L1 to its L2 (L2 -> L1 instead of L1 -> L1);\n"
                 "          pick a size between the L1d and L2 sizes, e.g. 2x L1d (default 0)\n"
                 "  -s      seed that picks the lines (default: new each run, printed);\n"
                 "          the same seed reproduces a run's line set\n"
                 "  -t      include SMT siblings (with the default CPU list)\n"

                 "  --csv   one row per pair: a,b,mode,best,p10,median,p90,worst (ns),seed\n"
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
        else if (k == "--offset") {
            std::string v = val();
            size_t mult = 1;
            if (!v.empty() && (v.back() == 'K' || v.back() == 'k')) mult = 1024, v.pop_back();
            else if (!v.empty() && (v.back() == 'M' || v.back() == 'm')) mult = 1024 * 1024, v.pop_back();
            o.offset = std::stoull(v) * mult;
        }
        else if (k == "-t") smt = true;
        else if (k == "--csv") out = "csv";
        else if (k == "-p") out = "plot";
        else usage();
    }
    if ((o.mode != "cas" && o.mode != "chase") || o.lines < 2 || o.rounds == 0 || o.passes == 0)
        usage();
    if (o.seed == 0) {
        std::random_device rd;
        o.seed = 1 + (uint64_t(rd()) << 32 | rd()) % 1000000000;
    }

    std::vector<int> cpus = cpus_spec.empty() ? usable_cpus(!smt) : parse_cpus(cpus_spec);
    if (cpus.size() < 2)
        throw std::runtime_error("need at least two CPUs");
    if (o.mode == "chase" && o.offset) {
        size_t l1 = cache_size(cpus[0], 1), l2 = cache_size(cpus[0], 2);
        if (l1 && o.offset < l1)
            std::fprintf(stderr, "c2cbench: --offset %zu < L1d (%zu): the chain stays in the writer's L1\n", o.offset, l1);
        if (l2 && o.offset >= l2)
            std::fprintf(stderr, "c2cbench: --offset %zu >= L2 (%zu): the chain leaves the writer's L2, so reads "
                                 "become shared-cache hits, not core-to-core transfers\n", o.offset, l2);
    }
    auto lines = make_lines(o.lines, o.seed);
    Pool pool(cpus);
    // Let every worker spin before the first pair so idle cores reach their clock.
    std::this_thread::sleep_for(std::chrono::milliseconds(o.settle_ms));

    struct Row {
        int a, b;
        double best, p10, med, p90, worst;
    };
    std::vector<Row> rows;
    if (out == "csv")
        std::printf("a,b,mode,best_ns,p10_ns,median_ns,p90_ns,worst_ns,seed\n");
    for (size_t i = 0; i < cpus.size(); ++i)
        for (size_t j = i + 1; j < cpus.size(); ++j) {
            std::vector<double> v = o.mode == "cas" ? measure_cas(pool, cpus[i], cpus[j], lines, o)
                                                    : measure_chase(pool, cpus[i], cpus[j], lines, o);
            Row r{cpus[i], cpus[j], quantile(v, 0), quantile(v, 0.1), quantile(v, 0.5), quantile(v, 0.9), quantile(v, 1)};
            rows.push_back(r);
            if (out == "csv") {
                std::printf("%d,%d,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%llu\n", r.a, r.b, o.mode.c_str(), r.best, r.p10, r.med, r.p90,
                            r.worst, (unsigned long long)o.seed);
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
            std::printf("set title \"c2cbench %s: %s one-way (ns)\"\nset xlabel \"CPU\"\nset ylabel \"CPU\"\n"
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
        std::printf("c2cbench %s: median line, one-way ns (%zu lines per pair)\n     ", o.mode.c_str(), lines.size());
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
        std::fprintf(out == "plot" ? stderr : stdout, "\nseed %llu (reproduce with -s %llu)",
                     (unsigned long long)o.seed, (unsigned long long)o.seed);
        if (o.mode == "chase")
            std::fprintf(out == "plot" ? stderr : stdout, "; writer offset %zu B", o.offset);
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
