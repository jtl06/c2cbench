"""Heatmaps for the README from c2cbench --csv output.
usage: python3 docs/heatmaps.py CAS.csv CHASE.csv OUTDIR"""
import csv, sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap

SURFACE, INK, INK2 = "#fcfcfb", "#0b0b0b", "#52514e"
RAMP = ["#86b6ef", "#6da7ec", "#5598e7", "#3987e5", "#2a78d6", "#256abf", "#1c5cab", "#184f95", "#104281", "#0d366b"]
cmap = LinearSegmentedColormap.from_list("blue", RAMP)
cmap.set_bad(SURFACE)


def matrix(path, f):
    rows = list(csv.DictReader(open(path)))
    cpus = sorted({int(r["a"]) for r in rows} | {int(r["b"]) for r in rows})
    ix = {c: i for i, c in enumerate(cpus)}
    m = [[float("nan")] * len(cpus) for _ in cpus]
    for r in rows:
        v = f(r)
        m[ix[int(r["a"])]][ix[int(r["b"])]] = m[ix[int(r["b"])]][ix[int(r["a"])]] = v
    return cpus, m


for mode, path in (("cas", sys.argv[1]), ("chase", sys.argv[2])):
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 9})
    fig, axes = plt.subplots(1, 2, figsize=(10, 4.6), facecolor=SURFACE)
    for ax, (title, f) in zip(axes, [("median line (ns)", lambda r: float(r["median_ns"])),
                                     ("spread over lines: worst - best (ns)",
                                      lambda r: float(r["worst_ns"]) - float(r["best_ns"]))]):
        cpus, m = matrix(path, f)
        im = ax.imshow(m, cmap=cmap, interpolation="nearest")
        ax.set_title(f"{mode}: {title}", color=INK, loc="left", fontsize=10)
        ticks = [i for i, c in enumerate(cpus) if c % 4 == 0]
        ax.set_xticks(ticks, [cpus[i] for i in ticks]); ax.set_yticks(ticks, [cpus[i] for i in ticks])
        ax.tick_params(colors=INK2, length=0)
        ax.set_xlabel("CPU", color=INK2); ax.set_ylabel("CPU", color=INK2)
        for s in ax.spines.values():
            s.set_visible(False)
        cb = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.03)
        cb.outline.set_visible(False); cb.ax.tick_params(colors=INK2, length=0)
    fig.text(0.01, 0.015, "Xeon Gold 6314U, 32 cores, load clocks (cores 2.9 GHz, mesh 2.2 GHz), 64 lines per pair.",
             color=INK2, fontsize=8)
    fig.tight_layout(rect=(0, 0.06, 1, 0.97))
    fig.savefig(f"{sys.argv[3]}/heatmap-{mode}.png", dpi=150, facecolor=SURFACE)
    print("wrote", f"{sys.argv[3]}/heatmap-{mode}.png")
