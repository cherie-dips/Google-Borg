// Cell compaction (§5): the smallest number of machines a workload fits into.
#pragma once

#include <vector>

#include "borg/scheduler.h"
#include "workload.h"

namespace sim {

struct PackResult {
    size_t machines = 0;
    size_t tasks = 0;
    size_t pending = 0;
    borg::ScheduleResult sched;
    borg::Stranded stranded;
};

// Random machine order used by compaction for a given seed (removing machines in random order
// keeps the cell heterogeneous).
std::vector<size_t> shuffled_order(size_t n, uint64_t seed);

// Packs the whole workload from scratch onto the first n machines of `order`
// (wrapping around and cloning machines if n > order.size()).
PackResult pack(const Workload& w, const std::vector<size_t>& order, size_t n,
                const borg::SchedulerConfig& cfg);

struct CompactionOptions {
    int seeds = 11;                    // paper: 11 runs with different seeds (1000, 1001, ...)
    double pending_tolerance = 0.002;  // paper: up to 0.2% "picky" tasks may stay pending
};

struct CompactionResult {
    std::vector<size_t> per_seed;  // machines needed for each seed
    size_t p90 = 0;                // the paper reports the 90th percentile
    size_t min = 0, max = 0;
};

// For each seed: shuffle the machines (removing them in random order keeps heterogeneity),
// then binary-search the smallest prefix that still fits the workload.
CompactionResult compact(const Workload& w, borg::SchedulerConfig cfg, const CompactionOptions& opt);

}  // namespace sim
