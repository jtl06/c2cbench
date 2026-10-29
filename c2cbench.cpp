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
//
// Modes (ns, one way): cas (cas.hpp) and chase (chase.hpp). harness.hpp has the
// CPU list, the pinned workers and the lines; this file is the command line and
// the output.
//
// Linux, x86-64 or AArch64, C++17. Build: g++ -O2 -std=c++17 -pthread c2cbench.cpp -o c2cbench
// SPDX-License-Identifier: Apache-2.0
#include "cas.hpp"
#include "chase.hpp"
#include <cstdio>
#include <cstdlib>
#include <numeric>

using namespace c2c;

namespace {

struct Options {
    std::string mode = "cas";
    size_t lines = 0; // per pair; 0: 256 (cas), 8192 (chase)
    unsigned rounds = 200, passes = 3, settle_ms = 200; // warm-up once, at the start
    uint64_t seed = 0; // 0: pick a new random seed each run (printed)
    size_t offset = 0; // chase: bytes the writer stores after each chain (pushes it out of L1)
};

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: c2cbench [-m cas|chase] [-c CPUS] [-l LINES] [-r ROUNDS] [-n PASSES] [-t] [--csv] [--hist]\n"
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
    std::string cpus_spec;
    bool smt = false, hist = false, csv = false;
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
        else if (k == "--csv") csv = true;
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
    const bool line_rows = csv && hist;
    if (line_rows)
        std::printf("a,b,mode,line,ns,seed\n");
    else if (csv)
        std::printf("a,b,mode,best_ns,p10_ns,median_ns,p90_ns,worst_ns,mean_ns,seed\n");
    std::vector<std::pair<std::pair<int, int>, std::vector<double>>> hists;
    for (size_t i = 0; i < cpus.size(); ++i)
        for (size_t j = i + 1; j < cpus.size(); ++j) {
            std::vector<double> v = o.mode == "cas" ? measure_cas(pool, cpus[i], cpus[j], lines, o.rounds, o.passes, o.seed)
                                                    : measure_chase(pool, cpus[i], cpus[j], fresh, o.lines, o.offset, &dropped);
            Row r{cpus[i], cpus[j], quantile(v, 0), quantile(v, 0.1), quantile(v, 0.5), quantile(v, 0.9), quantile(v, 1),
                  std::accumulate(v.begin(), v.end(), 0.0) / double(v.size())};
            rows.push_back(r);
            if (line_rows) {
                for (size_t l = 0; l < v.size(); ++l)
                    std::printf("%d,%d,%s,%zu,%.2f,%llu\n", r.a, r.b, o.mode.c_str(), l, v[l], (unsigned long long)o.seed);
                std::fflush(stdout);
            } else if (csv) {
                std::printf("%d,%d,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%llu\n", r.a, r.b, o.mode.c_str(), r.best, r.p10, r.med,
                            r.p90, r.worst, r.mean, (unsigned long long)o.seed);
                std::fflush(stdout);
            } else if (hist) {
                hists.push_back({{r.a, r.b}, v});
            }
        }
    if (csv)
        return 0;

    auto mean_of = [&](int a, int b) {
        for (auto &r : rows)
            if ((r.a == a && r.b == b) || (r.a == b && r.b == a))
                return r.mean;
        return 0.0;
    };
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
            a == b ? std::printf("%5s", "") : std::printf("%5.0f", mean_of(a, b));
        std::printf("\n");
    }
    for (auto &[ab, v] : hists)
        histogram(ab.first, ab.second, v, bucket);

    std::printf("\nseed %llu (reproduce with -s %llu)", (unsigned long long)o.seed, (unsigned long long)o.seed);
    if (o.mode == "chase") {
        std::vector<double> spread;
        for (auto &r : rows)
            spread.push_back(r.p90 - r.p10);
        std::printf("; writer offset %zu B\nchains of %zu lines from a pool of %zu: p90 - p10 over chains = %.1f ns "
                    "(median over pairs); %zu disturbed chains dropped\n",
                    o.offset, kChain, kPool, quantile(spread, 0.5), dropped);
    } else {
        std::vector<double> spread, gain;
        for (auto &r : rows) {
            spread.push_back(r.worst - r.best);
            gain.push_back((r.mean - r.best) / r.mean);
        }
        std::printf("\nper pair, over %zu lines: worst - best = %.1f ns (median over pairs), up to %.1f ns;\n"
                    "best line vs mean: %.0f%% faster (median over pairs), up to %.0f%%\n",
                    o.lines, quantile(spread, 0.5), quantile(spread, 1), 100 * quantile(gain, 0.5), 100 * quantile(gain, 1));
    }
    return 0;
} catch (const std::exception &e) {
    std::fprintf(stderr, "c2cbench: %s\n", e.what());
    return 1;
}
