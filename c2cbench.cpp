// c2cbench: core-to-core latency, per cache line.
//
// Tools like c2clat and core-to-core-latency time one shared cache line per
// core pair. On mesh and ring CPUs that line moves owner -> home slice ->
// requester, and its home comes from an undocumented address hash, so the
// "latency between two cores" depends on which line you happened to get.
// c2cbench times many lines per pair and reports the mean over lines.
//
//   c2cbench                 all pairs of physical cores, matrix of the mean over lines
//   c2cbench -m chase -c 0-7 --csv   pointer chase, CPUs 0-7, one CSV row per pair
//   c2cbench -c 0,1 --hist   cas, histogram of the per-line times (one mode per home distance)
//   c2cbench -p | gnuplot -p      heatmaps
//
// Modes (ns, one way):
//   cas    both cores CAS one line in turn (a lock handoff), each retrying until
//          its turn, as c2clat -w and core-to-core-latency do: half a round trip.
//          Timed per line, so it shows how the line (its home) changes the time.
//   chase  the lower CPU writes a pointer chain, the other follows it: one dependent
//          read of a line the other core just wrote, per hop. Every chain is made of
//          lines no core has loaded before in this run (drawn from a large pool), so
//          no prefetcher can have learned them; some recent Arm cores learn chains
//          they have seen and fetch ahead. The result is a mean over many fresh
//          lines, not per-line values.
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

// Candidate lines: one per 128 B block, at a random half of it (so the
// adjacent-line prefetcher never pairs two of them, and the lines use every L1
// set), in 32 MiB of explicit 2 MiB hugepages from the reserved pool
// (vm.nr_hugepages): fixed physical pages, so a line keeps its home for the whole
// run, and 16 pages, which the first-level TLB covers (no TLB misses).
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
        out.push_back(new (base + b * 128 + 64 * (rng() & 1)) std::atomic<uint64_t>(0));
    std::shuffle(out.begin(), out.end(), rng);
    return out;
}

struct Options {
    std::string mode = "cas";
    size_t lines = 0; // per pair; 0: 256 (cas), 8192 (chase)
    unsigned rounds = 200, passes = 3, settle_ms = 200; // warm-up once, at the start
    uint64_t seed = 0; // 0: pick a new random seed each run (printed)
    size_t offset = 0; // chase: bytes the writer stores after each chain (pushes it out of L1)
};

// chase: chains of kChain lines (16 KB: in every L1), drawn from kPool fresh lines.
const size_t kChain = 256, kPool = 100000, kWarmChains = 4;

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

// Fresh lines for the chase. Each pair takes the next lines of a shuffled pool;
// when the pool runs out it is reshuffled. A prefetcher that learned "B follows A"
// from an earlier chain then finds B in the current chain with probability about
// kChain / kPool (0.3%), so it cannot help.
class Fresh {
  public:
    Fresh(std::vector<std::atomic<uint64_t> *> lines, uint64_t seed) : lines_(std::move(lines)), rng_(seed) {}
    std::vector<std::atomic<uint64_t> *> take(size_t n) {
        if (next_ + n > lines_.size())
            std::shuffle(lines_.begin(), lines_.end(), rng_), next_ = 0;
        next_ += n;
        return {lines_.begin() + long(next_ - n), lines_.begin() + long(next_)};
    }

  private:
    std::vector<std::atomic<uint64_t> *> lines_;
    std::mt19937_64 rng_;
    size_t next_ = 0;
};

// chase: a (the writer) links a chain of fresh lines, so each sits modified in a's
// cache; b follows it, timing the dependent loads (one clock read around the whole
// walk: under 1% overhead at 255 hops). Returns ns per hop for each timed chain,
// without chains far above the pair's median (a disturbance: an interrupt during
// its ~15 us); *dropped counts those.
std::vector<double> measure_chase(Pool &pool, int a, int b, Fresh &fresh, const Options &o, size_t *dropped) {
    std::vector<std::vector<std::atomic<uint64_t> *>> chains(kWarmChains + std::max<size_t>(1, o.lines / kChain));
    for (auto &c : chains)
        c = fresh.take(kChain);
    struct alignas(128) Ctl {
        std::atomic<uint64_t> seq{0};
        std::atomic<uint64_t> head{0}, hops{0};
        std::atomic<bool> quit{false};
    } go;
    struct alignas(128) Ack {
        std::atomic<uint64_t> seq{0}, end{0};
        std::atomic<double> ns{0};
    } ack;
    std::vector<double> per_hop;
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
        std::vector<uint64_t> spill(o.offset / 8 + 8); // a's own buffer, one store per 64 B line
        uint64_t seq = 0;
        for (size_t w = 0; w < chains.size(); ++w) { // the first kWarmChains are untimed
            auto &ch = chains[w];
            for (size_t i = 0; i < ch.size(); ++i) // the last line points to a sentinel value
                ch[i]->store(i + 1 < ch.size() ? reinterpret_cast<uint64_t>(ch[i + 1]) : 1, std::memory_order_relaxed);
            volatile uint64_t *v = spill.data(); // --offset: push the chain into a's L2
            for (size_t i = 0; i + 8 <= spill.size() && o.offset; i += 8)
                v[i] = seq;
            go.head.store(reinterpret_cast<uint64_t>(ch[0]), std::memory_order_relaxed);
            go.hops.store(ch.size() - 1, std::memory_order_relaxed); // ends on the last line's address
            go.seq.store(++seq, std::memory_order_release);
            while (ack.seq.load(std::memory_order_acquire) != seq)
                relax();
            wrong |= ack.end.load(std::memory_order_relaxed) != reinterpret_cast<uint64_t>(ch.back());
            if (w >= kWarmChains)
                per_hop.push_back(ack.ns.load(std::memory_order_relaxed) / double(ch.size() - 1));
        }
        go.quit.store(true, std::memory_order_release);
        go.seq.store(++seq, std::memory_order_release);
    });
    pool.wait(a);
    pool.wait(b);
    if (wrong)
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

double quantile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
}

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: c2cbench [-m cas|chase] [-c CPUS] [-l LINES] [-r ROUNDS] [-n PASSES] [-t] [--csv | -p] [--hist]\n"
                 "                [--bucket NS] [--offset SIZE] [-s SEED]\n"
                 "  -m      cas: both cores CAS one line in turn, timed per line (default)\n"
                 "          chase: the lower CPU writes a pointer chain of fresh lines, the other follows it\n"
                 "  -c      CPUs to use, e.g. 0-7,16 (default: one CPU per physical core)\n"
                 "  -l      lines per pair (cas: 256, each timed on its own; chase: 8192, in chains of 256)\n"
                 "  -r, -n  cas: round trips per line per pass (200), passes (3)\n"
                 "  --hist  cas: histogram of the per-line times for each pair (with --csv: one row per line)\n"
                 "  --bucket NS  histogram bucket width (default 1)\n"
                 "  --offset SIZE  chase: the writer stores SIZE bytes of its own after each chain,\n"
                 "          pushing the chain from its L1 to its L2 (L2 -> L1 instead of L1 -> L1);\n"
                 "          pick a size between the L1d and L2 sizes, e.g. 2x L1d (default 0)\n"
                 "  -s      seed that picks the lines (default: new each run, printed);\n"
                 "          the same seed reproduces a run's line set\n"
                 "  -t      include SMT siblings (with the default CPU list)\n"
                 "  --csv   one row per pair: a,b,mode,best,p10,median,p90,worst,mean (ns),seed\n"
                 "          (cas: over lines; chase: over chains of 256 lines)\n"
                 "  -p      gnuplot script: heatmaps of the mean (and, for cas, the best line)\n"
                 "Default output: matrix of the mean one-way time (ns), then a summary.\n");
    std::exit(2);
}

// Text histogram of one pair's per-line times: on a mesh, one mode per distance
// from the pair to the lines' home slices.
void histogram(int a, int b, const std::vector<double> &v, double width) {
    const double lo = std::floor(*std::min_element(v.begin(), v.end()) / width) * width;
    std::vector<size_t> count(size_t((*std::max_element(v.begin(), v.end()) - lo) / width) + 1);
    for (double x : v)
        ++count[size_t((x - lo) / width)];
    const size_t top = *std::max_element(count.begin(), count.end());
    std::printf("\nCPUs %d-%d: %zu lines, mean %.1f ns\n", a, b, v.size(),
                std::accumulate(v.begin(), v.end(), 0.0) / double(v.size()));
    for (size_t i = 0; i < count.size(); ++i)
        std::printf("%7.1f %5zu %s\n", lo + double(i) * width, count[i], std::string((count[i] * 50 + top - 1) / top, '#').c_str());
}

} // namespace

int main(int argc, char **argv) try {
    Options o;
    std::string cpus_spec, out = "matrix";
    bool smt = false, hist = false;
    double bucket = 1;
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
        else if (k == "--hist") hist = true;
        else if (k == "--bucket") bucket = std::stod(val());
        else if (k == "-t") smt = true;
        else if (k == "--csv") out = "csv";
        else if (k == "-p") out = "plot";
        else usage();
    }
    if (o.lines == 0)
        o.lines = o.mode == "cas" ? 256 : 8192;
    if ((o.mode != "cas" && o.mode != "chase") || o.lines < 2 || o.rounds == 0 || o.passes == 0 || !(bucket > 0))
        usage();
    if (o.mode == "chase" && o.lines < kChain)
        throw std::runtime_error("chase needs at least " + std::to_string(kChain) + " lines per pair");
    if (hist && o.mode != "cas")
        throw std::runtime_error("--hist needs per-line times: use it with -m cas");
    if (o.mode == "cas" && o.offset)
        throw std::runtime_error("--offset applies to -m chase");
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
    auto lines = make_lines(o.mode == "cas" ? o.lines : kPool, o.seed);
    Fresh fresh(lines, o.seed * 31 + 7);
    size_t dropped = 0;
    Pool pool(cpus);
    // Let every worker spin before the first pair so idle cores reach their clock.
    std::this_thread::sleep_for(std::chrono::milliseconds(o.settle_ms));

    struct Row {
        int a, b;
        double best, p10, med, p90, worst, mean;
    };
    std::vector<Row> rows;
    const bool line_rows = out == "csv" && hist;
    if (line_rows)
        std::printf("a,b,mode,line,ns,seed\n");
    else if (out == "csv")
        std::printf("a,b,mode,best_ns,p10_ns,median_ns,p90_ns,worst_ns,mean_ns,seed\n");
    std::vector<std::pair<std::pair<int, int>, std::vector<double>>> hists;
    for (size_t i = 0; i < cpus.size(); ++i)
        for (size_t j = i + 1; j < cpus.size(); ++j) {
            std::vector<double> v = o.mode == "cas" ? measure_cas(pool, cpus[i], cpus[j], lines, o)
                                                    : measure_chase(pool, cpus[i], cpus[j], fresh, o, &dropped);
            Row r{cpus[i], cpus[j], quantile(v, 0), quantile(v, 0.1), quantile(v, 0.5), quantile(v, 0.9), quantile(v, 1),
                  std::accumulate(v.begin(), v.end(), 0.0) / double(v.size())};
            rows.push_back(r);
            if (line_rows) {
                for (size_t l = 0; l < v.size(); ++l)
                    std::printf("%d,%d,%s,%zu,%.2f,%llu\n", r.a, r.b, o.mode.c_str(), l, v[l], (unsigned long long)o.seed);
                std::fflush(stdout);
            } else if (out == "csv") {
                std::printf("%d,%d,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%llu\n", r.a, r.b, o.mode.c_str(), r.best, r.p10, r.med,
                            r.p90, r.worst, r.mean, (unsigned long long)o.seed);
                std::fflush(stdout);
            } else if (hist) {
                hists.push_back({{r.a, r.b}, v});
            }
        }

    auto cell = [&](int a, int b, double Row::*f) {
        for (auto &r : rows)
            if ((r.a == a && r.b == b) || (r.a == b && r.b == a))
                return r.*f;
        return 0.0;
    };
    std::vector<std::pair<const char *, double Row::*>> maps{{"mean", &Row::mean}};
    if (o.mode == "cas")
        maps.push_back({"best line", &Row::best});
    if (out == "plot") {
        for (auto [title, f] : maps) {
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
        if (o.mode == "cas")
            std::printf("c2cbench cas: mean over lines, one-way ns (%zu lines per pair)\n     ", o.lines);
        else
            std::printf("c2cbench chase: mean over fresh lines, one-way ns (%zu lines per pair, each used once)\n     ",
                        (o.lines / kChain) * kChain);
        for (int c : cpus)
            std::printf("%5d", c);
        std::printf("\n");
        for (int a : cpus) {
            std::printf("%5d", a);
            for (int b : cpus)
                a == b ? std::printf("%5s", "") : std::printf("%5.0f", cell(a, b, &Row::mean));
            std::printf("\n");
        }
    }
    for (auto &[ab, v] : hists)
        histogram(ab.first, ab.second, v, bucket);
    if (out != "csv") {
        FILE *f = out == "plot" ? stderr : stdout;
        std::fprintf(f, "\nseed %llu (reproduce with -s %llu)", (unsigned long long)o.seed, (unsigned long long)o.seed);
        if (o.mode == "chase") {
            std::vector<double> spread;
            for (auto &r : rows)
                spread.push_back(r.p90 - r.p10);
            std::fprintf(f, "; writer offset %zu B\nchains of %zu lines from a pool of %zu: p90 - p10 over chains = %.1f ns "
                            "(median over pairs); %zu disturbed chains dropped\n",
                         o.offset, kChain, kPool, quantile(spread, 0.5), dropped);
        } else {
            std::vector<double> spread, gain;
            for (auto &r : rows) {
                spread.push_back(r.worst - r.best);
                gain.push_back((r.mean - r.best) / r.mean);
            }
            std::fprintf(f, "\nper pair, over %zu lines: worst - best = %.1f ns (median over pairs), up to %.1f ns;\n"
                            "best line vs mean: %.0f%% faster (median over pairs), up to %.0f%%\n",
                         o.lines, quantile(spread, 0.5), quantile(spread, 1), 100 * quantile(gain, 0.5), 100 * quantile(gain, 1));
        }
    }
    return 0;
} catch (const std::exception &e) {
    std::fprintf(stderr, "c2cbench: %s\n", e.what());
    return 1;
}
