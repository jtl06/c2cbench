# c2cbench

Core-to-core latency, **per cache line**.

Tools like [c2clat](https://github.com/rigtorp/c2clat) and
[core-to-core-latency](https://github.com/nviennot/core-to-core-latency) time one
shared cache line per core pair. On mesh and ring CPUs a shared line moves
owner → **home slice** → requester, and the home comes from an undocumented
hash of the line's address. So "the latency between two cores" depends on which
line you happened to get. c2cbench times many lines per pair (64 by default) and
reports the best, median and worst.

```
$ ./c2cbench -c 0,1,4,5,17,20
c2cbench cas: median line, one-way ns (64 lines per pair)
         0    1    4    5   17   20
    0        59   58   57   57   55
    1   59        57   56   57   55
    4   58   57        55   55   54
    5   57   56   55        55   54
   17   57   57   55   55        55
   20   55   55   54   54   55     

per pair, over 64 lines: worst - best = 14.5 ns (median over pairs), up to 19.5 ns;
best line vs median line: 9% faster (median over pairs), up to 19%
```

(Xeon Gold 6314U, default clocks, with other load on the machine.)

The median-line matrix is nearly flat: by the usual measure every pair looks
the same. Yet within each pair, the line you get changes the handoff by
~15 ns, because that line's home slice decides the route.

## Build and run

```sh
make            # or: g++ -O2 -std=c++17 -pthread c2cbench.cpp -o c2cbench
./c2cbench                       # every pair of physical cores
./c2cbench -c 0-7 --csv          # one row per pair: best,p10,median,p90,worst
./c2cbench -p | gnuplot -p       # heatmaps of the median and the best line
./c2cbench -m chase              # pointer chase: one-way read of a line the other core wrote
```

Linux on x86-64 or AArch64, C++17, one file, no dependencies. Lines live in
explicit 2 MiB hugepages when some are reserved (`sysctl vm.nr_hugepages=16`),
else in transparent hugepages (`--pages hugetlb|thp` to choose).

## Modes

- **cas**: both cores CAS one line in turn, like a lock handoff. Each core
  retries its `lock cmpxchg` until the other's has landed, exactly as c2clat's
  `-w` and core-to-core-latency's CAS test do, so only the number of lines
  differs. Reported as half a round trip.
- **chase**: the lower-numbered CPU writes a pointer chain through some of the
  lines, so each sits modified in its cache; the other CPU follows the chain.
  Each hop is one dependent read of a line the other core just wrote: the
  one-way transfer itself, with no round trip and no lock retries. Chains of
  different lengths over different lines are timed whole, and a least-squares
  fit splits them into a fixed overhead plus a cost per line, so even coarse
  timers give per-line results.
  The writer's copy is still in its L1 when the reader asks for it, as in a
  producer handing data straight to a consumer; a line already evicted to the
  writer's L2 hands off about 4 ns faster (Xeon 6314U).

Values are one-way ns per line. Candidate lines sit one per 128-byte block in
hugepage memory, so the adjacent-line prefetcher never pairs two of them, and
chains visit them in random order. Every selected CPU runs one pinned worker
that spins for the whole run, so idle cores are never timed before they ramp up
(this uses those CPUs at 100% while c2cbench runs).

## Reading the results

- **A large worst − best spread for one pair is the home effect.** It is placement, not noise; the same lines keep their homes for the life of the process.
- **Pin clocks for stable numbers,** including the uncore if you can. On some parts a quiet two-core ping-pong lets the mesh clock drop.
- **To use the fast lines,** see heatseeker: it finds them the same way and hands them to your data structures.

License: Apache-2.0.
