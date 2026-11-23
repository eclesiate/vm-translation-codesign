#!/usr/bin/env python3
"""Task 1.1 plots: 4KB-only vs 4KB+2MB.

Usage: plot_t11.py SUMMARY_CSV OUT_DIR --reach LABEL=REACH_CSV [LABEL=REACH_CSV ...]
  SUMMARY_CSV  from `lab6_stats.py summary ... --csv`
  REACH_CSV    from `lab6_stats.py reach RUN --csv`
"""
import argparse
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

# Reference palette (dataviz skill, light mode): categorical slots 1-2, text/grid inks.
C4K, CMIX = "#2a78d6", "#eb6834"
INK, INK2, GRID, SURF = "#0b0b0b", "#5f5e5a", "#e4e3df", "#fcfcfb"
LABELS = {0.0: "4KB only", 1.0: "4KB + 2MB"}
COLORS = {0.0: C4K, 1.0: CMIX}

plt.rcParams.update({
    "font.size": 10, "axes.edgecolor": GRID, "axes.labelcolor": INK2,
    "xtick.color": INK2, "ytick.color": INK2, "axes.titlecolor": INK,
    "axes.titlesize": 11, "axes.titleweight": "bold", "figure.facecolor": SURF,
    "axes.facecolor": SURF, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False, "axes.axisbelow": True,
})


def bars(summary, out, figtitle="randacc_8gb, 20M instructions", fname="t11_comparison.png"):
    s = summary.sort_values("frag")
    panels = [
        ("l2tlb_hit_rate", "L2 TLB hit rate", lambda v: f"{v:.1%}", 1),
        ("cycles", "Total execution cycles (M)", lambda v: f"{v:.1f}M", 1e6),
        ("walk_cycles_sum", "Page-table-walk cycles (M, summed)", lambda v: f"{v:.1f}M", 1e6),
        ("ipc", "IPC", lambda v: f"{v:.3g}", 1),
    ]
    fig, axes = plt.subplots(1, len(panels), figsize=(13, 3.6))
    for ax, (col, title, f, scale) in zip(axes, panels):
        vals = s[col] / scale
        names = [LABELS.get(x, f"frag {x}") for x in s["frag"]]
        cols = [COLORS.get(x, INK2) for x in s["frag"]]
        bs = ax.bar(names, vals, color=cols, width=0.55, edgecolor=SURF, linewidth=2)
        for b, v in zip(bs, vals):
            ax.annotate(f(v), (b.get_x() + b.get_width() / 2, b.get_height()),
                        ha="center", va="bottom", fontsize=9, color=INK, xytext=(0, 2),
                        textcoords="offset points")
        ax.set_title(title, loc="left")
        ax.grid(axis="x", visible=False)
        ax.margins(y=0.15)
        if col == "l2tlb_hit_rate":
            ax.yaxis.set_major_formatter(matplotlib.ticker.PercentFormatter(1.0, decimals=0))
    fig.suptitle(f"{figtitle}: 4KB-only vs 4KB+2MB pages",
                 x=0.01, ha="left", color=INK, fontsize=12)
    fig.tight_layout()
    p = os.path.join(out, fname)
    fig.savefig(p, dpi=160)
    print("wrote", p)


def reach(series, out, zoom=None, name="t11_reach_timeseries.png", interval="100K"):
    if zoom:
        fig, (ax, axz) = plt.subplots(1, 2, figsize=(14, 4), gridspec_kw={"width_ratios": [2, 1]})
    else:
        fig, ax = plt.subplots(figsize=(10, 4))
        axz = None
    for i, (label, path) in enumerate(series):
        df = pd.read_csv(path)
        # colour follows the config (same as bar chart), not plotting order
        color = CMIX if "2MB" in label else C4K if "4KB" in label else None
        ax.plot(df["instructions"] / 1e6, df["reach_kb"] / 1024, lw=2, color=color, label=label)
        if axz is not None:
            z = df[(df["instructions"] >= zoom[0]) & (df["instructions"] <= zoom[1])]
            axz.plot(z["instructions"] / 1e6, z["reach_kb"] / 1024, lw=2, color=color, marker="o", ms=4,
                     markeredgecolor=SURF, markeredgewidth=1)
        last = df.iloc[-1]
        ax.annotate(f"{label}: {last['reach_kb'] / 1024:,.0f} MB",
                    (last["instructions"] / 1e6, last["reach_kb"] / 1024),
                    xytext=(-6, 6), textcoords="offset points", ha="right", fontsize=9, color=INK)
    ax.set_yscale("symlog", linthresh=1)
    ax.set_xlabel(f"Instructions (M), {interval}-instruction snapshots")
    if axz is not None:
        axz.set_yscale("symlog", linthresh=1)
        axz.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
        axz.set_xlabel("Instructions (M)")
        axz.set_title(f"Zoom: TLB warm-up ({zoom[0]/1e6:.2f}-{zoom[1]/1e6:.2f}M)", loc="left")
    ax.set_ylabel("L2 TLB memory reach (MB, log)")
    ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:,.0f}"))
    ax.set_title("L2 TLB memory reach over time (512 entries)", loc="left")
    if len(series) > 1:
        ax.legend(frameon=False, loc="center right")
    fig.tight_layout()
    p = os.path.join(out, name)
    fig.savefig(p, dpi=160)
    print("wrote", p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("summary")
    ap.add_argument("out")
    ap.add_argument("--reach", nargs="*", default=[])
    ap.add_argument("--zoom", help="lo,hi instruction range for a zoom panel")
    ap.add_argument("--name", default="t11_reach_timeseries.png")
    ap.add_argument("--interval", default="100K")
    ap.add_argument("--title", default="randacc_8gb, 20M instructions")
    ap.add_argument("--bars-name", default="t11_comparison.png")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    bars(pd.read_csv(a.summary), a.out, a.title, a.bars_name)
    if a.reach:
        zoom = tuple(float(x) for x in a.zoom.split(",")) if a.zoom else None
        reach([r.split("=", 1) for r in a.reach], a.out, zoom=zoom, name=a.name, interval=a.interval)


if __name__ == "__main__":
    main()
