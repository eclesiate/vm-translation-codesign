#!/usr/bin/env python3
"""Final plots for Tasks 1.2, 1.4 and 2.3 (bars = mean over repetitions, whiskers = min..max).

Usage: plot_final.py [--results results] [--out results/plots]
Expects runs named t12_* (Task 1.2 policy sweep), f14_*_rN (Task 1.4), f23_*_rN (Task 2.3).
"""
import argparse
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aggregate import collect  # noqa: E402
from lab6_stats import read_stats  # noqa: E402

# dataviz reference palette (light): categorical slots 1-2, neutral baseline, inks
BLUE, ORANGE, GRAY = "#2a78d6", "#eb6834", "#a3a29c"
INK, INK2, GRID, SURF = "#0b0b0b", "#5f5e5a", "#e4e3df", "#fcfcfb"
plt.rcParams.update({
    "font.size": 9.5, "axes.edgecolor": GRID, "axes.labelcolor": INK2, "xtick.color": INK2,
    "ytick.color": INK2, "axes.titlecolor": INK, "axes.titlesize": 10.5, "axes.titleweight": "bold",
    "figure.facecolor": SURF, "axes.facecolor": SURF, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.8, "axes.spines.top": False, "axes.spines.right": False, "axes.axisbelow": True,
})
pct = matplotlib.ticker.PercentFormatter(1.0, decimals=0)


def stat(runs, key):
    vals = np.array([r[key] for r in runs if r.get(key) is not None], dtype=float)
    return vals.mean(), vals.min(), vals.max()


def bar_panel(ax, labels, runs_list, key, title, colors, fmt, scale=1.0, yfmt=None, ref=None):
    means, los, his = zip(*[stat(r, key) for r in runs_list])
    means, los, his = (np.array(x) / scale for x in (means, los, his))
    x = np.arange(len(labels))
    ax.bar(x, means, color=colors, width=0.62, edgecolor=SURF, linewidth=2)
    if any(len(r) > 1 for r in runs_list):
        ax.errorbar(x, means, yerr=[means - los, his - means], fmt="none", ecolor=INK, elinewidth=1, capsize=3)
    for xi, m in zip(x, means):
        ax.annotate(fmt(m), (xi, m), xytext=(0, 4), textcoords="offset points", ha="center",
                    va="bottom", fontsize=8.5, color=INK)
    if ref is not None:
        ax.axhline(ref, color=INK2, lw=1, ls="--")
    ax.set_xticks(x, labels, rotation=0)
    ax.set_title(title, loc="left")
    ax.grid(axis="x", visible=False)
    ax.margins(y=0.18)
    if yfmt:
        ax.yaxis.set_major_formatter(yfmt)


def plot_t12(res, out):
    order = [("t12_frag0.25", "LRU"), ("t12_srrip", "SRRIP"), ("t12_psa_default", "PSA\ndefault"),
             ("t12_psa_age4", "PSA\nage/4"), ("t12_psa_cap7", "PSA\ncap 7"), ("t12_psa_ins0", "PSA\nins 2M=0")]
    from lab6_stats import summarize
    runs = [(lab, [summarize(os.path.join(res, n))]) for n, lab in order if os.path.isfile(os.path.join(res, n, "sim.stats"))]
    if not runs:
        return
    labels, rl = zip(*runs)
    colors = [GRAY if l in ("LRU", "SRRIP") else BLUE for l in labels]
    for r in rl:
        r[0]["reach_mb"] = r[0]["l2tlb_reach_kb_end"] / 1024
    fig, axes = plt.subplots(1, 3, figsize=(14, 3.8))
    bar_panel(axes[0], labels, rl, "l2tlb_hit_rate", "L2 TLB hit rate", colors, lambda v: f"{v:.1%}", yfmt=pct)
    bar_panel(axes[1], labels, rl, "reach_mb", "L2 TLB reach at end (MB)", colors, lambda v: f"{v:,.0f}")
    bar_panel(axes[2], labels, rl, "l2tlb_mpki", "L2 TLB MPKI", colors, lambda v: f"{v:.1f}")
    fig.suptitle("Task 1.2 - page-size-aware L2 TLB replacement (randacc_8gb, ~50/50 4KB/2MB, 20M instr)",
                 x=0.01, ha="left", fontsize=11.5, color=INK)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "t12_tlb_policies.png"), dpi=160)
    print("wrote", os.path.join(out, "t12_tlb_policies.png"))


def plot_f14(res, out):
    g = collect("f14_", res)
    order = [("f14_base", "Baseline\nLRU+SRRIP"), ("f14_tlb", "PSA TLB"), ("f14_llc_bloom", "PTE-LLC\n(Bloom)"),
             ("f14_llc_type", "PTE-LLC\n(all PTEs)"), ("f14_comb", "PSA+PTE-LLC\n(Bloom)"), ("f14_comb_type", "PSA+PTE-LLC\n(all PTEs)")]
    order = [(k, l) for k, l in order if k in g]
    if not order:
        return
    labels = [l for _, l in order]
    rl = [g[k] for k, _ in order]
    base_cycles = stat(g["f14_base"], "cycles")[0] if "f14_base" in g else None
    for runs in rl:
        for r in runs:
            r["norm_cycles"] = r["cycles"] / base_cycles if base_cycles else None
    colors = [GRAY] + [BLUE] * (len(labels) - 1)
    fig, axes = plt.subplots(2, 2, figsize=(13, 7.2))
    bar_panel(axes[0, 0], labels, rl, "l2tlb_hit_rate", "L2 TLB hit rate", colors, lambda v: f"{v:.1%}", yfmt=pct)
    bar_panel(axes[0, 1], labels, rl, "ptw_llc_hit_rate", "LLC hit rate of page-walk loads (LLC vs DRAM)",
              colors, lambda v: f"{v:.1%}", yfmt=pct)
    bar_panel(axes[1, 0], labels, rl, "avg_walk_cycles", "Average page-walk latency (cycles)", colors, lambda v: f"{v:.0f}")
    bar_panel(axes[1, 1], labels, rl, "norm_cycles", "Total cycles, normalised to baseline (lower is better)",
              colors, lambda v: f"{v:.3f}", ref=1.0)
    lo = min(stat(r, "norm_cycles")[1] for r in rl)
    axes[1, 1].set_ylim(lo - 0.02, 1.01)
    fig.suptitle("Task 1.4 - TLB + LLC co-design vs default (randacc_8gb, ~50/50 4KB/2MB, 20M instr; "
                 "bars = mean of 3 runs, whiskers = min..max)", x=0.01, ha="left", fontsize=11.5, color=INK)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "t14_codesign.png"), dpi=160)
    print("wrote", os.path.join(out, "t14_codesign.png"))


def plot_f23(res, out):
    g = collect("f23_", res)
    if "f23_mixed" not in g or "f23_isolated" not in g:
        return
    mixed, iso = g["f23_mixed"], g["f23_isolated"]
    fig, axes = plt.subplots(1, 3, figsize=(14, 3.9))
    # (a) RB hit rate by request type
    ax = axes[0]
    x = np.arange(2)
    for i, (runs, lab, col) in enumerate([(mixed, "Mixed (baseline)", GRAY), (iso, "Isolated (PT in bank 15)", BLUE)]):
        ms = [stat(runs, "rb_hit_rate_pt"), stat(runs, "rb_hit_rate_data")]
        m = np.array([a[0] for a in ms]); lo = np.array([a[1] for a in ms]); hi = np.array([a[2] for a in ms])
        xx = x + (i - 0.5) * 0.36
        ax.bar(xx, m, width=0.34, color=col, edgecolor=SURF, linewidth=2, label=lab)
        ax.errorbar(xx, m, yerr=[m - lo, hi - m], fmt="none", ecolor=INK, elinewidth=1, capsize=3)
        for a, b in zip(xx, m):
            ax.annotate(f"{b:.1%}", (a, b), xytext=(0, 3), textcoords="offset points", ha="center", fontsize=8.5, color=INK)
    ax.set_xticks(x, ["PAGE_TABLE requests", "NON_PAGE_TABLE (data)"])
    ax.yaxis.set_major_formatter(pct)
    ax.set_title("DRAM row-buffer hit rate", loc="left")
    ax.grid(axis="x", visible=False)
    ax.legend(frameon=False, fontsize=8.5, loc="upper right")
    ax.margins(y=0.2)
    # (b) average DRAM latency by type
    ax = axes[1]
    for i, (runs, col) in enumerate([(mixed, GRAY), (iso, BLUE)]):
        ms = [stat(runs, "dram_avg_ns_pt"), stat(runs, "dram_avg_ns_data")]
        m = np.array([a[0] for a in ms]); lo = np.array([a[1] for a in ms]); hi = np.array([a[2] for a in ms])
        xx = x + (i - 0.5) * 0.36
        ax.bar(xx, m, width=0.34, color=col, edgecolor=SURF, linewidth=2)
        ax.errorbar(xx, m, yerr=[m - lo, hi - m], fmt="none", ecolor=INK, elinewidth=1, capsize=3)
        for a, b in zip(xx, m):
            ax.annotate(f"{b:.1f}", (a, b), xytext=(0, 3), textcoords="offset points", ha="center", fontsize=8.5, color=INK)
    ax.set_xticks(x, ["PAGE_TABLE", "data"])
    ax.set_title("Average DRAM access latency (ns)", loc="left")
    ax.grid(axis="x", visible=False)
    ax.margins(y=0.2)
    # (c) IPC
    bar_panel(axes[2], ["Mixed", "Isolated"], [mixed, iso], "ipc", "IPC (mean of 3, min..max)",
              [GRAY, BLUE], lambda v: f"{v:.4f}")
    lo = min(stat(mixed, "ipc")[1], stat(iso, "ipc")[1])
    hi = max(stat(mixed, "ipc")[2], stat(iso, "ipc")[2])
    axes[2].set_ylim(lo - (hi - lo) * 3 - 0.002, hi + (hi - lo) * 3 + 0.002)
    fig.suptitle("Task 2.3 - DRAM-aware page-table placement (randacc_8gb, 4KB pages only, 20M instr)",
                 x=0.01, ha="left", fontsize=11.5, color=INK)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "t23_placement.png"), dpi=160)
    print("wrote", os.path.join(out, "t23_placement.png"))

    # per-bank access distribution (hot bank), first repetition of each
    fig, axes = plt.subplots(1, 2, figsize=(13, 3.6), sharey=True)
    for ax, name, title in [(axes[0], "f23_mixed_r1", "Mixed (baseline)"), (axes[1], "f23_isolated_r1", "Isolated (PT in bank 15)")]:
        p = os.path.join(res, name)
        if not os.path.isfile(os.path.join(p, "sim.stats")):
            continue
        s = read_stats(p)
        nb = len([k for k in s if re.match(r"ddr\.bank-\d+-accesses-pt$", k)])
        pt = np.array([s.get(f"ddr.bank-{b}-accesses-pt", 0) for b in range(nb)]) / 1e3
        da = np.array([s.get(f"ddr.bank-{b}-accesses-data", 0) for b in range(nb)]) / 1e3
        b = np.arange(nb)
        ax.bar(b, da, color=GRAY, width=0.8, edgecolor=SURF, linewidth=1, label="data")
        ax.bar(b, pt, bottom=da, color=ORANGE, width=0.8, edgecolor=SURF, linewidth=1, label="PAGE_TABLE")
        ax.set_xticks(b)
        ax.set_xlabel("DRAM bank index (summed over 4 channels x 4 ranks)")
        ax.set_title(title, loc="left")
        ax.grid(axis="x", visible=False)
    axes[0].set_ylabel("DRAM requests (thousands)")
    axes[1].legend(frameon=False, fontsize=8.5, loc="upper left")
    fig.suptitle("Task 2.3 - where DRAM requests land: isolation turns bank 15 into a page-table hot bank",
                 x=0.01, ha="left", fontsize=11.5, color=INK)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "t23_bank_distribution.png"), dpi=160)
    print("wrote", os.path.join(out, "t23_bank_distribution.png"))


def plot_f23_ablation(res, out):
    g = collect("f23_", res)
    order = [("f23_mixed", "Mixed\n(baseline)"), ("f23_dataonly", "data kept\nout of\nbank 15"),
             ("f23_ptonly", "PT in\nbank 15,\ndata anywhere"), ("f23_isolated", "isolated\n1 bank"),
             ("f23_iso2banks", "isolated\n2 banks"), ("f23_iso4banks", "isolated\n4 banks")]
    order = [(k, l) for k, l in order if k in g]
    if len(order) < 3:
        return
    labels = [l for _, l in order]
    rl = [g[k] for k, _ in order]
    base = stat(g["f23_mixed"], "ipc")[0]
    for runs in rl:
        for r in runs:
            r["norm_ipc"] = r["ipc"] / base
    colors = [GRAY] + [BLUE] * (len(labels) - 1)
    fig, axes = plt.subplots(1, 3, figsize=(17, 4.2))
    bar_panel(axes[0], labels, rl, "rb_hit_rate_pt", "Row-buffer hit rate: PAGE_TABLE", colors, lambda v: f"{v:.1%}", yfmt=pct)
    bar_panel(axes[1], labels, rl, "rb_hit_rate_data", "Row-buffer hit rate: data", colors, lambda v: f"{v:.1%}", yfmt=pct)
    bar_panel(axes[2], labels, rl, "norm_ipc", "IPC normalised to mixed (higher is better)", colors,
              lambda v: f"{v:.3f}", ref=1.0)
    lo = min(stat(r, "norm_ipc")[1] for r in rl)
    hi = max(stat(r, "norm_ipc")[2] for r in rl)
    axes[2].set_ylim(min(lo, 1.0) - 0.01, max(hi, 1.0) + 0.009)
    for ax in axes:
        ax.tick_params(axis="x", labelsize=8)
    fig.suptitle("Task 2.3 ablation - which part of page-table isolation costs performance? (3 runs each)",
                 x=0.01, ha="left", fontsize=11.5, color=INK)
    fig.tight_layout()
    fig.savefig(os.path.join(out, "t23_ablation.png"), dpi=160)
    print("wrote", os.path.join(out, "t23_ablation.png"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results")
    ap.add_argument("--out", default="results/plots")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    plot_t12(a.results, a.out)
    plot_f14(a.results, a.out)
    plot_f23(a.results, a.out)
    plot_f23_ablation(a.results, a.out)


if __name__ == "__main__":
    main()
