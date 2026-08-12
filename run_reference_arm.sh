#!/usr/bin/env bash
# run_reference_arm.sh -- 40-run GRAF-Global reference arm for the extended sweeps.
#
# WHY THIS EXISTS
# ---------------
# run_extended_sweep.sh emits only the arms under test: sweep 1 emits the three
# competitive baselines, sweep 2 emits the three ablation arms. Neither emits the
# GRAF-Global / --ablation=full arm they are measured against, so that reference
# has to come from Run Set A.
#
# But the two use different seed streams. Run Set A seeds runs at 1000 + 17*run;
# the extended sweeps use run * 1e6. Run 1 of Set B is therefore a completely
# different topology and failure order from run 1 of Set A, and no seed matches
# across the two. Table X and Table X-B can only be unpaired comparisons.
#
# Unpaired at n=20 is perfectly publishable -- analyze_results.py falls back to
# Welch with Cohen's d and labels the test type. This script is the cheaper,
# stronger option: 40 runs of the reference arm on the *extended* seed stream,
# which makes both tables paired and removes topology variance from the
# comparison.
#
# One cell serves both tables: Scenario 3, GRAF-Global, --baseline=none
# --ablation=full, seeds i*1e6 for i in 1..20, both protocols. Table X uses both
# protocols; Table X-B uses the OLSR half.
#
# Run this ON THE SERVER, from the ns-3 root, AFTER Sets B and C have finished:
#     cd ~/ns-allinone-3.39/ns-3.39 && bash ~/graf-src/run_reference_arm.sh
#
# Then include its output directory in the analysis:
#     python3 analyze_results.py \
#         --dir sim_results_v3/raw sim_results_v3_setB/raw \
#               sim_results_v3_setC/raw sim_results_v3_setD/raw \
#               sim_results_v3_ref/raw \
#         --out sim_results_v3/analysis_v3
#
# Env overrides:
#     OUTDIR     results dir     (default: sim_results_v3_ref)
#     RUNS       replications    (default: 20)
#     PROTOCOLS  protocols       (default: "OLSR AODV")
#     JOBS       parallel jobs   (default: nproc-1)
#     DRY_RUN    set to any value to print the job list and stop

set -uo pipefail

TARGET="scratch/hybrid-star-mesh-sim"
OUTDIR="${OUTDIR:-sim_results_v3_ref}"
RUNS="${RUNS:-20}"
PROTOCOLS="${PROTOCOLS:-OLSR AODV}"
PROTOCOLS="${PROTOCOLS//,/ }"
NPROC=$(nproc 2>/dev/null || echo 2)
JOBS="${JOBS:-$(( NPROC > 1 ? NPROC - 1 : 1 ))}"

[ -f "./ns3" ] || { echo "FATAL: run me from the ns-3 root (no ./ns3 here)" >&2; exit 1; }

mkdir -p "$OUTDIR/raw" "$OUTDIR/logs"
manifest="$OUTDIR/run_manifest_ref.csv"
[ -f "$manifest" ] || echo "protocol,scenario,mode,run,seed,status,logfile,prefix" > "$manifest"

JOBS_FILE=$(mktemp)
trap 'rm -f "$JOBS_FILE"' EXIT

skipped=0; queued=0
for protocol in $PROTOCOLS; do
  for run in $(seq 1 "$RUNS"); do
    # Must match SEEDS_STD in run_extended_sweep.sh -- this is the whole point.
    seed=$(( run * 1000000 ))
    prefix="$OUTDIR/raw/REF_${protocol}_sc3_global_run${run}"
    logfile="$OUTDIR/logs/REF_${protocol}_sc3_global_run${run}.log"

    # Resumable: skip any cell that already produced a summary.
    if compgen -G "${prefix}*_summary.csv" > /dev/null; then
      skipped=$(( skipped + 1 )); continue
    fi

    printf '%s\n' "./ns3 run '$TARGET --protocol=$protocol --scenario=3 --graf=global --baseline=none --ablation=full --chs=8 --sensors=80 --seed=$seed --run=$run --output=$prefix' > $logfile 2>&1 && echo '$protocol,3,global,$run,$seed,OK,$logfile,$prefix' >> $manifest || { echo '$protocol,3,global,$run,$seed,FAIL,$logfile,$prefix' >> $manifest; echo 'FAILED: $protocol sc3 global run$run'; }" >> "$JOBS_FILE"
    queued=$(( queued + 1 ))
  done
done

echo "============================================="
echo "Reference arm: GRAF-Global, Sc3, --ablation=full, seeds run*1e6"
echo "  protocols: $PROTOCOLS"
echo "  queued:    $queued"
echo "  skipped:   $skipped (already have a summary)"
echo "============================================="

if [ "$queued" -eq 0 ]; then
  echo "Nothing to run."
  exit 0
fi

if [ -n "${DRY_RUN:-}" ]; then
  echo "DRY_RUN set -- job list follows, nothing built or executed."
  cat "$JOBS_FILE"
  exit 0
fi

echo "==> Explicit build first (prevents parallel build collisions)"
if ! ./ns3 build "scratch_$(basename "$TARGET")" > /tmp/graf_build_ref.log 2>&1; then
  ./ns3 build > /tmp/graf_build_ref.log 2>&1 || {
    tail -30 /tmp/graf_build_ref.log; echo "FATAL: build failed" >&2; exit 1; }
fi
tail -3 /tmp/graf_build_ref.log

echo "==> Running $queued jobs, $JOBS at a time"
xargs -I CMD -P "$JOBS" bash -c CMD < "$JOBS_FILE"

echo
echo "Done. Summaries: $(ls "$OUTDIR"/raw/*_summary.csv 2>/dev/null | wc -l)"
echo "Failures:        $(grep -c ',FAIL,' "$manifest" 2>/dev/null || echo 0)"
