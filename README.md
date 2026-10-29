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
c2cbench cas: median line, one-way handoff ns (64 lines per pair)
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
./c2cbench -m rw                 # one writer per line (SP/SC queue indices)
```

Linux on x86-64 or AArch64, C++17, one file, no dependencies. Lines live in
explicit 2 MiB hugepages when some are reserved (`sysctl vm.nr_hugepages=16`),
else in transparent hugepages (`--pages hugetlb|thp` to choose).

## Modes

- **cas**: both cores CAS one line in turn, like a lock handoff (as in
  core-to-core-latency's CAS test and c2clat's `-w`).
- **rw**: core a writes line x and b reads it, then b writes line y and a reads
  it. Each line has one writer, like an SP/SC queue's indices (as in c2clat's
  default test).

Reported values are one-way handoffs in ns (half a round trip). Candidate lines
sit one per 128-byte block in hugepage memory, so the adjacent-line prefetcher
never pairs two of them. Every selected CPU runs one pinned worker that spins
for the whole run, so idle cores are never timed before they ramp up (this uses
those CPUs at 100% while c2cbench runs).

## Reading the results

- **A large worst − best spread for one pair is the home effect.** It is placement, not noise; the same lines keep their homes for the life of the process.
- **Pin clocks for stable numbers,** including the uncore if you can. On some parts a quiet two-core ping-pong lets the mesh clock drop.
- **To use the fast lines,** see heatseeker: it finds them the same way and hands them to your data structures.

License: Apache-2.0.
