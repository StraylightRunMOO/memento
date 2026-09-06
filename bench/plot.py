#!/usr/bin/env python3
"""Regenerate benchmark graphs from captured CSVs in bench/results.

    results/compare.csv                head-to-head compare.c (ns/op)
    results/mimalloc-bench-allt.csv    mimalloc-bench 'allt' benchres.csv
    results/rptest-threads.csv         optional rptest thread sweep

Memento is drawn last (on top) with emphasis. Allocators more than
SLOW_FACTOR times slower than memento on a given test are omitted from
that chart; the CSVs stay complete. Requires matplotlib.

Usage:
    python3 plot.py
    python3 plot.py --collect-compare results/raw
"""
from __future__ import print_function

import argparse
import csv
import os
import re
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

SLOW_FACTOR = 4.0
BASELINE_COMPARE = "memento-3.0"
BASELINE_ALLT = "me"

basedir = os.path.dirname(os.path.abspath(__file__))
resultdir = os.path.join(basedir, "results")
imagedir = os.path.join(basedir, "images")
os.makedirs(imagedir, exist_ok=True)
os.makedirs(resultdir, exist_ok=True)

COMPARE_ORDER = [
    "glibc",
    "mimalloc-2.1.7",
    "rpmalloc-1.4.5",
    "memento-3.0",
    "memento-3.0-nostats",
]
COMPARE_LABEL = {
    "glibc": "glibc",
    "mimalloc-2.1.7": "mimalloc 2.1.7",
    "rpmalloc-1.4.5": "rpmalloc 1.4.5",
    "memento-3.0": "memento 3.0",
    "memento-3.0-nostats": "memento 3.0 STATS=0",
}
COMPARE_WORKLOADS = [
    "churn-64B",
    "churn-1KiB",
    "bulk-64B",
    "bulk-4KiB",
    "mixed-16-256B",
    "churn-16KiB",
    "threads-4x-64B",
]

ALLOC_ORDER = [
    "sys", "me", "me0", "rp", "mi", "mi2", "mi3", "je", "tc", "sn", "hd", "tbb", "sm",
    "scudo", "lt", "mng", "iso", "sn-sec", "hml", "hm", "ff", "gd", "sg",
    "fg", "lf", "lp", "mesh", "nomesh", "yal", "rmalloc",
]
ALLOC_LABEL = {
    "sys": "glibc",
    "me": "memento",
    "me0": "memento STATS=0",
    "rp": "rpmalloc",
    "mi": "mimalloc 1",
    "mi2": "mimalloc 2",
    "mi3": "mimalloc 3",
    "je": "jemalloc",
    "tc": "tcmalloc",
    "sn": "snmalloc",
    "sn-sec": "snmalloc-checks",
    "hd": "hoard",
    "tbb": "tbbmalloc",
    "sm": "supermalloc",
    "scudo": "scudo",
    "lt": "ltalloc",
    "mng": "mallocng",
    "iso": "isoalloc",
    "hm": "hardened_malloc",
    "hml": "hardened_malloc-light",
    "sg": "SlimGuard",
    "ff": "ffmalloc",
    "fg": "FreeGuard",
    "gd": "Guarder",
    "mesh": "mesh",
    "nomesh": "mesh (no-mesh)",
    "lf": "lockfree-malloc",
    "lp": "libpas",
    "yal": "yalloc",
    "rmalloc": "rmalloc",
}


def alloc_label(name):
    return ALLOC_LABEL.get(name, COMPARE_LABEL.get(name, name))


def alloc_sort_key(name, order):
    return (order.index(name) if name in order else 1000, name)


def parse_time(text):
    parts = text.split(":")
    seconds = 0.0
    for part in parts:
        seconds = seconds * 60.0 + float(part)
    return seconds


def is_memento(name):
    return name == "me" or name.startswith("memento")


# --------------------------------------------------------------------
# Collect min-of-N from compare.c raw logs
# --------------------------------------------------------------------

HEADER_RE = re.compile(r"^\[(.+)\]\s*$")
ROW_RE = re.compile(r"^\s+(\S+)\s+([0-9.]+)\s+ns/op\s*$")


def collect_compare(rawdir, out_csv):
    # rawdir contains files named run<n>-<anything>.txt, each one compare.c stdout
    series = {}  # alloc -> workload -> [ns, ...]
    for name in sorted(os.listdir(rawdir)):
        if not name.endswith(".txt"):
            continue
        path = os.path.join(rawdir, name)
        alloc = None
        with open(path) as f:
            for line in f:
                m = HEADER_RE.match(line)
                if m:
                    alloc = m.group(1)
                    series.setdefault(alloc, {})
                    continue
                m = ROW_RE.match(line)
                if m and alloc:
                    series[alloc].setdefault(m.group(1), []).append(float(m.group(2)))

    if not series:
        raise SystemExit("no compare.c results in %s" % rawdir)

    with open(out_csv, "w") as f:
        f.write("# compare.c head-to-head, ns per alloc+free pair, min of N runs\n")
        try:
            import platform
            f.write("# machine: %s %s %s\n" % (
                platform.machine(), platform.system(), platform.release()))
        except Exception:
            pass
        f.write("allocator,workload,ns_per_op\n")
        for alloc in sorted(series, key=lambda n: alloc_sort_key(n, COMPARE_ORDER)):
            for wl in COMPARE_WORKLOADS:
                if wl not in series[alloc]:
                    continue
                best = min(series[alloc][wl])
                f.write("%s,%s,%.2f\n" % (alloc, wl, best))
    print("wrote", out_csv)


def read_compare(path):
    results = {}  # workload -> alloc -> ns
    with open(path) as f:
        for row in csv.DictReader(line for line in f if not line.startswith("#")):
            results.setdefault(row["workload"], {})[row["allocator"]] = float(row["ns_per_op"])
    return results


def plot_compare(results):
    tests = [w for w in COMPARE_WORKLOADS if w in results] or sorted(results)
    cols = 2
    rows = (len(tests) + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(11, 3.1 * rows))
    if rows == 1:
        axes = [axes]
    for iplot, test in enumerate(tests):
        ax = axes[iplot // cols][iplot % cols]
        entries = sorted(results[test].items(), key=lambda kv: kv[1])
        names = [alloc_label(name) for name, _ in entries]
        values = [value for _, value in entries]
        colors = ["#202020" if is_memento(name) else "#5a9bd4" for name, _ in entries]
        ax.barh(range(len(names)), values, color=colors)
        ax.set_yticks(range(len(names)))
        ax.set_yticklabels(names, fontsize=8)
        ax.invert_yaxis()
        ax.set_title(test, fontsize=10)
        ax.tick_params(axis="x", labelsize=8)
        ax.grid(True, axis="x", alpha=0.3)
        ax.set_xlabel("ns / alloc+free pair", fontsize=8)
        for ibar, value in enumerate(values):
            ax.text(value, ibar, " %.1f" % value, va="center", fontsize=7)
    for iplot in range(len(tests), rows * cols):
        axes[iplot // cols][iplot % cols].axis("off")
    fig.suptitle("compare.c, ns per alloc+free pair (lower is better)", fontsize=13)
    fig.tight_layout(rect=(0, 0, 1, 0.98))
    out = os.path.join(imagedir, "compare-ns.png")
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print("wrote", out)


# --------------------------------------------------------------------
# rptest thread sweep
# --------------------------------------------------------------------

def plot_rptest():
    path = os.path.join(resultdir, "rptest-threads.csv")
    if not os.path.isfile(path):
        return
    series = {}
    with open(path) as f:
        lines = [l for l in f if not l.startswith("#")]
    for row in csv.DictReader(lines):
        if row.get("mops_per_cpu_second"):
            entry = series.setdefault(row["allocator"], {})
            entry[int(row["threads"])] = (
                int(row["mops_per_cpu_second"]),
                int(row["peak_mib"]),
            )
    if not series:
        return

    def mean_mops(name):
        values = [v[0] for v in series[name].values()]
        return sum(values) / len(values) if values else 0.0

    baseline_name = BASELINE_ALLT if BASELINE_ALLT in series else (
        BASELINE_COMPARE if BASELINE_COMPARE in series else None)
    if baseline_name:
        threshold = mean_mops(baseline_name) / SLOW_FACTOR
        dropped = [n for n in series if n != baseline_name and mean_mops(n) < threshold]
        for name in dropped:
            del series[name]
        if dropped:
            print("rptest: omitted (>{:g}x slower than {}): {}".format(
                SLOW_FACTOR, baseline_name, ", ".join(sorted(dropped))))

    allocs = sorted(series.keys(), key=lambda n: alloc_sort_key(n, ALLOC_ORDER + COMPARE_ORDER))
    cmap = plt.get_cmap("tab20")
    colors = {name: cmap(i % 20) for i, name in enumerate(allocs)}
    markers = ["o", "s", "D", "^", "v", "<", ">", "p", "*", "X", "P", "h", "8", "d", "+"]

    for index, ylabel, title, image in (
        (0, "memory ops / CPU second", "rptest performance", "rptest-perf.png"),
        (1, "peak resident memory (MiB)", "rptest peak memory", "rptest-memory.png"),
    ):
        fig, ax = plt.subplots(figsize=(10, 6.5))
        for i, name in enumerate(allocs):
            threads = sorted(series[name].keys())
            values = [series[name][t][index] for t in threads]
            emphasize = is_memento(name)
            ax.plot(
                threads,
                values,
                label=alloc_label(name),
                color="black" if emphasize else colors[name],
                marker=markers[i % len(markers)],
                markersize=5 if emphasize else 4,
                linewidth=2.5 if emphasize else 1.2,
                zorder=10 if emphasize else 2,
            )
        better = "higher is better" if index == 0 else "lower is better"
        ax.set_title(title + "\nrandom size [16,8000] linear falloff, cross-thread frees, " + better)
        ax.set_xlabel("threads")
        ax.set_ylabel(ylabel)
        ax.set_xlim(left=0.5)
        ax.set_ylim(bottom=0)
        ax.grid(True, alpha=0.3)
        ax.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0), fontsize=9)
        fig.tight_layout()
        fig.savefig(os.path.join(imagedir, image), dpi=150)
        plt.close(fig)
        print("wrote", os.path.join(imagedir, image))


# --------------------------------------------------------------------
# mimalloc-bench allt
# --------------------------------------------------------------------

FAIL_LIMIT = 2


def read_allt():
    path = os.path.join(resultdir, "mimalloc-bench-allt.csv")
    if not os.path.isfile(path):
        return None
    results = {}
    fail_count = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) < 8:
                if len(fields) >= 2:
                    fail_count[fields[1]] = fail_count.get(fields[1], 0) + 1
                continue
            test, alloc, time_text, rss_text = fields[0], fields[1], fields[2], fields[3]
            try:
                seconds = parse_time(time_text)
                rss = float(rss_text) / 1024.0
                cpu = float(fields[4]) + float(fields[5])
            except ValueError:
                continue
            if seconds == 0.0 or cpu == 0.0:
                fail_count[alloc] = fail_count.get(alloc, 0) + 1
                continue
            results.setdefault(test, {})[alloc] = (seconds, rss)

    # A crash that happens after a few hundred ms still has nonzero CPU
    # (espresso under a broken shim is ~0.3s vs ~8s). Treat anything more
    # than 4x faster than the median of the other allocators as a failure
    # rather than as the winner.
    for test, entries in list(results.items()):
        if len(entries) < 2:
            continue
        times = sorted(v[0] for v in entries.values())
        mid = times[len(times) // 2]
        if mid <= 0:
            continue
        floor = mid / SLOW_FACTOR
        for alloc in list(entries.keys()):
            if entries[alloc][0] < floor:
                fail_count[alloc] = fail_count.get(alloc, 0) + 1
                del entries[alloc]

    excluded = sorted(a for a, n in fail_count.items() if n > FAIL_LIMIT)
    for alloc in excluded:
        for entries in results.values():
            entries.pop(alloc, None)
    if excluded:
        print("allt: excluded (failed >{} benchmarks): {}".format(
            FAIL_LIMIT, ", ".join("{} ({})".format(a, fail_count[a]) for a in excluded)))
    isolated = sorted(a for a, n in fail_count.items() if 0 < n <= FAIL_LIMIT)
    if isolated:
        print("allt: isolated failures dropped as data points: " + ", ".join(
            "{} ({})".format(a, fail_count[a]) for a in isolated))
    return results


def filter_slow(results, baseline):
    dropped = []
    for test, entries in results.items():
        if baseline not in entries:
            continue
        limit = entries[baseline][0] * SLOW_FACTOR
        for alloc in list(entries.keys()):
            if alloc != baseline and entries[alloc][0] > limit:
                dropped.append("{}/{}".format(test, alloc))
                del entries[alloc]
    if dropped:
        print("allt: omitted (>{:g}x slower than {}): {}".format(
            SLOW_FACTOR, baseline, ", ".join(sorted(dropped))))
    return results


def plot_allt_grid(results, index, fmt, title, image):
    tests = sorted(results.keys())
    if not tests:
        return
    cols = 3
    rows = (len(tests) + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(15, 3.2 * rows))
    if rows == 1:
        axes = [axes]
    for iplot, test in enumerate(tests):
        ax = axes[iplot // cols][iplot % cols]
        entries = sorted(results[test].items(), key=lambda kv: kv[1][index])
        names = [alloc_label(name) for name, _ in entries]
        values = [value[index] for _, value in entries]
        bar_colors = ["#202020" if is_memento(name) else "#5a9bd4" for name, _ in entries]
        ax.barh(range(len(names)), values, color=bar_colors)
        ax.set_yticks(range(len(names)))
        ax.set_yticklabels(names, fontsize=7)
        ax.invert_yaxis()
        ax.set_title(test, fontsize=10)
        ax.tick_params(axis="x", labelsize=7)
        ax.grid(True, axis="x", alpha=0.3)
        for ibar, value in enumerate(values):
            ax.text(value, ibar, " " + fmt.format(value), va="center", fontsize=6)
    for iplot in range(len(tests), rows * cols):
        axes[iplot // cols][iplot % cols].axis("off")
    fig.suptitle(title, fontsize=13)
    fig.tight_layout(rect=(0, 0, 1, 0.99))
    fig.savefig(os.path.join(imagedir, image), dpi=150)
    plt.close(fig)
    print("wrote", os.path.join(imagedir, image))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--collect-compare", metavar="RAWDIR",
                        help="min-of-N collect from compare.c raw logs into compare.csv")
    args = parser.parse_args()

    if args.collect_compare:
        collect_compare(args.collect_compare, os.path.join(resultdir, "compare.csv"))

    compare_path = os.path.join(resultdir, "compare.csv")
    if os.path.isfile(compare_path):
        plot_compare(read_compare(compare_path))
    else:
        print("skip compare: no", compare_path)

    plot_rptest()

    allt = read_allt()
    if allt:
        me_present = any(BASELINE_ALLT in e for e in allt.values())
        baseline = BASELINE_ALLT if me_present else "rp"
        results = filter_slow(allt, baseline)
        plot_allt_grid(results, 0, "{:.2f}",
                       "mimalloc-bench, elapsed time in seconds (lower is better)",
                       "allt-time.png")
        plot_allt_grid(results, 1, "{:.0f}",
                       "mimalloc-bench, peak resident memory in MiB (lower is better)",
                       "allt-rss.png")
    else:
        print("skip allt: no mimalloc-bench-allt.csv")

    print("graphs written to", imagedir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
