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
    "energy_per_bit_j": "Energy/Bit (J/bit)",
}
GRAF_ONLY_METRICS = {
    "mean_reconv_s": "Service Restoration Latency (s)",
    "cluster_recovery_rate_percent": "End-to-End Cluster Recovery (%)",
    "sensor_recovery_rate_percent": "Sensor Recovery (%)",
    "total_recovery_bytes": "App Bytes Recovered Post-Failure",
}
# num_chs belongs here even though the main sweep is single-size: the scalability
# sweep reuses one seed across its 16- and 32-CH cells, so a merge that omitted it
# would match each run twice and silently double the reported n.
PAIR_KEYS = ["scenario", "protocol", "num_chs", "seed", "run"]


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


def derive_arm(row: pd.Series) -> str:
    """Experimental condition of a run.

    `mode` alone is not enough once the extended sweeps are in play: every
    competitive-baseline run is --graf=off (mode "Baseline") and every ablation
    run is --graf=global (mode "GRAF-Global"), so all three baseline strategies
    -- and all three ablation arms -- would otherwise collapse into a single
    group and be averaged together.
    """
    baseline = str(row.get("baseline", "none") or "none").strip().lower()
    ablation = str(row.get("ablation", "full") or "full").strip().lower()
    if baseline != "none":
        return f"BL-{baseline}"
    if ablation != "full":
        return f"ABL-{ablation}"
    return str(row["mode"])


def load_all(results_dirs: Sequence[str]) -> pd.DataFrame:
    if isinstance(results_dirs, str):
        results_dirs = [results_dirs]
    paths: List[str] = []
    for d in results_dirs:
        found = sorted(glob.glob(os.path.join(d, "*_summary.csv")))
        if not found:
            print(f"WARNING: no *_summary.csv files found in {d}")
        paths.extend(found)
    rows = [parse_summary(path) for path in paths]
    rows = [r for r in rows if r]
    if not rows:
        print(f"ERROR: no *_summary.csv files found in {list(results_dirs)}")
        sys.exit(1)

    df = pd.DataFrame(rows)
    if "graf" not in df.columns:
        raise RuntimeError("Missing 'graf' column in summaries.")

    # The simulation renamed this export when it moved to the IEEE
    # application-bit convention. Accept the old name so pre-rename result sets
    # still load, but standardise on the current one everywhere downstream.
    if "energy_per_bit_j" not in df.columns and "energy_per_delivered_bit_j" in df.columns:
        df["energy_per_bit_j"] = df["energy_per_delivered_bit_j"]

    df["mode"] = df["graf"].apply(lambda x: "Baseline" if x == "off" else f"GRAF-{str(x).capitalize()}")

    # Older summaries predate these exports; default them so a mixed set loads.
    if "baseline" not in df.columns:
        df["baseline"] = "none"
    if "ablation" not in df.columns:
        df["ablation"] = "full"
    df["baseline"] = df["baseline"].fillna("none").astype(str).str.strip().str.lower()
    df["ablation"] = df["ablation"].fillna("full").astype(str).str.strip().str.lower()
    if "num_chs" not in df.columns:
        df["num_chs"] = 8
    df["arm"] = df.apply(derive_arm, axis=1)

    for col in ["scenario", "seed", "run", "num_chs"]:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce").astype("Int64")

    numeric_cols = [
        "pdr_percent", "throughput_kbps_active_window", "avg_delay_ms",
        "total_rx_packets", "total_tx_packets", "total_lost_packets",
        "mean_reconv_s", "median_reconv_s", "max_reconv_s",
        "cluster_recovery_rate_percent", "sensor_recovery_rate_percent",
        "total_recovery_bytes", "jain_fairness_index",
        "total_consumed_ch_energy_j", "total_residual_ch_energy_j",
        "chs_depleted", "energy_per_bit_j", "energy_per_bit_ip_legacy_j",
        "mean_reconv_eventdriven_s", "n_sensors_eventdriven",
        "total_initial_ch_energy_j",
        "hb_detected_count", "hb_mean_detection_latency_s",
        "routing_overhead_bytes", "routing_overhead_packets",
        "normalized_overhead_ctrl_per_data",
    ]
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    return df


def base_cluster_count(df: pd.DataFrame) -> int:
    """The Nc the main sweep was run at, as opposed to the scalability sizes."""
    if "num_chs" not in df.columns or df["num_chs"].dropna().empty:
        return 8
    return int(df["num_chs"].dropna().mode().iloc[0])


def canonical_subset(df: pd.DataFrame) -> pd.DataFrame:
    """Run Set A rows only: no competitive baseline, no ablation, base network size.

    The main tables, improvement table, mode-vs-mode significance tests and plots
    all describe the main sweep. Without this filter, loading several result
    directories at once would silently fold competitive-baseline and ablation
    runs into those outputs.
    """
    mask = (df["baseline"] == "none") & (df["ablation"] == "full")
    if "num_chs" in df.columns:
        mask &= df["num_chs"] == base_cluster_count(df)
    return df[mask].copy()


def valid_series(series: pd.Series, col_name: str) -> pd.Series:
    if col_name in NA_METRICS:
        return series[series >= 0].dropna()
    return series.dropna()


def format_value(val: float, col_name: str) -> str:
    if pd.isna(val):
        return "N/A"
    if col_name in {"energy_per_bit_j", "energy_per_bit_ip_legacy_j",
                    "energy_per_delivered_bit_j"}:
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
        ("energy_per_bit_j", "Energy/Bit (J/bit)"),
        ("mean_reconv_eventdriven_s", "SRL, event-driven (s)"),
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
    # `arm` and `num_chs` are part of the identity of a run, not decoration:
    # without them the three competitive baselines (all mode "Baseline") and the
    # three scalability sizes collide on the same key and every one of them is
    # reported as a duplicate.
    required = ["scenario", "protocol", "arm", "num_chs", "run"]
    missing_cols = [c for c in required if c not in df.columns]
    if missing_cols:
        print(f"[WARN] Missing columns for full audit: {missing_cols}")
    dup = df[df.duplicated(subset=[c for c in required if c in df.columns], keep=False)]
    if not dup.empty:
        print("[WARN] Duplicate scenario/protocol/arm/Nc/run combinations found:")
        print(dup[[c for c in required if c in dup.columns]].to_string(index=False))
    else:
        print("[PASS] No duplicated scenario/protocol/arm/Nc/run combinations.")
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


BASELINE_ARMS = ["BL-rand", "BL-energy", "BL-nearest"]
ABLATION_ARMS = ["ABL-energy", "ABL-topo", "ABL-proxcov"]
ARM_TABLE_COLS = [
    ("pdr_percent", "PDR (%)"),
    ("throughput_kbps_active_window", "Throughput (kbps)"),
    ("avg_delay_ms", "Avg Delay (ms)"),
    ("mean_reconv_s", "Service Restoration Latency (s)"),
    ("cluster_recovery_rate_percent", "End-to-End Cluster Recovery (%)"),
    ("sensor_recovery_rate_percent", "Sensor Recovery (%)"),
    ("jain_fairness_index", "Jain Fairness Index"),
    ("total_consumed_ch_energy_j", "CH Energy Consumed (J)"),
    ("energy_per_bit_j", "Energy/Bit (J/bit)"),
]


def compare_metric(left: pd.DataFrame, right: pd.DataFrame, m_key: str) -> Optional[Dict[str, object]]:
    """Paired test on seed-matched runs where possible, Welch otherwise.

    Run Set A seeds runs at 1000 + 17*run while the extended sweeps use
    run * 1e6, so an arm from one set can never pair with an arm from the other.
    The test actually used is always reported, so an unpaired comparison is
    never silently presented as a paired one. Returns None when neither test
    has enough data rather than emitting an empty row.
    """
    if m_key not in left.columns or m_key not in right.columns:
        return None
    x, y = aligned_pair_values(left, right, m_key)
    if len(x) >= 2 and len(y) >= 2:
        t_stat, p_val = st.ttest_rel(x, y)
        return {"x": x, "y": y, "n": len(x), "test": "paired t",
                "effect_kind": "dz", "t": float(t_stat), "p": float(p_val),
                "effect": paired_effect_size(x, y)}
    xi = valid_series(left[m_key], m_key).to_numpy()
    yi = valid_series(right[m_key], m_key).to_numpy()
    if len(xi) < 2 or len(yi) < 2:
        return None
    t_stat, p_val = st.ttest_ind(xi, yi, equal_var=False)
    return {"x": xi, "y": yi, "n": min(len(xi), len(yi)), "test": "Welch t (unpaired)",
            "effect_kind": "Cohen d", "t": float(t_stat), "p": float(p_val),
            "effect": independent_effect_size(xi, yi)}


def arm_table(df: pd.DataFrame, out_csv: str, title: str) -> pd.DataFrame:
    """Per-arm summary, keyed by scenario/protocol/Nc/arm rather than by mode."""
    rows: List[Dict[str, object]] = []
    for (scen, proto, nchs, arm), g in df.groupby(
            ["scenario", "protocol", "num_chs", "arm"], dropna=False):
        row: Dict[str, object] = {
            "Scenario": int(scen), "Protocol": str(proto),
            "Nc": int(nchs), "Arm": str(arm),
        }
        for src, label in ARM_TABLE_COLS:
            row[label] = fmt(g[src], src) if src in g.columns else "—"
        rows.append(row)
    tbl = pd.DataFrame(rows).sort_values(["Scenario", "Protocol", "Nc", "Arm"]).reset_index(drop=True)
    tbl.to_csv(out_csv, index=False)
    print(f"\n===== {title} =====")
    print(tbl.to_string(index=False))
    print(f"Saved to {out_csv}")
    return tbl


def compare_arms(df: pd.DataFrame, test_arms: Sequence[str], ref_arm: str,
                 out_csv: str, title: str) -> pd.DataFrame:
    """Significance tests of each test arm against a common reference arm."""
    metrics = dict(PRIMARY_METRICS)
    metrics.update(GRAF_ONLY_METRICS)
    rows: List[Dict[str, object]] = []
    for (scen, proto), g in df.groupby(["scenario", "protocol"]):
        ref = g[g["arm"] == ref_arm]
        if ref.empty:
            continue
        for arm in test_arms:
            sub = g[g["arm"] == arm]
            if sub.empty:
                continue
            for m_key, m_label in metrics.items():
                # SRL and recovery are undefined only for the arm that runs no
                # recovery logic at all -- --graf=off with --baseline=none, which
                # derive_arm names "Baseline".
                #
                # The competitive baselines are NOT that arm. Star_mesh sets
                # g_recoveryEnabled = (grafMode != "off" || baselineMode !=
                # "none"), so every BL-* run rehomes its orphans and exports a
                # real SRL -- table_x_baselines.csv shows 2.52-2.68 s for them.
                # Skipping those comparisons suppressed the strongest result
                # Table X has: GRAF-Global restores service faster than all
                # three competitive heuristics even where PDR is level.
                if m_key in GRAF_ONLY_METRICS and str(arm) == "Baseline":
                    continue
                res = compare_metric(sub, ref, m_key)
                if res is None:
                    continue
                rows.append({
                    "Scenario": int(scen), "Protocol": str(proto),
                    "Comparison": f"{arm} vs {ref_arm}", "Metric": m_label,
                    "Mean Left": float(np.mean(res["x"])),
                    "Mean Right": float(np.mean(res["y"])),
                    "Mean Diff (Left-Right)": float(np.mean(res["x"]) - np.mean(res["y"])),
                    "n": int(res["n"]), "Test": res["test"],
                    "t-statistic": res["t"], "p-value": res["p"],
                    "Effect size": res["effect"], "Effect type": res["effect_kind"],
                })
    out = pd.DataFrame(rows)
    if out.empty:
        print(f"\n===== {title} =====\nNo comparable arms present; skipped.")
        return out
    out["Holm-adjusted p"] = holm_correction(out["p-value"].tolist())
    out["Significant (Holm<0.05)"] = np.where(out["Holm-adjusted p"] < 0.05, "Yes", "No")
    out = out.sort_values(["Scenario", "Protocol", "Comparison", "Metric"]).reset_index(drop=True)
    out.to_csv(out_csv, index=False)
    print(f"\n===== {title} =====")
    print(out[["Scenario", "Protocol", "Comparison", "Metric",
               "Mean Diff (Left-Right)", "n", "Test", "Holm-adjusted p",
               "Effect size", "Significant (Holm<0.05)"]].to_string(index=False))
    if (out["Test"] == "Welch t (unpaired)").any():
        print("\n[NOTE] Some comparisons are unpaired. The extended sweeps seed runs at")
        print("       run*1e6 while the main sweep uses 1000+17*run, so no seed matches")
        print("       across sets. Add a same-seed reference arm to enable paired tests.")
    print(f"Saved to {out_csv}")
    return out


def extended_analyses(df: pd.DataFrame, out_dir: str) -> None:
    """Table X (baselines), Table X-B (ablation) and the scalability table.

    Each is emitted only when its arms are actually present, so a Run-Set-A-only
    invocation produces exactly the outputs it always did.
    """
    present = set(df["arm"].unique())
    base_nc = base_cluster_count(df)

    bl_arms = [a for a in BASELINE_ARMS if a in present]
    if bl_arms:
        sub = df[df["arm"].isin(bl_arms + ["GRAF-Global", "GRAF-Local"])]
        arm_table(sub, os.path.join(out_dir, "table_x_baselines.csv"),
                  "TABLE X — COMPETITIVE BASELINES")
        compare_arms(sub, bl_arms, "GRAF-Global",
                     os.path.join(out_dir, "table_x_baselines_tests.csv"),
                     "TABLE X — BASELINE SIGNIFICANCE TESTS")

    abl_arms = [a for a in ABLATION_ARMS if a in present]
    if abl_arms:
        sub = df[df["arm"].isin(abl_arms + ["GRAF-Global"])]
        arm_table(sub, os.path.join(out_dir, "table_xb_ablation.csv"),
                  "TABLE X-B — FITNESS ABLATION")
        compare_arms(sub, abl_arms, "GRAF-Global",
                     os.path.join(out_dir, "table_xb_ablation_tests.csv"),
                     "TABLE X-B — ABLATION SIGNIFICANCE TESTS")

    if "num_chs" in df.columns and df["num_chs"].nunique(dropna=True) > 1:
        sub = df[(df["baseline"] == "none") & (df["ablation"] == "full")]
        arm_table(sub, os.path.join(out_dir, "table_scalability.csv"),
                  f"SCALABILITY (base Nc={base_nc})")


def phase2_checks(df: pd.DataFrame) -> None:
    """The four post-re-run checks that decide how the paper is written."""
    print("\n===== PHASE 2 CHECKS =====")
    canon = canonical_subset(df)

    # 1. CH energy must no longer be identical between arms.
    if "total_consumed_ch_energy_j" in canon.columns:
        worst: Optional[float] = None
        for (_, _), g in canon.groupby(["scenario", "protocol"]):
            means = g.groupby("arm")["total_consumed_ch_energy_j"].mean().dropna()
            if len(means) >= 2:
                spread = float(means.max() - means.min())
                worst = spread if worst is None else max(worst, spread)
        if worst is None:
            print("[SKIP] Energy delta: need at least two arms in a cell.")
        elif worst > 1e-9:
            print(f"[PASS] CH energy differs between arms (max spread {worst:.6f} J).")
        else:
            print("[FAIL] CH energy is still identical across arms — the fix is not in effect.")

    # 2. FIX-A1 randomisation should give the baseline real topology variance.
    sc1 = canon[(canon["scenario"] == 1) & (canon["protocol"] == "OLSR") &
                (canon["arm"] == "Baseline")]
    if len(sc1) >= 2 and "pdr_percent" in sc1.columns:
        sd = float(sc1["pdr_percent"].std(ddof=1))
        verdict = "PASS" if sd > 0.30 else "WARN"
        print(f"[{verdict}] OLSR Sc1 baseline PDR SD = {sd:.3f} pp (want > 0.30).")

    # 3. Event-driven SRL should agree with the snapshot measure.
    if {"mean_reconv_s", "mean_reconv_eventdriven_s"}.issubset(canon.columns):
        both = canon[(canon["mean_reconv_s"] >= 0) & (canon["mean_reconv_eventdriven_s"] >= 0)]
        if not both.empty:
            gap = float((both["mean_reconv_s"] - both["mean_reconv_eventdriven_s"]).abs().max())
            verdict = "PASS" if gap <= 0.5 else "WARN"
            print(f"[{verdict}] Max |snapshot - event-driven| SRL = {gap:.3f} s (want <= 0.5).")
        else:
            print("[SKIP] Event-driven SRL: no runs with both measures defined.")

    # 4. Failures are schedule-injected, so nothing should die of flat battery.
    if "chs_depleted" in df.columns:
        n_bad = int((df["chs_depleted"].fillna(0) > 0).sum())
        if n_bad == 0:
            print(f"[PASS] chs_depleted == 0 in all {len(df)} runs.")
        else:
            print(f"[FAIL] {n_bad} run(s) report chs_depleted > 0 — jitter may be too tight.")


def main() -> None:
    ap = argparse.ArgumentParser(description="Analyze hybrid star-mesh simulation results")
    ap.add_argument("--dir", nargs="+", default=["sim_results/raw"],
                    help="One or more directories containing *_summary.csv files. "
                         "Pass the extended-sweep dirs alongside the main sweep to "
                         "produce the baseline, ablation and scalability tables.")
    ap.add_argument("--out", default="sim_results/analysis_review_ready", help="Output directory for tables and plots")
    # "%%" is required: argparse runs help strings through %-formatting, and a
    # bare "95% CI" raises ValueError on --help.
    ap.add_argument("--sd-bars", action="store_true", help="Use SD instead of 95%% CI in plot error bars")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    df = load_all(args.dir)
    print(f"Loaded {len(df)} runs from {args.dir}")
    print(f"Protocols: {sorted(df['protocol'].dropna().unique())}")
    print(f"Scenarios: {sorted(df['scenario'].dropna().astype(int).unique())}")
    print(f"Modes:     {sorted(df['mode'].dropna().unique())}")
    print(f"Arms:      {sorted(df['arm'].dropna().unique())}")
    if "num_chs" in df.columns:
        print(f"Nc sizes:  {sorted(df['num_chs'].dropna().astype(int).unique())}")
    if 'run' in df.columns:
        print(f"Runs:      {sorted(df['run'].dropna().astype(int).unique())[:5]} ...")

    audit_runs(df)

    # The main sweep's own outputs must describe the main sweep only. Competitive
    # baseline, ablation and scalability runs get their own tables below.
    canon = canonical_subset(df)
    if len(canon) != len(df):
        print(f"\nMain-sweep outputs use {len(canon)} of {len(df)} runs "
              f"(excluding baseline/ablation/scalability arms).")
    main_tables(canon, args.out)
    improvement_table(canon, args.out)
    run_significance_tests(canon, args.out)
    generate_plots(canon, args.out, use_ci=not args.sd_bars)
    print_metric_story_validation(canon)

    extended_analyses(df, args.out)
    phase2_checks(df)
    print(f"\nAll outputs saved to {args.out}/")


if __name__ == "__main__":
    main()