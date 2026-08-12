#!/usr/bin/env python3
"""
analyze_results_review_ready.py

Reviewer-ready analysis for hybrid star-mesh GRAF simulations.

Key fixes over the previous script:
- uses paired tests on matched runs when possible
- excludes baseline-vs-GRAF tests for service restoration latency
- adds GRAF-Global vs GRAF-Local tests
- reports effect sizes and Holm-corrected p-values
- formats tiny metrics like energy/bit in scientific notation
- can plot 95% confidence intervals instead of SD
"""

import argparse
import glob
import math
import os
import sys
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import scipy.stats as st


NA_METRICS = {
    "mean_reconv_s",
    "median_reconv_s",
    "max_reconv_s",
    "cluster_recovery_rate_percent",
}

MODE_ORDER = ["Baseline", "GRAF-Global", "GRAF-Local"]
MODE_COLORS = {
    "Baseline": "#6c757d",
    "GRAF-Global": "#0d6efd",
    "GRAF-Local": "#198754",
}
MODE_MARKERS = {"Baseline": "o", "GRAF-Global": "s", "GRAF-Local": "^"}
SCENARIO_LABELS = {1: "1 (Mild)", 2: "2 (Moderate)", 3: "3 (Severe)", 4: "4 (Extreme)"}
PRIMARY_METRICS = {
    "pdr_percent": "PDR (%)",
    "throughput_kbps_active_window": "Throughput (kbps)",
    "avg_delay_ms": "Avg Delay (ms)",
    "jain_fairness_index": "Jain Fairness Index",
    "total_consumed_ch_energy_j": "CH Energy Consumed (J)",
    "energy_per_delivered_bit_j": "Energy/Bit (J/bit)",
}
GRAF_ONLY_METRICS = {
    "mean_reconv_s": "Service Restoration Latency (s)",
    "cluster_recovery_rate_percent": "End-to-End Cluster Recovery (%)",
    "sensor_recovery_rate_percent": "Sensor Recovery (%)",
    "total_recovery_bytes": "App Bytes Recovered Post-Failure",
}
PAIR_KEYS = ["scenario", "protocol", "seed", "run"]


def parse_summary(path: str) -> Dict[str, object]:
    data: Dict[str, object] = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("metric"):
                continue
            parts = line.split(",", 1)
            if len(parts) != 2:
                continue
            key, val = parts
            try:
                data[key] = float(val)
            except ValueError:
                data[key] = val
    return data


def load_all(results_dir: str) -> pd.DataFrame:
    pattern = os.path.join(results_dir, "*_summary.csv")
    rows = [parse_summary(path) for path in sorted(glob.glob(pattern))]
    rows = [r for r in rows if r]
    if not rows:
        print(f"ERROR: no *_summary.csv files found in {results_dir}")
        sys.exit(1)

    df = pd.DataFrame(rows)
    if "graf" not in df.columns:
        raise RuntimeError("Missing 'graf' column in summaries.")

    df["mode"] = df["graf"].apply(lambda x: "Baseline" if x == "off" else f"GRAF-{str(x).capitalize()}")

    for col in ["scenario", "seed", "run"]:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce").astype("Int64")

    numeric_cols = [
        "pdr_percent", "throughput_kbps_active_window", "avg_delay_ms",
        "total_rx_packets", "total_tx_packets", "total_lost_packets",
        "mean_reconv_s", "median_reconv_s", "max_reconv_s",
        "cluster_recovery_rate_percent", "sensor_recovery_rate_percent",
        "total_recovery_bytes", "jain_fairness_index",
        "total_consumed_ch_energy_j", "total_residual_ch_energy_j",
        "chs_depleted", "energy_per_delivered_bit_j",
        "hb_detected_count", "hb_mean_detection_latency_s",
        "routing_overhead_bytes", "routing_overhead_packets",
        "normalized_overhead_ctrl_per_data",
    ]
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    return df


def valid_series(series: pd.Series, col_name: str) -> pd.Series:
    if col_name in NA_METRICS:
        return series[series >= 0].dropna()
    return series.dropna()


def format_value(val: float, col_name: str) -> str:
    if pd.isna(val):
        return "N/A"
    if col_name == "energy_per_delivered_bit_j":
        return f"{val:.3e}"
    if col_name in {"normalized_overhead_ctrl_per_data"}:
        return f"{val:.4f}"
    return f"{val:.2f}"


def fmt(series: pd.Series, col_name: str) -> str:
    valid = valid_series(series, col_name)
    if valid.empty:
        return "N/A"
    n = len(valid)
    mean = valid.mean()
    if n == 1:
        return f"{format_value(mean, col_name)} [n=1]"
    std = valid.std(ddof=1)
    sem = std / math.sqrt(n)
    ci = st.t.interval(0.95, n - 1, loc=mean, scale=sem)
    ci_half = (ci[1] - ci[0]) / 2 if np.isfinite(ci[1]) else np.nan
    return f"{format_value(mean, col_name)} ± {format_value(std, col_name)} (95% CI ± {format_value(ci_half, col_name)}) [n={n}]"


def summarize_table(df: pd.DataFrame, cols: Sequence[Tuple[str, str]], out_csv: str) -> pd.DataFrame:
    rows: List[Dict[str, str]] = []
    for (scen, proto, mode), g in df.groupby(["scenario", "protocol", "mode"], dropna=False):
        row: Dict[str, str] = {"Scenario": int(scen), "Protocol": str(proto), "Mode": str(mode)}
        for src, label in cols:
            row[label] = fmt(g[src], src) if src in g.columns else "—"
        rows.append(row)
    tbl = pd.DataFrame(rows).sort_values(["Scenario", "Protocol", "Mode"]).reset_index(drop=True)
    tbl.to_csv(out_csv, index=False)
    return tbl


def main_tables(df: pd.DataFrame, out_dir: str) -> Tuple[pd.DataFrame, pd.DataFrame]:
    core_cols = [
        ("pdr_percent", "PDR (%)"),
        ("throughput_kbps_active_window", "Throughput (kbps)"),
        ("avg_delay_ms", "Avg Delay (ms)"),
        ("mean_reconv_s", "Service Restoration Latency (s)"),
        ("cluster_recovery_rate_percent", "End-to-End Cluster Recovery (%)"),
        ("sensor_recovery_rate_percent", "Sensor Recovery (%)"),
        ("jain_fairness_index", "Jain Fairness Index"),
        ("total_consumed_ch_energy_j", "CH Energy Consumed (J)"),
        ("chs_depleted", "CHs Depleted"),
        ("energy_per_delivered_bit_j", "Energy/Bit (J/bit)"),
        ("hb_detected_count", "HB Detections"),
        ("hb_mean_detection_latency_s", "HB Mean Latency (s)"),
    ]
    appendix_cols = [
        ("total_rx_packets", "Rx Packets"),
        ("total_recovery_bytes", "App Bytes Recovered Post-Failure"),
        ("routing_overhead_bytes", "Routing Overhead (Bytes)"),
        ("normalized_overhead_ctrl_per_data", "Normalized Routing Overhead"),
    ]
    core = summarize_table(df, core_cols, os.path.join(out_dir, "main_results_table_core.csv"))
    appendix = summarize_table(df, appendix_cols, os.path.join(out_dir, "main_results_table_appendix.csv"))
    print("\n===== MAIN RESULTS TABLE (mean ± SD, with 95% CI) =====")
    print(core.to_string(index=False))
    return core, appendix


def improvement_table(df: pd.DataFrame, out_dir: str) -> pd.DataFrame:
    delta_cols = [
        ("pdr_percent", "ΔPDR (pp)"),
        ("throughput_kbps_active_window", "ΔThroughput (kbps)"),
        ("total_rx_packets", "ΔRx Packets"),
        ("jain_fairness_index", "ΔJain Fairness"),
    ]
    rows: List[Dict[str, str]] = []
    for (scen, proto), g in df.groupby(["scenario", "protocol"]):
        base = g[g["mode"] == "Baseline"]
        if base.empty:
            continue
        for mode in ["GRAF-Global", "GRAF-Local"]:
            sub = g[g["mode"] == mode]
            if sub.empty:
                continue
            row: Dict[str, str] = {"Scenario": int(scen), "Protocol": str(proto), "Mode vs Baseline": f"{mode} vs Baseline"}
            for src, label in delta_cols:
                if src not in g.columns:
                    row[label] = "—"
                    continue
                row[label] = f"{sub[src].mean() - base[src].mean():+.2f}"
            rows.append(row)
    tbl = pd.DataFrame(rows).sort_values(["Scenario", "Protocol", "Mode vs Baseline"]).reset_index(drop=True)
    tbl.to_csv(os.path.join(out_dir, "improvement_table.csv"), index=False)
    print("\n===== IMPROVEMENT TABLE =====")
    print(tbl.to_string(index=False))
    return tbl


def audit_runs(df: pd.DataFrame) -> None:
    print("\n===== AUDIT =====")
    required = ["scenario", "protocol", "mode", "run"]
    missing_cols = [c for c in required if c not in df.columns]
    if missing_cols:
        print(f"[WARN] Missing columns for full audit: {missing_cols}")
    dup = df[df.duplicated(subset=[c for c in required if c in df.columns], keep=False)]
    if not dup.empty:
        print("[WARN] Duplicate scenario/protocol/mode/run combinations found:")
        print(dup[[c for c in required if c in dup.columns]].to_string(index=False))
    else:
        print("[PASS] No duplicated scenario/protocol/mode/run combinations.")
    issues: List[str] = []
    if (df.get("pdr_percent", pd.Series(dtype=float)) > 100).any():
        issues.append("PDR > 100%")
    if (df.get("avg_delay_ms", pd.Series(dtype=float)) < 0).any():
        issues.append("Negative delay")
    if (df.get("throughput_kbps_active_window", pd.Series(dtype=float)) < 0).any():
        issues.append("Negative throughput")
    if issues:
        print(f"[CRITICAL] Impossible values found: {', '.join(issues)}")
    else:
        print("[PASS] Metric values are within physical bounds.")


def paired_effect_size(x: np.ndarray, y: np.ndarray) -> float:
    diff = x - y
    sd = diff.std(ddof=1)
    if sd == 0 or np.isnan(sd):
        return np.nan
    return diff.mean() / sd


def independent_effect_size(x: np.ndarray, y: np.ndarray) -> float:
    nx, ny = len(x), len(y)
    if nx < 2 or ny < 2:
        return np.nan
    vx, vy = x.var(ddof=1), y.var(ddof=1)
    pooled = ((nx - 1) * vx + (ny - 1) * vy) / (nx + ny - 2)
    if pooled <= 0 or np.isnan(pooled):
        return np.nan
    return (x.mean() - y.mean()) / math.sqrt(pooled)


def holm_correction(p_vals: Sequence[float]) -> List[float]:
    m = len(p_vals)
    order = np.argsort(p_vals)
    adj = np.empty(m, dtype=float)
    running_max = 0.0
    for rank, idx in enumerate(order):
        adj_p = (m - rank) * p_vals[idx]
        running_max = max(running_max, adj_p)
        adj[idx] = min(running_max, 1.0)
    return adj.tolist()


def aligned_pair_values(left: pd.DataFrame, right: pd.DataFrame, metric: str) -> Tuple[np.ndarray, np.ndarray]:
    keys = [k for k in PAIR_KEYS if k in left.columns and k in right.columns]
    if not keys:
        return np.array([]), np.array([])
    l = left[keys + [metric]].rename(columns={metric: "left_val"})
    r = right[keys + [metric]].rename(columns={metric: "right_val"})
    if metric in NA_METRICS:
        l = l[l["left_val"] >= 0]
        r = r[r["right_val"] >= 0]
    merged = l.merge(r, on=keys, how="inner").dropna()
    return merged["left_val"].to_numpy(), merged["right_val"].to_numpy()


def run_significance_tests(df: pd.DataFrame, out_dir: str) -> pd.DataFrame:
    print("\n===== STATISTICAL SIGNIFICANCE TESTS =====")
    rows: List[Dict[str, object]] = []
    # comparisons that apply to all modes
    all_mode_metrics = {
        "pdr_percent": "PDR (%)",
        "throughput_kbps_active_window": "Throughput (kbps)",
        "avg_delay_ms": "Avg Delay (ms)",
        "jain_fairness_index": "Jain Fairness Index",
        "total_consumed_ch_energy_j": "CH Energy Consumed (J)",
    }
    # graf-only comparisons
    graf_metrics = {
        "mean_reconv_s": "Service Restoration Latency (s)",
        "cluster_recovery_rate_percent": "End-to-End Cluster Recovery (%)",
        "sensor_recovery_rate_percent": "Sensor Recovery (%)",
        "total_recovery_bytes": "App Bytes Recovered Post-Failure",
    }

    for (scen, proto), g in df.groupby(["scenario", "protocol"]):
        groups = {m: g[g["mode"] == m].copy() for m in MODE_ORDER if not g[g["mode"] == m].empty}

        for left_mode, right_mode in [("GRAF-Global", "Baseline"), ("GRAF-Local", "Baseline"), ("GRAF-Global", "GRAF-Local")]:
            if left_mode not in groups or right_mode not in groups:
                continue
            metric_map = dict(all_mode_metrics)
            if "GRAF" in left_mode and "GRAF" in right_mode:
                metric_map.update(graf_metrics)
            for m_key, m_label in metric_map.items():
                if m_key not in df.columns:
                    continue
                x, y = aligned_pair_values(groups[left_mode], groups[right_mode], m_key)
                test_type = "paired"
                if len(x) >= 2 and len(y) >= 2:
                    t_stat, p_val = st.ttest_rel(x, y)
                    effect = paired_effect_size(x, y)
                    n = len(x)
                else:
                    x = valid_series(groups[left_mode][m_key], m_key).to_numpy()
                    y = valid_series(groups[right_mode][m_key], m_key).to_numpy()
                    if len(x) < 2 or len(y) < 2:
                        continue
                    t_stat, p_val = st.ttest_ind(x, y, equal_var=False)
                    effect = independent_effect_size(x, y)
                    n = min(len(x), len(y))
                    test_type = "Welch"
                rows.append({
                    "Scenario": int(scen),
                    "Protocol": str(proto),
                    "Comparison": f"{left_mode} vs {right_mode}",
                    "Metric": m_label,
                    "Mean Left": float(np.mean(x)),
                    "Mean Right": float(np.mean(y)),
                    "Mean Diff (Left-Right)": float(np.mean(x) - np.mean(y)),
                    "n": int(n),
                    "Test": test_type,
                    "t-statistic": float(t_stat),
                    "p-value": float(p_val),
                    "Effect size": float(effect) if not np.isnan(effect) else np.nan,
                })

    sig_df = pd.DataFrame(rows)
    if sig_df.empty:
        print("No significance tests were produced.")
        return sig_df
    sig_df["Holm-adjusted p"] = holm_correction(sig_df["p-value"].tolist())
    sig_df["Significant (Holm<0.05)"] = np.where(sig_df["Holm-adjusted p"] < 0.05, "Yes", "No")
    sig_df = sig_df.sort_values(["Scenario", "Protocol", "Comparison", "Metric"]).reset_index(drop=True)
    sig_df.to_csv(os.path.join(out_dir, "significance_tests.csv"), index=False)
    display_cols = [
        "Scenario", "Protocol", "Comparison", "Metric", "Mean Diff (Left-Right)",
        "Test", "p-value", "Holm-adjusted p", "Effect size", "Significant (Holm<0.05)"
    ]
    print(sig_df[display_cols].to_string(index=False))
    print(f"\nSaved significance tests to {os.path.join(out_dir, 'significance_tests.csv')}")
    return sig_df


def agg_with_error(df: pd.DataFrame, metric: str, use_ci: bool = True) -> pd.DataFrame:
    def err(vals: pd.Series) -> float:
        vals = valid_series(vals, metric)
        n = len(vals)
        if n <= 1:
            return 0.0
        std = vals.std(ddof=1)
        if not use_ci:
            return float(std)
        sem = std / math.sqrt(n)
        tcrit = st.t.ppf(0.975, n - 1)
        return float(tcrit * sem)

    out = []
    for scen, g in df.groupby("scenario"):
        vals = valid_series(g[metric], metric)
        if vals.empty:
            continue
        out.append({"scenario": int(scen), "mean": vals.mean(), "err": err(g[metric])})
    return pd.DataFrame(out)


def grouped_line_plot(df: pd.DataFrame, metric: str, ylabel: str, title: str, filename: str, out_dir: str, exclude_baseline: bool = False, use_ci: bool = True) -> None:
    modes = [m for m in MODE_ORDER if not (exclude_baseline and m == "Baseline")]
    fig, axes = plt.subplots(1, 2, figsize=(12, 5), sharey=True)
    for ax, proto in zip(axes, ["OLSR", "AODV"]):
        sub = df[df["protocol"] == proto]
        for mode in modes:
            ms = sub[sub["mode"] == mode]
            if ms.empty:
                continue
            agg = agg_with_error(ms, metric, use_ci=use_ci)
            if agg.empty:
                continue
            ax.errorbar(
                agg["scenario"], agg["mean"], yerr=agg["err"],
                label=mode, color=MODE_COLORS[mode], marker=MODE_MARKERS[mode],
                capsize=4, linewidth=2, markersize=7,
            )
        ax.set_title(proto, fontsize=13, fontweight="bold")
        ax.set_xlabel("Failure Scenario", fontsize=11)
        scen_ticks = sorted(sub["scenario"].dropna().astype(int).unique())
        ax.set_xticks(scen_ticks)
        ax.set_xticklabels([SCENARIO_LABELS.get(s, str(s)) for s in scen_ticks])
        ax.grid(True, alpha=0.3)
    axes[0].set_ylabel(ylabel, fontsize=11)
    handles, labels = axes[0].get_legend_handles_labels()
    if handles:
        axes[0].legend(fontsize=9)
    fig.suptitle(title, fontsize=14, fontweight="bold", y=1.02)
    fig.tight_layout()
    path = os.path.join(out_dir, filename)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved {path}")


def plot_raw_reconv_distribution(df: pd.DataFrame, out_dir: str) -> None:
    if "mean_reconv_s" not in df.columns:
        return
    graf_df = df[(df["mode"].str.startswith("GRAF")) & (df["mean_reconv_s"] >= 0)].copy()
    if graf_df.empty:
        return
    for proto in ["OLSR", "AODV"]:
        sub = graf_df[graf_df["protocol"] == proto]
        if sub.empty:
            continue
        fig, ax = plt.subplots(figsize=(9, 5))
        data, labels, positions = [], [], []
        pos = 1
        for scen in sorted(sub["scenario"].unique()):
            for mode in ["GRAF-Global", "GRAF-Local"]:
                vals = sub[(sub["scenario"] == scen) & (sub["mode"] == mode)]["mean_reconv_s"]
                if not vals.empty:
                    data.append(vals.values)
                    labels.append(f"Sc{int(scen)}\n{mode.replace('GRAF-', '')}")
                    positions.append(pos)
                pos += 1
            pos += 1
        if not data:
            plt.close(fig)
            continue
        ax.boxplot(data, positions=positions, widths=0.6, patch_artist=True,
                   boxprops=dict(facecolor="#e9ecef", color="#495057"),
                   medianprops=dict(color="#d63384", linewidth=2), showfliers=False)
        np.random.seed(42)
        for i, vals in enumerate(data):
            x = np.random.normal(positions[i], 0.05, size=len(vals))
            ax.scatter(x, vals, alpha=0.6, s=20, color="#0d6efd", zorder=3)
        ax.set_xticks(positions)
        ax.set_xticklabels(labels)
        ax.set_ylabel("Service Restoration Latency (s)")
        ax.set_title(f"Raw Per-Run SRL Distribution ({proto})", fontsize=13, fontweight="bold")
        ax.grid(True, alpha=0.3, axis="y")
        fig.tight_layout()
        path = os.path.join(out_dir, f"plot_raw_reconv_distribution_{proto}.png")
        fig.savefig(path, dpi=150, bbox_inches="tight")
        plt.close(fig)
        print(f"  Saved {path}")


def generate_plots(df: pd.DataFrame, out_dir: str, use_ci: bool = True) -> None:
    print("\n===== GENERATING PLOTS =====")
    plot_raw_reconv_distribution(df, out_dir)
    common_plots = [
        ("pdr_percent", "PDR (%)", "PDR vs Scenario", "plot_pdr.png"),
        ("throughput_kbps_active_window", "Throughput (kbps)", "Throughput vs Scenario", "plot_throughput.png"),
        ("jain_fairness_index", "Jain's Fairness Index\n(per-port Rx bytes; 1=perfectly fair)", "Jain's Fairness Index vs Scenario", "plot_jain_fairness.png"),
    ]
    for metric, ylabel, title, fname in common_plots:
        if metric in df.columns:
            grouped_line_plot(df, metric, ylabel, title, fname, out_dir, exclude_baseline=False, use_ci=use_ci)
    recovery_plots = [
        ("mean_reconv_s", "Service Restoration Latency (s)", "Service Restoration Latency vs Scenario (GRAF only)", "plot_reconv.png"),
        ("cluster_recovery_rate_percent", "End-to-End Cluster Recovery (%)", "End-to-End Cluster Recovery vs Scenario (GRAF only)", "plot_recovery_rate.png"),
        ("total_recovery_bytes", "App Bytes Recovered Post-Failure", "App Bytes Recovered Post-Failure vs Scenario (GRAF only)", "plot_recovery_bytes.png"),
    ]
    for metric, ylabel, title, fname in recovery_plots:
        if metric in df.columns:
            grouped_line_plot(df, metric, ylabel, title, fname, out_dir, exclude_baseline=True, use_ci=use_ci)


def print_metric_story_validation(df: pd.DataFrame) -> None:
    print("\n===== METRIC STORY VALIDATION =====")
    if df.empty:
        return
    overall = df.groupby("mode")[["pdr_percent", "throughput_kbps_active_window", "jain_fairness_index"]].mean(numeric_only=True)
    if "Baseline" in overall.index and "GRAF-Global" in overall.index:
        if overall.loc["GRAF-Global", "pdr_percent"] > overall.loc["Baseline", "pdr_percent"] + 5:
            print("[PASS] GRAF-Global substantially improves PDR over baseline.")
        else:
            print("[WARN] GRAF-Global PDR improvement is small.")
    if "Baseline" in overall.index and "GRAF-Global" in overall.index:
        if overall.loc["GRAF-Global", "jain_fairness_index"] >= overall.loc["Baseline", "jain_fairness_index"] - 0.05:
            print("[PASS] GRAF-Global maintains or improves fairness.")
        else:
            print("[WARN] GRAF-Global reduces fairness.")
    print("Review SRL only inside GRAF-vs-GRAF comparisons; baseline SRL is N/A by design.")


def main() -> None:
    ap = argparse.ArgumentParser(description="Analyze hybrid star-mesh simulation results")
    ap.add_argument("--dir", default="sim_results/raw", help="Directory containing *_summary.csv files")
    ap.add_argument("--out", default="sim_results/analysis_review_ready", help="Output directory for tables and plots")
    ap.add_argument("--sd-bars", action="store_true", help="Use SD instead of 95% CI in plot error bars")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    df = load_all(args.dir)
    print(f"Loaded {len(df)} runs from {args.dir}")
    print(f"Protocols: {sorted(df['protocol'].dropna().unique())}")
    print(f"Scenarios: {sorted(df['scenario'].dropna().astype(int).unique())}")
    print(f"Modes:     {sorted(df['mode'].dropna().unique())}")
    if 'run' in df.columns:
        print(f"Runs:      {sorted(df['run'].dropna().astype(int).unique())[:5]} ...")
    audit_runs(df)
    main_tables(df, args.out)
    improvement_table(df, args.out)
    run_significance_tests(df, args.out)
    generate_plots(df, args.out, use_ci=not args.sd_bars)
    print_metric_story_validation(df)
    print(f"\nAll outputs saved to {args.out}/")


if __name__ == "__main__":
    main()