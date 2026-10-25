// borgsim: Plan A. A Fauxmaster-like simulator that runs the Borg scheduler over a workload
// snapshot and reproduces the paper's scheduling experiments with cell compaction.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "compaction.h"
#include "workload.h"

namespace fs = std::filesystem;
using borg::Policy;
using borg::SchedulerConfig;

namespace {

struct Options {
    std::string workload = "synthetic";
    std::string trace_dir = "data/trace";
    int64_t snapshot_us = 0;
    int machines = 2000;
    double fill = 0.75;
    int seeds = 11;
    Policy policy = Policy::Hybrid;
    std::string out = "results";
    size_t scal_tasks = 20000;
    std::vector<std::string> experiments;
};

void usage() {
    std::cerr <<
R"(usage: borgsim [options] <experiment>...

experiments:
  info          workload summary
  policies      worst fit vs best fit vs hybrid scoring (§3.2)
  segregation   prod and non-prod in one cell vs separate cells (Fig. 5)
  buckets       exact requests vs power-of-two buckets (Fig. 8)
  scalability   §3.4 optimizations on/off: time, feasibility checks, score computations
  reclamation   scheduling non-prod work into reclaimed resources (Fig. 10)
  alpha         sweep of the hybrid policy's stranding weight (ablation, 3 seeds)
  all           everything above

options:
  --workload synthetic|trace   (default synthetic)
  --trace-dir DIR              Google 2011 trace slice (default data/trace)
  --snapshot-us T              snapshot time in microseconds (default: end of the slice)
  --machines N                 synthetic cell size (default 2000)
  --fill F                     synthetic CPU fill ratio (default 0.75)
  --seeds N                    compaction runs per configuration (default 11)
  --policy worst_fit|best_fit|hybrid   policy for non-policy experiments (default hybrid)
  --scal-tasks N               tasks used by the scalability experiment (default 20000)
  --out DIR                    CSV output directory (default results)
)";
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << a << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--workload") o.workload = val();
        else if (a == "--trace-dir") o.trace_dir = val();
        else if (a == "--snapshot-us") o.snapshot_us = std::stoll(val());
        else if (a == "--machines") o.machines = std::stoi(val());
        else if (a == "--fill") o.fill = std::stod(val());
        else if (a == "--seeds") o.seeds = std::stoi(val());
        else if (a == "--scal-tasks") o.scal_tasks = std::stoul(val());
        else if (a == "--out") o.out = val();
        else if (a == "--policy") {
            if (!borg::parse_policy(val(), &o.policy)) return false;
        } else if (a == "-h" || a == "--help") return false;
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown option " << a << "\n";
            return false;
        } else o.experiments.push_back(a);
    }
    return !o.experiments.empty();
}

class Csv {
public:
    Csv(const std::string& path, const std::string& header) : f_(path) { f_ << header << "\n"; }
    template <typename... A>
    void row(const A&... a) {
        std::ostringstream s;
        bool first = true;
        ((s << (first ? "" : ",") << a, first = false), ...);
        f_ << s.str() << "\n";
        f_.flush();
    }

private:
    std::ofstream f_;
};

double pct(double a, double b) { return b > 0 ? 100.0 * (a - b) / b : 0; }

std::string join(const std::vector<size_t>& v) {
    std::ostringstream s;
    for (size_t i = 0; i < v.size(); ++i) s << (i ? " " : "") << v[i];
    return s.str();
}

SchedulerConfig base_config(const Options& o, const sim::Workload& w) {
    SchedulerConfig c;
    c.policy = o.policy;
    c.prod_min_priority = w.prod_min_priority;
    return c;
}

sim::CompactionResult run_compaction(const char* label, const sim::Workload& w,
                                     const SchedulerConfig& cfg, const Options& o) {
    auto t0 = std::chrono::steady_clock::now();
    sim::CompactionOptions co;
    co.seeds = o.seeds;
    auto r = sim::compact(w, cfg, co);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  %-28s p90=%6zu machines  (min %zu, max %zu)  [%.1fs]\n", label, r.p90, r.min, r.max, secs);
    return r;
}

void exp_info(const sim::Workload& w) {
    auto cap = w.total_capacity(), lim = w.total_limits();
    std::set<borg::JobId> jobs;
    for (const auto& t : w.tasks) jobs.insert(t.job);
    std::printf("\n== info ==\n");
    std::printf("  source           %s\n", w.source.c_str());
    std::printf("  machines         %zu (capacity cpu %.1f, ram %.1f)\n", w.machines.size(), cap.cpu, cap.ram);
    std::printf("  jobs / tasks     %zu / %zu (%zu prod)\n", jobs.size(), w.tasks.size(), w.prod_tasks());
    std::printf("  requested        cpu %.1f (%.0f%%), ram %.1f (%.0f%%)\n", lim.cpu,
                100 * lim.cpu / cap.cpu, lim.ram, 100 * lim.ram / cap.ram);
}

void exp_policies(const sim::Workload& w, const Options& o) {
    std::printf("\n== policies: scoring policy vs cell size (§3.2) ==\n");
    Csv csv(o.out + "/policies.csv", "policy,p90_machines,min_machines,max_machines,stranded_cpu_pct,stranded_ram_pct,per_seed");
    size_t best_fit = 0;
    std::vector<std::pair<Policy, sim::CompactionResult>> rows;
    for (Policy p : {Policy::WorstFit, Policy::BestFit, Policy::Hybrid}) {
        SchedulerConfig cfg = base_config(o, w);
        cfg.policy = p;
        auto r = run_compaction(borg::policy_name(p), w, cfg, o);
        if (p == Policy::BestFit) best_fit = r.p90;
        rows.push_back({p, r});
    }
    // Stranded resources when every policy packs the same best-fit-sized cell (first seed's machines).
    std::vector<size_t> order = sim::shuffled_order(w.machines.size(), 1000);
    for (auto& [p, r] : rows) {
        SchedulerConfig cfg = base_config(o, w);
        cfg.policy = p;
        auto pr = sim::pack(w, order, best_fit, cfg);
        std::printf("  %-12s stranded at %zu machines: cpu %.2f%%, ram %.2f%%, pending %zu, vs best fit %+.1f%% machines\n",
                    borg::policy_name(p), best_fit, 100 * pr.stranded.cpu, 100 * pr.stranded.ram, pr.pending,
                    pct(static_cast<double>(r.p90), static_cast<double>(best_fit)));
        csv.row(borg::policy_name(p), r.p90, r.min, r.max, 100 * pr.stranded.cpu, 100 * pr.stranded.ram, join(r.per_seed));
    }
}

void exp_segregation(const sim::Workload& w0, const Options& o) {
    std::printf("\n== segregation: shared cell vs separate prod/non-prod cells (Fig. 5) ==\n");
    // Without reclamation only bin-packing gains remain; with it (as Borg runs) non-prod work in a
    // shared cell also uses resources that prod tasks reserve but do not use (§5.1).
    Csv csv(o.out + "/segregation.csv", "config,reclamation,p90_machines");
    for (bool reclaim : {false, true}) {
        sim::Workload w = w0;
        if (reclaim) sim::estimate_reservations(w, 0.1, 99);
        SchedulerConfig cfg = base_config(o, w);
        cfg.reclamation = reclaim;
        std::printf("  -- %s reclamation --\n", reclaim ? "with" : "without");
        auto both = run_compaction("prod + non-prod together", w, cfg, o);
        auto prod = run_compaction("prod only", sim::filter_band(w, true), cfg, o);
        auto nonprod = run_compaction("non-prod only", sim::filter_band(w, false), cfg, o);
        size_t seg = prod.p90 + nonprod.p90;
        std::printf("  segregated total %zu vs shared %zu  ->  %+.1f%% machines if segregated\n", seg, both.p90,
                    pct(static_cast<double>(seg), static_cast<double>(both.p90)));
        csv.row("shared", reclaim, both.p90);
        csv.row("prod_only", reclaim, prod.p90);
        csv.row("nonprod_only", reclaim, nonprod.p90);
        csv.row("segregated_total", reclaim, seg);
    }
}

void exp_buckets(const sim::Workload& w, const Options& o) {
    std::printf("\n== buckets: exact requests vs power-of-two buckets (Fig. 8) ==\n");
    SchedulerConfig cfg = base_config(o, w);
    auto exact = run_compaction("exact requests", w, cfg, o);
    Csv csv(o.out + "/buckets.csv", "variant,min_bucket,p90_machines,overhead_pct");
    csv.row("exact", 0, exact.p90, 0);
    // Normalized units: 1/64 of the biggest machine is ~0.5 core on a 32-core machine.
    for (double b : {1.0 / 256, 1.0 / 64, 1.0 / 16}) {
        char label[64];
        std::snprintf(label, sizeof label, "buckets >= 1/%.0f", 1 / b);
        auto r = run_compaction(label, sim::bucketed(w, b, b), cfg, o);
        double ov = pct(static_cast<double>(r.p90), static_cast<double>(exact.p90));
        std::printf("    overhead %+.1f%%\n", ov);
        csv.row("bucketed", b, r.p90, ov);
    }
}

void exp_scalability(const sim::Workload& full, const Options& o) {
    sim::Workload w = sim::sample_tasks(full, o.scal_tasks, 7);
    std::printf("\n== scalability: §3.4 optimizations (%zu tasks, %zu machines, one full pass) ==\n",
                w.tasks.size(), w.machines.size());
    struct V {
        const char* name;
        bool eq, cache, rnd;
    };
    const V variants[] = {{"none", false, false, false},
                          {"equivalence_classes", true, false, false},
                          {"score_cache", false, true, false},
                          {"relaxed_randomization", false, false, true},
                          {"all", true, true, true}};
    Csv csv(o.out + "/scalability.csv",
            "variant,equivalence_classes,score_cache,relaxed_randomization,elapsed_ms,feasibility_checks,score_computations,cache_hits,pending");
    std::vector<size_t> order(w.machines.size());
    std::iota(order.begin(), order.end(), 0);
    std::printf("  %-22s %10s %14s %14s %12s %8s\n", "variant", "time(ms)", "feas.checks", "scores", "cache hits", "pending");
    for (const auto& v : variants) {
        SchedulerConfig cfg = base_config(o, w);
        cfg.equivalence_classes = v.eq;
        cfg.score_cache = v.cache;
        cfg.relaxed_randomization = v.rnd;
        auto r = sim::pack(w, order, w.machines.size(), cfg);
        std::printf("  %-22s %10.0f %14llu %14llu %12llu %8zu\n", v.name, r.sched.elapsed_ms,
                    static_cast<unsigned long long>(r.sched.feasibility_checks),
                    static_cast<unsigned long long>(r.sched.score_computations),
                    static_cast<unsigned long long>(r.sched.cache_hits), r.pending);
        csv.row(v.name, v.eq, v.cache, v.rnd, r.sched.elapsed_ms, r.sched.feasibility_checks,
                r.sched.score_computations, r.sched.cache_hits, r.pending);
    }
}

void exp_alpha(const sim::Workload& w, const Options& o) {
    std::printf("\n== alpha: hybrid stranding weight (ablation; alpha = 0 is best fit) ==\n");
    Options few = o;
    few.seeds = std::min(o.seeds, 3);
    Csv csv(o.out + "/hybrid_alpha.csv", "alpha,p90_machines,min_machines,max_machines");
    for (double a : {0.0, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0}) {
        SchedulerConfig cfg = base_config(o, w);
        cfg.policy = Policy::Hybrid;
        cfg.hybrid_alpha = a;
        char label[32];
        std::snprintf(label, sizeof label, "alpha = %g", a);
        auto r = run_compaction(label, w, cfg, few);
        csv.row(a, r.p90, r.min, r.max);
    }
}

void exp_reclamation(const sim::Workload& w0, const Options& o) {
    std::printf("\n== reclamation: non-prod tasks in reclaimed resources (Fig. 10) ==\n");
    sim::Workload w = w0;
    sim::estimate_reservations(w, 0.1, 99);
    auto lim = w.total_limits(), res = w.total_reservations();
    std::printf("  reservations / limits: cpu %.0f%%, ram %.0f%% (usage model, §5.5)\n",
                100 * res.cpu / lim.cpu, 100 * res.ram / lim.ram);
    SchedulerConfig cfg = base_config(o, w);
    cfg.reclamation = false;
    auto off = run_compaction("without reclamation", w, cfg, o);
    cfg.reclamation = true;
    auto on = run_compaction("with reclamation", w, cfg, o);
    std::printf("  without reclamation the cell needs %+.1f%% machines\n",
                pct(static_cast<double>(off.p90), static_cast<double>(on.p90)));
    Csv csv(o.out + "/reclamation.csv", "variant,p90_machines");
    csv.row("without_reclamation", off.p90);
    csv.row("with_reclamation", on.p90);
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) {
        usage();
        return 2;
    }
    fs::create_directories(o.out);

    sim::Workload w;
    if (o.workload == "trace") {
        std::string log;
        w = sim::load_trace(o.trace_dir, o.snapshot_us, &log);
        std::cout << log;
        if (w.machines.empty() || w.tasks.empty()) {
            std::cerr << "no trace data under " << o.trace_dir << " (run tools/fetch_trace.sh)\n";
            return 1;
        }
    } else {
        sim::SyntheticOptions so;
        so.machines = o.machines;
        so.target_cpu_fill = o.fill;
        w = sim::synthetic(so);
    }

    std::set<std::string> want(o.experiments.begin(), o.experiments.end());
    bool all = want.count("all");
    auto on = [&](const char* e) { return all || want.count(e); };

    exp_info(w);
    if (on("policies")) exp_policies(w, o);
    if (on("segregation")) exp_segregation(w, o);
    if (on("buckets")) exp_buckets(w, o);
    if (on("scalability")) exp_scalability(w, o);
    if (on("reclamation")) exp_reclamation(w, o);
    if (on("alpha")) exp_alpha(w, o);

    std::ofstream meta(o.out + "/workload.csv");
    auto cap = w.total_capacity(), lim = w.total_limits();
    meta << "source,machines,tasks,prod_tasks,cpu_capacity,ram_capacity,cpu_requested,ram_requested,seeds,policy\n"
         << w.source << "," << w.machines.size() << "," << w.tasks.size() << "," << w.prod_tasks() << ","
         << cap.cpu << "," << cap.ram << "," << lim.cpu << "," << lim.ram << "," << o.seeds << ","
         << borg::policy_name(o.policy) << "\n";
    std::printf("\nCSV results written to %s/\n", o.out.c_str());
    return 0;
}
