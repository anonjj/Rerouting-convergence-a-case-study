
/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * hybrid-star-mesh-sim-v3.cc
 *
 * VERSION 3 — Reviewer Revision (2026-04-15)
 * ===========================================
 * Changes from v2 (Star_mesh_simulation_code.cc):
 *
 * [FAULT-4] Competitive Baselines
 *   Added --baseline parameter: none | rand | energy | nearest
 *   - rand:    upon CH failure, orphaned sensors select any alive CH within
 *              access-plane range uniformly at random.
 *   - energy:  sensors select the alive CH with highest residual energy,
 *              ignoring topology/distance.
 *   - nearest: sensors select the geographically closest alive CH.
 *   New functions: SelectRandBackup(), SelectEnergyBackup(), SelectNearestBackup()
 *   Recovery dispatch in DetectAndRecoverCluster() updated accordingly.
 *
 * [FAULT-5] Fitness Function Ablation Study
 *   Added --ablation parameter: full | energy | topo | proxcov
 *   - full:     α=0.35, β=0.25, γ=0.15, δ=0.25  (existing GRAF weights)
 *   - energy:   α=1.0,  β=0,    γ=0,    δ=0      (energy-only criterion)
 *   - topo:     α=0,    β=1.0,  γ=0,    δ=0      (betweenness centrality only)
 *   - proxcov:  α=0,    β=0,    γ=0.5,  δ=0.5    (proximity + coverage only)
 *   Applies to SelectGraphAwareBackup() only (GRAF-Global fitness function).
 *   GRAF-Local not ablated (different formula; ablation is GRAF-Global only).
 *
 * [FAULT-9] Scalability Support
 *   Added --deathfrac parameter: fraction of CHs to fail (overrides scenario
 *   default). Shell script uses this for medium (Nc=16) and large (Nc=32) runs.
 *   The grid layout auto-scales via ceil(sqrt(numCHs)) already.
 *
 * [FAULT-2&10] Statistical Strengthening
 *   No C++ change needed. New shell script (run_extended_sweep.sh) uses
 *   seeds spaced by 10^6 and n=50 for Scenario 4.
 *
 * [METRIC] Energy-Per-Bit (IEEE IoT Journal convention)
 *   Corrected formula: η = E_consumed / (PDR × TotalOfferedBits)
 *   Added as "energy_per_bit_ieee_j" in _summary.csv (alongside existing
 *   energyPerDeliveredBit which uses E/rxBytes for backward compatibility).
 *
 * [FIX-F1] FATAL — Removed OLSR TcInterval override (from v2).
 * [FIX-M6] MAJOR — Fixed finalBytes overwrite in recovery bytes computation (v2).
 *
 * Compatible target: NS-3.39
 */

#include "ns3/aodv-module.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/energy-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-list-routing-helper.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/olsr-module.h"
#include "ns3/wifi-module.h"

#include "ns3/olsr-header.h"
#include "ns3/udp-header.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <vector>
using namespace ns3;

NS_LOG_COMPONENT_DEFINE("HybridStarMeshSimRewrite");

// -----------------------------------------------------------------------------
// Global experiment state
// -----------------------------------------------------------------------------
static std::vector<double> g_chDeathTimes;
static std::vector<double> g_chDeathTimeByIndex;
static std::vector<double> g_chDetectTimeByIndex;
static std::vector<bool> g_clusterRecovered;
static std::vector<bool> g_recoveryScheduled;
static std::vector<uint32_t> g_backupLoad;
static std::vector<uint32_t> g_clusterRecoveredSensorCount;

static std::map<uint32_t, bool> g_chAlive;

static EnergySourceContainer *g_chEnergyPtr = nullptr;
static NodeContainer *g_chNodesPtr = nullptr;
static NodeContainer *g_sensorNodesPtr = nullptr;
static ApplicationContainer *g_sensorSinkAppsPtr = nullptr;

static std::string g_grafMode = "off";
static Ipv4Address g_gatewayIp;
static double g_simStopTime = 300.0;
static double g_backboneMaxRange = 160.0;
static double g_accessMaxRange = 140.0;
static double g_detectionDelay = 1.5;

static uint16_t g_sensorBasePort = 9000;
static uint16_t g_warmupBasePort = 20000;

static std::vector<Vector> g_chPositions;
static std::vector<Vector> g_sensorPositions;
static std::vector<Ipv4Address> g_chAccessIps;
static std::vector<uint32_t> g_sensorAccessIfIndex;
static std::vector<std::vector<uint32_t>> g_clusterMembers;

// Scoring weights (overridden by ablation)
static double g_alpha = 0.35; // remaining energy
static double g_beta = 0.25;  // global centrality / local 2-hop reach
static double g_gamma = 0.15; // backbone proximity
static double g_delta = 0.25; // access coverage of failed cluster
static uint32_t g_hMax = 3;   // max backup hop distance

// [FAULT-4] Competitive baseline mode
// "none"    = GRAF or no-recovery (g_grafMode controls)
// "rand"    = random surviving CH within access range
// "energy"  = highest residual energy CH within access range
// "nearest" = geographically closest CH within access range
static std::string g_baselineMode = "none";

// [FAULT-5] Fitness function ablation condition
// "full"    = standard GRAF weights (α=0.35, β=0.25, γ=0.15, δ=0.25)
// "energy"  = energy-only (α=1, β=γ=δ=0)
// "topo"    = topology/BC-only (β=1, α=γ=δ=0)
// "proxcov" = proximity+coverage only (γ=0.5, δ=0.5, α=β=0)
static std::string g_ablation = "full";

// Recovery enabled when grafMode != "off" OR baselineMode != "none"
static bool g_recoveryEnabled = false;

struct ClusterSnapshot {
  double time;
  uint32_t clusterIdx;
  uint64_t cumulativeBytes;
};
static std::vector<ClusterSnapshot> g_clusterSnapshots;

// -----------------------------------------------------------------------------
// Utility helpers
// -----------------------------------------------------------------------------
bool IsChAliveByIndex(uint32_t idx) {
  if (!g_chNodesPtr || idx >= g_chNodesPtr->GetN()) {
    return false;
  }

  uint32_t nodeId = g_chNodesPtr->Get(idx)->GetId();
  auto it = g_chAlive.find(nodeId);
  return (it != g_chAlive.end()) && it->second;
}

void DisableNodeRadios(Ptr<Node> node) {
  for (uint32_t i = 0; i < node->GetNDevices(); ++i) {
    Ptr<WifiNetDevice> wifiDev = DynamicCast<WifiNetDevice>(node->GetDevice(i));
    if (wifiDev && wifiDev->GetPhy()) {
      wifiDev->GetPhy()->SetOffMode();
    }
  }
}

std::vector<std::vector<uint32_t>> BuildBackboneAdjacency(bool aliveOnly) {
  uint32_t n = g_chPositions.size();
  std::vector<std::vector<uint32_t>> adj(n);

  for (uint32_t i = 0; i < n; ++i) {
    if (aliveOnly && !IsChAliveByIndex(i)) {
      continue;
    }

    for (uint32_t j = i + 1; j < n; ++j) {
      if (aliveOnly && !IsChAliveByIndex(j)) {
        continue;
      }

      double d = CalculateDistance(g_chPositions[i], g_chPositions[j]);
      if (d <= g_backboneMaxRange) {
        adj[i].push_back(j);
        adj[j].push_back(i);
      }
    }
  }
  return adj;
}

uint32_t BfsHopDistance(const std::vector<std::vector<uint32_t>> &adj,
                        uint32_t src, uint32_t dst) {
  if (src == dst) {
    return 0;
  }

  const uint32_t INF = std::numeric_limits<uint32_t>::max();
  std::vector<uint32_t> dist(adj.size(), INF);
  std::queue<uint32_t> q;

  dist[src] = 0;
  q.push(src);

  while (!q.empty()) {
    uint32_t u = q.front();
    q.pop();

    for (uint32_t v : adj[u]) {
      if (dist[v] == INF) {
        dist[v] = dist[u] + 1;
        if (v == dst) {
          return dist[v];
        }
        q.push(v);
      }
    }
  }
  return INF;
}

std::vector<double>
ComputeBetweennessCentrality(const std::vector<std::vector<uint32_t>> &adj) {
  uint32_t n = adj.size();
  std::vector<double> bc(n, 0.0);

  for (uint32_t s = 0; s < n; ++s) {
    if (!IsChAliveByIndex(s)) {
      continue;
    }

    std::vector<std::vector<uint32_t>> pred(n);
    std::vector<int> dist(n, -1);
    std::vector<double> sigma(n, 0.0);
    std::vector<uint32_t> stack;
    std::queue<uint32_t> q;

    dist[s] = 0;
    sigma[s] = 1.0;
    q.push(s);

    while (!q.empty()) {
      uint32_t v = q.front();
      q.pop();
      stack.push_back(v);

      for (uint32_t w : adj[v]) {
        if (dist[w] < 0) {
          dist[w] = dist[v] + 1;
          q.push(w);
        }
        if (dist[w] == dist[v] + 1) {
          sigma[w] += sigma[v];
          pred[w].push_back(v);
        }
      }
    }

    std::vector<double> delta(n, 0.0);
    while (!stack.empty()) {
      uint32_t w = stack.back();
      stack.pop_back();

      for (uint32_t v : pred[w]) {
        if (sigma[w] > 0.0) {
          delta[v] += (sigma[v] / sigma[w]) * (1.0 + delta[w]);
        }
      }
      if (w != s) {
        bc[w] += delta[w];
      }
    }
  }

  return bc;
}

uint32_t CountAliveTwoHopReach(const std::vector<std::vector<uint32_t>> &adj,
                               uint32_t idx) {
  std::set<uint32_t> reach;
  for (uint32_t n1 : adj[idx]) {
    if (!IsChAliveByIndex(n1)) {
      continue;
    }
    reach.insert(n1);

    for (uint32_t n2 : adj[n1]) {
      if (!IsChAliveByIndex(n2) || n2 == idx) {
        continue;
      }
      reach.insert(n2);
    }
  }
  return static_cast<uint32_t>(reach.size());
}

bool SensorCanReachBackup(uint32_t sensorIdx, uint32_t backupChIdx) {
  if (sensorIdx >= g_sensorPositions.size() ||
      backupChIdx >= g_chPositions.size()) {
    return false;
  }
  return CalculateDistance(g_sensorPositions[sensorIdx],
                           g_chPositions[backupChIdx]) <= g_accessMaxRange;
}

double CoverageFraction(uint32_t failedChIdx, uint32_t backupChIdx) {
  if (failedChIdx >= g_clusterMembers.size()) {
    return 0.0;
  }

  uint32_t total = 0;
  uint32_t covered = 0;
  for (uint32_t sIdx : g_clusterMembers[failedChIdx]) {
    total++;
    if (SensorCanReachBackup(sIdx, backupChIdx)) {
      covered++;
    }
  }
  return (total > 0) ? static_cast<double>(covered) / total : 0.0;
}

uint64_t GetClusterTotalRxBytes(uint32_t clusterIdx) {
  if (!g_sensorSinkAppsPtr || clusterIdx >= g_clusterMembers.size()) {
    return 0;
  }

  uint64_t total = 0;
  for (uint32_t sIdx : g_clusterMembers[clusterIdx]) {
    Ptr<PacketSink> sink =
        DynamicCast<PacketSink>(g_sensorSinkAppsPtr->Get(sIdx));
    if (sink) {
      total += sink->GetTotalRx();
    }
  }
  return total;
}

// -----------------------------------------------------------------------------
// Backup selection — GRAF-Global (with ablation support)
// -----------------------------------------------------------------------------
uint32_t SelectGraphAwareBackup(uint32_t failedChIndex) {
  if (!g_chNodesPtr || !g_chEnergyPtr || g_chPositions.empty()) {
    return std::numeric_limits<uint32_t>::max();
  }

  auto staticAdj = BuildBackboneAdjacency(false);
  auto aliveAdj = BuildBackboneAdjacency(true);
  auto bc = ComputeBetweennessCentrality(aliveAdj);

  // [FAULT-5] Apply ablation overrides to local weight copies
  double aAlpha = g_alpha;
  double aBeta  = g_beta;
  double aGamma = g_gamma;
  double aDelta = g_delta;
  if (g_ablation == "energy") {
    aAlpha = 1.0; aBeta = 0.0; aGamma = 0.0; aDelta = 0.0;
    NS_LOG_UNCOND("  [ABLATION] energy-only weights active");
  } else if (g_ablation == "topo") {
    aAlpha = 0.0; aBeta = 1.0; aGamma = 0.0; aDelta = 0.0;
    NS_LOG_UNCOND("  [ABLATION] topology(BC)-only weights active");
  } else if (g_ablation == "proxcov") {
    aAlpha = 0.0; aBeta = 0.0; aGamma = 0.5; aDelta = 0.5;
    NS_LOG_UNCOND("  [ABLATION] proximity+coverage-only weights active");
  }
  // "full" keeps defaults above

  struct Candidate {
    uint32_t idx;
    double energy;
    double bc;
    uint32_t hops;
    double coverage;
  };

  std::vector<Candidate> candidates;
  double maxEnergy = 0.0;
  double maxBc = 0.0;
  uint32_t maxHop = 1;

  for (uint32_t j = 0; j < g_chNodesPtr->GetN(); ++j) {
    if (j == failedChIndex || !IsChAliveByIndex(j)) {
      continue;
    }

    Ptr<BasicEnergySource> source =
        DynamicCast<BasicEnergySource>(g_chEnergyPtr->Get(j));
    double rem = source ? source->GetRemainingEnergy() : 0.0;
    if (rem <= 0.1) {
      continue;
    }

    uint32_t hops = BfsHopDistance(staticAdj, failedChIndex, j);
    if (hops == std::numeric_limits<uint32_t>::max() || hops > g_hMax) {
      continue;
    }

    double coverage = CoverageFraction(failedChIndex, j);
    if (coverage <= 0.0) {
      continue;
    }

    candidates.push_back({j, rem, bc[j], hops, coverage});
    maxEnergy = std::max(maxEnergy, rem);
    maxBc = std::max(maxBc, bc[j]);
    maxHop = std::max(maxHop, hops);
  }

  if (candidates.empty()) {
    return std::numeric_limits<uint32_t>::max();
  }

  double bestScore = -1e9;
  uint32_t bestIdx = std::numeric_limits<uint32_t>::max();

  NS_LOG_UNCOND("  [GRAF-GLOBAL] Candidates for failed CH index "
                << failedChIndex);
  for (const auto &c : candidates) {
    double eNorm = (maxEnergy > 0.0) ? (c.energy / maxEnergy) : 0.0;
    double bcNorm = (maxBc > 0.0) ? (c.bc / maxBc) : 0.0;
    double hopPenalty =
        (maxHop > 0) ? static_cast<double>(c.hops) / maxHop : 0.0;
    double loadPenalty = 0.15 * g_backupLoad[c.idx];

    // Use ablation-adjusted weights (aAlpha/aBeta/aGamma/aDelta)
    double score = aAlpha * eNorm + aBeta * bcNorm +
                   aGamma * (1.0 - hopPenalty) + aDelta * c.coverage -
                   loadPenalty;

    NS_LOG_UNCOND("    idx=" << c.idx << " energy=" << c.energy
                             << " bc=" << c.bc << " hops=" << c.hops
                             << " coverage=" << c.coverage << " load="
                             << g_backupLoad[c.idx] << " score=" << score);

    if (score > bestScore) {
      bestScore = score;
      bestIdx = c.idx;
    }
  }

  return bestIdx;
}

// -----------------------------------------------------------------------------
// Backup selection — GRAF-Local
// -----------------------------------------------------------------------------
uint32_t SelectLocalBackup(uint32_t failedChIndex) {
  if (!g_chNodesPtr || !g_chEnergyPtr || g_chPositions.empty()) {
    return std::numeric_limits<uint32_t>::max();
  }

  auto adj = BuildBackboneAdjacency(false);
  auto aliveAdj = BuildBackboneAdjacency(true);

  struct Candidate {
    uint32_t idx;
    double energy;
    uint32_t reach2;
    double distance;
    double coverage;
    uint32_t load;
  };

  std::vector<Candidate> candidates;
  double maxEnergy = 0.0;
  uint32_t maxReach2 = 1;
  double maxDist = 1.0;

  if (failedChIndex >= adj.size()) {
    return std::numeric_limits<uint32_t>::max();
  }

  for (uint32_t j : adj[failedChIndex]) {
    if (!IsChAliveByIndex(j)) {
      continue;
    }

    Ptr<BasicEnergySource> source =
        DynamicCast<BasicEnergySource>(g_chEnergyPtr->Get(j));
    double rem = source ? source->GetRemainingEnergy() : 0.0;
    if (rem <= 0.1) {
      continue;
    }

    double coverage = CoverageFraction(failedChIndex, j);
    if (coverage <= 0.0) {
      continue;
    }

    uint32_t reach2 = CountAliveTwoHopReach(aliveAdj, j);
    double dist =
        CalculateDistance(g_chPositions[failedChIndex], g_chPositions[j]);

    candidates.push_back({j, rem, reach2, dist, coverage, g_backupLoad[j]});
    maxEnergy = std::max(maxEnergy, rem);
    maxReach2 = std::max(maxReach2, reach2);
    maxDist = std::max(maxDist, dist);
  }

  if (candidates.empty()) {
    return std::numeric_limits<uint32_t>::max();
  }

  double bestScore = -1e9;
  uint32_t bestIdx = std::numeric_limits<uint32_t>::max();

  NS_LOG_UNCOND("  [GRAF-LOCAL] Candidates for failed CH index "
                << failedChIndex);
  for (const auto &c : candidates) {
    double eNorm = (maxEnergy > 0.0) ? (c.energy / maxEnergy) : 0.0;
    double reachNorm =
        (maxReach2 > 0) ? static_cast<double>(c.reach2) / maxReach2 : 0.0;
    double distNorm = (maxDist > 0.0) ? (c.distance / maxDist) : 0.0;
    double loadPenalty = 0.20 * c.load;

    double score = 0.35 * eNorm + 0.25 * reachNorm + 0.20 * (1.0 - distNorm) +
                   0.20 * c.coverage - loadPenalty;

    NS_LOG_UNCOND("    idx=" << c.idx << " energy=" << c.energy
                             << " reach2=" << c.reach2 << " dist=" << c.distance
                             << " coverage=" << c.coverage << " load=" << c.load
                             << " score=" << score);

    if (score > bestScore) {
      bestScore = score;
      bestIdx = c.idx;
    }
  }

  return bestIdx;
}

// -----------------------------------------------------------------------------
// [FAULT-4] Competitive Baseline Selectors
// Each function finds alive CHs reachable from orphaned sensors on the
// access-plane and applies its selection criterion.
// -----------------------------------------------------------------------------

// RAND-Recovery: randomly pick any surviving CH within access range of at
// least one orphaned sensor from failedChIndex's cluster.
uint32_t SelectRandBackup(uint32_t failedChIndex) {
  if (!g_chNodesPtr || !g_chEnergyPtr || g_chPositions.empty()) {
    return std::numeric_limits<uint32_t>::max();
  }

  std::vector<uint32_t> eligible;
  uint32_t n = static_cast<uint32_t>(g_chNodesPtr->GetN());
  for (uint32_t j = 0; j < n; ++j) {
    if (j == failedChIndex || !IsChAliveByIndex(j)) {
      continue;
    }
    Ptr<BasicEnergySource> src =
        DynamicCast<BasicEnergySource>(g_chEnergyPtr->Get(j));
    if (!src || src->GetRemainingEnergy() <= 0.1) {
      continue;
    }
    // Must reach at least one sensor in the failed cluster
    double cov = CoverageFraction(failedChIndex, j);
    if (cov <= 0.0) {
      continue;
    }
    eligible.push_back(j);
  }

  if (eligible.empty()) {
    return std::numeric_limits<uint32_t>::max();
  }

  // Uniform random selection
  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();
  rng->SetAttribute("Min", DoubleValue(0.0));
  rng->SetAttribute("Max", DoubleValue(static_cast<double>(eligible.size())));
  uint32_t pick = static_cast<uint32_t>(rng->GetValue()) % eligible.size();

  NS_LOG_UNCOND("  [RAND-RECOVERY] failedCH=" << failedChIndex
                << " selected=" << eligible[pick]
                << " from " << eligible.size() << " eligible CHs");
  return eligible[pick];
}

// ENERGY-Recovery: pick the alive CH with highest residual energy that has
// access-range coverage of at least one orphaned sensor.
uint32_t SelectEnergyBackup(uint32_t failedChIndex) {
  if (!g_chNodesPtr || !g_chEnergyPtr || g_chPositions.empty()) {
    return std::numeric_limits<uint32_t>::max();
  }

  double bestEnergy = -1.0;
  uint32_t bestIdx = std::numeric_limits<uint32_t>::max();
  uint32_t n = static_cast<uint32_t>(g_chNodesPtr->GetN());
  for (uint32_t j = 0; j < n; ++j) {
    if (j == failedChIndex || !IsChAliveByIndex(j)) {
      continue;
    }
    Ptr<BasicEnergySource> src =
        DynamicCast<BasicEnergySource>(g_chEnergyPtr->Get(j));
    if (!src) {
      continue;
    }
    double rem = src->GetRemainingEnergy();
    if (rem <= 0.1) {
      continue;
    }
    double cov = CoverageFraction(failedChIndex, j);
    if (cov <= 0.0) {
      continue;
    }
    if (rem > bestEnergy) {
      bestEnergy = rem;
      bestIdx = j;
    }
  }

  if (bestIdx != std::numeric_limits<uint32_t>::max()) {
    NS_LOG_UNCOND("  [ENERGY-RECOVERY] failedCH=" << failedChIndex
                  << " selected=" << bestIdx
                  << " energy=" << bestEnergy << " J");
  }
  return bestIdx;
}

// NEAREST-Recovery: pick the alive CH that is geographically closest to the
// failed CH's centroid and has access-range coverage of at least one sensor.
uint32_t SelectNearestBackup(uint32_t failedChIndex) {
  if (!g_chNodesPtr || !g_chEnergyPtr || g_chPositions.empty() ||
      failedChIndex >= g_chPositions.size()) {
    return std::numeric_limits<uint32_t>::max();
  }

  double bestDist = std::numeric_limits<double>::max();
  uint32_t bestIdx = std::numeric_limits<uint32_t>::max();
  uint32_t n = static_cast<uint32_t>(g_chNodesPtr->GetN());
  for (uint32_t j = 0; j < n; ++j) {
    if (j == failedChIndex || !IsChAliveByIndex(j)) {
      continue;
    }
    Ptr<BasicEnergySource> src =
        DynamicCast<BasicEnergySource>(g_chEnergyPtr->Get(j));
    if (!src || src->GetRemainingEnergy() <= 0.1) {
      continue;
    }
    double cov = CoverageFraction(failedChIndex, j);
    if (cov <= 0.0) {
      continue;
    }
    double d = CalculateDistance(g_chPositions[failedChIndex], g_chPositions[j]);
    if (d < bestDist) {
      bestDist = d;
      bestIdx = j;
    }
  }

  if (bestIdx != std::numeric_limits<uint32_t>::max()) {
    NS_LOG_UNCOND("  [NEAREST-RECOVERY] failedCH=" << failedChIndex
                  << " selected=" << bestIdx
                  << " dist=" << bestDist << " m");
  }
  return bestIdx;
}

// -----------------------------------------------------------------------------
// Recovery logic
// -----------------------------------------------------------------------------
void RehomeClusterSensors(uint32_t failedChIndex, uint32_t backupIdx) {
  if (!g_sensorNodesPtr || !g_chNodesPtr) {
    return;
  }
  if (failedChIndex >= g_clusterMembers.size() ||
      backupIdx >= g_chAccessIps.size()) {
    return;
  }

  uint32_t moved = 0;
  for (uint32_t sIdx : g_clusterMembers[failedChIndex]) {
    if (sIdx >= g_sensorAccessIfIndex.size()) {
      continue;
    }

    if (!SensorCanReachBackup(sIdx, backupIdx)) {
      continue;
    }

    Ptr<Node> sensor = g_sensorNodesPtr->Get(sIdx);
    Ptr<Ipv4> ipv4 = sensor->GetObject<Ipv4>();
    Ipv4StaticRoutingHelper helper;
    Ptr<Ipv4StaticRouting> sRoute = helper.GetStaticRouting(ipv4);
    sRoute->SetDefaultRoute(g_chAccessIps[backupIdx],
                            g_sensorAccessIfIndex[sIdx]);
    moved++;
  }

  g_clusterRecovered[failedChIndex] = (moved > 0);
  g_clusterRecoveredSensorCount[failedChIndex] = moved;
  g_backupLoad[backupIdx]++;

  NS_LOG_UNCOND("  [REHOME] failedCH="
                << failedChIndex << " backupCH=" << backupIdx
                << " movedSensors=" << moved << "/"
                << g_clusterMembers[failedChIndex].size()
                << " newNextHop=" << g_chAccessIps[backupIdx]);
}

void DetectAndRecoverCluster(uint32_t failedChIndex, double detectTime) {
  // [FAULT-4] Guard now uses g_recoveryEnabled (true for GRAF or any baseline)
  if (!g_recoveryEnabled) {
    return;
  }
  if (failedChIndex >= g_clusterRecovered.size() ||
      g_clusterRecovered[failedChIndex]) {
    return;
  }

  g_chDetectTimeByIndex[failedChIndex] = detectTime;

  // [FAULT-4] Dispatch to appropriate selector based on mode
  uint32_t backupIdx;
  if (g_baselineMode == "rand") {
    backupIdx = SelectRandBackup(failedChIndex);
  } else if (g_baselineMode == "energy") {
    backupIdx = SelectEnergyBackup(failedChIndex);
  } else if (g_baselineMode == "nearest") {
    backupIdx = SelectNearestBackup(failedChIndex);
  } else if (g_grafMode == "global") {
    backupIdx = SelectGraphAwareBackup(failedChIndex);
  } else {
    backupIdx = SelectLocalBackup(failedChIndex);
  }

  if (backupIdx == std::numeric_limits<uint32_t>::max()) {
    NS_LOG_UNCOND("  [RECOVERY] No feasible backup for failed CH index "
                  << failedChIndex << " at t=" << detectTime << "s");
    return;
  }

  if (!IsChAliveByIndex(backupIdx)) {
    NS_LOG_UNCOND("  [RECOVERY] Selected backup CH index " << backupIdx
                                                       << " is already dead");
    return;
  }

  RehomeClusterSensors(failedChIndex, backupIdx);

  // Post-promotion successor selection (only for GRAF-Global full)
  if (g_grafMode == "global" && g_baselineMode == "none") {
    uint32_t successorIdx = SelectGraphAwareBackup(backupIdx);
    if (successorIdx != std::numeric_limits<uint32_t>::max()) {
      NS_LOG_UNCOND("  [GRAF] Post-promotion successor for backup idx "
                    << backupIdx << " -> successor idx " << successorIdx);
    } else {
      NS_LOG_UNCOND("  [GRAF] No successor found for promoted CH idx "
                    << backupIdx << " (network may be nearly depleted)");
    }
  }
}

void HandleChDeath(uint32_t chIndex, double deathTime,
                   const std::string &reason) {
  if (!g_chNodesPtr || chIndex >= g_chNodesPtr->GetN()) {
    return;
  }

  Ptr<Node> node = g_chNodesPtr->Get(chIndex);
  uint32_t nodeId = node->GetId();

  if (!g_chAlive[nodeId]) {
    return;
  }

  g_chAlive[nodeId] = false;
  g_chDeathTimes.push_back(deathTime);
  if (chIndex < g_chDeathTimeByIndex.size()) {
    g_chDeathTimeByIndex[chIndex] = deathTime;
  }

  DisableNodeRadios(node);

  NS_LOG_UNCOND("  [CH DEATH] Node "
                << nodeId << " idx=" << chIndex << " at t=" << deathTime
                << "s reason=" << reason << " (" << g_chDeathTimes.size()
                << " CHs dead)");

  // [FAULT-4] Use g_recoveryEnabled (covers both GRAF and all baselines)
  if (g_recoveryEnabled && chIndex < g_recoveryScheduled.size() &&
      !g_recoveryScheduled[chIndex]) {
    g_recoveryScheduled[chIndex] = true;
    double detectTime = deathTime + g_detectionDelay;
    Simulator::Schedule(Seconds(g_detectionDelay), &DetectAndRecoverCluster,
                        chIndex, detectTime);
    NS_LOG_UNCOND("  [RECOVERY] Scheduled for failed CH idx="
                  << chIndex << " at detection time t=" << detectTime << "s");
  }
}

void KillCH(uint32_t chIndex, double deathTime) {
  HandleChDeath(chIndex, deathTime, "forced");
}

void CheckChEnergy(double interval, double endTime) {
  double now = Simulator::Now().GetSeconds();
  if (now >= endTime || !g_chEnergyPtr || !g_chNodesPtr) {
    return;
  }

  for (uint32_t c = 0; c < g_chNodesPtr->GetN(); ++c) {
    Ptr<BasicEnergySource> source =
        DynamicCast<BasicEnergySource>(g_chEnergyPtr->Get(c));
    if (!source) {
      continue;
    }

    uint32_t nodeId = g_chNodesPtr->Get(c)->GetId();
    if (g_chAlive[nodeId] && source->GetRemainingEnergy() <= 0.0) {
      HandleChDeath(c, now, "energy");
    }
  }

  Simulator::Schedule(Seconds(interval), &CheckChEnergy, interval, endTime);
}

// -----------------------------------------------------------------------------
// Logging
// -----------------------------------------------------------------------------
void LogPeriodicMetrics(std::ofstream *logFile, double interval,
                        double endTime) {
  double now = Simulator::Now().GetSeconds();
  if (now >= endTime) {
    return;
  }

  uint32_t chAliveCount = 0;
  for (const auto &pair : g_chAlive) {
    if (pair.second) {
      chAliveCount++;
    }
  }

  *logFile << now << "," << chAliveCount << "," << g_chDeathTimes.size()
           << "\n";

  Simulator::Schedule(Seconds(interval), &LogPeriodicMetrics, logFile, interval,
                      endTime);
}

static std::string g_clusterLogMode;
static std::string g_clusterLogProto;
static uint32_t g_clusterLogScenario = 0;
static uint32_t g_clusterLogSeed = 0;
static uint32_t g_clusterLogRun = 0;

void LogClusterBytes(std::ofstream *clusterLogFile, double interval,
                     double endTime) {
  double now = Simulator::Now().GetSeconds();
  if (now >= endTime || !g_sensorSinkAppsPtr) {
    return;
  }

  for (uint32_t c = 0; c < g_clusterMembers.size(); ++c) {
    uint64_t bytes = GetClusterTotalRxBytes(c);
    *clusterLogFile << now << "," << c << "," << bytes << ","
                    << g_clusterLogMode << "," << g_clusterLogProto << ","
                    << g_clusterLogScenario << "," << g_clusterLogSeed << ","
                    << g_clusterLogRun << "\n";
    g_clusterSnapshots.push_back({now, c, bytes});
  }

  Simulator::Schedule(Seconds(interval), &LogClusterBytes, clusterLogFile,
                      interval, endTime);
}

// ── Topology Stress Index (TSI) ──────────────────────────────────────────
static uint32_t g_numCHsInitial = 0;
static double g_tsiW1 = 0.40;
static double g_tsiW2 = 0.35;
static double g_tsiW3 = 0.25;
static double g_tsiBaseline = 2.0;
static std::vector<double> g_recentReconvTimes;

double ComputeTSI() {
  double chLossRatio = (g_numCHsInitial > 0)
                           ? (double)g_chDeathTimes.size() / g_numCHsInitial
                           : 0.0;

  auto aliveAdj = BuildBackboneAdjacency(true);
  uint32_t n = aliveAdj.size();
  uint32_t aliveCount = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (IsChAliveByIndex(i))
      aliveCount++;

  uint32_t largestCC = 0;
  if (aliveCount > 0) {
    std::vector<bool> visited(n, false);
    for (uint32_t start = 0; start < n; ++start) {
      if (!IsChAliveByIndex(start) || visited[start])
        continue;
      std::queue<uint32_t> q;
      q.push(start);
      visited[start] = true;
      uint32_t compSize = 0;
      while (!q.empty()) {
        uint32_t u = q.front();
        q.pop();
        compSize++;
        for (uint32_t v : aliveAdj[u])
          if (!visited[v]) {
            visited[v] = true;
            q.push(v);
          }
      }
      largestCC = std::max(largestCC, compSize);
    }
  }
  double partitionIndicator =
      (aliveCount > 0) ? 1.0 - (double)largestCC / aliveCount : 1.0;

  double reconvStress = 0.0;
  if (!g_recentReconvTimes.empty()) {
    double avg = std::accumulate(g_recentReconvTimes.begin(),
                                 g_recentReconvTimes.end(), 0.0) /
                 g_recentReconvTimes.size();
    reconvStress = std::min(avg / g_tsiBaseline, 1.0);
  }

  return std::min(g_tsiW1 * chLossRatio + g_tsiW2 * partitionIndicator +
                      g_tsiW3 * reconvStress,
                  1.0);
}

std::string TSIRegion(double tsi) {
  if (tsi < 0.3)
    return "GREEN";
  if (tsi < 0.6)
    return "YELLOW";
  return "RED";
}

void LogTSI(std::ofstream *tsiFile, double interval, double endTime) {
  double now = Simulator::Now().GetSeconds();
  if (now >= endTime)
    return;
  double tsi = ComputeTSI();
  std::string region = TSIRegion(tsi);
  uint32_t alive = 0;
  for (auto &p : g_chAlive)
    if (p.second)
      alive++;
  *tsiFile << now << "," << tsi << "," << region << "," << alive << ","
           << g_chDeathTimes.size() << "\n";
  Simulator::Schedule(Seconds(interval), &LogTSI, tsiFile, interval, endTime);
}

// ── Passive Heartbeat Detection ──────────────────────────────────────────
// Timing Semantics:
// The controller polls nodes asynchronously relative to their failure times.
// Polls occur on fixed absolute intervals (e.g., t=0.5, 1.0, 1.5...).
// If a node dies at t=1.2, it misses its first poll at t=1.5 (0.3s later).
// The first missed poll happens (next_poll - death_time) seconds after death.
// Total latency = (next_poll - death_time) + (k - 1) * T_hb.
// Knowing (next_poll - death_time) is uniformly distributed on (0, T_hb],
// expected latency is T_hb / 2 + (k - 1) * T_hb = (k - 0.5) * T_hb.
// For T_hb = 0.5s, k = 3, the expected latency is 1.25s, matching experimental
// averages.
static double g_hbInterval = 0.5;
static uint32_t g_hbMissThreshold = 3;
static std::vector<uint32_t> g_hbMissCount;
static std::vector<double> g_hbDetectionTime;
static std::vector<double> g_hbDetectionLatency;

void HeartbeatCheck(double endTime) {
  double now = Simulator::Now().GetSeconds();
  if (now >= endTime || !g_chEnergyPtr)
    return;

  for (uint32_t c = 0; c < g_chNodesPtr->GetN(); ++c) {
    if (g_hbDetectionTime[c] >= 0.0)
      continue;
    Ptr<BasicEnergySource> src =
        DynamicCast<BasicEnergySource>(g_chEnergyPtr->Get(c));
    bool depleted = (!src || src->GetRemainingEnergy() <= 0.0);
    uint32_t nodeId = g_chNodesPtr->Get(c)->GetId();
    bool killed = (g_chAlive.count(nodeId) && !g_chAlive[nodeId]);

    if (depleted || killed) {
      g_hbMissCount[c]++;
      if (g_hbMissCount[c] >= g_hbMissThreshold) {
        g_hbDetectionTime[c] = now;
        double deathTime =
            (c < g_chDeathTimeByIndex.size()) ? g_chDeathTimeByIndex[c] : -1.0;
        double latency = (deathTime >= 0.0) ? now - deathTime : -1.0;
        g_hbDetectionLatency.push_back(latency);
        NS_LOG_UNCOND("  [HB-DETECT] CH idx="
                      << c << " detected dead at t=" << now << "s"
                      << " detection_latency=" << latency << "s");
      }
    } else {
      g_hbMissCount[c] = 0;
    }
  }
  Simulator::Schedule(Seconds(g_hbInterval), &HeartbeatCheck, endTime);
}

// ── Routing Overhead Tracking ──────────────────────────────────────────────
static uint64_t g_routingOverheadBytes = 0;
static uint64_t g_routingOverheadPackets = 0;

// AODV and OLSR packet size tracking for routing overhead.
// These exact signatures suppress unused parameter warnings.
void AodvIpTxTrace(const Ipv4Header &header, Ptr<const Packet> packet,
                   uint32_t interface) {
  // AODV control uses UDP dst port 654
  // Check by peeking UDP header
  UdpHeader udpHdr;
  Ptr<Packet> copy = packet->Copy();
  if (copy->PeekHeader(udpHdr)) {
    if (udpHdr.GetDestinationPort() == 654) {
      g_routingOverheadPackets++;
      g_routingOverheadBytes += packet->GetSize() + 20; // IP header
    }
  }
}

void OlsrTxTrace(const ns3::olsr::PacketHeader &header,
                 const ns3::olsr::MessageList &messages) {
  g_routingOverheadPackets++;
  g_routingOverheadBytes += header.GetSerializedSize() + 28;
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
int main(int argc, char *argv[]) {
  std::string protocol = "OLSR";
  uint32_t numSensors = 80;
  uint32_t numCHs = 8;
  uint32_t failScenario = 1;
  double simTime = 300.0;
  double areaSize = 400.0;
  double chInitialEnergy = 10.0;
  double sensorInitialEnergy = 20.0;
  double chDrainMultiplier = 4.0;
  uint32_t runSeed = 12345;
  uint32_t runNumber = 1;
  std::string grafMode = "off";
  std::string outputPrefix = "results";
  // [FAULT-4] Competitive baseline selection
  std::string baselineMode = "none";
  // [FAULT-5] Fitness function ablation condition
  std::string ablation = "full";
  // [FAULT-9] Death fraction override for scalability runs (-1 = use default)
  double deathFrac = -1.0;

  CommandLine cmd;
  cmd.AddValue("protocol", "Routing protocol: OLSR or AODV", protocol);
  cmd.AddValue("sensors", "Number of sensor nodes", numSensors);
  cmd.AddValue("chs", "Number of cluster heads", numCHs);
  cmd.AddValue("scenario",
               "Failure scenario: 1=mild, 2=moderate, 3=severe, 4=extreme",
               failScenario);
  cmd.AddValue("simTime", "Simulation time in seconds", simTime);
  cmd.AddValue("area", "Deployment area side length (meters)", areaSize);
  cmd.AddValue("chEnergy", "Initial CH energy (Joules)", chInitialEnergy);
  cmd.AddValue("sensorEnergy", "Initial sensor energy (Joules)",
               sensorInitialEnergy);
  cmd.AddValue("drainMult", "CH energy drain multiplier", chDrainMultiplier);
  cmd.AddValue("seed", "Fixed RNG seed", runSeed);
  cmd.AddValue("run", "RNG run number", runNumber);
  cmd.AddValue("graf", "GRAF mode: off, local, or global", grafMode);
  cmd.AddValue("output", "Output file prefix", outputPrefix);
  // [FAULT-4] --baseline: competitive recovery baseline
  cmd.AddValue("baseline",
               "Competitive baseline: none | rand | energy | nearest",
               baselineMode);
  // [FAULT-5] --ablation: fitness function ablation
  cmd.AddValue("ablation",
               "Fitness ablation (applies to graf=global only): "
               "full | energy | topo | proxcov",
               ablation);
  // [FAULT-9] --deathfrac: fraction of CHs to fail (overrides scenario default)
  cmd.AddValue("deathfrac",
               "Fraction of CHs to kill [0.0-1.0], -1=use scenario default",
               deathFrac);
  cmd.Parse(argc, argv);

  // Normalize grafMode aliases
  if (grafMode == "false" || grafMode == "no" || grafMode == "none" ||
      grafMode == "0") {
    grafMode = "off";
  } else if (grafMode == "true" || grafMode == "1") {
    grafMode = "global";
  }
  if (grafMode != "off" && grafMode != "local" && grafMode != "global") {
    NS_LOG_UNCOND("ERROR: Unknown --graf value '"
                  << grafMode << "'. Use off, local, or global.");
    return 1;
  }

  // [FAULT-4] Validate --baseline
  if (baselineMode != "none" && baselineMode != "rand" &&
      baselineMode != "energy" && baselineMode != "nearest") {
    NS_LOG_UNCOND("ERROR: Unknown --baseline value '"
                  << baselineMode
                  << "'. Use none, rand, energy, or nearest.");
    return 1;
  }

  // [FAULT-5] Validate --ablation
  if (ablation != "full" && ablation != "energy" && ablation != "topo" &&
      ablation != "proxcov") {
    NS_LOG_UNCOND("ERROR: Unknown --ablation value '"
                  << ablation
                  << "'. Use full, energy, topo, or proxcov.");
    return 1;
  }

  // If a competitive baseline is active, grafMode must be "off" (baselines
  // don't use the GRAF fitness function; they have their own selector).
  if (baselineMode != "none" && grafMode != "off") {
    NS_LOG_UNCOND("WARNING: --baseline=" << baselineMode
                  << " overrides --graf=" << grafMode
                  << ". Setting grafMode to off for this run.");
    grafMode = "off";
  }

  // Set globals
  g_baselineMode = baselineMode;
  g_ablation     = ablation;
  g_recoveryEnabled = (grafMode != "off" || baselineMode != "none");

  RngSeedManager::SetSeed(runSeed);
  RngSeedManager::SetRun(runNumber);

  double scenarioEnergyFactor = 1.0;
  uint32_t expectedDeaths = 3;
  switch (failScenario) {
  case 1:
    scenarioEnergyFactor = 1.0;
    expectedDeaths = 3;
    break;
  case 2:
    scenarioEnergyFactor = 0.7;
    expectedDeaths = 5;
    break;
  case 3:
    scenarioEnergyFactor = 0.45;
    expectedDeaths = 7;
    break;
  case 4:
    scenarioEnergyFactor = 0.45; // Same severe energy drain
    g_accessMaxRange *= 0.7;     // Reduced radio ranges (harder topology)
    g_backboneMaxRange *= 0.7;
    expectedDeaths = 7;
    break;
  default:
    NS_LOG_UNCOND("ERROR: scenario must be 1, 2, 3, or 4");
    return 1;
  }

  // [FAULT-9] Override expectedDeaths for scalability runs
  if (deathFrac > 0.0) {
    expectedDeaths = static_cast<uint32_t>(deathFrac * numCHs);
    if (expectedDeaths > numCHs) {
      expectedDeaths = numCHs;
    }
    if (expectedDeaths == 0) {
      expectedDeaths = 1;
    }
    NS_LOG_UNCOND("  [SCALABILITY] deathFrac=" << deathFrac
                  << " -> expectedDeaths=" << expectedDeaths
                  << "/" << numCHs);
  }

  double adjustedChEnergy = chInitialEnergy * scenarioEnergyFactor;

  g_grafMode = grafMode;
  g_simStopTime = simTime;
  g_chDeathTimes.clear();
  g_chAlive.clear();
  g_clusterSnapshots.clear();
  g_clusterMembers.assign(numCHs, {});
  g_backupLoad.assign(numCHs, 0);
  g_clusterRecovered.assign(numCHs, false);
  g_recoveryScheduled.assign(numCHs, false);
  g_clusterRecoveredSensorCount.assign(numCHs, 0);
  g_chDeathTimeByIndex.assign(numCHs, -1.0);
  g_chDetectTimeByIndex.assign(numCHs, -1.0);
  g_chAccessIps.assign(numCHs, Ipv4Address("0.0.0.0"));

  // Build human-readable mode tag for filenames
  // [FAULT-4] Includes baseline identifier
  // [FAULT-5] Includes ablation identifier for GRAF-Global runs
  std::string modeTag;
  if (baselineMode != "none") {
    modeTag = "BL_" + baselineMode;
  } else if (grafMode == "off") {
    modeTag = "BASE";
  } else {
    modeTag = "GRAF_" + grafMode;
    if (ablation != "full") {
      modeTag += "_abl_" + ablation;
    }
  }

  NS_LOG_UNCOND("========================================");
  NS_LOG_UNCOND("Hybrid Star-Mesh Simulation v3 (Reviewer Revision)");
  NS_LOG_UNCOND("Protocol: " << protocol);
  NS_LOG_UNCOND("Scenario: " << failScenario << " (target " << expectedDeaths
                             << " CH failures)");
  NS_LOG_UNCOND("GRAF: " << grafMode);
  NS_LOG_UNCOND("Baseline: " << baselineMode);
  NS_LOG_UNCOND("Ablation: " << ablation);
  NS_LOG_UNCOND("Sensors: " << numSensors << " | CHs: " << numCHs);
  NS_LOG_UNCOND("CH Energy: " << adjustedChEnergy
                              << " J | Drain: " << chDrainMultiplier << "x");
  NS_LOG_UNCOND("Seed: " << runSeed << " | Run: " << runNumber);
  NS_LOG_UNCOND("Mode tag: " << modeTag);
  NS_LOG_UNCOND("Recovery enabled: " << (g_recoveryEnabled ? "YES" : "NO"));
  NS_LOG_UNCOND("========================================");

  // ---------------------------------------------------------------------------
  // Nodes
  // ---------------------------------------------------------------------------
  NodeContainer sensorNodes;
  sensorNodes.Create(numSensors);

  NodeContainer chNodes;
  chNodes.Create(numCHs);

  NodeContainer gatewayNode;
  gatewayNode.Create(1);

  NodeContainer backboneNodes;
  backboneNodes.Add(chNodes);
  backboneNodes.Add(gatewayNode);

  NodeContainer accessNodes;
  accessNodes.Add(chNodes);
  accessNodes.Add(sensorNodes);

  g_chNodesPtr = &chNodes;
  g_sensorNodesPtr = &sensorNodes;

  // Balanced cluster membership
  for (uint32_t s = 0; s < numSensors; ++s) {
    g_clusterMembers[s % numCHs].push_back(s);
  }

  // ---------------------------------------------------------------------------
  // Wi-Fi setup
  // ---------------------------------------------------------------------------
  WifiMacHelper adhocMac;
  adhocMac.SetType("ns3::AdhocWifiMac");

  WifiHelper backboneWifi;
  backboneWifi.SetStandard(WIFI_STANDARD_80211b);
  backboneWifi.SetRemoteStationManager(
      "ns3::ConstantRateWifiManager", "DataMode", StringValue("DsssRate1Mbps"),
      "ControlMode", StringValue("DsssRate1Mbps"), "RtsCtsThreshold",
      UintegerValue(0));

  WifiHelper accessWifi;
  accessWifi.SetStandard(WIFI_STANDARD_80211b);
  accessWifi.SetRemoteStationManager(
      "ns3::ConstantRateWifiManager", "DataMode", StringValue("DsssRate1Mbps"),
      "ControlMode", StringValue("DsssRate1Mbps"));

  YansWifiChannelHelper backboneChannel;
  backboneChannel.SetPropagationDelay(
      "ns3::ConstantSpeedPropagationDelayModel");
  backboneChannel.AddPropagationLoss("ns3::RangePropagationLossModel",
                                     "MaxRange",
                                     DoubleValue(g_backboneMaxRange));

  YansWifiPhyHelper backbonePhy;
  backbonePhy.SetChannel(backboneChannel.Create());
  backbonePhy.Set("TxPowerStart", DoubleValue(20.0));
  backbonePhy.Set("TxPowerEnd", DoubleValue(20.0));

  NetDeviceContainer backboneDevices =
      backboneWifi.Install(backbonePhy, adhocMac, backboneNodes);

  // Shared access plane so orphan sensors can be rehomed to a backup CH
  YansWifiChannelHelper accessChannel;
  accessChannel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
  accessChannel.AddPropagationLoss("ns3::RangePropagationLossModel", "MaxRange",
                                   DoubleValue(g_accessMaxRange));

  YansWifiPhyHelper accessPhy;
  accessPhy.SetChannel(accessChannel.Create());
  accessPhy.Set("TxPowerStart", DoubleValue(16.0));
  accessPhy.Set("TxPowerEnd", DoubleValue(16.0));

  NetDeviceContainer accessDevices =
      accessWifi.Install(accessPhy, adhocMac, accessNodes);

  NetDeviceContainer chAccessDevices;
  NetDeviceContainer sensorAccessDevices;
  for (uint32_t c = 0; c < numCHs; ++c) {
    chAccessDevices.Add(accessDevices.Get(c));
  }
  for (uint32_t s = 0; s < numSensors; ++s) {
    sensorAccessDevices.Add(accessDevices.Get(numCHs + s));
  }

  // ---------------------------------------------------------------------------
  // Mobility / layout
  // [FAULT-9] Grid layout uses ceil(sqrt(numCHs)), auto-scales to 16 and 32
  // ---------------------------------------------------------------------------
  MobilityHelper mobility;
  mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");

  Ptr<ListPositionAllocator> chPositions =
      CreateObject<ListPositionAllocator>();
  Ptr<UniformRandomVariable> posJitter = CreateObject<UniformRandomVariable>();
  posJitter->SetAttribute("Min", DoubleValue(-15.0));
  posJitter->SetAttribute("Max", DoubleValue(15.0));

  uint32_t chGridSide =
      static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(numCHs))));
  double chSpacing = areaSize / (chGridSide + 1);

  g_chPositions.clear();
  uint32_t placed = 0;
  for (uint32_t row = 0; row < chGridSide && placed < numCHs; ++row) {
    for (uint32_t col = 0; col < chGridSide && placed < numCHs; ++col) {
      double x = chSpacing * (col + 1) + posJitter->GetValue();
      double y = chSpacing * (row + 1) + posJitter->GetValue();
      x = std::min(std::max(x, 20.0), areaSize - 20.0);
      y = std::min(std::max(y, 20.0), areaSize - 20.0);
      Vector v(x, y, 0.0);
      chPositions->Add(v);
      g_chPositions.push_back(v);
      placed++;
    }
  }

  mobility.SetPositionAllocator(chPositions);
  mobility.Install(chNodes);

  Ptr<ListPositionAllocator> sensorPositions =
      CreateObject<ListPositionAllocator>();
  Ptr<UniformRandomVariable> radiusRv = CreateObject<UniformRandomVariable>();
  Ptr<UniformRandomVariable> angleRv = CreateObject<UniformRandomVariable>();
  radiusRv->SetAttribute("Min", DoubleValue(5.0));
  radiusRv->SetAttribute("Max", DoubleValue(20.0));
  angleRv->SetAttribute("Min", DoubleValue(0.0));
  angleRv->SetAttribute("Max", DoubleValue(2.0 * M_PI));

  g_sensorPositions.assign(numSensors, Vector(0, 0, 0));
  for (uint32_t c = 0; c < numCHs; ++c) {
    Vector center = g_chPositions[c];
    for (uint32_t sIdx : g_clusterMembers[c]) {
      double r = radiusRv->GetValue();
      double a = angleRv->GetValue();
      double x =
          std::min(std::max(center.x + r * std::cos(a), 1.0), areaSize - 1.0);
      double y =
          std::min(std::max(center.y + r * std::sin(a), 1.0), areaSize - 1.0);
      g_sensorPositions[sIdx] = Vector(x, y, 0.0);
    }
  }
  for (uint32_t s = 0; s < numSensors; ++s) {
    sensorPositions->Add(g_sensorPositions[s]);
  }

  mobility.SetPositionAllocator(sensorPositions);
  mobility.Install(sensorNodes);

  Ptr<ListPositionAllocator> gwPosition = CreateObject<ListPositionAllocator>();
  gwPosition->Add(Vector(areaSize / 2.0 + 10.0, areaSize / 2.0 + 10.0, 0.0));
  mobility.SetPositionAllocator(gwPosition);
  mobility.Install(gatewayNode);

  // ---------------------------------------------------------------------------
  // Energy models
  // ---------------------------------------------------------------------------
  BasicEnergySourceHelper sensorEnergyHelper;
  sensorEnergyHelper.Set("BasicEnergySourceInitialEnergyJ",
                         DoubleValue(sensorInitialEnergy));
  sensorEnergyHelper.Set("BasicEnergySupplyVoltageV", DoubleValue(3.3));
  EnergySourceContainer sensorEnergySources =
      sensorEnergyHelper.Install(sensorNodes);

  WifiRadioEnergyModelHelper sensorRadioHelper;
  sensorRadioHelper.Set("TxCurrentA", DoubleValue(0.017));
  sensorRadioHelper.Set("RxCurrentA", DoubleValue(0.0197));
  sensorRadioHelper.Set("IdleCurrentA", DoubleValue(0.000426));
  sensorRadioHelper.Set("SleepCurrentA", DoubleValue(0.000014));

  for (uint32_t s = 0; s < numSensors; ++s) {
    sensorRadioHelper.Install(sensorAccessDevices.Get(s),
                              sensorEnergySources.Get(s));
  }

  Ptr<UniformRandomVariable> energyJitter =
      CreateObject<UniformRandomVariable>();
  energyJitter->SetAttribute("Min", DoubleValue(0.90));
  energyJitter->SetAttribute("Max", DoubleValue(1.10));

  EnergySourceContainer chEnergySources;
  for (uint32_t c = 0; c < numCHs; ++c) {
    BasicEnergySourceHelper chEnergyHelper;
    chEnergyHelper.Set(
        "BasicEnergySourceInitialEnergyJ",
        DoubleValue(adjustedChEnergy * energyJitter->GetValue()));
    chEnergyHelper.Set("BasicEnergySupplyVoltageV", DoubleValue(3.3));
    EnergySourceContainer src = chEnergyHelper.Install(chNodes.Get(c));
    chEnergySources.Add(src);
  }
  g_chEnergyPtr = &chEnergySources;

  WifiRadioEnergyModelHelper chRadioHelper;
  chRadioHelper.Set("TxCurrentA", DoubleValue(0.017 * chDrainMultiplier));
  chRadioHelper.Set("RxCurrentA", DoubleValue(0.0197 * chDrainMultiplier));
  chRadioHelper.Set("IdleCurrentA", DoubleValue(0.000426 * 2.0));
  chRadioHelper.Set("SleepCurrentA", DoubleValue(0.000014));

  for (uint32_t c = 0; c < numCHs; ++c) {
    chRadioHelper.Install(backboneDevices.Get(c), chEnergySources.Get(c));
    chRadioHelper.Install(chAccessDevices.Get(c), chEnergySources.Get(c));
    g_chAlive[chNodes.Get(c)->GetId()] = true;
  }

  BasicEnergySourceHelper gwEnergyHelper;
  gwEnergyHelper.Set("BasicEnergySourceInitialEnergyJ", DoubleValue(100000.0));
  EnergySourceContainer gwEnergySources = gwEnergyHelper.Install(gatewayNode);

  WifiRadioEnergyModelHelper gwRadioHelper;
  gwRadioHelper.Set("TxCurrentA", DoubleValue(0.017));
  gwRadioHelper.Set("RxCurrentA", DoubleValue(0.0197));
  gwRadioHelper.Set("IdleCurrentA", DoubleValue(0.000426));
  gwRadioHelper.Set("SleepCurrentA", DoubleValue(0.000014));
  gwRadioHelper.Install(backboneDevices.Get(numCHs), gwEnergySources.Get(0));

  // ---------------------------------------------------------------------------
  // Routing + Internet
  // ---------------------------------------------------------------------------
  Ipv4StaticRoutingHelper staticRouting;

  InternetStackHelper sensorInternet;
  sensorInternet.SetRoutingHelper(staticRouting);
  sensorInternet.Install(sensorNodes);

  InternetStackHelper backboneInternet;
  if (protocol == "OLSR") {
    OlsrHelper olsr;
    // [FIX-F1] TcInterval override REMOVED. Both baseline and GRAF runs now
    // use the NS-3 default TcInterval (5.0s), ensuring a fair protocol-neutral
    // comparison.
    for (uint32_t c = 0; c < numCHs; ++c) {
      olsr.ExcludeInterface(chNodes.Get(c), 2); // iface 2 = shared access
    }

    Ipv4ListRoutingHelper olsrList;
    Ipv4StaticRoutingHelper fallback;
    olsrList.Add(olsr, 10);
    olsrList.Add(fallback, 0);
    backboneInternet.SetRoutingHelper(olsrList);
  } else if (protocol == "AODV") {
    AodvHelper aodv;
    aodv.Set("EnableHello", BooleanValue(true));
    aodv.Set("HelloInterval", TimeValue(Seconds(2.0)));
    aodv.Set("RreqRetries", UintegerValue(5));
    aodv.Set("NodeTraversalTime", TimeValue(MilliSeconds(20)));
    aodv.Set("ActiveRouteTimeout", TimeValue(Seconds(10.0)));

    Ipv4ListRoutingHelper aodvList;
    Ipv4StaticRoutingHelper fallback;
    aodvList.Add(aodv, 10);
    aodvList.Add(fallback, 0);
    backboneInternet.SetRoutingHelper(aodvList);
  } else {
    NS_LOG_UNCOND("ERROR: Unknown protocol. Use OLSR or AODV.");
    return 1;
  }
  backboneInternet.Install(backboneNodes);

  // ---------------------------------------------------------------------------
  // IP addressing
  // ---------------------------------------------------------------------------
  Ipv4AddressHelper backboneAddr;
  backboneAddr.SetBase("10.2.0.0", "255.255.255.0");
  Ipv4InterfaceContainer backboneIfs = backboneAddr.Assign(backboneDevices);
  g_gatewayIp = backboneIfs.GetAddress(numCHs);

  Ipv4AddressHelper accessAddr;
  accessAddr.SetBase("10.1.0.0", "255.255.0.0");
  Ipv4InterfaceContainer accessIfs = accessAddr.Assign(accessDevices);

  for (uint32_t c = 0; c < numCHs; ++c) {
    g_chAccessIps[c] = accessIfs.GetAddress(c);
  }

  g_sensorAccessIfIndex.assign(numSensors, 0);
  for (uint32_t s = 0; s < numSensors; ++s) {
    Ptr<Node> sensor = sensorNodes.Get(s);
    Ptr<Ipv4> ipv4 = sensor->GetObject<Ipv4>();
    uint32_t iface = ipv4->GetInterfaceForDevice(sensorAccessDevices.Get(s));
    g_sensorAccessIfIndex[s] = iface;
  }

  // Initial sensor default routes: each sensor forwards to its home CH
  for (uint32_t c = 0; c < numCHs; ++c) {
    for (uint32_t sIdx : g_clusterMembers[c]) {
      Ptr<Node> sensor = sensorNodes.Get(sIdx);
      Ptr<Ipv4> ipv4 = sensor->GetObject<Ipv4>();
      Ptr<Ipv4StaticRouting> sRoute = staticRouting.GetStaticRouting(ipv4);
      sRoute->SetDefaultRoute(g_chAccessIps[c], g_sensorAccessIfIndex[sIdx]);
    }
  }

  // ---------------------------------------------------------------------------
  // Traffic
  // ---------------------------------------------------------------------------
  ApplicationContainer sensorSinks;
  for (uint32_t s = 0; s < numSensors; ++s) {
    PacketSinkHelper sinkHelper(
        "ns3::UdpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), g_sensorBasePort + s));
    ApplicationContainer sink = sinkHelper.Install(gatewayNode.Get(0));
    sensorSinks.Add(sink);
  }
  sensorSinks.Start(Seconds(1.0));
  sensorSinks.Stop(Seconds(simTime));
  g_sensorSinkAppsPtr = &sensorSinks;

  ApplicationContainer warmupSinks;
  for (uint32_t s = 0; s < numSensors; ++s) {
    PacketSinkHelper sinkHelper(
        "ns3::UdpSocketFactory",
        InetSocketAddress(Ipv4Address::GetAny(), g_warmupBasePort + s));
    ApplicationContainer sink = sinkHelper.Install(gatewayNode.Get(0));
    warmupSinks.Add(sink);
  }
  warmupSinks.Start(Seconds(1.0));
  warmupSinks.Stop(Seconds(simTime));

  uint32_t packetSize = 64;
  double dataStartBase = 30.0;
  double startStep = 0.02;

  // Symmetric warm-up for both protocols; excluded from final metrics
  for (uint32_t s = 0; s < numSensors; ++s) {
    double warmupStart = 20.0 + s * 0.01;
    double warmupStop = 29.0 + s * 0.01;

    OnOffHelper warmup("ns3::UdpSocketFactory",
                       InetSocketAddress(g_gatewayIp, g_warmupBasePort + s));
    warmup.SetConstantRate(DataRate("32bps"), packetSize);

    ApplicationContainer app = warmup.Install(sensorNodes.Get(s));
    app.Start(Seconds(warmupStart));
    app.Stop(Seconds(warmupStop));
  }

  // Real sensor traffic: 102 bps per sensor => 1020 bps/cluster for 10 members
  for (uint32_t s = 0; s < numSensors; ++s) {
    double startTime = dataStartBase + s * startStep;

    OnOffHelper source("ns3::UdpSocketFactory",
                       InetSocketAddress(g_gatewayIp, g_sensorBasePort + s));
    source.SetConstantRate(DataRate("102bps"), packetSize);

    ApplicationContainer app = source.Install(sensorNodes.Get(s));
    app.Start(Seconds(startTime));
    app.Stop(Seconds(simTime - 1.0));
  }

  // ---------------------------------------------------------------------------
  // Failure process
  // ---------------------------------------------------------------------------
  Simulator::Schedule(Seconds(1.0), &CheckChEnergy, 1.0, simTime);
  Simulator::Schedule(Seconds(1.0), &HeartbeatCheck, simTime);

  // TSI + Heartbeat init
  g_numCHsInitial = numCHs;
  g_hbMissCount.assign(numCHs, 0);
  g_hbDetectionTime.assign(numCHs, -1.0);
  g_hbDetectionLatency.clear();
  g_recentReconvTimes.clear();

  Ptr<UniformRandomVariable> failJitter = CreateObject<UniformRandomVariable>();
  failJitter->SetAttribute("Min", DoubleValue(-3.0));
  failJitter->SetAttribute("Max", DoubleValue(3.0));

  double failStartTime = 60.0;
  double failInterval = 30.0;
  for (uint32_t k = 0; k < expectedDeaths && k < numCHs; ++k) {
    double deathTime =
        failStartTime + k * failInterval + failJitter->GetValue();
    Simulator::Schedule(Seconds(deathTime), &KillCH, k, deathTime);
  }

  // ---------------------------------------------------------------------------
  // Logs
  // ---------------------------------------------------------------------------
  std::string periodicFilename =
      outputPrefix + "_" + protocol + "_scenario" +
      std::to_string(failScenario) + "_" + modeTag + "_seed" +
      std::to_string(runSeed) + "_run" + std::to_string(runNumber) +
      "_periodic.csv";
  std::ofstream periodicLog(periodicFilename);
  periodicLog << "time,ch_alive,ch_dead\n";
  Simulator::Schedule(Seconds(dataStartBase), &LogPeriodicMetrics, &periodicLog,
                      5.0, simTime);

  std::string clusterBytesFilename =
      outputPrefix + "_" + protocol + "_scenario" +
      std::to_string(failScenario) + "_" + modeTag + "_seed" +
      std::to_string(runSeed) + "_run" + std::to_string(runNumber) +
      "_clusterbytes.csv";
  std::ofstream clusterBytesLog(clusterBytesFilename);
  clusterBytesLog
      << "time,cluster_idx,cumulative_bytes,mode,protocol,scenario,seed,run\n";
  g_clusterLogMode    = modeTag;
  g_clusterLogProto   = protocol;
  g_clusterLogScenario = failScenario;
  g_clusterLogSeed    = runSeed;
  g_clusterLogRun     = runNumber;
  Simulator::Schedule(Seconds(dataStartBase), &LogClusterBytes,
                      &clusterBytesLog, 1.0, simTime);

  // TSI log
  std::string tsiFilename = outputPrefix + "_" + protocol + "_scenario" +
                            std::to_string(failScenario) + "_" + modeTag +
                            "_seed" + std::to_string(runSeed) + "_run" +
                            std::to_string(runNumber) + "_tsi.csv";
  std::ofstream tsiLog(tsiFilename);
  tsiLog << "time,tsi,region,ch_alive,ch_dead\n";
  if (g_recoveryEnabled)
    Simulator::Schedule(Seconds(dataStartBase), &LogTSI, &tsiLog, 10.0,
                        simTime);

  // ---------------------------------------------------------------------------
  // Run
  // ---------------------------------------------------------------------------
  FlowMonitorHelper flowHelper;
  Ptr<FlowMonitor> flowMonitor = flowHelper.InstallAll();

  Simulator::Stop(Seconds(simTime));
  // ── Connect Routing Traces Manually ───────────────────────────────────────
  // Config::Connect fails due to Ipv4ListRouting encapsulation. Connect
  // directly:
  for (uint32_t i = 0; i < backboneNodes.GetN(); ++i) {
    Ptr<Ipv4> ipv4 = backboneNodes.Get(i)->GetObject<Ipv4>();
    Ptr<Ipv4RoutingProtocol> rootRouting = ipv4->GetRoutingProtocol();
    Ptr<Ipv4ListRouting> listRouting =
        DynamicCast<Ipv4ListRouting>(rootRouting);

    if (listRouting) {
      if (protocol == "AODV") {
        Ptr<Ipv4> ipv4bb = backboneNodes.Get(i)->GetObject<Ipv4>();
        // Interface 1 = backbone (interface 0 = loopback)
        // Track all packets sent on backbone interface; filter by port range
        // for AODV control (UDP 654)
        ipv4bb->TraceConnectWithoutContext("SendOutgoing",
                                           MakeCallback(&AodvIpTxTrace));
      } else if (protocol == "OLSR") {
        int16_t priority;
        Ptr<Ipv4RoutingProtocol> olsrProto =
            listRouting->GetRoutingProtocol(0, priority);
        if (olsrProto) {
          olsrProto->TraceConnectWithoutContext("Tx",
                                                MakeCallback(&OlsrTxTrace));
        }
      }
    }
  }

  NS_LOG_UNCOND("Starting simulation...");
  Simulator::Run();
  NS_LOG_UNCOND("Simulation complete.");

  // ---------------------------------------------------------------------------
  // Collect stats
  // ---------------------------------------------------------------------------
  flowMonitor->CheckForLostPackets();
  FlowMonitor::FlowStatsContainer stats = flowMonitor->GetFlowStats();
  Ptr<Ipv4FlowClassifier> classifier =
      DynamicCast<Ipv4FlowClassifier>(flowHelper.GetClassifier());

  uint64_t totalTxBytes = 0, totalRxBytes = 0;
  uint64_t totalTxPkts = 0, totalRxPkts = 0, totalLostPkts = 0;
  double totalDelay = 0.0;
  uint32_t flowCount = 0;

  for (const auto &flow : stats) {
    Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(flow.first);

    if (t.destinationPort >= g_warmupBasePort &&
        t.destinationPort < g_warmupBasePort + numSensors) {
      continue;
    }

    if (t.destinationPort < g_sensorBasePort ||
        t.destinationPort >= g_sensorBasePort + numSensors) {
      continue;
    }

    totalTxBytes += flow.second.txBytes;
    totalRxBytes += flow.second.rxBytes;
    totalTxPkts += flow.second.txPackets;
    totalRxPkts += flow.second.rxPackets;
    totalLostPkts += flow.second.lostPackets;
    totalDelay += flow.second.delaySum.GetSeconds();
    flowCount++;
  }

  double pdr = (totalTxPkts > 0)
                   ? (100.0 * static_cast<double>(totalRxPkts) / totalTxPkts)
                   : 0.0;
  double avgDelayMs =
      (totalRxPkts > 0) ? (1000.0 * totalDelay / totalRxPkts) : 0.0;
  double offeredDuration = simTime - dataStartBase;
  double throughputKbps =
      (offeredDuration > 0.0)
          ? (totalRxBytes * 8.0) / (offeredDuration * 1000.0)
          : 0.0;

  double firstChDeath =
      g_chDeathTimes.empty()
          ? simTime
          : *std::min_element(g_chDeathTimes.begin(), g_chDeathTimes.end());

  // Energy metrics
  double totalInitialEnergy = 0.0;
  double totalResidualEnergy = 0.0;
  uint32_t chsDepleted = 0;
  for (uint32_t c = 0; c < numCHs; ++c) {
    Ptr<BasicEnergySource> src =
        DynamicCast<BasicEnergySource>(g_chEnergyPtr->Get(c));
    if (!src) {
      continue;
    }
    totalInitialEnergy += src->GetInitialEnergy();
    totalResidualEnergy += src->GetRemainingEnergy();
    if (src->GetRemainingEnergy() <= 0.0) {
      chsDepleted++;
    }
  }
  double totalConsumedEnergy = totalInitialEnergy - totalResidualEnergy;

  // Legacy energy-per-delivered-bit (kept for backward compatibility)
  double energyPerDeliveredBit =
      (totalRxBytes > 0 && totalConsumedEnergy > 0.0)
          ? totalConsumedEnergy / (totalRxBytes * 8.0)
          : -1.0;

  // [METRIC] IEEE IoT Journal Energy-Per-Bit formula:
  // η = E_consumed / (PDR_fraction × TotalOfferedBits)
  // where TotalOfferedBits = totalTxPkts * packetSize * 8
  // This normalises energy by the *offered* traffic, not just received bytes,
  // enabling apples-to-apples comparison with LEACH/HEED benchmarks.
  double pdrFraction = pdr / 100.0;
  double totalOfferedBits =
      static_cast<double>(totalTxPkts) * packetSize * 8.0;
  double etaIeee = (pdrFraction > 1e-6 && totalOfferedBits > 0.0 &&
                    totalConsumedEnergy > 0.0)
                       ? (totalConsumedEnergy / (pdrFraction * totalOfferedBits))
                       : -1.0;

  // Recovery metrics based on real source traffic from affected clusters
  std::vector<double> reconvTimes;
  uint32_t failedClusters = 0;
  uint32_t recoveredClusters = 0;
  uint32_t totalAffectedSensors = 0;
  uint32_t totalRecoveredSensors = 0;
  // Metric: Application Bytes Recovered Post-Failure (formerly Recovery Bytes)
  uint64_t totalRecoveryBytes = 0;

  for (uint32_t c = 0; c < numCHs; ++c) {
    if (g_chDeathTimeByIndex[c] < 0.0) {
      continue;
    }

    failedClusters++;
    totalAffectedSensors += static_cast<uint32_t>(g_clusterMembers[c].size());
    totalRecoveredSensors += g_clusterRecoveredSensorCount[c];

    bool rehomedCluster =
        (g_recoveryEnabled && c < g_clusterRecovered.size() &&
         g_clusterRecovered[c]);
    if (!rehomedCluster) {
      continue;
    }

    double detectT = (g_chDetectTimeByIndex[c] >= 0.0)
                         ? g_chDetectTimeByIndex[c]
                         : g_chDeathTimeByIndex[c];
    double byteCutoff = detectT + 1.0;

    uint64_t bytesAtCutoff = 0;
    for (const auto &snap : g_clusterSnapshots) {
      if (snap.clusterIdx == c && snap.time <= byteCutoff) {
        bytesAtCutoff = snap.cumulativeBytes;
      }
    }

    // [FIX-M6] reconvTime and finalBytes are derived from snapshots.
    // GetClusterTotalRxBytes() is used ONLY as a fallback for the recovery
    // bytes delta — it must NOT overwrite the snapshot-based finalBytes used
    // for reconvergence time detection.
    double reconvTime = -1.0;
    uint64_t snapshotFinalBytes = bytesAtCutoff;
    bool sawPostCutoffSnapshot = false;
    for (const auto &snap : g_clusterSnapshots) {
      if (snap.clusterIdx == c && snap.time > byteCutoff) {
        sawPostCutoffSnapshot = true;
        snapshotFinalBytes = snap.cumulativeBytes;
        if (reconvTime < 0.0 && snap.cumulativeBytes > bytesAtCutoff) {
          reconvTime = snap.time - detectT;
        }
      }
    }

    // Use the same snapshot series as reconvergence detection. The live sink
    // total is only a fallback for runs where post-cutoff snapshots are absent.
    uint64_t liveBytes = GetClusterTotalRxBytes(c);
    uint64_t finalBytesForDelta =
        sawPostCutoffSnapshot ? snapshotFinalBytes : liveBytes;
    uint64_t recoveryBytes = (finalBytesForDelta > bytesAtCutoff)
                                 ? (finalBytesForDelta - bytesAtCutoff)
                                 : 0;
    totalRecoveryBytes += recoveryBytes;

    if (reconvTime >= 0.0) {
      reconvTimes.push_back(reconvTime);
      recoveredClusters++;
    }
  }

  double meanReconv = -1.0;
  double medianReconv = -1.0;
  double maxReconv = -1.0;
  if (!reconvTimes.empty()) {
    meanReconv = std::accumulate(reconvTimes.begin(), reconvTimes.end(), 0.0) /
                 reconvTimes.size();
    std::sort(reconvTimes.begin(), reconvTimes.end());
    maxReconv = reconvTimes.back();
    size_t mid = reconvTimes.size() / 2;
    medianReconv = (reconvTimes.size() % 2 == 0)
                       ? 0.5 * (reconvTimes[mid - 1] + reconvTimes[mid])
                       : reconvTimes[mid];
  }

  double clusterRecoveryRate =
      (failedClusters > 0) ? 100.0 * recoveredClusters / failedClusters : -1.0;
  double sensorRecoveryRate =
      (totalAffectedSensors > 0)
          ? 100.0 * totalRecoveredSensors / totalAffectedSensors
          : -1.0;

  // Jain fairness across sensor sinks
  double fairnessSum = 0.0;
  double fairnessSumSq = 0.0;
  for (uint32_t s = 0; s < numSensors; ++s) {
    Ptr<PacketSink> sink = DynamicCast<PacketSink>(sensorSinks.Get(s));
    double x = sink ? static_cast<double>(sink->GetTotalRx()) : 0.0;
    fairnessSum += x;
    fairnessSumSq += x * x;
  }
  double jainFairnessIndex =
      (numSensors > 0 && fairnessSumSq > 0.0)
          ? (fairnessSum * fairnessSum) / (numSensors * fairnessSumSq)
          : -1.0;

  double normalizedOverhead =
      (totalRxPkts > 0) ? (double)g_routingOverheadPackets / totalRxPkts : -1.0;

  // ---------------------------------------------------------------------------
  // Output
  // ---------------------------------------------------------------------------
  std::string summaryFilename =
      outputPrefix + "_" + protocol + "_scenario" +
      std::to_string(failScenario) + "_" + modeTag + "_seed" +
      std::to_string(runSeed) + "_run" + std::to_string(runNumber) +
      "_summary.csv";
  std::ofstream summaryFile(summaryFilename, std::ios::out);
  summaryFile << "metric,value\n";
  summaryFile << "protocol," << protocol << "\n";
  summaryFile << "scenario," << failScenario << "\n";
  summaryFile << "graf," << grafMode << "\n";
  // [FAULT-4] Record baseline mode
  summaryFile << "baseline," << baselineMode << "\n";
  // [FAULT-5] Record ablation condition
  summaryFile << "ablation," << ablation << "\n";
  summaryFile << "seed," << runSeed << "\n";
  summaryFile << "run," << runNumber << "\n";
  summaryFile << "sim_time_s," << simTime << "\n";
  // [FAULT-9] Record network scale
  summaryFile << "num_chs," << numCHs << "\n";
  summaryFile << "num_sensors," << numSensors << "\n";
  summaryFile << "routing_overhead_packets," << g_routingOverheadPackets
              << "\n";
  summaryFile << "routing_overhead_bytes," << g_routingOverheadBytes << "\n";
  summaryFile << "normalized_overhead_ctrl_per_data," << normalizedOverhead
              << "\n";
  summaryFile << "sensor_flows," << numSensors << "\n";
  summaryFile << "total_tx_packets," << totalTxPkts << "\n";
  summaryFile << "total_rx_packets," << totalRxPkts << "\n";
  summaryFile << "total_lost_packets," << totalLostPkts << "\n";
  summaryFile << "pdr_percent," << pdr << "\n";
  summaryFile << "avg_delay_ms," << avgDelayMs << "\n";
  summaryFile << "throughput_kbps_active_window," << throughputKbps << "\n";
  summaryFile << "flow_count," << flowCount << "\n";
  summaryFile << "ch_deaths," << g_chDeathTimes.size() << "\n";
  summaryFile << "first_ch_death_time_s," << firstChDeath << "\n";
  summaryFile << "failed_clusters," << failedClusters << "\n";
  summaryFile << "recovered_clusters," << recoveredClusters << "\n";
  summaryFile << "cluster_recovery_rate_percent," << clusterRecoveryRate
              << "\n";
  summaryFile << "affected_sensors," << totalAffectedSensors << "\n";
  summaryFile << "recovered_sensors," << totalRecoveredSensors << "\n";
  summaryFile << "sensor_recovery_rate_percent," << sensorRecoveryRate << "\n";
  summaryFile << "mean_reconv_s," << meanReconv << "\n";
  summaryFile << "median_reconv_s," << medianReconv << "\n";
  summaryFile << "max_reconv_s," << maxReconv << "\n";
  summaryFile << "total_recovery_bytes," << totalRecoveryBytes << "\n";
  summaryFile << "total_initial_ch_energy_j," << totalInitialEnergy << "\n";
  summaryFile << "total_residual_ch_energy_j," << totalResidualEnergy << "\n";
  summaryFile << "total_consumed_ch_energy_j," << totalConsumedEnergy << "\n";
  summaryFile << "chs_depleted," << chsDepleted << "\n";
  // Legacy metric (E per received bit) — kept for backward compatibility
  summaryFile << "energy_per_delivered_bit_j," << energyPerDeliveredBit << "\n";
  // [METRIC] IEEE IoT Journal convention: η = E_consumed / (PDR × OfferedBits)
  summaryFile << "energy_per_bit_ieee_j," << etaIeee << "\n";
  summaryFile << "jain_fairness_index," << jainFairnessIndex << "\n";
  // Heartbeat detection metrics
  double meanDetectionLatency = 0.0, maxDetectionLatency = 0.0;
  if (!g_hbDetectionLatency.empty()) {
    meanDetectionLatency = std::accumulate(g_hbDetectionLatency.begin(),
                                           g_hbDetectionLatency.end(), 0.0) /
                           g_hbDetectionLatency.size();
    maxDetectionLatency = *std::max_element(g_hbDetectionLatency.begin(),
                                            g_hbDetectionLatency.end());
  }
  uint32_t hbDetectedCount = (uint32_t)g_hbDetectionLatency.size();
  summaryFile << "hb_detected_count," << hbDetectedCount << "\n";
  summaryFile << "hb_mean_detection_latency_s," << meanDetectionLatency << "\n";
  summaryFile << "hb_max_detection_latency_s," << maxDetectionLatency << "\n";
  summaryFile.close();

  std::string xmlFilename = outputPrefix + "_" + protocol + "_scenario" +
                            std::to_string(failScenario) + "_" + modeTag +
                            "_seed" + std::to_string(runSeed) + "_run" +
                            std::to_string(runNumber) + "_flowmon.xml";
  flowMonitor->SerializeToXmlFile(xmlFilename, true, true);

  NS_LOG_UNCOND("");
  NS_LOG_UNCOND("========== RESULTS ==========");
  NS_LOG_UNCOND("Protocol:               " << protocol
      << (grafMode != "off" ? (" + GRAF(" + grafMode + ")") : " (baseline)"));
  NS_LOG_UNCOND("Baseline:               " << baselineMode);
  NS_LOG_UNCOND("Ablation:               " << ablation);
  NS_LOG_UNCOND("Scenario:               " << failScenario);
  NS_LOG_UNCOND("Scale:                  " << numCHs << " CHs / "
                                           << numSensors << " sensors");
  NS_LOG_UNCOND("PDR:                    " << pdr << "%");
  NS_LOG_UNCOND("Avg Delay:              " << avgDelayMs << " ms");
  NS_LOG_UNCOND("Throughput(active):     " << throughputKbps << " kbps");
  NS_LOG_UNCOND("Packets Sent:           " << totalTxPkts);
  NS_LOG_UNCOND("Packets Received:       " << totalRxPkts);
  NS_LOG_UNCOND("Packets Lost:           " << totalLostPkts);
  NS_LOG_UNCOND("CH Deaths:              " << g_chDeathTimes.size() << "/"
                                           << numCHs);
  NS_LOG_UNCOND("First CH Death:         t=" << firstChDeath << "s");
  NS_LOG_UNCOND("Failed Clusters:        " << failedClusters);
  NS_LOG_UNCOND("Recovered Clusters:     " << recoveredClusters);
  NS_LOG_UNCOND("Cluster Recovery Rate:  " << clusterRecoveryRate << "%");
  NS_LOG_UNCOND("Sensor Recovery Rate:   " << sensorRecoveryRate << "%");
  NS_LOG_UNCOND("Mean Reconv:            " << meanReconv << " s");
  NS_LOG_UNCOND("Median Reconv:          " << medianReconv << " s");
  NS_LOG_UNCOND("Max Reconv:             " << maxReconv << " s");
  NS_LOG_UNCOND("App Rx Recovered (B):   " << totalRecoveryBytes);
  NS_LOG_UNCOND("Routing Overhead (P):   " << g_routingOverheadPackets);
  NS_LOG_UNCOND("Routing Overhead (B):   " << g_routingOverheadBytes);
  NS_LOG_UNCOND("Normalized Overhead:    " << normalizedOverhead
                                           << " ctrl/data");
  NS_LOG_UNCOND("Energy/Delivered bit:   " << energyPerDeliveredBit
                                           << " J/bit (legacy)");
  NS_LOG_UNCOND("Energy/Bit (IEEE eta):  " << etaIeee << " J/bit");
  NS_LOG_UNCOND("Jain Fairness Index:    " << std::fixed << std::setprecision(4)
                                           << jainFairnessIndex);
  NS_LOG_UNCOND("Summary:                " << summaryFilename);
  NS_LOG_UNCOND("=============================");

  periodicLog.close();
  clusterBytesLog.close();
  tsiLog.close();
  Simulator::Destroy();
  return 0;
}
