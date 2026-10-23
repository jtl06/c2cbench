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
    0        67   57   61   71   63
    1   67        99   70   69   68
    4   57   99        81   64   63
    5   61   70   81        81   64
   17   71   69   64   81        62
   20   63   68   63   64   62

per pair, over 64 lines: worst - best = 27.6 ns (median over pairs), up to 55.3 ns;
best line vs median line: 12% faster (median over pairs), up to 33%
```

(Xeon Gold 6314U, default clocks, with other load on the machine.)

## Build and run

```sh
make            # or: g++ -O2 -std=c++17 -pthread c2cbench.cpp -o c2cbench
./c2cbench                       # every pair of physical cores
./c2cbench -c 0-7 --csv          # one row per pair: best,p10,median,p90,worst
./c2cbench -p | gnuplot -p       # heatmaps of the median and the best line
./c2cbench -m rw                 # one writer per line (SP/SC queue indices)
```

Linux on x86-64 or AArch64, C++17, one file, no dependencies.

## Modes

- **cas**: both cores CAS one line in turn, like a lock handoff (as in
  core-to-core-latency's CAS test and c2clat's `-w`).
- **rw**: core a writes line x and b reads it, then b writes line y and a reads
  it. Each line has one writer, like an SP/SC queue's indices (as in c2clat's
  default test).

Reported values are one-way handoffs in ns (half a round trip). Candidate lines
sit one per 128-byte block in hugepage memory, so the adjacent-line prefetcher
never pairs two of them. Both cores spin for 20 ms before each pair so idle
cores are not timed before they ramp up.

## Reading the results

- **A large worst − best spread for one pair is the home effect.** It is placement, not noise; the same lines keep their homes for the life of the process.
- **Pin clocks for stable numbers,** including the uncore if you can. On some parts a quiet two-core ping-pong lets the mesh clock drop.
- **To use the fast lines,** see heatseeker: it finds them the same way and hands them to your data structures.

License: Apache-2.0.
