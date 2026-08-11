#!/usr/bin/env bash
# graf_deploy_smoke.sh — deploy the modularized GRAF sources into ns-3 scratch,
# build, and run a 3-run smoke test that verifies the three recovery fixes.
#
# Run this ON THE SERVER (bluejay), from anywhere:
#     bash ~/graf_deploy_smoke.sh
#
# Env overrides:
#     NS3_ROOT   ns-3 tree            (default: ~/ns-allinone-3.39/ns-3.39)
#     SRC_DIR    dir holding the .cc + graf_*.h (default: ~/graf-src)

set -uo pipefail

NS3_ROOT="${NS3_ROOT:-$HOME/ns-allinone-3.39/ns-3.39}"
SRC_DIR="${SRC_DIR:-$HOME/graf-src}"
TARGET="hybrid-star-mesh-sim"
SMOKE="smoke"

die() { echo "FATAL: $*" >&2; exit 1; }

[ -d "$NS3_ROOT" ] || die "ns-3 not found at $NS3_ROOT"
[ -d "$SRC_DIR" ]  || die "source dir not found at $SRC_DIR"
[ -f "$SRC_DIR/Star_mesh_simulation_code.cc" ] || die "Star_mesh_simulation_code.cc missing in $SRC_DIR"

# ---------------------------------------------------------------------------
# 1. Stage sources into scratch/
# ---------------------------------------------------------------------------
# The .cc includes its five headers with quoted includes, which the compiler
# resolves relative to the .cc's own directory -- so the headers must sit
# alongside it in scratch/, not in an include path.
echo "==> Staging sources into $NS3_ROOT/scratch/"
cp "$SRC_DIR/Star_mesh_simulation_code.cc" "$NS3_ROOT/scratch/$TARGET.cc" || die "copy .cc failed"
cp "$SRC_DIR"/graf_*.h "$NS3_ROOT/scratch/" || die "copy headers failed"
ls -l "$NS3_ROOT/scratch/$TARGET.cc" "$NS3_ROOT/scratch"/graf_*.h

cd "$NS3_ROOT" || die "cannot cd $NS3_ROOT"

# ---------------------------------------------------------------------------
# 2. Build
# ---------------------------------------------------------------------------
echo
echo "==> Building (this can take a few minutes on a cold cache)"
# ns-3.39 names scratch targets "scratch_<stem>"; if that lookup misses, fall
# back to a plain incremental build rather than failing the deploy.
if ! ./ns3 build "scratch_$TARGET" > /tmp/graf_build.log 2>&1; then
  echo "    (targeted build missed; falling back to full incremental build)"
  ./ns3 build > /tmp/graf_build.log 2>&1 || { tail -30 /tmp/graf_build.log; die "build failed"; }
fi
tail -5 /tmp/graf_build.log

# ---------------------------------------------------------------------------
# 3. Smoke runs
# ---------------------------------------------------------------------------
# Scenario 1 kills 3 of 8 CHs; scenario 4 kills 7 of 8. The GRAF-Global
# successor fast path only fires when a CH that was *itself* promoted as a
# backup later dies, so it is only reachable under a deep cascade -- hence the
# scenario-4 run. Seed 1017 = 1000 + 1*17, matching run index 1 of the sweep.
mkdir -p "$SMOKE"
echo
echo "==> Smoke runs (3)"

run_one() {
  local tag="$1"; shift
  echo "--- $tag"
  ./ns3 run "scratch/$TARGET" -- "$@" --output="$SMOKE/$tag" \
    > "$SMOKE/$tag.log" 2>&1
  local rc=$?
  [ $rc -eq 0 ] || { echo "    RUN FAILED (rc=$rc), tail of log:"; tail -20 "$SMOKE/$tag.log"; }
  return $rc
}

run_one base_sc1   --protocol=OLSR --scenario=1 --graf=off    --seed=1017 --run=1
run_one global_sc1 --protocol=OLSR --scenario=1 --graf=global --seed=1017 --run=1
run_one global_sc4 --protocol=OLSR --scenario=4 --graf=global --seed=1017 --run=1

# ---------------------------------------------------------------------------
# 4. Verify the three fixes
# ---------------------------------------------------------------------------
echo
echo "======================= FIX VERIFICATION ======================="

pass=0; fail=0
check() { # check <label> <condition-rc> <detail>
  if [ "$2" -eq 0 ]; then echo "  PASS  $1"; echo "        $3"; pass=$((pass+1))
  else                    echo "  FAIL  $1"; echo "        $3"; fail=$((fail+1)); fi
}

summary_val() { # summary_val <tag> <key>
  local f; f=$(ls "$SMOKE/$1"*_summary.csv 2>/dev/null | head -1)
  [ -n "$f" ] && grep "^$2," "$f" | head -1 | cut -d, -f2
}

# --- Fix #1: GRAF-Global pre-selected successor is actually consumed ---------
# Before the fix, HandleChDeath() erased g_chSuccessor[chIndex] at death time,
# so DetectAndRecoverCluster() 1.5 s later always missed the map and fell back
# to full re-scoring. This log line proves the fast path now executes.
# NB: `grep -c` prints 0 *and* exits 1 on no-match, so `|| echo 0` would append a
# second line. `|| true` keeps grep's own "0" as the sole output.
n_succ=$(grep -c "Using pre-selected successor" "$SMOKE/global_sc4.log" 2>/dev/null || true)
n_succ=${n_succ:-0}
[ "$n_succ" -gt 0 ]; check "Fix #1 successor fast path fires (sc4)" $? \
  "'Using pre-selected successor' hits: $n_succ  (expected >0)"

n_presel=$(grep -c "Pre-selected successor idx" "$SMOKE/global_sc4.log" 2>/dev/null || true)
n_presel=${n_presel:-0}
echo "        (successors pre-selected during run: $n_presel)"

# --- Fix #2: TSI logged for baseline runs, so partition time is recorded -----
tsi_file=$(ls "$SMOKE/base_sc1"*_tsi.csv 2>/dev/null | head -1)
tsi_rows=0
[ -n "$tsi_file" ] && tsi_rows=$(( $(wc -l < "$tsi_file") - 1 ))
[ "$tsi_rows" -gt 0 ]; check "Fix #2 baseline run emits TSI data rows" $? \
  "data rows in $(basename "${tsi_file:-<none>}"): $tsi_rows  (expected >0; was header-only before)"

pt_base=$(summary_val base_sc1 partition_time_s)
echo "        baseline partition_time_s = ${pt_base:-<missing>}  (-1 means backbone never partitioned, which is a valid result)"

# --- Fix #3: SRL watches armed only for sensors actually rehomed -------------
# n_sensors_eventdriven must never exceed the sensors GRAF actually moved.
rec_g=$(summary_val global_sc1 recovered_sensors)
evt_g=$(summary_val global_sc1 n_sensors_eventdriven)
if [ -n "$rec_g" ] && [ -n "$evt_g" ]; then
  [ "$evt_g" -le "$rec_g" ]; check "Fix #3 event-driven sensor count <= rehomed count" $? \
    "n_sensors_eventdriven=$evt_g  recovered_sensors=$rec_g"
else
  check "Fix #3 event-driven sensor count <= rehomed count" 1 "could not read summary keys"
fi
echo "        mean_reconv_eventdriven_s = $(summary_val global_sc1 mean_reconv_eventdriven_s)"
echo "        mean_reconv_s (snapshot)  = $(summary_val global_sc1 mean_reconv_s)"

# --- Run set D: paired high-precision energy (FIX-A6) ------------------------
# MUST use ch_energy_consumed_hires, NOT total_consumed_ch_energy_j. The latter is
# written at default 6 s.f. and will show a spurious 0.00000 difference -- which is
# precisely the rounding artifact FIX-A6 exists to see through. The _hires keys are
# emitted under std::fixed << std::setprecision(10).
echo
echo "--- Run set D: paired CH energy, 10 d.p. (Sc1 / OLSR / seed 1017)"
e_base=$(summary_val base_sc1   ch_energy_consumed_hires)
e_graf=$(summary_val global_sc1 ch_energy_consumed_hires)
echo "        baseline     ch_energy_consumed_hires = ${e_base:-<missing>}"
echo "        GRAF-Global  ch_energy_consumed_hires = ${e_graf:-<missing>}"
echo "        (low-precision keys, for contrast: $(summary_val base_sc1 total_consumed_ch_energy_j) vs $(summary_val global_sc1 total_consumed_ch_energy_j))"
if [ -n "$e_base" ] && [ -n "$e_graf" ]; then
  python3 -c "
b, g = float('$e_base'), float('$e_graf')
d = g - b
print(f'        paired delta = {d:+.10f} J')
print('        -> TRUE zero: \"t-statistic undefined\" in SS VI-H is literally correct' if d == 0.0
      else f'        -> NONZERO at 10 d.p.: SS VI-H must be rewritten with the measured delta')
" 2>/dev/null || echo "        (python3 unavailable for delta)"
fi

# --- Sanity: PDR gain in the expected direction ------------------------------
echo
echo "--- Sanity"
echo "        PDR baseline    = $(summary_val base_sc1   pdr_percent) %"
echo "        PDR GRAF-Global = $(summary_val global_sc1 pdr_percent) %"

# Paper Table III: sc1 kills 3 of 8, sc4 kills 7 of 8. Counts may exceed the
# forced-kill schedule because CheckChEnergy() can also deplete a CH.
echo "        ch_deaths sc1   = $(summary_val base_sc1 ch_deaths)   (forced schedule: 3)"
echo "        ch_deaths sc4   = $(summary_val global_sc4 ch_deaths)   (forced schedule: 7)"

# Paper defines T_net as the FIRST CH failure, and its own failure model
# t_fail(k) = 60 + 30k + J_k puts that at 60 +/- 3 s. Table IX reports 90.0 +/- 1.73,
# which is t_fail(1) -- the second failure. This line settles it.
echo "        first_ch_death_time_s sc1 = $(summary_val base_sc1   first_ch_death_time_s)  <- expect ~60, paper Table IX says 90.0"
echo "        first_ch_death_time_s sc4 = $(summary_val global_sc4 first_ch_death_time_s)  <- expect ~60"

echo
echo "==============================================================="
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ] && echo "  Smoke test GREEN -- safe to launch the 480-run sweep." \
                  || echo "  Smoke test RED -- do NOT launch the sweep yet."
echo "==============================================================="
exit "$fail"
