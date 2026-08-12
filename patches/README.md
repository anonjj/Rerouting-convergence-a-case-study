# NS-3.39 patches required to reproduce these results

**The simulation will build and run without this patch, but the CH energy figures it
produces will be wrong.** Nothing else is affected — PDR, throughput, delay, service
restoration latency, and fairness all come from FlowMonitor and application traces and are
unaffected by the defect described below.

## `wifi-radio-energy-model.patch`

### What it changes

Removes a redundant self-scheduled shutoff timer from `WifiRadioEnergyModel` at three call
sites in `src/wifi/model/wifi-radio-energy-model.cc`:

| Function | ns-3.39 line | What was removed |
|---|---|---|
| `SetEnergySource()` | ~119 | initial arming of `m_switchToOffEvent` |
| `ChangeState()` | ~300 | re-arm on every radio state transition |
| `HandleEnergyChanged()` | ~376 | re-arm on every remaining-energy delta |

### The defect

On each radio state transition, stock `WifiRadioEnergyModel` schedules
`ChangeState(OFF)` at `now + GetMaximumTimeInState(newState)` — that is, "how long until
this node depletes *if the state it just entered persisted forever*". A ~1 ms CCA_BUSY or
RX burst is therefore treated as permanent, which can compute a window far shorter than the
true time to depletion. The higher the per-state current, the shorter the window, so the
problem worsens with `--drainMult`.

If that window elapses before the next real transition arrives — an ordinary traffic lull
is enough — the timer fires `ChangeState(OFF)` **directly on the energy model**, bypassing
the `WifiPhy` entirely. The real radio keeps transmitting and receiving normally.

It then latches. `SetWifiRadioState()` is guarded by
`if (m_currentState != WifiPhyState::OFF)`, so no subsequent legitimate `ChangeState()`
call can ever move the model off `OFF` again. Because `GetStateA(OFF) == 0`, all further
energy billing for that node is silently zeroed for the remainder of the run, with no
inconsistency visible at the PHY layer.

Confirmed directly by PHY-state trace instrumentation (DIAG-E3): CH2's access radio logged
**774 real state transitions after t = 45.49 s** while its energy trace had already frozen
at t = 45.49 s.

This is what produced the artifacts the earlier revision of this work reported as findings:
bit-identical CH energy between arms despite GRAF sending ~1,864 additional backbone
frames, and a CH killed at 29 s appearing to consume more energy than one that ran the full
300 s.

### Why removing it is safe

Real depletion is already handled independently and correctly by
`BasicEnergySource::UpdateEnergySource()`, via its own low/high battery threshold
hysteresis, evaluated on every update and every `PeriodicEnergyUpdateInterval`. When the
source genuinely crosses its threshold it invokes the model's `HandleEnergyDepletion()`,
which calls the installed depletion callback (`WifiPhy::SetOffMode` by default).

The removed watchdog was a second, heuristic path to the same outcome — redundant when it
was right and, as described above, harmful when it was wrong.

This simulation installs `BasicEnergySourceHelper` on all three node tiers — sensors
(`Star_mesh_simulation_code.cc` L450), CHs (L475), and the gateway (L497) — so that correct
depletion path is active for every node in every run.

### Applying it

From the ns-3.39 source root:

```bash
cd ~/ns-allinone-3.39/ns-3.39
cp src/wifi/model/wifi-radio-energy-model.cc \
   src/wifi/model/wifi-radio-energy-model.cc.ORIGINAL   # keep a revert copy
patch -p1 < /path/to/patches/wifi-radio-energy-model.patch
./ns3 build
```

Verify it applied:

```bash
grep -c 'FIX-E3' src/wifi/model/wifi-radio-energy-model.cc      # expect 3
grep -n 'm_switchToOffEvent' src/wifi/model/wifi-radio-energy-model.cc
```

The second command must show **no** remaining `Simulator::Schedule(...)` arming
`m_switchToOffEvent`. Any surviving arm site reintroduces the defect.

To revert, restore the `.ORIGINAL` copy and rebuild.

### Version

Written against **ns-3.39**. The affected code is upstream ns-3 and has changed across
releases; on any other version, apply by hand using the table above rather than forcing
this patch.
