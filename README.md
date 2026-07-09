# Rerouting Convergence — A Case Study
### GRAF: Graph-Aware Recovery Framework for Hybrid Star-Mesh IoT Networks

> **Simulation code accompanying the paper:**  
> *"Routing Reconvergence Under Cascading CH Energy Collapse in Hybrid Star-Mesh IoT Networks"*  
> Jay Joshi, Rushikesh Giri, Atharv Shah, Shweta Gore, Dr. Asha Rawat, Prof. Aditya Kasar  
> Department of Computer Engineering, NMIMS Kharghar, Navi Mumbai, India  
> *(Submitted for IEEE review — paper not included in this repository)*

---

## What is this about?

Clustered IoT networks are everywhere — industrial sensors, smart agriculture, environmental monitoring. They work great until a cluster head (CH) dies. When a CH runs out of energy, every sensor attached to it goes dark at once. And if CHs keep failing in sequence? The whole network can collapse faster than any routing protocol can recover.

Standard protocols like **OLSR** and **AODV** handle backbone routing just fine, but neither one knows how to bring orphaned sensor clusters back online. That gap is exactly what **GRAF** is designed to fill.

**GRAF (Graph-Aware Recovery Framework)** is a lightweight, protocol-agnostic overlay that sits *above* the routing layer. When a CH fails, GRAF picks the best available backup CH using a multi-criteria fitness score, then rehomes all orphaned sensors to it via static default-route updates — no protocol modification needed, no synthetic traffic injected.

This repository contains all the NS-3 simulation code, analysis scripts, and pre-computed result data used to evaluate GRAF across 480 simulation runs.

---

## Repository Structure

```
.
├── v2_fixed_simulation/               # Core simulation code (NS-3.39)
│   ├── Star_mesh_simulation_code.cc   # Main simulation entry point
│   ├── graf_globals.h                 # Shared experiment state & GRAF parameters
│   ├── graf_selectors.h               # Backup CH selection logic (fitness function)
│   ├── graf_recovery.h                # Cluster recovery & sensor rehoming
│   ├── graf_logging.h                 # CSV output & SRL event logging
│   ├── graf_utils.h                   # Graph utilities (betweenness centrality, etc.)
│   ├── compute_dz.py                  # Cohen's d_z effect size calculator
│   ├── dz_report.txt                  # Pre-computed effect size summary
│   ├── main_results_table_core.csv    # Primary results table (all scenarios)
│   ├── main_results_table_appendix.csv
│   ├── improvement_table.csv
│   ├── significance_tests.csv         # Paired t-tests + Holm-adjusted p-values
│   └── plot_*.png                     # Generated figures (PDR, throughput, JFI, etc.)
│
├── analyze_results.py            # Full statistical analysis + plot generation
├── run_all_sims_final.sh         # Shell script to batch-run all 480 simulations
└── run_extended_sweep.sh         # Extended sweep (larger networks, extra seeds)
```

---

## The Problem in Plain Terms

Imagine a field of 80 sensors grouped under 8 cluster heads, all reporting data back to a gateway. The CHs relay data across a Wi-Fi mesh backbone (OLSR or AODV), while sensors talk to their CH over a short-range star link.

Now imagine CH #3 runs low on energy and dies. The 10 sensors in its cluster are immediately orphaned — still alive, still trying to send data, but nobody is listening. Standard routing protocols will eventually reroute backbone traffic around the dead node, but they do nothing for those stranded sensors.

GRAF watches for CH failures, identifies the best surviving CH to act as a backup, and rewrites the static access-plane routes for the orphaned sensors. The whole process completes in **under 3 seconds** on average, and it works identically regardless of whether the backbone is running OLSR or AODV.

---

## Key Results

All numbers come from 480 NS-3.39 simulation runs (20 replications × 2 protocols × 4 scenarios × 3 modes).

| Scenario | Description | GRAF-Global PDR Gain | Cluster Recovery |
|----------|-------------|---------------------|-----------------|
| 1 — Mild | 3 CH failures, nominal energy | **+25.2 pp** | 100% |
| 2 — Moderate | 5 failures, reduced energy | **+24.1 pp** | 100% |
| 3 — Severe | 7 failures, low energy | **+24.6 pp** | ~95.7% |
| 4 — Extreme | 7 of 8 CHs fail + 30% range cut | positive, narrower | 63–76% |

- All Scenario 1–3 gains: **Holm-adjusted p < 10⁻¹⁴**, Cohen's d ≥ 6.20
- Mean Service Restoration Latency: **2.48–2.64 seconds** (Scenarios 1–3)
- CH energy overhead vs baseline: **statistically indistinguishable** (Holm p = 1.0)
- PDR gains are **identical under OLSR and AODV** — validating the access/backbone decoupling design

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

**GRAF-Local** swaps global betweenness centrality for 2-hop reachability, cutting topology state requirements while staying competitive on recovery metrics. In practice, GRAF-Local achieves higher *sensor-level* recovery rates (100.0% vs 91.5% in Scenario 1) while GRAF-Global leads on aggregate PDR.

---

## Running the Simulations

### Prerequisites

- **NS-3.39** — tested on NS-3.39 only; other versions are not guaranteed to work
- C++17-compatible compiler (GCC 9+ or Clang 10+)
- Python 3.8+ with `numpy`, `pandas`, `scipy`, `matplotlib`

### Setup

1. Copy the simulation files into your NS-3 scratch directory:
   ```bash
   cp v2_fixed_simulation/Star_mesh_simulation_code.cc  <ns3-root>/scratch/hybrid-star-mesh-sim.cc
   cp v2_fixed_simulation/graf_*.h                      <ns3-root>/scratch/
   ```

2. Build:
   ```bash
   cd <ns3-root>
   ./ns3 build
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
| `--numCHs` | integer | Number of cluster heads (default: 8) |
| `--numSensors` | integer | Number of sensor nodes (default: 80) |
| `--deathfrac` | float | Fraction of CHs to fail (overrides scenario default) |

### Run All 480 Simulations

```bash
cd <ns3-root>
bash run_all_sims_final.sh
```

This runs 3 simulations in parallel (tuned for a 4-thread CPU). Adjust `-P 3` in the script if you have more cores. Each run produces a `_summary.csv` in `sim_results_final/raw/`.

To verify everything ran cleanly:
```bash
wc -l sim_results_final/run_manifest.csv   # should be 481 (480 runs + header)
grep ',FAIL,' sim_results_final/run_manifest.csv  # should be empty
```

### Analyze Results

```bash
python3 analyze_results.py \
  --dir sim_results_final/raw \
  --out sim_results_final/analysis_v2
```

This generates all tables and plots used in the paper:
- `significance_tests.csv` — Paired t-tests, Holm correction, Cohen's d_z
- `main_results_table_core.csv` — Per-scenario summary statistics
- `plot_pdr.png`, `plot_throughput.png`, `plot_jain_fairness.png`, etc.

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
| Energy model | NS-3 WifiRadioEnergyModel |
| Traffic | UDP sensor-to-gateway, constant bit rate |
| Simulation duration | 300 s |
| Replications per cell | 20 independent seeds |

---

## Pre-Computed Results

The `v2_fixed_simulation/` folder includes pre-computed outputs from the full experiment if you want to inspect results without re-running everything:

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
- `run_extended_sweep.sh` covers the scalability experiments (Nc = 16 and 32 CHs) added during reviewer responses.
- All statistical tests use **Holm-Bonferroni correction** across the full family of comparisons.
