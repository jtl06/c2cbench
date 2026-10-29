# c2cbench

Core to core latency, per cache line.

Core to core latency is not only a function of the two cores. On mesh and ring
CPUs, a shared line moves owner → home slice → requester, and the home comes
from a hash of the line's physical address. So "the latency between cores 0
and 1" depends on which line you happened to get: on a Xeon Gold 6314U it is
anywhere from 40 to 60 ns. Tools that time one line per pair report one draw
from that spread, and a different one each run.

c2cbench times many lines per pair. `cas` passes 256 lines back and forth, one
at a time, and reports the mean, the spread, and on request a histogram whose
modes are the homes. `chase` follows pointer chains of 8192 fresh lines that no
core has loaded before, which is a one-way read with no retries and nothing for
a prefetcher to learn.

The story behind it: [Core to Core is not Core to Core](https://jacen.li/writings/core-to-core-is-not-core-to-core/).

## Quick start

Linux on x86-64 or AArch64, C++17, no dependencies.

```console
$ sudo sysctl vm.nr_hugepages=16     # lines live in explicit 2 MiB pages
$ make                               # or: g++ -O2 -std=c++17 -pthread c2cbench.cpp -o c2cbench
$ ./c2cbench -c 0,1,4,5,17,20
c2cbench cas: mean over lines, one-way ns (256 lines per pair)
         0    1    4    5   17   20
    0        51   51   50   51   50
    1   51        50   50   50   50
    4   51   50        50   51   50
    5   50   50   50        50   50
   17   51   50   51   50        50
   20   50   50   50   50   50

seed 1 (reproduce with -s 1)
per pair, over 256 lines: worst - best = 9.4 ns (median over pairs), up to 19.4 ns;
best line vs mean: 10% faster (median over pairs), up to 20%
```

The number the usual tools give you is one of those 256 lines. `--hist` shows
them all for one pair; each cluster is a group of homes at a similar distance:

```console
$ ./c2cbench -c 0,1 --hist
CPUs 0-1: 256 lines, mean 51.1 ns
   40.0    14 #####################
   41.0     0
   42.0     0
   43.0     0
   44.0     8 ############
   45.0    12 ##################
   46.0    12 ##################
   47.0    26 #######################################
   48.0    10 ###############
   49.0    27 ########################################
   50.0     0
   51.0    20 ##############################
   52.0    18 ###########################
   53.0    34 ##################################################
   54.0    15 #######################
   55.0    14 #####################
   56.0    17 #########################
   57.0    21 ###############################
   58.0     3 #####
   59.0     5 ########
```

```console
$ ./c2cbench                        # every pair of physical cores
$ ./c2cbench -c 0-7 --csv           # one row per pair: best,p10,median,p90,worst,mean,seed
$ ./c2cbench -c 0,1 --hist --csv    # one row per line
$ ./c2cbench -m chase               # pointer chase: one-way read of a line the other core wrote
$ ./c2cbench -m chase --offset 96K  # the same, with the writer's copy in its L2
$ ./c2cbench -s 1                   # measure the same lines again
```

Each run draws new lines and prints its seed; `-s` reproduces a line set.
`docs/heatmaps.py` draws heatmaps from `--csv`.

## What it measures

![Latency against route length through the home](docs/latency-vs-route.png)

Each line's latency follows its route, writer → home → reader, at about 1.5 ns
per mesh hop for the chase and about half that for CAS, whose request is
already in flight while the other core holds the line. Locating the homes takes
uncore counters and is not part of c2cbench; the article has the details.

![100 fresh core-to-core-latency runs against c2cbench's lines](docs/runs-vs-lines.png)

Run a one-line tool 100 times on neighbouring cores and the results land
anywhere from 40 to 59 ns, in clusters. That run-to-run spread is the
line-to-line spread c2cbench shows in a single run. For cores far apart the
spread is small, because most homes sit near the path between them.

![CAS heatmaps](docs/heatmap-cas.png)
![chase heatmap](docs/heatmap-chase.png)

All 32 cores of the Xeon 6314U at load clocks (turbo on, `performance`
governor, mesh at its 2.2 GHz maximum). The means vary by a few ns with where
the cores sit; the topology shows most in the spread.

## Modes

**cas** – both cores CAS one line in turn, like a lock handoff, each retrying
until the other's CAS has landed. This is the loop
[c2clat](https://github.com/rigtorp/c2clat) `-w` and
[core-to-core-latency](https://github.com/nviennot/core-to-core-latency) use;
only the number of lines differs. Reported as half a round trip, per line.

**chase** – the lower CPU writes a chain of 256 lines, so each sits modified
in its cache, and the other CPU follows it: one dependent read per hop, no
round trip, no retries. Every chain is made of lines no core has loaded before,
drawn from a pool of 100,000, because some cores learn chains: on Graviton5
walking a chain a second time is 35–38% cheaper per hop. The result is a mean
per pair over 8192 lines; chains hit by an interrupt are dropped and counted.
By default the writer's copy is in its L1 when the reader asks. `--offset SIZE`
has the writer store SIZE bytes of its own first, moving the chain into its L2,
which on the Xeon arrives 2.5–3 ns sooner. Keep it under the L2 size: past that
the chain falls to the shared cache and the reads are not core to core any more
(c2cbench warns).

Lines sit one per 128-byte block in 32 MiB of hugepage memory, so the
adjacent-line prefetcher never pairs two of them and walks pay no TLB misses.
Every selected CPU runs a pinned worker that spins for the whole run, so idle
cores are never timed before they ramp up.

## Reading the results

- **A large spread for one pair is the home effect**, not noise. The same lines
  keep their homes for the life of the process; `--hist` shows the clusters.
- **The mean is the headline.** It is the expected latency of a randomly placed
  line. A median jumps between clusters from run to run.
- **Measure at load clocks.** Left alone, power management can let the mesh
  drop to its lowest clock during a light two-core ping-pong and double every
  number.
- **Store/load ping-pongs are not one transfer.** c2clat's default test and
  core-to-core-latency's `-b 2` move each line twice per step, so they read
  about twice the chase.

## Timing

One `std::chrono::steady_clock` read before and after each timed block (a
line's 200 round trips, or a whole 255-hop chain), never per operation. On
Linux that is the vDSO, `rdtscp` on x86 and `CNTVCT_EL0` after an `isb` on
AArch64. A clock read costs 20–35 ns against a 15–20 µs block, 0.1–0.2%, and is
not subtracted. Each walk is checked to end on its chain's last line, so the
compiler cannot shorten it. See Matt Godbolt,
[It's About Time](https://hudson-trading.github.io/its-about-time/), for the
fully fenced variant; at these block lengths it does not change the result.

## Layout

`harness.hpp` (CPU list, pinned workers, lines, statistics), `cas.hpp` and
`chase.hpp` (one benchmark each), `c2cbench.cpp` (command line and output).
The headers stand alone, so other tools can reuse the harness or a benchmark.

## License

Apache-2.0.
