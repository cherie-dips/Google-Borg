// The Borg scheduler (§3.2 and §3.4 of the paper).
//
// One pass over the pending queue:
//   1. order tasks by priority (high to low), round-robin across users within a priority;
//   2. feasibility: machines that satisfy the task's constraints and have enough "available"
//      resources, counting resources of lower-priority (evictable) tasks as available;
//   3. scoring: pick the best feasible machine according to the configured policy;
//   4. preemption: if the chosen machine is short, evict lower-priority tasks (lowest first)
//      and put them back in the pending queue.
#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "borg/cell.h"

namespace borg {

enum class Policy {
    WorstFit,  // E-PVM-like: spread load, minimize the increase of an exponential cost (§3.2)
    BestFit,   // pack machines as tightly as possible
    Hybrid,    // best fit that also avoids stranding one resource while another runs out
};

const char* policy_name(Policy p);
bool parse_policy(const std::string& s, Policy* out);

struct SchedulerConfig {
    Policy policy = Policy::Hybrid;
    // Priorities >= this are "prod": prod tasks never preempt each other and never rely on
    // reclaimed resources (§2.5, §5.5). 9 matches the 2011 trace's production band.
    int prod_min_priority = 9;

    // §3.4 optimizations.
    bool equivalence_classes = true;
    bool score_cache = true;
    bool relaxed_randomization = true;
    int sample_feasible = 64;  // relaxed randomization stops after this many feasible machines

    // §5.5 resource reclamation: non-prod tasks are fitted against the reservations of
    // existing tasks instead of their limits.
    bool reclamation = false;

    // Weights of the built-in score terms shared by all policies.
    double w_preemption = 100.0;  // per preempted task (plus a little per priority level); dominates
                                  // the policy terms so preemption is a last resort
    double w_spread = 0.1;       // per task of the same job already on the machine
    double w_soft_constraint = 0.3;  // per satisfied soft constraint
    double w_package = 0.2;      // bonus if the machine already has the task's package
    double hybrid_alpha = 10.0;  // weight of the imbalance (stranding) term in Hybrid; 0 = best fit.
                                 // Chosen with `borgsim alpha` (gains level off around 10)
    double epvm_k = 4.0;         // steepness of the E-PVM cost curve

    uint64_t seed = 1;
};

struct Placement {
    TaskId task;
    MachineId machine;
};

struct Preemption {
    TaskId victim;
    MachineId machine;
    TaskId by;  // the task that needed the room
};

struct ScheduleResult {
    std::vector<Placement> placed;
    std::vector<Preemption> preempted;
    size_t unplaced = 0;

    uint64_t feasibility_checks = 0;
    uint64_t score_computations = 0;
    uint64_t cache_hits = 0;
    double elapsed_ms = 0;
};

class Scheduler {
public:
    explicit Scheduler(SchedulerConfig cfg = {});

    // One pass over the pending queue. Preempted tasks become pending again; they are
    // considered on the next pass.
    ScheduleResult schedule(Cell& cell);
    // Repeats schedule() until nothing changes (or max_rounds), merging the results.
    ScheduleResult schedule_until_stable(Cell& cell, int max_rounds = 8);

    const SchedulerConfig& config() const { return cfg_; }
    void set_config(const SchedulerConfig& cfg);
    void clear_cache() { cache_.clear(); }

    bool is_prod(int priority) const { return priority >= cfg_.prod_min_priority; }

    // Exposed for tests and for the live UI.
    bool feasible(const Cell& cell, const Task& t, const Machine& m) const;
    double score(const Cell& cell, const Task& t, const Machine& m) const;

private:
    struct Candidate {
        double score;
        MachineId machine;
        uint64_t version;
        bool operator<(const Candidate& o) const {
            return score != o.score ? score < o.score : machine > o.machine;
        }
    };
    struct CacheEntry {
        uint64_t version;
        double score;
    };

    // Lower bound of the priorities that the task cannot evict.
    int protected_from(const Task& t) const;
    bool use_reservations_for(const Task& t) const { return cfg_.reclamation && !is_prod(t.priority); }
    // Lower-priority tasks that would be evicted to fit t on m, lowest priority first.
    std::vector<TaskId> victims(const Cell& cell, const Task& t, const Machine& m) const;

    std::vector<Candidate> find_candidates(const Cell& cell, const Task& t, uint64_t class_key,
                                           ScheduleResult& res);
    double cached_score(const Cell& cell, const Task& t, const Machine& m, uint64_t class_key,
                        ScheduleResult& res);
    void place_with_preemption(Cell& cell, TaskId id, MachineId m, ScheduleResult& res);
    std::vector<std::vector<TaskId>> order_pending(const Cell& cell) const;
    uint64_t class_key(const Task& t) const;

    SchedulerConfig cfg_;
    std::mt19937_64 rng_;
    std::vector<MachineId> perm_;  // random machine order for relaxed randomization
    std::unordered_map<uint64_t, CacheEntry> cache_;
};

// Fraction of free CPU/RAM that cannot be used because the other resource on the same
// machine is (almost) exhausted. Used to compare scoring policies.
struct Stranded {
    double cpu = 0;  // stranded / total capacity
    double ram = 0;
};
Stranded stranded_resources(const Cell& cell, double threshold = 0.05);

}  // namespace borg
