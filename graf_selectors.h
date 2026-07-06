#pragma once
// graf_selectors.h — backup CH selection: GRAF-Global, GRAF-Local, baselines
// Included once from Star_mesh_simulation_code.cc after graf_utils.h.

// -----------------------------------------------------------------------------
// Backup selection — GRAF-Global (with ablation support)
// -----------------------------------------------------------------------------
uint32_t SelectGraphAwareBackup(uint32_t failedChIndex) {
  if (!g_chNodesPtr || !g_chEnergyPtr || g_chPositions.empty()) {
    return std::numeric_limits<uint32_t>::max();
  }

  auto aliveAdj = BuildBackboneAdjacency(true);
  auto bc = ComputeBetweennessCentrality(aliveAdj);

  // Build hop-distance graph: alive-only backbone with the failed CH's direct
  // links restored. BFS must start from failedChIndex, but BuildBackboneAdjacency(true)
  // gives it an empty list because it is dead. Restoring only its edges to
  // *alive* neighbors ensures paths never route through other dead intermediates.
  auto hopAdj = aliveAdj;
  {
    uint32_t nCHs = static_cast<uint32_t>(g_chPositions.size());
    for (uint32_t k = 0; k < nCHs; ++k) {
      if (k == failedChIndex || !IsChAliveByIndex(k)) continue;
      if (CalculateDistance(g_chPositions[failedChIndex], g_chPositions[k]) <=
          g_backboneMaxRange) {
        hopAdj[failedChIndex].push_back(k);
        hopAdj[k].push_back(failedChIndex);
      }
    }
  }

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

    uint32_t hops = BfsHopDistance(hopAdj, failedChIndex, j);
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

  // Uniform random selection using integer draw to avoid closed-interval bias.
  // GetInteger(0, N-1) returns values in [0, N-1] with equal probability.
  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();
  uint32_t pick = rng->GetInteger(0, static_cast<uint32_t>(eligible.size()) - 1);

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
