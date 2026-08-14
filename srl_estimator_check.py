#!/usr/bin/env python3
"""srl_estimator_check.py -- does the SRL result survive the better estimator?

WHY THIS EXISTS
---------------
Every run exports Service Restoration Latency twice:

  mean_reconv_s              snapshot estimator -- network state sampled on a
                             fixed grid, so a restoration is only observed at
                             the next sample point
  mean_reconv_eventdriven_s  event-driven measurement (FIX-A4) -- the actual
                             instant each sensor's route is restored

analyze_results.py reports the snapshot value as the primary SRL and carries
the event-driven one as a secondary row. Across the full result set the two
disagree by a median of 0.24 s and a mean of 0.34 s, with 24% of runs beyond
0.5 s and a worst case of 2.06 s -- against a reported SRL of roughly 2.5 s.
That is too large to leave undisclosed, and the disagreement is one-sided in
the tail (snapshot > event-driven in every one of the five worst runs), which
is the signature of grid quantisation rather than noise.

The event-driven measurement is the more precise of the two, and it is the one
the reviewer asked for. Before the paper switches to it, two things have to be
established, and this script establishes both:

  1. COVERAGE. An event-driven mean taken over a subset of sensors is not
     comparable to a snapshot mean taken over all of them. If
     n_sensors_eventdriven is materially below the snapshot's coverage, the
     event-driven mean is biased by *which* sensors it captured and cannot
     simply be swapped in.

  2. SURVIVAL. The headline claim is GRAF's SRL advantage over the baseline.
     This recomputes that comparison under both estimators, side by side, so
     the effect size and significance can be read off directly. If the
     advantage holds under the event-driven measurement, switching is pure
     upside; if it narrows, the paper needs to say so.

This deliberately writes a separate report instead of changing the analyzer's
primary metric: the tables, plots and significance tests are already generated
and correct for what they claim to measure, and re-plumbing them under deadline
risks more than it gains. Decide from this report first.

Usage (from the ns-3 root, same dirs as analyze_results.py):
    python3 srl_estimator_check.py \
        --dir sim_results_v3/raw sim_results_v3_setB/raw sim_results_v3_setC/raw \
              sim_results_v3_setD/raw sim_results_v3_setE/raw sim_results_v3_ref/raw \
        --out srl_estimator_report
"""

import argparse
import os
import sys

import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_results as ar  # noqa: E402

SNAP = "mean_reconv_s"
EVENT = "mean_reconv_eventdriven_s"


def valid(df: pd.DataFrame) -> pd.DataFrame:
    """Runs where both estimators produced a real measurement.

    Both metrics use a negative sentinel for "no reconvergence observed", so a
    comparison has to drop those rather than average them in as zeros.
    """
    need = {SNAP, EVENT}
    if not need.issubset(df.columns):
        sys.exit(f"ERROR: summaries are missing {sorted(need - set(df.columns))}")
    return df[(df[SNAP] >= 0) & (df[EVENT] >= 0)].copy()


def coverage_report(df: pd.DataFrame) -> pd.DataFrame:
    """How many sensors the event-driven measurement actually saw."""
    if "n_sensors_eventdriven" not in df.columns:
        print("NOTE: n_sensors_eventdriven not exported -- coverage unverifiable.")
        return pd.DataFrame()
    g = df.groupby(["scenario", "protocol"], dropna=False)["n_sensors_eventdriven"]
    out = g.agg(["count", "mean", "min", "max"]).reset_index()
    out.columns = ["scenario", "protocol", "runs", "mean_sensors", "min_sensors", "max_sensors"]
    return out


def agreement_report(df: pd.DataFrame) -> pd.DataFrame:
    """Signed snapshot - event-driven, per cell.

    Signed, not absolute: a one-sided gap means the snapshot estimator is
    biased (grid quantisation can only ever delay an observed restoration, so
    the bias should be positive), while a gap that straddles zero is noise.
    """
    d = df.copy()
    d["delta"] = d[SNAP] - d[EVENT]
    g = d.groupby(["scenario", "protocol", "arm"], dropna=False)["delta"]
    out = g.agg(["count", "mean", "median", "std", "min", "max"]).reset_index()
    return out.sort_values("mean", ascending=False)


REF_ARM = "GRAF-Global"


def survival_report(df: pd.DataFrame) -> pd.DataFrame:
    """Every SRL comparison in the paper, recomputed under each estimator.

    The reference is GRAF-Global, not Baseline: a --graf=off --baseline=none run
    has no recovery logic at all, so its SRL is the undefined sentinel and
    analyze_results.compare_arms already skips it. The comparisons that actually
    carry SRL claims are GRAF-Global against GRAF-Local and against each
    ablation arm.

    Reuses analyze_results.compare_metric so test selection (paired where seeds
    match, Welch otherwise) and effect size match significance_tests.csv exactly
    -- only the metric changes. x and y are dropped: they are the raw sample
    arrays, useful inside the analyzer but not as CSV cells.
    """
    rows = []
    for (scen, proto, chs), g in df.groupby(["scenario", "protocol", "num_chs"], dropna=False):
        ref = g[g["arm"] == REF_ARM]
        if ref.empty:
            continue
        for arm in sorted(set(g["arm"]) - {REF_ARM}):
            sub = g[g["arm"] == arm]
            if sub.empty:
                continue
            for label, key in (("snapshot", SNAP), ("event-driven", EVENT)):
                res = ar.compare_metric(sub, ref, key)
                if not res:
                    continue
                rows.append({
                    "scenario": scen, "protocol": proto, "num_chs": chs,
                    "arm": arm, "estimator": label,
                    "arm_mean": sub[key].mean(),
                    "ref_mean": ref[key].mean(),
                    "delta": sub[key].mean() - ref[key].mean(),
                    "test": res["test"], "n": res["n"],
                    "t": res["t"], "p": res["p"],
                    "effect_kind": res["effect_kind"], "effect": res["effect"],
                })
    return pd.DataFrame(rows)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dir", nargs="+", required=True, help="raw result directories")
    p.add_argument("--out", default="srl_estimator_report", help="output directory")
    a = p.parse_args()

    os.makedirs(a.out, exist_ok=True)
    df = valid(ar.load_all(a.dir))
    print(f"Runs with both estimators present: {len(df)}")

    delta = df[SNAP] - df[EVENT]
    print("\n=== AGREEMENT (signed: snapshot - event-driven) ===")
    print(f"  mean   {delta.mean():+.3f} s")
    print(f"  median {delta.median():+.3f} s")
    print(f"  sd     {delta.std():.3f} s")
    print(f"  share where snapshot is the larger of the two: "
          f"{(delta > 0).mean():.1%}")
    print("  A one-sided positive gap means the snapshot estimator systematically")
    print("  overstates SRL, which is what grid quantisation would produce.")

    cov = coverage_report(df)
    if not cov.empty:
        print("\n=== COVERAGE (sensors seen by the event-driven measurement) ===")
        print(cov.to_string(index=False))
        cov.to_csv(os.path.join(a.out, "srl_coverage.csv"), index=False)

    agr = agreement_report(df)
    agr.to_csv(os.path.join(a.out, "srl_agreement_by_cell.csv"), index=False)
    print("\n=== WORST 10 CELLS BY MEAN SIGNED GAP ===")
    print(agr.head(10).to_string(index=False))

    print("\n=== REPORTED SRL, BOTH ESTIMATORS (GRAF arms, Nc=8) ===")
    main = df[(df["num_chs"] == 8) & (df["arm"].isin(["GRAF-Global", "GRAF-Local"]))]
    if not main.empty:
        summ = main.groupby(["scenario", "arm"])[[SNAP, EVENT]].mean().reset_index()
        summ.columns = ["scenario", "arm", "snapshot_s", "eventdriven_s"]
        summ["shift_s"] = summ["eventdriven_s"] - summ["snapshot_s"]
        print(summ.to_string(index=False))
        summ.to_csv(os.path.join(a.out, "srl_reported_both.csv"), index=False)
        print("  shift_s is how much each reported SRL figure moves if the paper")
        print("  switches to the event-driven measurement.")

    surv = survival_report(df)
    if surv.empty:
        print("\nNo GRAF-Global reference found -- survival check skipped.")
    else:
        surv.to_csv(os.path.join(a.out, "srl_survival.csv"), index=False)
        print("\n=== DO THE SRL COMPARISONS SURVIVE THE SWITCH? ===")
        print("  Each arm vs GRAF-Global, under both estimators. A sign flip or a")
        print("  p crossing 0.05 between the two rows of a pair is what matters.")
        cols = ["scenario", "protocol", "arm", "estimator",
                "arm_mean", "ref_mean", "delta", "test", "n", "p", "effect"]
        print(surv.sort_values(["scenario", "protocol", "arm", "estimator"])[cols]
              .to_string(index=False))

    print(f"\nWritten to {a.out}/")


if __name__ == "__main__":
    main()
