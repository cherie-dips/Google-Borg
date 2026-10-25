#include "borg/scheduler.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <numeric>
#include <queue>

namespace borg {

namespace {

// Cap on cached scores; the cache is simply flushed when it grows past this.
constexpr size_t kMaxCacheEntries = 4'000'000;

uint64_t mix(uint64_t h, uint64_t v) {
    // splitmix-style combine
    v += 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
    return h ^ (v ^ (v >> 31));
}

uint64_t bits(double d) {
    uint64_t u;
    std::memcpy(&u, &d, sizeof u);
    return u;
}

uint64_t hash_str(const std::string& s) { return std::hash<std::string>{}(s); }

double clamp01(double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

}  // namespace

const char* policy_name(Policy p) {
    switch (p) {
        case Policy::WorstFit: return "worst_fit";
        case Policy::BestFit: return "best_fit";
        case Policy::Hybrid: return "hybrid";
    }
    return "?";
}

bool parse_policy(const std::string& s, Policy* out) {
    if (s == "worst_fit" || s == "worst" || s == "epvm") *out = Policy::WorstFit;
    else if (s == "best_fit" || s == "best") *out = Policy::BestFit;
    else if (s == "hybrid") *out = Policy::Hybrid;
    else return false;
    return true;
}

Scheduler::Scheduler(SchedulerConfig cfg) : cfg_(cfg), rng_(cfg.seed) {}

void Scheduler::set_config(const SchedulerConfig& cfg) {
    cfg_ = cfg;
    cache_.clear();
}

int Scheduler::protected_from(const Task& t) const {
    // Prod tasks may not preempt each other (§2.5), so everything in the prod band is protected.
    return is_prod(t.priority) ? cfg_.prod_min_priority : t.priority;
}

bool Scheduler::feasible(const Cell&, const Task& t, const Machine& m) const {
    if (!m.up) return false;
    for (const auto& c : t.constraints) {
        if (!c.hard) continue;
        auto it = m.attributes.find(c.key);
        if (it == m.attributes.end() || it->second != c.value) return false;
    }
    if (t.distinct_machines && m.job_count.count(t.job)) return false;
    Resources committed = m.committed(protected_from(t), use_reservations_for(t));
    return t.limit.fits_in(m.capacity - committed);
}

std::vector<TaskId> Scheduler::victims(const Cell& cell, const Task& t, const Machine& m) const {
    bool resv = use_reservations_for(t);
    Resources free = m.capacity - m.committed(INT_MIN, resv);
    if (t.limit.fits_in(free)) return {};

    int prot = protected_from(t);
    std::vector<const Task*> evictable;
    for (TaskId id : m.tasks) {
        const Task& o = cell.task(id);
        if (o.priority < prot) evictable.push_back(&o);
    }
    // Lowest priority first; within a priority evict the biggest first to kill fewer tasks.
    std::sort(evictable.begin(), evictable.end(), [](const Task* a, const Task* b) {
        if (a->priority != b->priority) return a->priority < b->priority;
        double sa = a->limit.cpu + a->limit.ram, sb = b->limit.cpu + b->limit.ram;
        if (sa != sb) return sa > sb;
        return a->id < b->id;
    });
    std::vector<TaskId> out;
    for (const Task* o : evictable) {
        free += resv ? o->reservation : o->limit;
        out.push_back(o->id);
        if (t.limit.fits_in(free)) break;
    }
    return out;
}

double Scheduler::score(const Cell& cell, const Task& t, const Machine& m) const {
    bool resv = use_reservations_for(t);
    Resources before = m.committed(INT_MIN, resv);
    Resources after = before;
    double preempt_cost = 0;
    for (TaskId v : victims(cell, t, m)) {
        const Task& o = cell.task(v);
        after -= resv ? o.reservation : o.limit;
        preempt_cost += cfg_.w_preemption * (1.0 + 0.01 * o.priority);
    }
    after += t.limit;

    double cap_c = std::max(m.capacity.cpu, kEps), cap_r = std::max(m.capacity.ram, kEps);
    double bu_c = clamp01(before.cpu / cap_c), bu_r = clamp01(before.ram / cap_r);
    double u_c = clamp01(after.cpu / cap_c), u_r = clamp01(after.ram / cap_r);
    double free_c = 1 - u_c, free_r = 1 - u_r;

    double base = 0;
    switch (cfg_.policy) {
        case Policy::WorstFit: {
            // E-PVM: each resource costs exp(k*utilization); choose the smallest increase in cost.
            auto cost = [&](double u) { return std::exp(cfg_.epvm_k * u); };
            double delta = (cost(u_c) - cost(bu_c)) + (cost(u_r) - cost(bu_r));
            base = -delta / std::exp(cfg_.epvm_k);
            break;
        }
        case Policy::BestFit:
            base = -(free_c + free_r);
            break;
        case Policy::Hybrid:
            // Best fit, but penalize leaving one resource free while the other is used up:
            // that leftover is "stranded" (§3.2).
            base = -(free_c + free_r) - cfg_.hybrid_alpha * std::fabs(free_c - free_r);
            break;
    }

    double s = base - preempt_cost;
    auto jc = m.job_count.find(t.job);
    if (jc != m.job_count.end()) s -= cfg_.w_spread * jc->second;
    if (!t.package.empty() && m.packages.count(t.package)) s += cfg_.w_package;
    for (const auto& c : t.constraints) {
        if (c.hard) continue;
        auto it = m.attributes.find(c.key);
        if (it != m.attributes.end() && it->second == c.value) s += cfg_.w_soft_constraint;
    }
    return s;
}

uint64_t Scheduler::class_key(const Task& t) const {
    uint64_t h = mix(0, static_cast<uint64_t>(t.job));
    h = mix(h, static_cast<uint64_t>(t.priority));
    h = mix(h, bits(t.limit.cpu));
    h = mix(h, bits(t.limit.ram));
    h = mix(h, t.distinct_machines);
    h = mix(h, hash_str(t.package));
    for (const auto& c : t.constraints) h = mix(mix(mix(h, hash_str(c.key)), hash_str(c.value)), c.hard);
    return h;
}

double Scheduler::cached_score(const Cell& cell, const Task& t, const Machine& m,
                               uint64_t key, ScheduleResult& res) {
    if (!cfg_.score_cache) {
        res.score_computations++;
        return score(cell, t, m);
    }
    uint64_t k = mix(key, static_cast<uint64_t>(m.id) + 1);
    auto it = cache_.find(k);
    if (it != cache_.end() && it->second.version == m.version) {
        res.cache_hits++;
        return it->second.score;
    }
    res.score_computations++;
    double s = score(cell, t, m);
    if (cache_.size() >= kMaxCacheEntries) cache_.clear();
    cache_[k] = {m.version, s};
    return s;
}

std::vector<Scheduler::Candidate> Scheduler::find_candidates(const Cell& cell, const Task& t,
                                                             uint64_t key, ScheduleResult& res) {
    const auto& ms = cell.machines();
    size_t n = ms.size();
    std::vector<Candidate> out;
    if (n == 0) return out;

    size_t start = 0, stride = 1;
    if (cfg_.relaxed_randomization) {
        if (perm_.size() != n) {
            perm_.resize(n);
            std::iota(perm_.begin(), perm_.end(), 0);
            std::shuffle(perm_.begin(), perm_.end(), rng_);
        }
        // Walk the machines in a fresh pseudo-random order: random start, random stride
        // co-prime with n, so every machine is visited once.
        start = rng_() % n;
        stride = 1 + rng_() % n;
        while (std::gcd(stride, n) != 1) stride = 1 + rng_() % n;
    }
    for (size_t i = 0; i < n; ++i) {
        size_t idx = cfg_.relaxed_randomization ? perm_[(start + i * stride) % n] : i;
        const Machine& m = ms[idx];
        res.feasibility_checks++;
        if (!feasible(cell, t, m)) continue;
        out.push_back({cached_score(cell, t, m, key, res), m.id, m.version});
        if (cfg_.relaxed_randomization && out.size() >= static_cast<size_t>(cfg_.sample_feasible)) break;
    }
    return out;
}

void Scheduler::place_with_preemption(Cell& cell, TaskId id, MachineId mid, ScheduleResult& res) {
    const Task& t = cell.task(id);
    for (TaskId v : victims(cell, t, cell.machine(mid))) {
        cell.unplace(v);
        cell.task(v).preemptions++;
        res.preempted.push_back({v, mid, id});
    }
    cell.place(id, mid);
    res.placed.push_back({id, mid});
}

std::vector<std::vector<TaskId>> Scheduler::order_pending(const Cell& cell) const {
    std::vector<TaskId> pend = cell.pending();
    auto by_job_index = [&](TaskId a, TaskId b) {
        const Task& x = cell.task(a);
        const Task& y = cell.task(b);
        if (x.job != y.job) return x.job < y.job;
        if (x.index != y.index) return x.index < y.index;
        return a < b;
    };
    std::sort(pend.begin(), pend.end(), by_job_index);

    // Group identical tasks (equivalence classes) or keep one task per group.
    std::vector<std::vector<TaskId>> groups;
    if (cfg_.equivalence_classes) {
        std::unordered_map<uint64_t, size_t> where;
        for (TaskId id : pend) {
            uint64_t k = class_key(cell.task(id));
            auto it = where.find(k);
            if (it == where.end()) {
                where[k] = groups.size();
                groups.push_back({id});
            } else {
                groups[it->second].push_back(id);
            }
        }
    } else {
        for (TaskId id : pend) groups.push_back({id});
    }

    // Priority high -> low; round-robin across users inside one priority so that one user's
    // huge job does not block everyone else (§3.2).
    std::map<int, std::map<std::string, std::vector<size_t>>, std::greater<int>> buckets;
    for (size_t g = 0; g < groups.size(); ++g) {
        const Task& t = cell.task(groups[g][0]);
        buckets[t.priority][t.user].push_back(g);
    }
    std::vector<std::vector<TaskId>> ordered;
    ordered.reserve(groups.size());
    for (auto& [prio, users] : buckets) {
        bool any = true;
        for (size_t round = 0; any; ++round) {
            any = false;
            for (auto& [user, gs] : users) {
                if (round < gs.size()) {
                    ordered.push_back(std::move(groups[gs[round]]));
                    any = true;
                }
            }
        }
    }
    return ordered;
}

ScheduleResult Scheduler::schedule(Cell& cell) {
    auto t0 = std::chrono::steady_clock::now();
    ScheduleResult res;

    for (auto& group : order_pending(cell)) {
        const Task& rep = cell.task(group[0]);
        uint64_t key = class_key(rep);

        if (!cfg_.equivalence_classes) {
            const Task& t = rep;
            auto cands = find_candidates(cell, t, key, res);
            if (cands.empty()) {
                res.unplaced++;
                continue;
            }
            auto best = std::max_element(cands.begin(), cands.end());
            place_with_preemption(cell, group[0], best->machine, res);
            continue;
        }

        // Equivalence class: feasibility + scoring once for the whole group, then hand out
        // machines from a heap; a machine whose state changed is re-scored when it is popped.
        std::priority_queue<Candidate> heap;
        auto refill = [&] {
            for (const auto& c : find_candidates(cell, rep, key, res)) heap.push(c);
        };
        refill();
        size_t i = 0;
        while (i < group.size()) {
            if (heap.empty()) {
                refill();
                if (heap.empty()) break;
            }
            Candidate c = heap.top();
            heap.pop();
            const Machine& m = cell.machine(c.machine);
            if (m.version != c.version) {
                res.feasibility_checks++;
                if (feasible(cell, rep, m))
                    heap.push({cached_score(cell, rep, m, key, res), m.id, m.version});
                continue;
            }
            uint64_t old_version = m.version;
            place_with_preemption(cell, group[i++], c.machine, res);
            heap.push({c.score, c.machine, old_version});  // stale: re-scored when popped
        }
        res.unplaced += group.size() - i;
    }

    res.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

ScheduleResult Scheduler::schedule_until_stable(Cell& cell, int max_rounds) {
    ScheduleResult total;
    for (int r = 0; r < max_rounds; ++r) {
        ScheduleResult res = schedule(cell);
        total.placed.insert(total.placed.end(), res.placed.begin(), res.placed.end());
        total.preempted.insert(total.preempted.end(), res.preempted.begin(), res.preempted.end());
        total.feasibility_checks += res.feasibility_checks;
        total.score_computations += res.score_computations;
        total.cache_hits += res.cache_hits;
        total.elapsed_ms += res.elapsed_ms;
        total.unplaced = res.unplaced;
        // Only preempted tasks are new to the queue; without preemptions another pass is a no-op.
        if (res.preempted.empty()) break;
    }
    total.unplaced = cell.pending_count();
    return total;
}

Stranded stranded_resources(const Cell& cell, double threshold) {
    double cap_c = 0, cap_r = 0, s_c = 0, s_r = 0;
    for (const auto& m : cell.machines()) {
        if (!m.up) continue;
        Resources used = m.total_limits();
        double free_c = std::max(0.0, m.capacity.cpu - used.cpu);
        double free_r = std::max(0.0, m.capacity.ram - used.ram);
        cap_c += m.capacity.cpu;
        cap_r += m.capacity.ram;
        if (free_c < threshold * m.capacity.cpu) s_r += free_r;
        if (free_r < threshold * m.capacity.ram) s_c += free_c;
    }
    Stranded s;
    if (cap_c > 0) s.cpu = s_c / cap_c;
    if (cap_r > 0) s.ram = s_r / cap_r;
    return s;
}

}  // namespace borg
