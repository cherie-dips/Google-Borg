#include "workload.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace sim {

size_t Workload::prod_tasks() const {
    size_t n = 0;
    for (const auto& t : tasks) n += t.priority >= prod_min_priority;
    return n;
}

borg::Resources Workload::total_capacity() const {
    borg::Resources r;
    for (const auto& m : machines) r += m.capacity;
    return r;
}

borg::Resources Workload::total_limits() const {
    borg::Resources r;
    for (const auto& t : tasks) r += t.limit;
    return r;
}

borg::Resources Workload::total_reservations() const {
    borg::Resources r;
    for (const auto& t : tasks) r += t.reservation;
    return r;
}

// ---------------------------------------------------------------------------------------------
// Google trace 2011
// ---------------------------------------------------------------------------------------------

namespace {

// Reads a .csv or .csv.gz file line by line (gzip is decompressed through `gzip -dc`).
class LineReader {
public:
    explicit LineReader(const std::string& path) {
        if (path.size() > 3 && path.compare(path.size() - 3, 3, ".gz") == 0) {
            std::string cmd = "gzip -dc '" + path + "'";
            pipe_ = popen(cmd.c_str(), "r");
        } else {
            file_ = std::fopen(path.c_str(), "r");
        }
    }
    ~LineReader() {
        if (pipe_) pclose(pipe_);
        if (file_) std::fclose(file_);
    }
    bool ok() const { return pipe_ || file_; }
    bool next(std::string& line) {
        FILE* f = pipe_ ? pipe_ : file_;
        line.clear();
        char buf[4096];
        while (std::fgets(buf, sizeof buf, f)) {
            line += buf;
            if (!line.empty() && line.back() == '\n') {
                line.pop_back();
                return true;
            }
        }
        return !line.empty();
    }

private:
    FILE* pipe_ = nullptr;
    FILE* file_ = nullptr;
};

void split(const std::string& s, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    while (true) {
        size_t p = s.find(',', start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            return;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
}

std::vector<std::string> csv_files(const std::string& dir) {
    std::vector<std::string> out;
    if (!fs::exists(dir)) return out;
    for (const auto& e : fs::directory_iterator(dir)) {
        std::string p = e.path().string();
        if (p.find(".csv") != std::string::npos) out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    return out;
}

struct TraceTask {
    bool running = false;
    int priority = 0;
    double cpu = 0, ram = 0;
    bool distinct = false;
    std::string user;
};

}  // namespace

Workload load_trace(const std::string& dir, int64_t snapshot_time, std::string* log) {
    Workload w;
    w.source = "trace";
    w.prod_min_priority = 9;
    std::ostringstream msg;

    // task_events: timestamp, missing info, job ID, task index, machine ID, event type, user,
    // scheduling class, priority, CPU request, memory request, disk request, different-machine
    std::unordered_map<uint64_t, TraceTask> tasks;
    std::unordered_map<uint64_t, uint64_t> task_job;  // key -> job id
    int64_t max_ts = 0;
    size_t events = 0;
    std::vector<std::string> f;
    std::string line;
    auto task_files = csv_files(dir + "/task_events");
    for (const auto& path : task_files) {
        LineReader r(path);
        if (!r.ok()) continue;
        while (r.next(line)) {
            split(line, f);
            if (f.size() < 13) continue;
            int64_t ts = std::stoll(f[0]);
            if (snapshot_time > 0 && ts > snapshot_time) continue;
            max_ts = std::max(max_ts, ts);
            uint64_t job = std::stoull(f[2]);
            uint64_t idx = std::stoull(f[3]);
            uint64_t key = (job << 20) ^ idx;
            TraceTask& t = tasks[key];
            task_job[key] = job;
            int type = std::stoi(f[5]);
            if (!f[6].empty()) t.user = f[6];
            if (!f[8].empty()) t.priority = std::stoi(f[8]);
            if (!f[9].empty()) t.cpu = std::stod(f[9]);
            if (!f[10].empty()) t.ram = std::stod(f[10]);
            if (!f[12].empty()) t.distinct = f[12] == "1";
            if (type == 1) t.running = true;                       // SCHEDULE
            else if (type >= 0 && type <= 6) t.running = false;    // SUBMIT/EVICT/FAIL/FINISH/KILL/LOST
            // 7/8 = UPDATE_PENDING/UPDATE_RUNNING: only the fields above change
            events++;
        }
    }
    int64_t T = snapshot_time > 0 ? snapshot_time : max_ts;
    msg << "trace: " << task_files.size() << " task_events files, " << events
        << " events, snapshot at t=" << T / 1000000 << "s\n";

    // machine_events: timestamp, machine ID, event type (0 add, 1 remove, 2 update), platform, CPUs, memory
    struct M {
        double cpu = 0, ram = 0;
        std::string platform;
        bool present = false;
    };
    std::unordered_map<uint64_t, M> machines;
    for (const auto& path : csv_files(dir + "/machine_events")) {
        LineReader r(path);
        if (!r.ok()) continue;
        while (r.next(line)) {
            split(line, f);
            if (f.size() < 6) continue;
            int64_t ts = std::stoll(f[0]);
            if (ts > T) continue;
            M& m = machines[std::stoull(f[1])];
            int type = std::stoi(f[2]);
            if (type == 1) {
                m.present = false;
                continue;
            }
            m.present = true;
            if (!f[3].empty()) m.platform = f[3];
            if (!f[4].empty()) m.cpu = std::stod(f[4]);
            if (!f[5].empty()) m.ram = std::stod(f[5]);
        }
    }
    std::unordered_map<std::string, std::string> platform_names;
    std::vector<uint64_t> ids;
    for (const auto& [id, m] : machines)
        if (m.present && m.cpu > 0 && m.ram > 0) ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    for (uint64_t id : ids) {
        const M& m = machines[id];
        auto& pn = platform_names[m.platform];
        if (pn.empty()) pn = "p" + std::to_string(platform_names.size() - 1);
        w.machines.push_back({"m" + std::to_string(id), {m.cpu, m.ram}, {{"platform", pn}}});
    }

    std::vector<uint64_t> keys;
    for (const auto& [k, t] : tasks)
        if (t.running && t.cpu > 0 && t.ram > 0) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    size_t skipped = 0;
    for (uint64_t k : keys) {
        const TraceTask& tt = tasks[k];
        borg::Task t;
        t.job = static_cast<borg::JobId>(task_job[k]);
        t.index = static_cast<int>(k & ((1u << 20) - 1));
        t.user = tt.user;
        t.priority = tt.priority;
        t.limit = {tt.cpu, tt.ram};
        t.reservation = t.limit;
        t.distinct_machines = tt.distinct;
        w.tasks.push_back(std::move(t));
    }
    size_t no_request = 0;
    for (const auto& [k, t] : tasks) no_request += t.running && !(t.cpu > 0 && t.ram > 0);
    skipped = no_request;
    msg << "trace: " << w.machines.size() << " machines, " << w.tasks.size()
        << " running tasks (" << w.prod_tasks() << " prod), " << skipped
        << " running tasks skipped for missing requests\n";
    if (log) *log = msg.str();
    return w;
}

// ---------------------------------------------------------------------------------------------
// Synthetic workload
// ---------------------------------------------------------------------------------------------

Workload synthetic(const SyntheticOptions& opt) {
    Workload w;
    w.source = "synthetic";
    w.prod_min_priority = 9;
    std::mt19937_64 rng(opt.seed);
    std::uniform_real_distribution<double> U(0, 1);
    auto uni = [&](double a, double b) { return a + (b - a) * U(rng); };

    // Machine mix loosely follows the 2011 trace (normalized to the largest machine).
    struct Type {
        const char* platform;
        double cpu, ram, share;
    };
    const Type types[] = {{"A", 0.5, 0.5, 0.53}, {"B", 0.5, 0.25, 0.31}, {"C", 0.5, 0.75, 0.08},
                          {"D", 1.0, 1.0, 0.06}, {"E", 0.25, 0.25, 0.02}};
    for (int i = 0; i < opt.machines; ++i) {
        double r = U(rng), acc = 0;
        const Type* ty = &types[0];
        for (const auto& t : types) {
            acc += t.share;
            if (r <= acc) {
                ty = &t;
                break;
            }
        }
        w.machines.push_back({"m" + std::to_string(i),
                              {ty->cpu, ty->ram},
                              {{"platform", ty->platform},
                               {"ssd", U(rng) < 0.3 ? "true" : "false"},
                               {"rack", "r" + std::to_string(i / 40)}}});
    }

    double cpu_target = opt.target_cpu_fill * w.total_capacity().cpu;
    double cpu_total = 0;
    const int users = 60;
    borg::JobId job = 1;
    while (cpu_total < cpu_target) {
        borg::Task t;
        t.job = job++;
        // Zipf-ish users: a few heavy users, a long tail.
        t.user = "user" + std::to_string(static_cast<int>(users * std::pow(U(rng), 2.0)));
        int n;
        double r = U(rng);
        if (r < 0.03) {  // large prod tasks that need most of a machine
            t.priority = 10;
            n = 1 + static_cast<int>(uni(0, 20));
            t.limit = {uni(0.15, 0.4), uni(0.1, 0.4)};
            t.distinct_machines = true;
        } else if (r < 0.28) {  // prod services
            t.priority = U(rng) < 0.8 ? 9 : 11;
            n = 1 + static_cast<int>(std::exp(uni(0, 5)));
            double c = uni(0.01, 0.08);
            t.limit = {c, c * uni(0.4, 1.6)};
            t.distinct_machines = U(rng) < 0.5;
        } else {  // non-prod batch
            const int prios[] = {0, 0, 1, 2, 2, 4, 6, 8};
            t.priority = prios[static_cast<int>(uni(0, 8))];
            n = 1 + static_cast<int>(std::exp(uni(0, 5.5)));
            // 20% of non-prod tasks request less than 0.1 cores (~0.003 normalized).
            double c = U(rng) < 0.2 ? uni(0.0005, 0.003) : uni(0.004, 0.05);
            t.limit = {c, std::min(0.2, c * uni(0.3, 1.5))};
        }
        // Placement constraints: mostly on prod jobs; batch constraints are rarer and often soft
        // (the paper turns very picky constraints soft for compaction, §5).
        bool prod = t.priority >= w.prod_min_priority;
        if (U(rng) < (prod ? 0.10 : 0.04)) {
            bool hard = prod || U(rng) < 0.3;
            if (U(rng) < 0.5) t.constraints.push_back({"ssd", "true", hard});
            else t.constraints.push_back({"platform", U(rng) < 0.5 ? "A" : "B", hard});
        }
        t.reservation = t.limit;
        for (int i = 0; i < n && cpu_total < cpu_target; ++i) {
            borg::Task c = t;
            c.index = i;
            w.tasks.push_back(c);
            cpu_total += c.limit.cpu;
        }
    }
    return w;
}

void estimate_reservations(Workload& w, double safety_margin, uint64_t seed) {
    // Usage/limit ratios roughly as in Fig. 11: prod tasks use a small share of their limit,
    // batch tasks somewhat more.
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> U(0, 1);
    for (auto& t : w.tasks) {
        bool prod = t.priority >= w.prod_min_priority;
        double uc = prod ? 0.15 + 0.45 * U(rng) : 0.3 + 0.6 * U(rng);
        double ur = prod ? 0.4 + 0.4 * U(rng) : 0.5 + 0.4 * U(rng);
        t.reservation = {std::min(t.limit.cpu, t.limit.cpu * uc * (1 + safety_margin)),
                         std::min(t.limit.ram, t.limit.ram * ur * (1 + safety_margin))};
    }
}

Workload bucketed(const Workload& w, double min_cpu, double min_ram) {
    Workload out = w;
    auto up = [](double x, double b) {
        while (b < x - borg::kEps) b *= 2;
        return b;
    };
    for (auto& t : out.tasks) {
        t.limit = {up(t.limit.cpu, min_cpu), up(t.limit.ram, min_ram)};
        t.reservation = t.limit;
    }
    return out;
}

Workload filter_band(const Workload& w, bool prod) {
    Workload out = w;
    out.tasks.clear();
    for (const auto& t : w.tasks)
        if ((t.priority >= w.prod_min_priority) == prod) out.tasks.push_back(t);
    return out;
}

Workload sample_tasks(const Workload& w, size_t n, uint64_t seed) {
    if (n >= w.tasks.size()) return w;
    Workload out = w;
    std::mt19937_64 rng(seed);
    std::shuffle(out.tasks.begin(), out.tasks.end(), rng);
    out.tasks.resize(n);
    return out;
}

}  // namespace sim
