#pragma once
// graf_globals.h — all shared experiment state
// Included once from Star_mesh_simulation_code.cc; never include elsewhere.

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
// GRAF-Global: maps backup CH index → its pre-selected successor CH index.
// Populated after each rehoming; consumed or invalidated on the next failure.
static std::map<uint32_t, uint32_t> g_chSuccessor;

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

// FIX-A4: event-driven SRL — declared here so graf_recovery.h can arm them
// before graf_logging.h's SinkRxCallback reads them.
static std::map<uint32_t, double> g_sinkFirstRxAfterRecovery;
static std::map<uint32_t, double> g_clusterRecoveryWatchStart;
