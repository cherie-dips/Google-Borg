#include "compaction.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace sim {

std::vector<size_t> shuffled_order(size_t n, uint64_t seed) {
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937_64 rng(seed);
    std::shuffle(order.begin(), order.end(), rng);
    return order;
}

PackResult pack(const Workload& w, const std::vector<size_t>& order, size_t n,
                const borg::SchedulerConfig& cfg) {
    borg::Cell cell;
    for (size_t i = 0; i < n; ++i) {
        const MachineSpec& m = w.machines[order[i % order.size()]];
        std::string name = i < order.size() ? m.name : m.name + "-clone" + std::to_string(i / order.size());
        cell.add_machine(name, m.capacity, m.attributes);
    }
    for (const auto& t : w.tasks) {
        borg::Task c = t;
        c.id = -1;
        cell.add_task(std::move(c));
    }
    borg::Scheduler s(cfg);
    PackResult r;
    r.machines = n;
    r.tasks = w.tasks.size();
    r.sched = s.schedule_until_stable(cell);
    r.pending = cell.pending_count();
    r.stranded = borg::stranded_resources(cell);
    return r;
}

CompactionResult compact(const Workload& w, borg::SchedulerConfig cfg, const CompactionOptions& opt) {
    CompactionResult out;
    if (w.machines.empty()) return out;
    size_t allowed = static_cast<size_t>(std::floor(opt.pending_tolerance * w.tasks.size()));

    for (int s = 0; s < opt.seeds; ++s) {
        uint64_t seed = 1000 + static_cast<uint64_t>(s);
        std::vector<size_t> order = shuffled_order(w.machines.size(), seed);
        cfg.seed = seed;

        auto fits = [&](size_t n) { return pack(w, order, n, cfg).pending <= allowed; };

        // If the original cell is too small, grow it by cloning machines (as the paper does).
        size_t hi = w.machines.size();
        while (!fits(hi)) hi *= 2;
        size_t lo = 1;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (fits(mid)) hi = mid;
            else lo = mid + 1;
        }
        out.per_seed.push_back(hi);
    }
    std::vector<size_t> sorted = out.per_seed;
    std::sort(sorted.begin(), sorted.end());
    out.min = sorted.front();
    out.max = sorted.back();
    size_t idx = static_cast<size_t>(std::ceil(0.9 * sorted.size())) - 1;
    out.p90 = sorted[std::min(idx, sorted.size() - 1)];
    return out;
}

}  // namespace sim
