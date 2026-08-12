# Rerouting Convergence — A Case Study
### GRAF: Graph-Aware Recovery Framework for Hybrid Star-Mesh IoT Networks

> **Simulation code accompanying the paper:**
> *"Routing Reconvergence Under Cascading CH Energy Collapse in Hybrid Star-Mesh IoT Networks"*
> Jay Joshi, Rushikesh Giri, Atharv Shah, Shweta Gore, Dr. Asha Rawat, Prof. Aditya Kasar
> Department of Computer Engineering, NMIMS Kharghar, Navi Mumbai, India
> *(Under IEEE review — the paper itself is not included in this repository)*

---

> ### ⚠️ Status: results are being regenerated
>
> A defect was identified in the NS-3.39 energy model (see
> [Energy model — required NS-3 patch](#energy-model--required-ns-3-patch) below). It affects
> **energy metrics only**. The pre-computed CSVs, plots, and the energy figures quoted
> in this README come from the pre-fix runs and are **being replaced** by a full re-run.
>
> Packet delivery ratio, throughput, delay, service restoration latency, fairness, and
> recovery-rate results are derived from FlowMonitor and application traces, do not pass
> through the energy model, and are unaffected.

---

## What is this about?

Clustered IoT networks are everywhere — industrial sensors, smart agriculture, environmental monitoring. They work great until a cluster head (CH) dies. When a CH runs out of energy, every sensor attached to it goes dark at once. And if CHs keep failing in sequence? The whole network can collapse faster than any routing protocol can recover.

Standard protocols like **OLSR** and **AODV** handle backbone routing just fine, but neither one knows how to bring orphaned sensor clusters back online. That gap is exactly what **GRAF** is designed to fill.

**GRAF (Graph-Aware Recovery Framework)** is a lightweight, protocol-agnostic overlay that sits *above* the routing layer. When a CH fails, GRAF picks the best available backup CH using a multi-criteria fitness score, then rehomes all orphaned sensors to it via static default-route updates — no protocol modification needed, no synthetic traffic injected.

This repository contains the NS-3 simulation code, the batch-run scripts, and the analysis pipeline used to evaluate GRAF.

---

## Repository Structure

Everything lives at the repository root — a fresh clone gives you exactly this:

```
.
├── Star_mesh_simulation_code.cc   # Main simulation entry point
├── graf_globals.h                 # Shared experiment state & GRAF parameters
├── graf_utils.h                   # Graph utilities (betweenness centrality, BFS)
├── graf_selectors.h               # Backup CH selection logic (fitness function)
├── graf_recovery.h                # Cluster recovery & sensor rehoming
├── graf_logging.h                 # CSV output & SRL event logging
│
├── graf_deploy_smoke.sh           # Stage sources into ns-3, build, 3-run smoke test
├── graf_run_setA.sh               # Canonical 480-run sweep (resumable)
├── run_extended_sweep.sh          # Extended sweep: baselines, ablation, scalability
│
├── analyze_results.py             # Statistical analysis + plot generation
├── compute_dz.py                  # Cohen's d_z effect size calculator
│
├── dz_report.txt                  # Pre-computed effect size summary
├── main_results_table_core.csv    # Primary results table (all scenarios)
├── main_results_table_appendix.csv
├── improvement_table.csv
├── significance_tests.csv         # Paired t-tests + Holm-adjusted p-values
└── plot_*.png                     # Generated figures (PDR, throughput, JFI, etc.)
```

### A note on the header split

This is a **single-translation-unit `#include` split**. The five `graf_*.h` headers contain
function *definitions*, not just declarations, and all shared state is `static` at file
scope. They must **not** be compiled as separate translation units — each would get its own
copy of the globals and the simulation would silently corrupt at runtime.

Because the includes are quoted, the compiler resolves them relative to the `.cc` file's own
directory, so **the headers must sit beside the `.cc` in `scratch/`**. The deploy script
handles this for you.

---

## The Problem in Plain Terms

Imagine a field of 80 sensors grouped under 8 cluster heads, all reporting data back to a gateway. The CHs relay data across a Wi-Fi mesh backbone (OLSR or AODV), while sensors talk to their CH over a short-range star link.

Now imagine CH #3 runs low on energy and dies. The 10 sensors in its cluster are immediately orphaned — still alive, still trying to send data, but nobody is listening. Standard routing protocols will eventually reroute backbone traffic around the dead node, but they do nothing for those stranded sensors.

GRAF watches for CH failures, identifies the best surviving CH to act as a backup, and rewrites the static access-plane routes for the orphaned sensors. The whole process completes in **under 3 seconds** on average, and it works identically regardless of whether the backbone is running OLSR or AODV.

---

## Key Results

> These figures come from the **pre-fix** 480-run batch (20 replications × 2 protocols ×
> 4 scenarios × 3 modes) and are pending regeneration. Non-energy metrics are expected to
> hold; the energy row has been withdrawn.

| Scenario | Description | GRAF-Global PDR Gain | Cluster Recovery |
|----------|-------------|---------------------|-----------------|
| 1 — Mild | 3 CH failures, nominal energy | **+25.2 pp** | 100% |
| 2 — Moderate | 5 failures, reduced energy | **+24.1 pp** | 100% |
| 3 — Severe | 7 failures, low energy | **+24.6 pp** | ~95.7% |
| 4 — Extreme | 7 of 8 CHs fail + 30% range cut | positive, narrower | 63–76% |

- All Scenario 1–3 gains: **Holm-adjusted p < 10⁻¹⁴**, Cohen's d ≥ 6.20
- Mean Service Restoration Latency: **2.48–2.64 seconds** (Scenarios 1–3)
- PDR gains are **identical under OLSR and AODV** — validating the access/backbone decoupling design

**Withdrawn pending re-run:** an earlier version of this README reported CH energy overhead
as "statistically indistinguishable from baseline (Holm p = 1.0)". That result was an
artifact of the energy-model defect described below — the meter stopped accumulating partway
through each run, so both arms recorded the same frozen value regardless of actual radio
activity. No energy-overhead claim should be drawn from the current CSVs.

---

## How GRAF Selects a Backup CH

GRAF-Global scores each surviving candidate CH using a weighted fitness function:

```
fitness(v) = α · E_residual(v)       // remaining battery (weight 0.35)
           + β · BC(v)               // betweenness centrality in backbone (0.25)
           + γ · (1 / d_backbone)    // proximity on the mesh (0.15)
           + δ · coverage(v)         // how many orphaned sensors it can reach (0.25)
           - load_penalty(v)         // penalise already-overloaded CHs
```

**GRAF-Local** replaces global betweenness centrality with a local heuristic: candidates are
drawn from a **1-hop pool** and scored in part by a **2-hop alive-reachability count**. This
cuts topology state requirements while staying competitive on recovery metrics. In practice,
GRAF-Local achieves higher *sensor-level* recovery rates (100.0% vs 91.5% in Scenario 1)
while GRAF-Global leads on aggregate PDR.

Note that `--ablation` applies to **GRAF-Global only**; GRAF-Local uses a fixed weight set.

---

## Energy model — required NS-3 patch

Energy measurements require a patch to NS-3.39's own `WifiRadioEnergyModel`. **A stock
NS-3.39 install will not reproduce the energy results in the paper.**

`WifiRadioEnergyModel::ChangeState()` arms a self-scheduled shutoff on every radio state
transition, sized on the assumption that the current state persists indefinitely. With the
stock `CcaBusyCurrentA` default, that window can be short enough for an ordinary lull in
traffic to let it fire. Once it does, the model latches to `OFF` permanently — its guard only
permits leaving `OFF` if the state was not already `OFF` — and bills zero energy from that
point on, while the real PHY continues transmitting and receiving normally.

The observable symptom is a per-node energy total that freezes partway through a run and is
thereafter decoupled from the radio's actual activity and runtime.

The fix has two parts. **Both are now in this repository**; part 2 has to be applied to your
own NS-3 tree.

**1. Configuration — already in `Star_mesh_simulation_code.cc`.** `CcaBusyCurrentA` is now
set explicitly on all three radios rather than inheriting NS-3's stock 0.273 A, which is
~640× this radio's idle current. It is set equal to `IdleCurrentA` on each radio, which
preserves the relationship in NS-3's own stock defaults (where both are 0.273 A), rescaled
to this radio's current profile:

| Radio | `CcaBusyCurrentA` | Scaling |
|---|---|---|
| Sensor | `0.000426` | none |
| CH | `0.000426 * 2.0` | fixed 2.0 dual-radio factor — **not** `chDrainMultiplier` |
| Gateway | `0.000426` | none |

The CH value deliberately does not take `chDrainMultiplier`. That multiplier models elevated
*active* duty and applies to `TxCurrentA`/`RxCurrentA` only; CCA_BUSY is a passive sensing
state and scales like `IdleCurrentA`.

**2. A patch to the NS-3 core** removing the redundant watchdog — committed as
[`patches/wifi-radio-energy-model.patch`](patches/). This is safe: genuine battery depletion
is handled independently and correctly by `BasicEnergySource`'s own threshold logic, and this
simulation installs `BasicEnergySourceHelper` on all three node tiers. The watchdog was a
second, defective path to the same outcome.

```bash
cd ~/ns-allinone-3.39/ns-3.39
patch -p1 --dry-run < /path/to/patches/wifi-radio-energy-model.patch   # verify first
patch -p1           < /path/to/patches/wifi-radio-energy-model.patch
./ns3 build
grep -c 'FIX-E3' src/wifi/model/wifi-radio-energy-model.cc             # expect 3
```

See [`patches/README.md`](patches/README.md) for the full defect description, the evidence,
and revert instructions.

### A related change: initial-energy jitter

Per-CH initial energy is jittered to break ties between otherwise identical CHs. The bounds
were narrowed from `U(0.90, 1.10)` to **`U(0.995, 1.005)`** in the same revision.

With the energy model fixed, a Scenario 1 / OLSR run consumes **~21 J across the 8 CHs**
(roughly 2.6 J per CH against a 10 J budget). The old ±10 % jitter spanned 2.0 J — the same
order as consumption itself — so which CH held the most residual energy was substantially
decided by its initial draw rather than by what it had spent, making `--baseline=energy`
closer to a random selector than an energy-aware one. The new bounds span 0.1 J, a few
percent of consumption, so residual energy ranks CHs by what they actually spent.

The jitter is deliberately tighter than real battery manufacturing tolerance (typically
±2–5 %), because its purpose here is tie-breaking rather than modelling production spread.

> **Do not re-derive these bounds from any pre-fix energy figure.** Pre-fix runs under-billed
> by roughly an order of magnitude. The previously published total of 2.18 J for this
> configuration is below the ~9.95 J these radios consume *sitting completely idle* for the
> run, which is on its own sufficient to reject the old measurements.

---

## Running the Simulations

### Prerequisites

- **NS-3.39** — tested on NS-3.39 only; other versions are not guaranteed to work
- C++17-compatible compiler (GCC 9+ or Clang 10+)
- Python 3.8+ with `numpy`, `pandas`, `scipy`, `matplotlib`

### Setup

The deploy script stages the sources into your NS-3 tree, builds, and runs a 3-run smoke test
that verifies the recovery fixes:

```bash
cd ~
git clone https://github.com/anonjj/Rerouting-convergence-a-case-study.git graf-src
NS3_ROOT=~/ns-allinone-3.39/ns-3.39 SRC_DIR=~/graf-src bash graf-src/graf_deploy_smoke.sh
```

Both variables default to `~/ns-allinone-3.39/ns-3.39` and `~/graf-src`, so if you clone into
your home directory the two env assignments are optional. Clone elsewhere and you must set
`SRC_DIR` to match — it is not inferred from the script's own location. Do not launch a full
sweep until the smoke test is green.

To stage manually instead:

```bash
cp Star_mesh_simulation_code.cc  <ns3-root>/scratch/hybrid-star-mesh-sim.cc
cp graf_*.h                      <ns3-root>/scratch/
cd <ns3-root> && ./ns3 build
```

### Run a Single Simulation

```bash
# Baseline (no recovery), OLSR, Scenario 1
./ns3 run scratch/hybrid-star-mesh-sim -- \
  --protocol=OLSR --scenario=1 --graf=off \
  --seed=1017 --run=1 --output=results/test

# GRAF-Global enabled, AODV, Scenario 2
./ns3 run scratch/hybrid-star-mesh-sim -- \
  --protocol=AODV --scenario=2 --graf=global \
  --seed=1017 --run=1 --output=results/test
```

### Key Parameters

| Parameter | Options | Description |
|-----------|---------|-------------|
| `--protocol` | `OLSR`, `AODV` | Backbone routing protocol |
| `--scenario` | `1`, `2`, `3`, `4` | Failure severity (1 = mild → 4 = extreme) |
| `--graf` | `off`, `global`, `local` | Recovery mode |
| `--baseline` | `none`, `rand`, `energy`, `nearest` | Competitive baseline strategy |
| `--ablation` | `full`, `energy`, `topo`, `proxcov` | Fitness function ablation (GRAF-Global only) |
| `--chs` | integer | Number of cluster heads (default: 8) |
| `--sensors` | integer | Number of sensor nodes (default: 80) |
| `--deathfrac` | float | Fraction of CHs to fail (overrides scenario default) |
| `--drainMult` | float | CH energy drain multiplier |
| `--chEnergy` | float | Initial CH energy (Joules) |
| `--simTime` | float | Simulation duration in seconds (default: 300) |
| `--area` | float | Deployment area side length in metres (default: 400) |
| `--seed` / `--run` | integer | RNG seed and run number |
| `--output` | path prefix | Where to write `*_summary.csv` and friends |

### Run the Full Sweep

`graf_run_setA.sh` is the canonical sweep: 20 replications × 2 protocols × 4 scenarios ×
3 modes = 480 runs. It is **resumable** — any run whose `*_summary.csv` already exists is
skipped, so you can interrupt and relaunch without losing completed work.

```bash
cd <ns3-root>
bash ~/graf-src/graf_run_setA.sh
```

| Env var | Default | Meaning |
|---------|---------|---------|
| `JOBS` | `nproc - 1` | Parallel streams |
| `OUTDIR` | `sim_results_v3` | Results directory |
| `RUNS` | `20` | Replications per cell |

NS-3 with FlowMonitor over 89 nodes is memory-hungry; if the machine starts swapping, lower
`JOBS`.

To verify the sweep ran cleanly:

```bash
wc -l sim_results_v3/run_manifest.csv        # should be 481 (480 runs + header)
grep ',FAIL,' sim_results_v3/run_manifest.csv  # should be empty
```

### Extended Sweep

`run_extended_sweep.sh` covers the additional experiments added during the reviewer response.
It contains four independent sweeps:

| `SWEEPS` | Sweep | Cells | Runs |
|---|---|---|---|
| `1` | Competitive baselines | 3 baselines × 4 scenarios × 2 protocols × 20 seeds | 480 |
| `2` | Fitness ablation | 3 ablations × 4 scenarios × 2 protocols × 20 seeds | 480 |
| `3` | Scalability (Nc = 16, 32) | 2 sizes × 2 protocols × 3 modes × 20 seeds | 240 |
| `4` | Scenario 4 strengthening | 1 scenario × 3 modes × 2 protocols × 30 seeds | 180 |

Running it bare generates all **1,380** jobs, which is rarely what you want. Select sweeps and
cells with `SWEEPS`, `SCENARIOS`, and `PROTOCOLS`:

```bash
SWEEPS=1 SCENARIOS=3 ./run_extended_sweep.sh                 # baselines, Sc3 only  -> 120 runs
SWEEPS=2 SCENARIOS=3 PROTOCOLS=OLSR ./run_extended_sweep.sh  # ablation, Sc3/OLSR   ->  60 runs
SWEEPS=3 ./run_extended_sweep.sh                             # scalability          -> 240 runs
```

`SCENARIOS` applies to sweeps 1 and 2 only — sweeps 3 and 4 are pinned to Scenario 2 and
Scenario 4 by design. All three variables accept comma- or space-separated lists.

Set `DRY_RUN=1` to generate and count the job list without building or running anything —
useful for confirming a selection produces the cell count you expect before committing the
machine to it.

Also honours `NS3_ROOT`, `SCRATCH_NAME`, `OUTDIR`, and `PARALLEL_JOBS`; defaults match what
`graf_deploy_smoke.sh` produces.

**The ablation sweep must run with `--graf=global`.** `SelectLocalBackup` uses a fixed weight
set and ignores the `--ablation` flag entirely, so an ablation run under `--graf=local` would
silently report full-GRAF numbers. Likewise the baseline sweep must run with `--graf=off` —
a baseline strategy is ignored unless GRAF is off. The script already emits both correctly.

### Analyze Results

```bash
python3 analyze_results.py \
  --dir sim_results_v3/raw \
  --out sim_results_v3/analysis_v3
```

Options: `--sd-bars` uses standard deviation instead of 95% CI for plot error bars.

This generates the tables and plots used in the paper:
- `significance_tests.csv` — Paired t-tests, Holm correction, Cohen's d_z
- `main_results_table_core.csv` — Per-scenario summary statistics
- `plot_pdr.png`, `plot_throughput.png`, `plot_jain_fairness.png`, etc.

`compute_dz.py` reads `significance_tests.csv` and `main_results_table_core.csv` from the
current directory and writes `dz_report.txt`, a formatted Cohen's d_z table.

---

## Simulation Design Notes

| Parameter | Value |
|-----------|-------|
| Network size | 80 sensors, 8 CHs, 1 gateway |
| Area | 400 × 400 m |
| Radio standard | IEEE 802.11b ad hoc, 1 Mbps (DsssRate1Mbps) |
| Access-plane range | 140 m |
| Backbone range | 160 m |
| GRAF detection delay | 1.5 s after CH death |
| Topology jitter | ±15 m per run |
| Energy model | NS-3 `WifiRadioEnergyModel` (patch required — see above) |
| Traffic | UDP sensor-to-gateway, constant bit rate |
| Simulation duration | 300 s |
| Replications per cell | 20 independent seeds |
| Seed convention | `seed = 1000 + 17 × run_index`, `run_index ∈ {1…20}` |

CH failures are **schedule-injected**, not energy-driven: `t_fail(k) = 60 + 30k + J_k` with
`J_k ~ Uniform(−3, +3)` seconds. The first failure therefore occurs at 60.0 ± 1.73 s. Runs
report `chs_depleted = 0` because no CH reaches its energy threshold before the scheduled
kill.

---

## Pre-Computed Results

The repository ships pre-computed outputs so you can inspect results without re-running
everything. **These are from the pre-fix batch — see the status notice at the top.**

| File | Contents |
|------|----------|
| `significance_tests.csv` | All pairwise statistical comparisons |
| `main_results_table_core.csv` | PDR, throughput, delay, fairness, energy |
| `improvement_table.csv` | Percentage-point improvements over baseline |
| `dz_report.txt` | Cohen's d_z effect size summary |
| `plot_*.png` | Publication-ready figures |

---

## Authors

| Name | Affiliation |
|------|------------|
| Jay Joshi | NMIMS Kharghar, Navi Mumbai |
| Rushikesh Giri | NMIMS Kharghar, Navi Mumbai |
| Atharv Shah | NMIMS Kharghar, Navi Mumbai |
| Shweta Gore | NMIMS Kharghar, Navi Mumbai |
| Dr. Asha Rawat | NMIMS Kharghar, Navi Mumbai |
| Prof. Aditya Kasar | NMIMS Kharghar, Navi Mumbai |

---

## Citation

BibTeX will be added here upon paper acceptance. Please check back after IEEE review.

---

## A Few Notes

- The **paper itself is not in this repo** — it is under IEEE review.
- The codebase targets **NS-3.39 specifically**. We haven't tested it on other versions.
- Energy results additionally require the NS-3 core patch documented above.
- All statistical tests use **Holm-Bonferroni correction** across the full family of comparisons.
