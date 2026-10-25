// Workloads for the simulator: a point-in-time snapshot of machines + tasks (§5 "Evaluation
// methodology": the paper also packs a snapshot rather than replaying a long trace).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "borg/cell.h"

namespace sim {

struct MachineSpec {
    std::string name;
    borg::Resources capacity;
    std::map<std::string, std::string> attributes;
};

struct Workload {
    std::string source;  // "trace" or "synthetic"
    int prod_min_priority = 9;
    std::vector<MachineSpec> machines;
    std::vector<borg::Task> tasks;  // ids are ignored; the cell assigns fresh ones

    size_t prod_tasks() const;
    borg::Resources total_capacity() const;
    borg::Resources total_limits() const;
    borg::Resources total_reservations() const;
};

// Google cluster trace 2011 (clusterdata-2011-2). Reads machine_events/*.csv[.gz] and
// task_events/*.csv[.gz] under dir and returns the tasks running at snapshot_time
// (microseconds; <= 0 means "end of the loaded task_events files").
Workload load_trace(const std::string& dir, int64_t snapshot_time, std::string* log);

struct SyntheticOptions {
    int machines = 2000;
    double target_cpu_fill = 0.75;  // total CPU limits / total CPU capacity
    uint64_t seed = 42;
};
// A workload shaped like the paper's description: heterogeneous machines, prod services with
// spread constraints, lots of small batch tasks (20% of non-prod tasks ask for <0.1 cores).
Workload synthetic(const SyntheticOptions& opt);

// Fills Task::reservation from a usage model when the source has no usage data (§5.5:
// reservation = usage + safety margin, never above the limit).
void estimate_reservations(Workload& w, double safety_margin, uint64_t seed);

// Rounds each request up to a power-of-two bucket (Fig. 8). min_bucket is the smallest bucket.
Workload bucketed(const Workload& w, double min_cpu_bucket, double min_ram_bucket);

// Keeps only prod (or only non-prod) tasks.
Workload filter_band(const Workload& w, bool prod);

// Random subset of tasks (for the slow, unoptimized scalability runs).
Workload sample_tasks(const Workload& w, size_t n, uint64_t seed);

}  // namespace sim
