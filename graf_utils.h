#pragma once
// graf_utils.h — topology helpers (adjacency, BFS, betweenness, coverage)
// Included once from Star_mesh_simulation_code.cc after graf_globals.h.

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
