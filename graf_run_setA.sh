#!/usr/bin/env bash
# graf_run_setA.sh — Run set A: the full 480-run sweep on the fixed code.
#   20 replications x 2 protocols x 4 scenarios x 3 modes (base/global/local)
#
# Run this ON THE SERVER, from the ns-3 root, AFTER graf_deploy_smoke.sh is green:
#     cd ~/ns-allinone-3.39/ns-3.39 && bash ~/graf_run_setA.sh
#
# Env overrides:
#     JOBS     parallel streams (default: nproc-1)
#     OUTDIR   results dir      (default: sim_results_v3)
#     RUNS     replications     (default: 20)
#
# Resumable: a run whose *_summary.csv already exists is skipped, so you can
# Ctrl-C and re-launch without losing completed work.

set -uo pipefail

TARGET="scratch/hybrid-star-mesh-sim"
OUTDIR="${OUTDIR:-sim_results_v3}"
RUNS="${RUNS:-20}"
NPROC=$(nproc 2>/dev/null || echo 2)
JOBS="${JOBS:-$(( NPROC > 1 ? NPROC - 1 : 1 ))}"

[ -f "./ns3" ] || { echo "FATAL: run me from the ns-3 root (no ./ns3 here)" >&2; exit 1; }

mkdir -p "$OUTDIR/raw" "$OUTDIR/logs"
manifest="$OUTDIR/run_manifest.csv"
[ -f "$manifest" ] || echo "protocol,scenario,mode,run,seed,status,logfile,prefix" > "$manifest"

echo "==> Explicit build first (prevents parallel build collisions)"
if ! ./ns3 build "scratch_$(basename "$TARGET")" > /tmp/graf_build.log 2>&1; then
  ./ns3 build > /tmp/graf_build.log 2>&1 || { tail -30 /tmp/graf_build.log; echo "FATAL: build failed" >&2; exit 1; }
fi
tail -3 /tmp/graf_build.log

JOBS_FILE=$(mktemp)
trap 'rm -f "$JOBS_FILE"' EXIT

skipped=0; queued=0
for protocol in OLSR AODV; do
  for scenario in 1 2 3 4; do
    for mode in base global local; do
      case "$mode" in
        base)   graf="off"    ;;
        global) graf="global" ;;
        local)  graf="local"  ;;
      esac
      for run in $(seq 1 "$RUNS"); do
        seed=$(( 1000 + run * 17 ))
        prefix="$OUTDIR/raw/${protocol}_sc${scenario}_${mode}_run${run}"
        logfile="$OUTDIR/logs/${protocol}_sc${scenario}_${mode}_run${run}.log"

        # Resume: skip if this cell already produced a summary.
        if compgen -G "${prefix}*_summary.csv" > /dev/null; then
          skipped=$(( skipped + 1 )); continue
        fi

        printf '%s\n' "./ns3 run $TARGET -- --protocol=$protocol --scenario=$scenario --graf=$graf --seed=$seed --run=$run --output=$prefix > $logfile 2>&1 && echo '$protocol,$scenario,$mode,$run,$seed,OK,$logfile,$prefix' >> $manifest || { echo '$protocol,$scenario,$mode,$run,$seed,FAIL,$logfile,$prefix' >> $manifest; echo 'FAILED: $protocol sc$scenario $mode run$run'; }" >> "$JOBS_FILE"
        queued=$(( queued + 1 ))
      done
    done
  done
done

echo "==> $queued runs queued, $skipped already complete (skipped)"
echo "==> $NPROC cores detected, using $JOBS parallel streams"
[ "$queued" -eq 0 ] && { echo "Nothing to do."; exit 0; }

# ns-3 with FlowMonitor over 89 nodes is memory-hungry; if the box starts
# swapping, re-launch with a smaller JOBS.
echo "==> Starting. Progress: watch \`wc -l $manifest\` from another shell."
date
# -0 (null-delimited) disables xargs' own quote processing, which would
# otherwise eat the single quotes inside each job line and mangle the command.
tr '\n' '\0' < "$JOBS_FILE" | xargs -0 -I CMD -P "$JOBS" bash -c CMD
date

echo
echo "========================================="
ok=$(grep -c ',OK,'   "$manifest" 2>/dev/null || true); ok=${ok:-0}
bad=$(grep -c ',FAIL,' "$manifest" 2>/dev/null || true); bad=${bad:-0}
echo "Run set A complete: $ok OK, $bad FAIL"
[ "$bad" -gt 0 ] && { echo "Failed runs:"; grep ',FAIL,' "$manifest"; }
echo "Summaries: $(ls "$OUTDIR"/raw/*_summary.csv 2>/dev/null | wc -l) / $(( 2*4*3*RUNS ))"
echo "Next: python3 analyze_results.py --dir $OUTDIR/raw --out $OUTDIR/analysis_v3"
echo "========================================="
