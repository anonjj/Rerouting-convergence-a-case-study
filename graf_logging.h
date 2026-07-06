#pragma once
// graf_logging.h — periodic metrics, cluster bytes, TSI, heartbeat, SRL, traces
// Included once from Star_mesh_simulation_code.cc after graf_recovery.h.

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
// FIX-C2: first time largestCC < aliveCount (backbone partition detected)
static double g_partitionTime = -1.0;
static bool   g_partitionTimeLogged = false;
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
  // FIX-C2: record first moment the backbone becomes partitioned
  if (aliveCount > 0 && largestCC < aliveCount && !g_partitionTimeLogged) {
    g_partitionTime = Simulator::Now().GetSeconds();
    g_partitionTimeLogged = true;
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
  if (now >= endTime || !g_chEnergyPtr || !g_chNodesPtr)
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

// ── Event-driven SRL instrumentation (FIX-A4) ─────────────────────────────
// g_sinkFirstRxAfterRecovery and g_clusterRecoveryWatchStart are declared in
// graf_globals.h so graf_recovery.h can arm them before this callback fires.

void SinkRxCallback(uint32_t sensorIdx, Ptr<const Packet> /*pkt*/,
                    const Address & /*from*/) {
  double now = Simulator::Now().GetSeconds();
  auto watchIt = g_clusterRecoveryWatchStart.find(sensorIdx);
  if (watchIt == g_clusterRecoveryWatchStart.end()) return;          // not armed
  if (g_sinkFirstRxAfterRecovery.count(sensorIdx)) return;           // already recorded
  if (now > watchIt->second) {
    g_sinkFirstRxAfterRecovery[sensorIdx] = now;
  }
}

// ── Routing Overhead Tracking ──────────────────────────────────────────────
static uint64_t g_routingOverheadBytes = 0;
static uint64_t g_routingOverheadPackets = 0;
// Per-interface AODV split (FIX-A3): backbone (iface 1) vs access-plane (iface 2)
static uint64_t g_aodvOverheadBytesBB  = 0;
static uint64_t g_aodvOverheadBytesAcc = 0;

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
      uint32_t sz = packet->GetSize() + 20; // IP header
      g_routingOverheadPackets++;
      g_routingOverheadBytes += sz;
      // Split by interface: 1 = backbone, 2 = shared access plane
      if (interface == 1)      g_aodvOverheadBytesBB  += sz;
      else if (interface == 2) g_aodvOverheadBytesAcc += sz;
    }
  }
}

void OlsrTxTrace(const ns3::olsr::PacketHeader &header,
                 const ns3::olsr::MessageList &messages) {
  g_routingOverheadPackets++;
  g_routingOverheadBytes += header.GetSerializedSize() + 28;
}
