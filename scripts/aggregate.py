#!/usr/bin/env python3
"""Aggregate repeated runs (NAME_r1, NAME_r2, ...) -> mean / min / max per metric.

Usage: aggregate.py PREFIX [--csv out.csv] [--metrics m1,m2,...]
  e.g. aggregate.py f14_ --csv results/f14_agg.csv
Sniper is not fully deterministic (~0.5% cycle spread for identical runs), so
cycle/IPC comparisons should use the mean and show the range.
"""
import argparse
import csv
import glob
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lab6_stats import summarize  # noqa: E402

DEFAULT = ["cycles", "ipc", "l2tlb_hit_rate", "l2tlb_mpki", "avg_walk_cycles", "walk_cycles_sum",
           "ptw_llc_hit_rate", "data_llc_hit_rate", "rb_hit_rate_pt", "rb_hit_rate_data",
           "rb_conflict_rate_pt", "rb_conflict_rate_data", "dram_avg_ns_pt", "dram_avg_ns_data"]


def collect(prefix, results="results"):
    groups = {}
    for d in sorted(glob.glob(os.path.join(results, prefix + "*"))):
        if not os.path.isfile(os.path.join(d, "sim.stats")):
            continue
        name = os.path.basename(d)
        m = re.match(r"(.*)_r(\d+)$", name)
        groups.setdefault(m.group(1) if m else name, []).append(summarize(d))
    return groups


def aggregate(groups, metrics):
    rows = []
    for cfg, runs in groups.items():
        row = {"config": cfg, "n": len(runs)}
        for k in metrics:
            vals = [r[k] for r in runs if r.get(k) is not None]
            if not vals:
                continue
            row[k] = statistics.mean(vals)
            row[k + "_min"] = min(vals)
            row[k + "_max"] = max(vals)
        rows.append(row)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--csv")
    ap.add_argument("--metrics", default=",".join(DEFAULT))
    ap.add_argument("--results", default="results")
    a = ap.parse_args()
    metrics = a.metrics.split(",")
    rows = aggregate(collect(a.prefix, a.results), metrics)
    if not rows:
        sys.exit("no finished runs match " + a.prefix)
    if a.csv:
        cols = ["config", "n"] + [c for k in metrics for c in (k, k + "_min", k + "_max")]
        with open(a.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)
        print("wrote", a.csv)
    w0 = max(len(r["config"]) for r in rows)
    print(f"{'metric':24s}" + "".join(f"{r['config']:>{max(w0, 22) + 2}s}" for r in rows))
    print(f"{'n':24s}" + "".join(f"{r['n']:>{max(w0, 22) + 2}d}" for r in rows))
    for k in metrics:
        cells = []
        for r in rows:
            if k not in r:
                cells.append("-")
                continue
            v, lo, hi = r[k], r[k + "_min"], r[k + "_max"]
            fmt = (lambda x: f"{x:.4f}") if abs(v) < 100 else (lambda x: f"{x:,.0f}")
            cells.append(f"{fmt(v)} [{fmt(lo)}..{fmt(hi)}]" if r["n"] > 1 else fmt(v))
        print(f"{k:24s}" + "".join(f"{c:>{max(w0, 22) + 2}s}" for c in cells))


if __name__ == "__main__":
    main()
