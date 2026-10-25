// Core data model shared by the simulator (Plan A) and the mini-Borg cell (Plan B).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace borg {

constexpr double kEps = 1e-9;

// CPU and RAM. Units are up to the caller: cores + MB in the live cell,
// normalized 0..1 values (relative to the largest machine) in the Google trace.
struct Resources {
    double cpu = 0;
    double ram = 0;

    Resources() = default;
    Resources(double c, double r) : cpu(c), ram(r) {}

    Resources& operator+=(const Resources& o) { cpu += o.cpu; ram += o.ram; return *this; }
    Resources& operator-=(const Resources& o) { cpu -= o.cpu; ram -= o.ram; return *this; }
    friend Resources operator+(Resources a, const Resources& b) { return a += b; }
    friend Resources operator-(Resources a, const Resources& b) { return a -= b; }

    bool fits_in(const Resources& avail) const {
        return cpu <= avail.cpu + kEps && ram <= avail.ram + kEps;
    }
};

using TaskId = int64_t;
using JobId = int64_t;
using MachineId = int32_t;

// Hard constraints must match a machine attribute exactly; soft ones only affect the score.
struct Constraint {
    std::string key;
    std::string value;
    bool hard = true;
};

enum class TaskState { Pending, Running };

struct Task {
    TaskId id = -1;
    JobId job = 0;
    int index = 0;
    std::string user;
    int priority = 0;
    Resources limit;        // what the task asked for (§5.5: "limit")
    Resources reservation;  // Borgmaster's estimate of real usage (§5.5); == limit unless reclamation is used
    std::vector<Constraint> constraints;
    bool distinct_machines = false;  // no two tasks of this job on the same machine
    std::string package;             // binary package; machines that already have it start the task faster

    TaskState state = TaskState::Pending;
    MachineId machine = -1;
    int preemptions = 0;
};

struct Machine {
    MachineId id = -1;
    std::string name;
    Resources capacity;
    std::map<std::string, std::string> attributes;
    bool up = true;

    std::vector<TaskId> tasks;
    // Per-priority totals of limits and reservations, so feasibility is O(#priorities), not O(#tasks).
    std::map<int, Resources> limit_by_prio;
    std::map<int, Resources> resv_by_prio;
    std::unordered_map<JobId, int> job_count;
    std::unordered_set<std::string> packages;

    // Bumped whenever the set of tasks (or anything else a score depends on) changes.
    // Cached scores carry the version they were computed at (§3.4 "score caching").
    uint64_t version = 0;

    // Sum of limits (or reservations) of tasks with priority >= min_priority.
    Resources committed(int min_priority, bool use_reservations) const {
        const auto& m = use_reservations ? resv_by_prio : limit_by_prio;
        Resources r;
        for (auto it = m.lower_bound(min_priority); it != m.end(); ++it) r += it->second;
        return r;
    }
    Resources total_limits() const { return committed(INT32_MIN, false); }
};

}  // namespace borg
