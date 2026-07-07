#pragma once
// graf_recovery.h — cluster rehoming, failure handling, energy check
// Included once from Star_mesh_simulation_code.cc after graf_selectors.h.

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

  // [FAULT-4] Dispatch to appropriate selector based on mode.
  // For GRAF-Global, a successor may have been pre-selected when the previous
  // CH in this chain was promoted; try it first before full re-scoring.
  uint32_t backupIdx;
  if (g_baselineMode == "rand") {
    backupIdx = SelectRandBackup(failedChIndex);
  } else if (g_baselineMode == "energy") {
    backupIdx = SelectEnergyBackup(failedChIndex);
  } else if (g_baselineMode == "nearest") {
    backupIdx = SelectNearestBackup(failedChIndex);
  } else if (g_grafMode == "global") {
    auto succIt = g_chSuccessor.find(failedChIndex);
    if (succIt != g_chSuccessor.end()) {
      uint32_t preSelected = succIt->second;
      g_chSuccessor.erase(succIt); // consume regardless of outcome
      Ptr<BasicEnergySource> succSrc =
          DynamicCast<BasicEnergySource>(g_chEnergyPtr->Get(preSelected));
      bool viable = IsChAliveByIndex(preSelected) &&
                    succSrc && succSrc->GetRemainingEnergy() > 0.1 &&
                    CoverageFraction(failedChIndex, preSelected) > 0.0;
      if (viable) {
        NS_LOG_UNCOND("  [GRAF-GLOBAL] Using pre-selected successor idx="
                      << preSelected << " for failed CH idx=" << failedChIndex);
        backupIdx = preSelected;
      } else {
        NS_LOG_UNCOND("  [GRAF-GLOBAL] Pre-selected successor idx=" << preSelected
                      << " no longer viable, falling back to full selection");
        backupIdx = SelectGraphAwareBackup(failedChIndex);
      }
    } else {
      backupIdx = SelectGraphAwareBackup(failedChIndex);
    }
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

  // FIX-A4: arm event-driven SRL watches for every sensor in the failed cluster
  for (uint32_t sIdx : g_clusterMembers[failedChIndex]) {
    if (!g_clusterRecoveryWatchStart.count(sIdx)) {
      g_clusterRecoveryWatchStart[sIdx] = detectTime;
    }
  }

  // GRAF-Global: pre-select a successor for the newly promoted backup CH so
  // that if it fails next, the decision is already made with the current
  // topology rather than re-computed under further-depleted conditions.
  if (g_grafMode == "global" && g_baselineMode == "none") {
    uint32_t successorIdx = SelectGraphAwareBackup(backupIdx);
    if (successorIdx != std::numeric_limits<uint32_t>::max()) {
      g_chSuccessor[backupIdx] = successorIdx;
      NS_LOG_UNCOND("  [GRAF-GLOBAL] Pre-selected successor idx=" << successorIdx
                    << " for promoted backup CH idx=" << backupIdx);
    } else {
      NS_LOG_UNCOND("  [GRAF-GLOBAL] No successor available for promoted CH idx="
                    << backupIdx << " (network nearly depleted)");
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

  // FIX (#1): do NOT erase g_chSuccessor[chIndex] here. That entry is this CH's
  // own pre-selected successor, which the scheduled DetectAndRecoverCluster(chIndex)
  // consumes ~g_detectionDelay s from now via the GRAF-Global fast path (it erases
  // the entry itself, "consume regardless of outcome"). Erasing it at death time
  // made that fast path unreachable — every recovery fell back to full re-scoring.
  // We DO still invalidate entries where this CH was pre-selected as some other
  // backup CH's successor, so a dead candidate is not used at recovery time.
  for (auto it = g_chSuccessor.begin(); it != g_chSuccessor.end(); ) {
    if (it->second == chIndex) {
      NS_LOG_UNCOND("  [GRAF-GLOBAL] Invalidated stale successor idx=" << chIndex
                    << " (was pre-selected for backup CH idx=" << it->first << ")");
      it = g_chSuccessor.erase(it);
    } else {
      ++it;
    }
  }

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
