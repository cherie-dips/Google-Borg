#!/usr/bin/env python3
"""Turns the CSV files written by borgsim into PNG charts (results/*.png).

    python3 tools/plot.py [results_dir]
"""
import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

RESULTS = sys.argv[1] if len(sys.argv) > 1 else "results"
BLUE, AMBER, GREEN, RED, PURPLE, GREY = "#4c7ff0", "#e8992f", "#2fb37c", "#e05252", "#9672e8", "#8a91a3"


def load(name):
    path = os.path.join(RESULTS, name + ".csv")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return list(csv.DictReader(f))


def barh(ax, labels, values, colors, fmt="{:.0f}"):
    y = range(len(labels))
    ax.barh(y, values, color=colors)
    ax.set_yticks(list(y), labels)
    ax.invert_yaxis()
    for i, v in enumerate(values):
        ax.text(v, i, " " + fmt.format(v), va="center", fontsize=9)
    ax.spines[["top", "right"]].set_visible(False)
    ax.margins(x=0.15)


def save(fig, name):
    fig.tight_layout()
    out = os.path.join(RESULTS, name + ".png")
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print("wrote", out)


def main():
    if rows := load("policies"):
        fig, ax = plt.subplots(figsize=(6, 2.6))
        barh(ax, [r["policy"] for r in rows], [int(r["p90_machines"]) for r in rows], [RED, BLUE, GREEN])
        ax.set_xlabel("machines needed (p90 over seeds)")
        ax.set_title("Scoring policy vs compacted cell size (§3.2)")
        save(fig, "policies")

    if rows := load("segregation"):
        fig, ax = plt.subplots(figsize=(7, 2.8))
        labels, y = [], 0
        for rec in ("1", "0"):
            g = {r["config"]: int(r["p90_machines"]) for r in rows if r.get("reclamation", "0") == rec}
            if not g:
                continue
            tag = "with reclamation" if rec == "1" else "limits only"
            pct = 100 * (g["segregated_total"] - g["shared"]) / g["shared"]
            ax.barh([y], [g["shared"]], color=GREEN)
            ax.barh([y + 1], [g["prod_only"]], color=BLUE)
            ax.barh([y + 1], [g["nonprod_only"]], left=[g["prod_only"]], color=AMBER)
            ax.text(g["segregated_total"], y + 1, f" {pct:+.1f}%", va="center", fontsize=9)
            labels += [f"shared · {tag}", f"segregated · {tag}"]
            y += 2
        ax.set_yticks(range(len(labels)), labels)
        ax.invert_yaxis()
        ax.set_title("Separate prod (blue) and non-prod (amber) cells (Fig. 5)")
        ax.set_xlabel("machines needed")
        ax.spines[["top", "right"]].set_visible(False)
        ax.margins(x=0.12)
        save(fig, "segregation")

    if rows := load("buckets"):
        labels = ["exact" if r["variant"] == "exact" else f"buckets ≥ 1/{round(1 / float(r['min_bucket']))}" for r in rows]
        fig, ax = plt.subplots(figsize=(6, 2.6))
        barh(ax, labels, [int(r["p90_machines"]) for r in rows], [GREEN] + [AMBER] * (len(rows) - 1))
        ax.set_xlabel("machines needed")
        ax.set_title("Exact vs power-of-two bucketed requests (Fig. 8)")
        save(fig, "buckets")

    if rows := load("scalability"):
        fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 2.8))
        labels = [r["variant"].replace("_", " ") for r in rows]
        barh(a1, labels, [float(r["elapsed_ms"]) for r in rows], PURPLE, "{:.0f} ms")
        a1.set_xscale("log")
        a1.set_title("Time for one scheduling pass (log)")
        barh(a2, labels, [int(r["feasibility_checks"]) + int(r["score_computations"]) for r in rows], GREY, "{:,.0f}")
        a2.set_xscale("log")
        a2.set_yticks([])
        a2.set_title("Feasibility checks + score computations (log)")
        fig.suptitle("§3.4 scheduler optimizations")
        save(fig, "scalability")

    if rows := load("hybrid_alpha"):
        fig, ax = plt.subplots(figsize=(6, 2.8))
        barh(ax, [f"α = {float(r['alpha']):g}" for r in rows], [int(r["p90_machines"]) for r in rows],
             [GREEN if float(r["alpha"]) == 10 else BLUE for r in rows])
        ax.set_xlabel("machines needed (α = 0 is best fit)")
        ax.set_title("Hybrid policy: weight of the stranding term")
        save(fig, "hybrid_alpha")

    if rows := load("reclamation"):
        g = {r["variant"]: int(r["p90_machines"]) for r in rows}
        fig, ax = plt.subplots(figsize=(6, 2.0))
        barh(ax, ["with reclamation", "without reclamation"], [g["with_reclamation"], g["without_reclamation"]], [GREEN, RED])
        ax.set_xlabel("machines needed")
        ax.set_title("Resource reclamation (Fig. 10)")
        save(fig, "reclamation")


if __name__ == "__main__":
    main()
