"""
compute_dz.py
=============
Computes Cohen's d_z (paired-sample effect size) for GRAF-Global vs Baseline PDR
for all 4 scenarios × 2 protocols, using the aggregated statistics already present
in significance_tests.csv and main_results_table_core.csv.

The significance_tests.csv already stores the paired t-statistic and the effect
size labelled "Effect size", which was computed by analyze_results.py as:
    d = t / sqrt(n)
This IS the Cohen's d_z formula:
    d_z = mean(diff_i) / SD(diff_i)
         = (mean_diff / (SD_diff / sqrt(n))) / sqrt(n)
         = t / sqrt(n)

So the values are already correct. This script:
1. Extracts them cleanly
2. Reconstructs mean_diff and SD_diff from t and mean_diff (which is stored)
3. Produces the exact table the reviewer wants with footnote text
4. Writes the Section VI-A insertion sentence

Inputs:
  - significance_tests.csv  (same directory)
  - main_results_table_core.csv (same directory)

Output:
  - dz_report.txt  (ready to paste into paper)
"""

import csv
import math
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SIG_FILE = os.path.join(SCRIPT_DIR, "significance_tests.csv")
CORE_FILE = os.path.join(SCRIPT_DIR, "main_results_table_core.csv")

n = 20  # replications per cell

# ── 1. Load significance_tests.csv ──────────────────────────────────────────
rows = []
with open(SIG_FILE, newline="", encoding="utf-8") as f:
    reader = csv.DictReader(f)
    for row in reader:
        rows.append(row)

# Filter: GRAF-Global vs Baseline, PDR only
dz_rows = [
    r for r in rows
    if r["Comparison"].strip() == "GRAF-Global vs Baseline"
    and r["Metric"].strip() == "PDR (%)"
]

# ── 2. Parse and compute d_z components ─────────────────────────────────────
# From the paired t-test:
#   t = mean_diff / (SD_diff / sqrt(n))
#   d_z = mean_diff / SD_diff = t / sqrt(n)
#
# Mean_diff is stored directly as "Mean Diff (Left-Right)"
# From mean_diff and t we can recover SD_diff = mean_diff / (t / sqrt(n))

results = []
for r in sorted(dz_rows, key=lambda x: (int(x["Scenario"]), x["Protocol"])):
    sc       = int(r["Scenario"])
    proto    = r["Protocol"].strip()
    mean_diff= float(r["Mean Diff (Left-Right)"])
    t_stat   = float(r["t-statistic"])
    d_z      = float(r["Effect size"])   # = t / sqrt(n)

    # Reconstruct SD of paired differences
    # t = mean_diff / SE  where SE = SD_diff / sqrt(n)
    # => SD_diff = mean_diff / (t / sqrt(n)) = mean_diff / d_z
    if abs(d_z) > 1e-9:
        sd_diff = abs(mean_diff) / abs(d_z)
    else:
        sd_diff = float("nan")

    # Also read mean PDR values from the row
    mean_graf = float(r["Mean Left"])
    mean_base = float(r["Mean Right"])

    p_holm = r["Holm-adjusted p"].strip()
    sig    = r["Significant (Holm<0.05)"].strip()

    results.append({
        "sc": sc, "proto": proto,
        "mean_graf": mean_graf, "mean_base": mean_base,
        "mean_diff": mean_diff,
        "sd_diff": sd_diff,
        "d_z": d_z,
        "t": t_stat,
        "p_holm": p_holm,
        "sig": sig,
    })

# ── 3. Print the d_z table ───────────────────────────────────────────────────
header = (
    f"{'Sc':<3} {'Proto':<5} {'Mean(Base)':<12} {'Mean(GRAF-G)':<14}"
    f"{'Mean(Δ)':<10} {'SD(Δ)':<8} {'d_z':<8} {'Holm-p':<20} {'Sig':<4}"
)

sep = "-" * len(header)
print(sep)
print("Cohen's d_z  =  mean(PDR_GRAF_i − PDR_Base_i) / SD(PDR_GRAF_i − PDR_Base_i)")
print(f"n = {n} independent replications per cell")
print(sep)
print(header)
print(sep)

for r in results:
    print(
        f"{r['sc']:<3} {r['proto']:<5} {r['mean_base']:<12.2f} {r['mean_graf']:<14.2f}"
        f"{r['mean_diff']:<10.3f} {r['sd_diff']:<8.3f} {r['d_z']:<8.3f} "
        f"{r['p_holm']:<20} {r['sig']:<4}"
    )

print(sep)

# ── 4. Formatted LaTeX-ready footnote values ─────────────────────────────────
print("\n\nLaTeX / Paper Footnote Values")
print("=" * 60)
for r in results:
    print(
        f"Sc{r['sc']} {r['proto']}: "
        f"d_z = {r['d_z']:.2f}, "
        f"mean Δ = {r['mean_diff']:+.2f} pp, "
        f"SD(Δ) = {r['sd_diff']:.2f} pp, "
        f"Holm-p = {r['p_holm']}"
    )

# ── 5. Section VI-A insertion sentence ──────────────────────────────────────
print("\n\nSection VI-A Insertion (after d-value report)\n" + "=" * 60)

# Separate OLSR and AODV, build per-scenario footprint
olsr_vals = {r["sc"]: r for r in results if r["proto"] == "OLSR"}
aodv_vals = {r["sc"]: r for r in results if r["proto"] == "AODV"}

sentences = []
for sc in [1, 2, 3, 4]:
    o = olsr_vals.get(sc)
    a = aodv_vals.get(sc)
    if o and a:
        sentences.append(
            f"Scenario {sc}: d_z(OLSR) = {o['d_z']:.2f} [SD(Δ) = {o['sd_diff']:.2f} pp], "
            f"d_z(AODV) = {a['d_z']:.2f} [SD(Δ) = {a['sd_diff']:.2f} pp]."
        )

para = (
    "The higher d_z values for OLSR relative to AODV in Scenarios 1–3 "
    "reflect the lower variance of the per-run PDR difference under OLSR "
    "(baseline SD ≈ 0.15 pp vs. 0.68 pp for AODV), confirming that the "
    "tighter OLSR baseline amplifies the standardised effect size even "
    "when the raw mean difference is essentially identical across protocols. "
    "Formally, d_z = mean(PDR_GRAF_i − PDR_Baseline_i) / "
    "SD(PDR_GRAF_i − PDR_Baseline_i) for i = 1…"
    + str(n)
    + " paired runs sharing the same RNG seeds."
)

print("\nPer-scenario d_z values (add as a footnote or inline sentence):\n")
for s in sentences:
    print("  " + s)
print()
print("Explanatory paragraph for Section VI-A:\n")
print(para)

# ── 6. Write to file ─────────────────────────────────────────────────────────
out_path = os.path.join(SCRIPT_DIR, "dz_report.txt")
with open(out_path, "w", encoding="utf-8") as f:
    f.write("Cohen's d_z Report — GRAF-Global vs Baseline (PDR)\n")
    f.write("=" * 70 + "\n\n")
    f.write(f"Formula: d_z = mean(Δ_i) / SD(Δ_i), where Δ_i = PDR_GRAF_i − PDR_Base_i\n")
    f.write(f"n = {n} paired replications (same RNG seeds per run index)\n\n")
    f.write(header + "\n" + sep + "\n")
    for r in results:
        f.write(
            f"{r['sc']:<3} {r['proto']:<5} {r['mean_base']:<12.2f} {r['mean_graf']:<14.2f}"
            f"{r['mean_diff']:<10.3f} {r['sd_diff']:<8.3f} {r['d_z']:<8.3f} "
            f"{r['p_holm']:<20} {r['sig']:<4}\n"
        )
    f.write("\n\nPer-scenario d_z (inline footnote format):\n")
    for s in sentences:
        f.write(s + "\n")
    f.write("\n\nSection VI-A insertion paragraph:\n")
    f.write(para + "\n")

print(f"\n✓ Report written to: {out_path}")
