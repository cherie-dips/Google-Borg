// Unit tests for the scheduler core. Plain asserts, no framework.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "borg/scheduler.h"

using namespace borg;

namespace {

int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

Task make(JobId job, int prio, double cpu, double ram, std::string user = "u") {
    Task t;
    t.job = job;
    t.priority = prio;
    t.limit = {cpu, ram};
    t.reservation = t.limit;
    t.user = std::move(user);
    return t;
}

SchedulerConfig cfg(Policy p = Policy::Hybrid) {
    SchedulerConfig c;
    c.policy = p;
    c.prod_min_priority = 9;
    c.w_spread = 0;
    return c;
}

void test_basic_placement() {
    Cell cell;
    cell.add_machine("a", {1, 1});
    TaskId t = cell.add_task(make(1, 0, 0.5, 0.5));
    Scheduler s(cfg());
    auto r = s.schedule(cell);
    CHECK(r.placed.size() == 1);
    CHECK(cell.task(t).machine == 0);
    CHECK(cell.pending_count() == 0);
}

void test_does_not_overcommit() {
    Cell cell;
    cell.add_machine("a", {1, 1});
    for (int i = 0; i < 3; ++i) cell.add_task(make(1, 0, 0.4, 0.1));
    Scheduler s(cfg());
    s.schedule(cell);
    CHECK(cell.running_count() == 2);
    CHECK(cell.pending_count() == 1);
}

void test_constraints() {
    Cell cell;
    cell.add_machine("plain", {1, 1}, {{"ssd", "false"}});
    cell.add_machine("fast", {1, 1}, {{"ssd", "true"}});
    Task t = make(1, 0, 0.1, 0.1);
    t.constraints.push_back({"ssd", "true", true});
    TaskId id = cell.add_task(t);
    Scheduler s(cfg());
    s.schedule(cell);
    CHECK(cell.task(id).machine == 1);
}

void test_distinct_machines() {
    Cell cell;
    for (int i = 0; i < 3; ++i) cell.add_machine("m" + std::to_string(i), {1, 1});
    for (int i = 0; i < 4; ++i) {
        Task t = make(7, 0, 0.1, 0.1);
        t.index = i;
        t.distinct_machines = true;
        cell.add_task(t);
    }
    Scheduler s(cfg(Policy::BestFit));
    s.schedule(cell);
    CHECK(cell.running_count() == 3);
    for (const auto& m : cell.machines()) CHECK(m.tasks.size() == 1);
}

void test_preemption_lowest_priority_first() {
    Cell cell;
    cell.add_machine("a", {1, 1});
    TaskId low = cell.add_task(make(1, 0, 0.5, 0.5));
    TaskId mid = cell.add_task(make(2, 4, 0.5, 0.5));
    Scheduler s(cfg());
    s.schedule(cell);
    CHECK(cell.running_count() == 2);

    TaskId prod = cell.add_task(make(3, 10, 0.5, 0.5));
    auto r = s.schedule(cell);
    CHECK(cell.task(prod).machine == 0);
    CHECK(r.preempted.size() == 1);
    CHECK(!r.preempted.empty() && r.preempted[0].victim == low);
    CHECK(cell.task(low).state == TaskState::Pending);
    CHECK(cell.task(mid).state == TaskState::Running);
}

void test_prod_does_not_preempt_prod() {
    Cell cell;
    cell.add_machine("a", {1, 1});
    cell.add_task(make(1, 9, 0.8, 0.8));
    Scheduler s(cfg());
    s.schedule(cell);
    TaskId other = cell.add_task(make(2, 11, 0.5, 0.5));
    auto r = s.schedule(cell);
    CHECK(r.preempted.empty());
    CHECK(cell.task(other).state == TaskState::Pending);
}

void test_prefers_machine_without_preemption() {
    Cell cell;
    cell.add_machine("busy", {1, 1});
    cell.add_machine("idle", {1, 1});
    TaskId batch = cell.add_task(make(1, 0, 0.9, 0.9));
    Scheduler s(cfg(Policy::BestFit));
    s.schedule(cell);
    MachineId busy = cell.task(batch).machine;
    TaskId prod = cell.add_task(make(2, 10, 0.5, 0.5));
    auto r = s.schedule(cell);
    CHECK(r.preempted.empty());
    CHECK(cell.task(prod).machine != busy);
}

void test_best_fit_vs_worst_fit() {
    auto run = [](Policy p) {
        Cell cell;
        cell.add_machine("half_full", {1, 1});
        cell.add_machine("empty", {1, 1});
        TaskId first = cell.add_task(make(1, 0, 0.5, 0.5));
        cell.place(first, 0);
        TaskId t = cell.add_task(make(2, 0, 0.2, 0.2));
        Scheduler s(cfg(p));
        s.schedule(cell);
        return cell.task(t).machine;
    };
    CHECK(run(Policy::BestFit) == 0);   // pack onto the half-full machine
    CHECK(run(Policy::WorstFit) == 1);  // spread onto the empty one
}

void test_hybrid_avoids_stranding() {
    // Task is CPU-heavy. Machine 0 would end up CPU-full with RAM stranded;
    // machine 1 is RAM-heavy already, so the task balances it.
    Cell cell;
    cell.add_machine("a", {1, 1});
    cell.add_machine("b", {1, 1});
    TaskId x = cell.add_task(make(1, 0, 0.5, 0.0));
    TaskId y = cell.add_task(make(2, 0, 0.0, 0.5));
    cell.place(x, 0);
    cell.place(y, 1);
    TaskId t = cell.add_task(make(3, 0, 0.4, 0.05));
    Scheduler s(cfg(Policy::Hybrid));
    s.schedule(cell);
    CHECK(cell.task(t).machine == 1);
}

void test_down_machine_is_skipped() {
    Cell cell;
    cell.add_machine("a", {1, 1});
    cell.add_machine("b", {1, 1});
    cell.set_machine_up(0, false);
    TaskId t = cell.add_task(make(1, 0, 0.1, 0.1));
    Scheduler s(cfg());
    s.schedule(cell);
    CHECK(cell.task(t).machine == 1);
}

void test_reclamation() {
    // A prod task uses only 20% of its limit. Without reclamation a batch task cannot fit
    // next to it; with reclamation it can (it runs in the reclaimed resources).
    auto run = [](bool reclaim) {
        Cell cell;
        cell.add_machine("a", {1, 1});
        Task prod = make(1, 10, 0.8, 0.8);
        prod.reservation = {0.2, 0.2};
        cell.place(cell.add_task(prod), 0);
        TaskId batch = cell.add_task(make(2, 0, 0.5, 0.5));
        SchedulerConfig c = cfg();
        c.reclamation = reclaim;
        Scheduler s(c);
        s.schedule(cell);
        return cell.task(batch).state == TaskState::Running;
    };
    CHECK(!run(false));
    CHECK(run(true));
}

void test_round_robin_across_users() {
    // Same priority, one machine with room for 2 tasks: alice has a big job, bob a small one.
    // Round-robin must let bob in instead of giving both slots to alice.
    Cell cell;
    cell.add_machine("a", {1, 1});
    for (int i = 0; i < 5; ++i) {
        Task t = make(1, 0, 0.5, 0.1, "alice");
        t.index = i;
        cell.add_task(t);
    }
    TaskId bob = cell.add_task(make(2, 0, 0.5, 0.1, "bob"));
    SchedulerConfig c = cfg();
    c.equivalence_classes = false;
    Scheduler s(c);
    s.schedule(cell);
    CHECK(cell.task(bob).state == TaskState::Running);
}

void test_optimizations_keep_results_sane() {
    // Same workload with and without the §3.4 optimizations must place everything when it fits.
    auto build = [](Cell& cell) {
        for (int i = 0; i < 50; ++i) cell.add_machine("m" + std::to_string(i), {1, 1});
        for (int j = 0; j < 20; ++j)
            for (int i = 0; i < 20; ++i) {
                Task t = make(j, j % 12, 0.05 + 0.01 * (j % 3), 0.04);
                t.index = i;
                cell.add_task(t);
            }
    };
    for (int mask = 0; mask < 8; ++mask) {
        Cell cell;
        build(cell);
        SchedulerConfig c = cfg(Policy::BestFit);
        c.equivalence_classes = mask & 1;
        c.score_cache = mask & 2;
        c.relaxed_randomization = mask & 4;
        Scheduler s(c);
        auto r = s.schedule_until_stable(cell);
        CHECK(r.unplaced == 0);
        for (const auto& m : cell.machines()) CHECK(m.total_limits().fits_in(m.capacity));
    }
}

void test_score_cache_hits() {
    Cell cell;
    for (int i = 0; i < 10; ++i) cell.add_machine("m" + std::to_string(i), {1, 1});
    for (int i = 0; i < 30; ++i) {
        Task t = make(1, 0, 0.6, 0.6);
        t.index = i;
        cell.add_task(t);
    }
    SchedulerConfig c = cfg();
    c.equivalence_classes = false;
    c.relaxed_randomization = false;
    Scheduler s(c);
    auto r = s.schedule(cell);
    CHECK(r.placed.size() == 10);
    CHECK(r.cache_hits > 0);
}

}  // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"basic_placement", test_basic_placement},
        {"does_not_overcommit", test_does_not_overcommit},
        {"constraints", test_constraints},
        {"distinct_machines", test_distinct_machines},
        {"preemption_lowest_priority_first", test_preemption_lowest_priority_first},
        {"prod_does_not_preempt_prod", test_prod_does_not_preempt_prod},
        {"prefers_machine_without_preemption", test_prefers_machine_without_preemption},
        {"best_fit_vs_worst_fit", test_best_fit_vs_worst_fit},
        {"hybrid_avoids_stranding", test_hybrid_avoids_stranding},
        {"down_machine_is_skipped", test_down_machine_is_skipped},
        {"reclamation", test_reclamation},
        {"round_robin_across_users", test_round_robin_across_users},
        {"optimizations_keep_results_sane", test_optimizations_keep_results_sane},
        {"score_cache_hits", test_score_cache_hits},
    };
    for (const auto& [name, fn] : tests) {
        int before = failures;
        fn();
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", name);
    }
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
