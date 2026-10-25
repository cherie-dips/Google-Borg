# Plan: Replicating the Borg Scheduler (C++ Simulation + Mini Cell)

Paper: *Large-scale cluster management at Google with Borg*, Verma et al., EuroSys 2015.

> Status: Plan A and Plan B are both implemented. This file is the design record; see the
> [README](../README.md) for how to run things and for the results.

---

## 1. What the paper did

Borg is Google's cluster manager. It runs hundreds of thousands of jobs across "cells" of about 10k machines each.

**Architecture (one cell):**
- **Borgmaster:** the central controller, replicated 5× with Paxos. It accepts jobs, tracks state, and serves the UI and RPCs.
- **Scheduler:** a separate process that assigns pending tasks to machines.
- **Borglet:** an agent on every machine. It starts and stops tasks, enforces limits, and reports health when polled.
- **Fauxmaster:** a simulator that runs the real scheduler code against checkpoint data. All of the paper's evaluations used it.

**Key ideas:**
- **Priorities and bands.** prod (high priority) and non-prod (batch). Higher priority can preempt lower; prod never preempts prod.
- **Scheduling (§3.2).** Each task goes through:
  - *feasibility* (constraints plus resources, counting lower-priority tasks as evictable);
  - *scoring* (worst fit / E-PVM, best fit, or a hybrid that reduces stranded resources);
  - *preemption*, lowest priority first.
- **Scalability (§3.4).** Score caching, equivalence classes, relaxed randomization.
- **Evaluation (§5).** *Cell compaction*: the fewest machines that fit a workload. Results:
  - a shared cell beats segregated prod / non-prod cells;
  - exact requests beat bucketed ones;
  - resource reclamation saves machines.

## 2. Can we build the whole thing?

**Not meaningfully.** Paxos replication, real machines, kernel isolation at scale, naming and monitoring all need hardware or are separate projects. We built:

| Component | Built? | How |
|---|---|---|
| **Scheduler** (feasibility, scoring, preemption, §3.4 optimizations) | ✅ Plan A + B | `core/`, shared C++ library |
| **Fauxmaster-style simulator + cell compaction** | ✅ Plan A | `sim/` (`borgsim`) on the Google 2011 trace |
| Resource reclamation | ✅ Plan A | reservations from a usage model |
| Borgmaster (single replica, no Paxos) | ✅ Plan B | `master/` (`borgmaster`) |
| Borglet with cgroup v2 isolation | ✅ Plan B | `borglet/` (`borglet`), one container per machine |
| Web UI | ✅ Plan B | `ui/`, served by the master |
| Paxos replication, naming (BNS/Chubby), borgcfg | ❌ | out of scope |

## 3. Design

```
                 ┌───────────── core/ (libborg) ─────────────┐
                 │ Cell · Task · Machine · Scheduler          │
                 └──────▲───────────────────────────▲─────────┘
                        │                           │
     Plan A             │                           │            Plan B
 ┌──────────────────────┴──┐            ┌───────────┴──────────────┐   HTTP/JSON   ┌───────────┐
 │ borgsim                 │            │ borgmaster               │◀────────────▶│ borglet ×5│
 │  trace / synthetic      │            │  REST API · scheduler    │  poll 1/s     │  cgroup v2│
 │  cell compaction        │──CSV──▶    │  loop · poller · UI      │  start/kill   │  workers  │
 │  experiments            │ results/   └───────────▲──────────────┘               └───────────┘
 └─────────────────────────┘                        │ browser polls /api/state 1/s
                                                ┌───┴───┐
                                                │  UI   │  live cell + Plan A results tab
                                                └───────┘
```

**Decisions:**
- **C++17 + Makefile.** There's no CMake on the dev machine, and a Makefile works on macOS clang and in Ubuntu.
- **HTTP + JSON** via the header-only `cpp-httplib` and `nlohmann/json`, instead of gRPC. That avoids a heavy build.
- **Pluggable scoring.** The paper doesn't give the hybrid formula, so ours is `-(free_cpu + free_ram) − α·|free_cpu − free_ram|`, which is best fit plus a penalty for leaving one resource stranded.
  - We picked α = 10 with an ablation sweep (`borgsim alpha`). The gains level off there.
  - We checked that the gain comes from the stranding term itself, not from it diluting the spreading term: with spreading disabled, the result is the same.
  - The preemption cost (100 per victim) is larger than any policy term, so preemption stays a last resort.
- **Score terms shared by all policies:**
  - a preemption cost (per victim, plus a little per priority level);
  - spreading a job's tasks across machines;
  - package locality;
  - soft constraints.
- **Score cache.** Scores are keyed by (equivalence class, machine) and checked against a per-machine version counter.
- **Equivalence classes.** Tasks with the same job and requirements share one feasibility + scoring pass. A heap of candidates is lazily re-scored when a machine changes.
- **Relaxed randomization.** Walk machines in a random co-prime-stride order and stop after 64 feasible ones.

## 4. Data
- **The Google cluster trace 2011** (`clusterdata-2011-2`, cited by the paper as [80]). `tools/fetch_trace.sh` downloads `machine_events` plus the first 6 `task_events` parts (~11 MB). The snapshot is every task running at the end of the slice: 12,480 machines and ~144k tasks.
- **A synthetic generator** with the paper's workload shape, for tests and fast runs.

## 5. Experiments (Plan A)
1. Scoring policies: worst fit vs best fit vs hybrid (§3.2).
2. Shared cell vs segregated prod / non-prod (Fig. 5).
3. Exact vs power-of-two bucketed requests (Fig. 8).
4. §3.4 optimizations on and off: time, feasibility checks, score computations.
5. Resource reclamation (Fig. 10).
6. Ablation: the hybrid's stranding weight α (α = 0 is best fit).

Segregation (2) runs twice: with limits only, and with reclamation, as Borg runs. The paper attributes most of the cost of segregating to non-prod work no longer fitting into resources that prod reserves but doesn't use.

## 6. Demos (Plan B)
1. Submit jobs and watch placement, spreading and constraints (`ssd=true`).
2. A high-priority job preempts batch, which cascades onto free-tier tasks.
3. Use `docker compose stop borglet-3` or the UI **Fail** button: the machine goes DOWN after 3 missed polls, its tasks are rescheduled, and stray tasks are killed when it returns.
4. A memory hog is OOM-killed by its cgroup. A crashing service restarts with exponential backoff.
5. CPU-burning tasks are throttled to their `cpu.max` limit, visible as actual usage in the UI.
6. Switch the scoring policy live from the UI.

## 7. Problems we hit / limitations
- **No source code; hybrid formula unknown.** Ours is our own design, documented above.
- **The trace is normalized and anonymized** (CPU and RAM scaled 0–1, no constraint table loaded). Results are relative, not cores or GB.
- **The trace overcommits.** Running tasks at the snapshot request 116% of CPU, because the real Borg schedules non-prod work into reclaimed resources. Compaction therefore needs *more* machines than the original cell, so we clone machines, as the paper does.
- **Different dataset.** One public 2011 cell vs 15 internal 2014 cells: we match **trends**, not numbers.
- **No usage data loaded** (`task_usage` is huge). Reclamation uses a usage model based on Fig. 11.
- **Picky tasks.** Constrained low-priority tasks can stay pending. Like the paper, compaction tolerates 0.2% pending tasks.
- **cgroups on macOS** only exist inside Docker's Linux VM. Run natively on macOS, the Borglet falls back to no isolation and logs a warning.
- **Single master, no Paxos.** If the master restarts it loses state; Borglets re-register automatically.
