#!/bin/bash
# =============================================================================
# run_extended_sweep.sh — Reviewer Revision Extended Simulation Sweep
# =============================================================================
# Covers all five reviewer fault fixes requiring additional simulation runs:
#
# [FAULT-4]  Competitive Baselines: RAND / ENERGY / NEAREST
#            3 baselines × 4 scenarios × 2 protocols × 20 seeds = 480 runs
#
# [FAULT-5]  Fitness Ablation Study: energy-only / topo-only / proxcov-only
#            3 ablations × 4 scenarios × 2 protocols × 20 seeds = 480 runs
#            (all run with --graf=global, --baseline=none)
#
# [FAULT-9]  Scalability: medium (Nc=16,Ns=160) and large (Nc=32,Ns=320)
#            Scenario 2 (Moderate), deathfrac=0.625 (proportional to 5/8)
#            2 sizes × 2 protocols × 3 modes × 20 seeds = 240 runs
#
# [FAULT-2&10] Sc4 Statistical Strengthening: n=50, seeds spaced by 10^6
#            1 scenario × 3 modes × 2 protocols × 30 extra seeds = 180 runs
#            (20 seeds already done; 30 additional seeds = runs 21-50)
#
# Total new runs: 480 + 480 + 240 + 180 = 1,380
#
# Usage:
#   chmod +x run_extended_sweep.sh
#   ./run_extended_sweep.sh
#
# Defaults assume the layout produced by graf_deploy_smoke.sh. Override with
# env vars if your install differs, e.g.:
#   NS3_ROOT=~/ns-3.39 PARALLEL_JOBS=8 ./run_extended_sweep.sh
# =============================================================================

set -uo pipefail

# ---------------- User Configuration ----------------------------------------
# Defaults match graf_deploy_smoke.sh, which stages the sources into
# scratch/hybrid-star-mesh-sim.cc. Override any of these from the environment.
NS3_ROOT="${NS3_ROOT:-$HOME/ns-allinone-3.39/ns-3.39}"  # NS-3.39 build directory
SCRATCH_NAME="${SCRATCH_NAME:-hybrid-star-mesh-sim}"    # Name of .cc in scratch/
OUTDIR="${OUTDIR:-sim_results_extended}"                # Output dir for this sweep
PARALLEL_JOBS="${PARALLEL_JOBS:-3}"                     # Parallel simulation jobs

# Sweep selection. Running everything is ~1,380 jobs, which is rarely what you
# want -- gate it down to the sweeps and cells you actually need.
#   SWEEPS     which of the four sweeps to generate (1=baselines, 2=ablation,
#              3=scalability, 4=Sc4 strengthening)
#   SCENARIOS  scenarios for sweeps 1 and 2 only; sweeps 3 and 4 are pinned to
#              Scenario 2 and Scenario 4 respectively by design
#   PROTOCOLS  routing protocols, applies to all sweeps
#
# Examples:
#   SWEEPS=1 SCENARIOS=3 ./run_extended_sweep.sh                   # 120 runs
#   SWEEPS=2 SCENARIOS=3 PROTOCOLS=OLSR ./run_extended_sweep.sh    #  60 runs
#   SWEEPS=3 ./run_extended_sweep.sh                               # 240 runs
# CH energy budget for the 32-CH scalability cell only -- see the long note at
# the large-scale block in SWEEP 3 for why it cannot use the 10 J default.
# Worst observed demand at 32 CHs was ~9.5 J over 300 s; 30 J puts the 10%
# low-battery threshold at 3 J, which is comfortably below that.
CH_ENERGY_LARGE="${CH_ENERGY_LARGE:-30}"

SWEEPS="${SWEEPS:-1 2 3 4}"
SCENARIOS="${SCENARIOS:-1 2 3 4}"
PROTOCOLS="${PROTOCOLS:-OLSR AODV}"
# Accept comma-separated lists as well as space-separated.
SWEEPS="${SWEEPS//,/ }"
SCENARIOS="${SCENARIOS//,/ }"
PROTOCOLS="${PROTOCOLS//,/ }"

# True when sweep $1 is in $SWEEPS.
sweep_enabled() {
  local want="$1" s
  for s in $SWEEPS; do [ "$s" = "$want" ] && return 0; done
  return 1
}
# -----------------------------------------------------------------------------

cd "$NS3_ROOT" || { echo "ERROR: NS3_ROOT not found: $NS3_ROOT"; exit 1; }

mkdir -p "$OUTDIR/raw" "$OUTDIR/logs"

JOBS_FILE="$OUTDIR/extended_jobs.txt"
> "$JOBS_FILE"

SKIPPED=0
QUEUED=0

MANIFEST="$OUTDIR/run_manifest_extended.csv"
# Append, don't truncate. A resumed sweep must not erase the record of the runs
# that came before it -- truncating here is what made a half-finished Set B look
# like it had zero failures.
[ -f "$MANIFEST" ] || echo "sweep,protocol,scenario,mode,baseline,ablation,num_chs,num_sensors,run,seed,status,logfile,prefix" > "$MANIFEST"

# ─── Helper: emit one job line ────────────────────────────────────────────────
# Usage: emit_job SWEEP PROTOCOL SCENARIO MOD BASELINE ABLATION NUMCHS NUMSENSORS RUN SEED [EXTRA_ARGS]
emit_job() {
  local sweep="$1"
  local proto="$2"
  local sc="$3"
  local graf_mode="$4"
  local baseline="$5"
  local ablation="$6"
  local num_chs="$7"
  local num_sensors="$8"
  local run="$9"
  local seed="${10}"
  local extra="${11:-}"

  local prefix="$OUTDIR/raw/${sweep}_${proto}_sc${sc}_${graf_mode}_bl${baseline}_abl${ablation}_chs${num_chs}_run${run}"
  local logfile="$OUTDIR/logs/${sweep}_${proto}_sc${sc}_${graf_mode}_bl${baseline}_abl${ablation}_chs${num_chs}_run${run}.log"

  # Resumable: skip any cell that already produced a summary. Without this a
  # sweep that is interrupted -- or that loses one size class to a crash -- has
  # to redo every completed run to recover the missing ones. Set RESUME=0 to
  # force a full re-run (e.g. after changing a simulation parameter).
  if [ "${RESUME:-1}" = "1" ] && compgen -G "${prefix}*_summary.csv" > /dev/null; then
    SKIPPED=$(( SKIPPED + 1 ))
    return 0
  fi
  QUEUED=$(( QUEUED + 1 ))

  local cmd="./ns3 run 'scratch/$SCRATCH_NAME \
    --protocol=$proto \
    --scenario=$sc \
    --graf=$graf_mode \
    --baseline=$baseline \
    --ablation=$ablation \
    --chs=$num_chs \
    --sensors=$num_sensors \
    --seed=$seed \
    --run=$run \
    --output=$prefix \
    $extra' > '$logfile' 2>&1"

  local log_ok="echo '${sweep},${proto},${sc},${graf_mode},${baseline},${ablation},${num_chs},${num_sensors},${run},${seed},OK,${logfile},${prefix}' >> '$MANIFEST'"
  local log_fail="echo '${sweep},${proto},${sc},${graf_mode},${baseline},${ablation},${num_chs},${num_sensors},${run},${seed},FAIL,${logfile},${prefix}' >> '$MANIFEST'"

  echo "if eval \"$cmd\"; then eval \"$log_ok\"; else eval \"$log_fail\"; echo 'FAILED: $sweep $proto sc$sc $graf_mode bl=$baseline abl=$ablation chs=$num_chs run$run'; fi" >> "$JOBS_FILE"
}

# ─── Seed strategies ─────────────────────────────────────────────────────────
# [FAULT-2&10] Widely-spaced seeds: seed_i = i * 1,000,000
# This ensures NS-3 RNG streams are truly independent across replication runs.
# Standard runs (fault4/5/9): seeds 1..20 with spacing 10^6
declare -a SEEDS_STD
for i in $(seq 1 20); do
  SEEDS_STD[$i]=$(( i * 1000000 ))
done

# Sc4 extra seeds (fault 2&10): runs 21..50 with same spacing
declare -a SEEDS_SC4_EXTRA
for i in $(seq 21 50); do
  SEEDS_SC4_EXTRA[$i]=$(( i * 1000000 ))
done

RUNS_STD=20
RUNS_SC4_EXTRA=30   # runs 21-50

# =============================================================================
# SWEEP 1: Competitive Baselines [FAULT-4]
# --baseline=rand/energy/nearest, --graf=off
# 3 baselines × 4 scenarios × 2 protocols × 20 seeds = 480 runs
# =============================================================================
if sweep_enabled 1; then
echo "=== Generating SWEEP 1: Competitive Baselines [FAULT-4] ==="
for proto in $PROTOCOLS; do
  for sc in $SCENARIOS; do
    for baseline in rand energy nearest; do
      for run in $(seq 1 $RUNS_STD); do
        seed=${SEEDS_STD[$run]}
        emit_job "F4base" "$proto" "$sc" "off" "$baseline" "full" "8" "80" "$run" "$seed" ""
      done
    done
  done
done
echo "  Fault-4 jobs appended."
else echo "=== SWEEP 1 skipped (not in SWEEPS=$SWEEPS) ==="; fi

# =============================================================================
# SWEEP 2: Fitness Ablation Study [FAULT-5]
# --graf=global, --ablation=energy/topo/proxcov, --baseline=none
# 3 ablations × 4 scenarios × 2 protocols × 20 seeds = 480 runs
# =============================================================================
if sweep_enabled 2; then
echo "=== Generating SWEEP 2: Fitness Ablation [FAULT-5] ==="
for proto in $PROTOCOLS; do
  for sc in $SCENARIOS; do
    for ablation in energy topo proxcov; do
      for run in $(seq 1 $RUNS_STD); do
        seed=${SEEDS_STD[$run]}
        emit_job "F5abl" "$proto" "$sc" "global" "none" "$ablation" "8" "80" "$run" "$seed" ""
      done
    done
  done
done
echo "  Fault-5 jobs appended."
else echo "=== SWEEP 2 skipped (not in SWEEPS=$SWEEPS) ==="; fi

# =============================================================================
# SWEEP 3: Scalability Validation [FAULT-9]
# Scenario 2 (Moderate), deathfrac=0.625 (proportional to 5/8 of CHs)
# Sizes: medium (16 CHs, 160 sensors), large (32 CHs, 320 sensors)
# 2 sizes × 2 protocols × 3 modes × 20 seeds = 240 runs
# =============================================================================
if sweep_enabled 3; then
echo "=== Generating SWEEP 3: Scalability [FAULT-9] ==="
for proto in $PROTOCOLS; do
  # Medium scale: 16 CHs, 160 sensors
  for mode in off local global; do
    for run in $(seq 1 $RUNS_STD); do
      seed=${SEEDS_STD[$run]}
      emit_job "F9scale" "$proto" "2" "$mode" "none" "full" "16" "160" "$run" "$seed" "--deathfrac=0.625"
    done
  done
  # Large scale: 32 CHs, 320 sensors
  #
  # The 32-CH cell needs a larger CH energy budget than the 10 J default. A
  # 32-node backbone carries far more routing traffic per CH than an 8- or
  # 16-node one, and at 10 J the busiest CHs cross BasicEnergySource's 10%
  # low-battery threshold before the 300 s horizon. The depletion callback then
  # switches the PHY off mid-reception and the run aborts on
  #   NS_ASSERT failed, cond="IsStateIdle() || IsStateCcaBusy()"
  # in wifi-phy-state-helper.cc:414. Every one of the 120 large-scale runs died
  # this way; a candidate dump at t=275 s showed CHs down to 1.32 J remaining.
  #
  # This is a simulation-configuration floor, not a result: CH failures in this
  # study are schedule-injected (t_fail(k) = 60 + 30k + J_k), so a CH must never
  # die of depletion -- chs_depleted is expected to be 0 in every run. Raising
  # the budget does not bias the arm comparison either. GRAF normalises energy
  # as a ratio (eNorm = c.energy / maxEnergy), so a common scale factor is
  # invariant, and the scalability table compares off/local/global *within* each
  # size class, never across sizes. Consumption in Joules is unaffected by the
  # starting budget, so the energy metrics stay comparable.
  #
  # The 16-CH cell is left at the 10 J default: all 120 of its runs completed
  # with chs_depleted == 0, so it has adequate headroom as-is.
  for mode in off local global; do
    for run in $(seq 1 $RUNS_STD); do
      seed=${SEEDS_STD[$run]}
      emit_job "F9scale" "$proto" "2" "$mode" "none" "full" "32" "320" "$run" "$seed" \
        "--deathfrac=0.625 --chEnergy=$CH_ENERGY_LARGE"
    done
  done
done
echo "  Fault-9 jobs appended."
else echo "=== SWEEP 3 skipped (not in SWEEPS=$SWEEPS) ==="; fi

# =============================================================================
# SWEEP 4: Scenario 4 Statistical Strengthening [FAULT-2 & 10]
# n=20 already done; add 30 more seeds (runs 21-50) for n=50 total.
# 1 scenario × 3 modes × 2 protocols × 30 extra seeds = 180 runs
# Seeds use 10^6 spacing starting at run 21 (i.e., seed = 21_000_000 ...).
# =============================================================================
if sweep_enabled 4; then
echo "=== Generating SWEEP 4: Sc4 Statistical Strengthening [FAULT-2&10] ==="
for proto in $PROTOCOLS; do
  for mode in off local global; do
    for run in $(seq 21 50); do
      seed=${SEEDS_SC4_EXTRA[$run]}
      emit_job "F2F10sc4" "$proto" "4" "$mode" "none" "full" "8" "80" "$run" "$seed" ""
    done
  done
done
echo "  Fault-2&10 Sc4 jobs appended."
else echo "=== SWEEP 4 skipped (not in SWEEPS=$SWEEPS) ==="; fi

# =============================================================================
# Summary and execution
# =============================================================================
TOTAL_JOBS=$(wc -l < "$JOBS_FILE" | tr -d ' ')
echo ""
echo "============================================="
echo "Total jobs generated: $TOTAL_JOBS"
echo "  SWEEPS=$SWEEPS  SCENARIOS=$SCENARIOS  PROTOCOLS=$PROTOCOLS"
echo "  queued:  $QUEUED"
echo "  skipped: $SKIPPED (already have a summary; RESUME=0 to force re-run)"
echo "  (all four sweeps, all scenarios, both protocols = 1380)"
echo "Per-sweep breakdown:"
SWEEP_TAGS=("F4base" "F5abl" "F9scale" "F2F10sc4")
for s in 1 2 3 4; do
  tag="${SWEEP_TAGS[$((s-1))]}"
  n=$(grep -c "$tag" "$JOBS_FILE" 2>/dev/null || true)
  printf '  sweep %s (%-9s): %s\n' "$s" "$tag" "${n:-0}"
done
echo "Jobs file:            $JOBS_FILE"
echo "============================================="

if [ "$TOTAL_JOBS" -eq 0 ]; then
  echo "Nothing to run -- SWEEPS=$SWEEPS selected no jobs."
  exit 0
fi

# DRY_RUN=1 stops here: inspect $JOBS_FILE without building or running anything.
if [ -n "${DRY_RUN:-}" ]; then
  echo "DRY_RUN set -- job list written, not building or executing."
  exit 0
fi

echo ""
echo "Building NS-3 (single-threaded to avoid collision)..."
./ns3 build
if [ $? -ne 0 ]; then
  echo "ERROR: NS-3 build failed. Aborting."
  exit 1
fi
echo "Build successful."

echo ""
echo "Starting simulation sweep with $PARALLEL_JOBS parallel jobs..."
echo "Progress is logged in: $OUTDIR/logs/"
echo "Manifest: $MANIFEST"
echo ""

# -0 (null-delimited) disables xargs' own quote processing, which would
# otherwise eat the single quotes inside each job line. Without it, ns3 receives
# the simulation's arguments as its own options and every run fails.
tr '\n' '\0' < "$JOBS_FILE" | xargs -0 -I CMD -P "$PARALLEL_JOBS" bash -c CMD

echo ""
echo "============================================="
echo "Extended sweep complete!"
echo "Total jobs attempted: $TOTAL_JOBS"
echo "Check $MANIFEST for per-run status."
echo "Analyze results with:"
echo "  python analyze_results.py --dir $OUTDIR/raw --out $OUTDIR/analysis"
echo "============================================="
