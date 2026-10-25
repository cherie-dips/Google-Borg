// borgmaster: Plan B's central controller (§3.1). Single replica, state in memory (no Paxos).
//
//  - accepts jobs over a REST API (used by the web UI)
//  - runs the Borg scheduler from core/ every second on the pending queue
//  - starts/kills tasks on Borglets and polls every Borglet for state and usage (§3.3)
//  - marks a machine down after missed polls and reschedules its tasks; kills stray tasks
//    on machines that come back
//  - serves the dashboard (ui/) and the Plan A results (results/)
#include <signal.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "borg/scheduler.h"
#include "httplib.h"
#include "json.hpp"

using json = nlohmann::json;
using namespace borg;

namespace {

constexpr int kProdMin = 200;    // priority bands: free <100, batch 100-199, production 200-299, monitoring 300+
constexpr int kMissesForDown = 3;
constexpr double kLostAfter = 6;  // seconds a started task may be missing from its Borglet's report

double now_s() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string band(int prio) {
    if (prio >= 300) return "monitoring";
    if (prio >= 200) return "production";
    if (prio >= 100) return "batch";
    return "free";
}

bool terminal(const std::string& s) { return s == "finished" || s == "failed" || s == "oom" || s == "killed"; }

struct JobInfo {
    JobId id = 0;
    std::string name, user, type;  // type: service (restarted on failure) | batch (runs to completion)
    int priority = 0;
    int task_count = 0;
    double cpu = 0;
    int64_t ram_mb = 0;
    json workload;
    std::vector<Constraint> constraints;
    bool distinct = false;
    double submitted = 0;
    std::vector<TaskId> tasks;
};

struct TaskInfo {
    TaskId id = 0;
    JobId job = 0;
    int index = 0;
    std::string state = "pending";  // pending, starting, installing, running, finished, failed, oom, killed
    std::string reason;
    MachineId machine = -1;         // current (or last, for finished tasks) machine
    int64_t attempt = 0;
    int restarts = 0, preemptions = 0;
    double since = 0;
    double cpu_usage = 0, mem_mb = 0;
};

struct MachineInfo {
    std::string address;
    std::string status = "up";  // up | down
    int misses = 0;
    double last_seen = 0;
    double cpu_usage = 0, mem_mb = 0;
    bool cgroups = false;
};

struct Event {
    uint64_t seq;
    double time;
    std::string kind, msg;
};

// An RPC to a Borglet, executed outside the state lock.
struct Action {
    enum Kind { Start, Kill } kind;
    MachineId machine;
    std::string address;
    TaskId task;
    int64_t attempt = 0;
    json body;
};

class Master {
public:
    explicit Master(Policy policy) {
        SchedulerConfig c;
        c.policy = policy;
        c.prod_min_priority = kProdMin;
        c.w_spread = 0.5;   // live cell: prefer spreading a job's tasks across machines
        c.w_package = 1.0;  // and machines that already have its package
        sched_.set_config(c);
    }

    void serve(int port, const std::string& ui_dir, const std::string& results_dir) {
        std::thread([this] { scheduler_loop(); }).detach();
        std::thread([this] { poll_loop(); }).detach();

        httplib::Server svr;
        svr.new_task_queue = [] { return new httplib::ThreadPool(8); };
        if (!svr.set_mount_point("/", ui_dir)) std::cerr << "warning: UI dir " << ui_dir << " not found\n";
        if (std::filesystem::exists(results_dir)) svr.set_mount_point("/results", results_dir);

        auto reply = [](httplib::Response& res, const json& j, int status = 200) {
            res.status = status;
            res.set_content(j.dump(), "application/json");
        };
        svr.Get("/api/state", [&](const httplib::Request&, httplib::Response& res) { reply(res, state_json()); });
        svr.Get("/api/events", [&](const httplib::Request& req, httplib::Response& res) {
            uint64_t since = req.has_param("since") ? std::stoull(req.get_param_value("since")) : 0;
            reply(res, events_json(since));
        });
        svr.Post("/api/jobs", [&](const httplib::Request& req, httplib::Response& res) {
            std::string err;
            auto body = json::parse(req.body, nullptr, false);
            JobId id = body.is_object() ? submit(body, &err) : (err = "invalid JSON", 0);
            if (id) reply(res, {{"id", id}});
            else reply(res, {{"error", err}}, 400);
        });
        svr.Delete(R"(/api/jobs/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
            bool ok = kill_job(std::stoll(req.matches[1].str()));
            reply(res, {{"ok", ok}}, ok ? 200 : 404);
        });
        svr.Post("/api/config", [&](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body, nullptr, false);
            Policy p;
            if (!body.is_object() || !parse_policy(body.value("policy", ""), &p)) {
                reply(res, {{"error", "policy must be worst_fit, best_fit or hybrid"}}, 400);
                return;
            }
            std::lock_guard<std::mutex> g(mu_);
            SchedulerConfig c = sched_.config();
            c.policy = p;
            sched_.set_config(c);
            event("config", std::string("scoring policy set to ") + policy_name(p));
            reply(res, {{"ok", true}});
        });
        svr.Post(R"(/api/machines/(\d+)/fail)", [&](const httplib::Request& req, httplib::Response& res) {
            MachineId m = std::stoi(req.matches[1].str());
            std::string addr;
            {
                std::lock_guard<std::mutex> g(mu_);
                if (m < 0 || m >= static_cast<MachineId>(minfo_.size())) {
                    reply(res, {{"error", "no such machine"}}, 404);
                    return;
                }
                addr = minfo_[m].address;
                event("machine", cell_.machine(m).name + ": failure injected from the UI");
            }
            httplib::Client cli(addr);
            cli.set_connection_timeout(1, 0);
            auto r = cli.Post("/admin/fail", R"({"seconds":20})", "application/json");
            reply(res, {{"ok", static_cast<bool>(r)}});
        });
        svr.Post("/api/register", [&](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body, nullptr, false);
            if (!body.is_object() || !body.contains("name") || !body.contains("address")) {
                reply(res, {{"error", "bad registration"}}, 400);
                return;
            }
            register_machine(body);
            reply(res, {{"ok", true}});
        });

        std::cout << "borgmaster listening on :" << port << " (UI at http://localhost:" << port << ")" << std::endl;
        svr.listen("0.0.0.0", port);
    }

private:
    // ---- state changes (all called with mu_ held) ------------------------------------------

    void event(const std::string& kind, const std::string& msg) {
        events_.push_back({++event_seq_, now_s(), kind, msg});
        if (events_.size() > 1000) events_.pop_front();
        std::cout << "[" << kind << "] " << msg << std::endl;
    }

    std::string task_name(TaskId id) const {
        const TaskInfo& t = tinfo_.at(id);
        return jobs_.at(t.job).name + "/" + std::to_string(t.index);
    }

    std::string machine_name(MachineId m) const { return m >= 0 ? cell_.machine(m).name : "-"; }

    void set_state(TaskInfo& t, const std::string& state, const std::string& reason) {
        t.state = state;
        t.reason = reason;
        t.since = now_s();
        if (state != "running" && state != "installing") t.cpu_usage = t.mem_mb = 0;
    }

    // Puts a placed task back in the pending queue (machine failure, preemption, restart).
    void requeue(TaskId id, const std::string& reason) {
        if (cell_.has_task(id)) cell_.unplace(id);
        set_state(tinfo_.at(id), "pending", reason);
    }

    void finish(TaskId id, const std::string& state, const std::string& reason) {
        cell_.remove_task(id);
        set_state(tinfo_.at(id), state, reason);
    }

    void register_machine(const json& b) {
        std::lock_guard<std::mutex> g(mu_);
        std::string name = b["name"];
        Resources cap{b.value("cpu", 1.0), b.value("ram", 512.0)};
        std::map<std::string, std::string> attrs;
        if (b.contains("attributes") && b["attributes"].is_object())
            for (auto& [k, v] : b["attributes"].items()) attrs[k] = v.is_string() ? v.get<std::string>() : v.dump();
        for (auto& m : cell_.machines()) {
            if (m.name != name) continue;
            minfo_[m.id].address = b["address"];
            m.capacity = cap;
            m.attributes = attrs;
            m.version++;
            return;  // the poller marks it up again on the next successful poll
        }
        MachineId id = cell_.add_machine(name, cap, attrs);
        MachineInfo mi;
        mi.address = b["address"];
        mi.last_seen = now_s();
        minfo_.push_back(mi);
        event("machine", name + " joined the cell (" + fmt(cap.cpu) + " cpu, " + fmt(cap.ram, 0) + " MB)");
        (void)id;
        wake_.notify_all();
    }

    JobId submit(const json& b, std::string* err) {
        JobInfo j;
        j.name = b.value("name", "");
        j.user = b.value("user", "user");
        j.type = b.value("type", "batch");
        j.priority = b.value("priority", 100);
        j.task_count = b.value("tasks", 1);
        j.cpu = b.value("cpu", 0.1);
        j.ram_mb = b.value("ram_mb", 64LL);
        j.workload = b.value("workload", json{{"type", j.type == "service" ? "service" : "sleep"}, {"duration", 30}});
        j.distinct = b.value("distinct", false);
        if (b.contains("constraints") && b["constraints"].is_array())
            for (const auto& c : b["constraints"])
                if (c.is_object() && !c.value("key", "").empty())
                    j.constraints.push_back({c.value("key", ""), c.value("value", ""), c.value("hard", true)});

        if (j.name.empty()) *err = "name is required";
        else if (j.task_count < 1 || j.task_count > 200) *err = "tasks must be 1..200";
        else if (j.cpu <= 0 || j.ram_mb <= 0) *err = "cpu and ram_mb must be positive";
        else if (j.type != "service" && j.type != "batch") *err = "type must be service or batch";
        if (!err->empty()) return 0;

        std::lock_guard<std::mutex> g(mu_);
        j.id = next_job_++;
        j.submitted = now_s();
        for (int i = 0; i < j.task_count; ++i) {
            Task t;
            t.job = j.id;
            t.index = i;
            t.user = j.user;
            t.priority = j.priority;
            t.limit = {j.cpu, static_cast<double>(j.ram_mb)};
            t.reservation = t.limit;
            t.constraints = j.constraints;
            t.distinct_machines = j.distinct;
            t.package = j.name;
            TaskId id = cell_.add_task(t);
            TaskInfo ti;
            ti.id = id;
            ti.job = j.id;
            ti.index = i;
            ti.since = now_s();
            ti.reason = "waiting for the scheduler";
            tinfo_[id] = ti;
            j.tasks.push_back(id);
        }
        event("job", "submitted " + j.name + " (" + std::to_string(j.task_count) + " x " + fmt(j.cpu) + " cpu / " +
                         std::to_string(j.ram_mb) + " MB, priority " + std::to_string(j.priority) + " " +
                         band(j.priority) + ", " + j.type + ")");
        JobId id = j.id;
        jobs_[id] = std::move(j);
        wake_.notify_all();
        return id;
    }

    bool kill_job(JobId id) {
        std::vector<Action> actions;
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = jobs_.find(id);
            if (it == jobs_.end()) return false;
            bool any_live = false;
            for (TaskId t : it->second.tasks) {
                TaskInfo& ti = tinfo_.at(t);
                if (terminal(ti.state)) continue;
                any_live = true;
                backoff_.erase(t);
                if (ti.machine >= 0 && ti.state != "pending")
                    actions.push_back({Action::Kill, ti.machine, minfo_[ti.machine].address, t, 0, json()});
                finish(t, "killed", "job killed by user");
            }
            if (any_live) {
                event("job", "killed " + it->second.name);
            } else {  // already done: forget it
                for (TaskId t : it->second.tasks) tinfo_.erase(t);
                event("job", "removed " + it->second.name);
                jobs_.erase(it);
            }
        }
        run_actions(actions);
        return true;
    }

    // ---- scheduler ---------------------------------------------------------------------------

    void scheduler_loop() {
        while (true) {
            std::vector<Action> actions;
            {
                std::unique_lock<std::mutex> lk(mu_);
                wake_.wait_for(lk, std::chrono::seconds(1));
                for (auto it = backoff_.begin(); it != backoff_.end();) {
                    if (it->second.first > now_s()) { ++it; continue; }
                    cell_.add_task(it->second.second);
                    it = backoff_.erase(it);
                }
                if (cell_.pending_count() == 0) continue;
                ScheduleResult r = sched_.schedule(cell_);
                last_stats_ = {{"placed", r.placed.size()},
                               {"preempted", r.preempted.size()},
                               {"unplaced", r.unplaced},
                               {"feasibility_checks", r.feasibility_checks},
                               {"score_computations", r.score_computations},
                               {"cache_hits", r.cache_hits},
                               {"elapsed_ms", r.elapsed_ms},
                               {"time", now_s()}};
                for (const auto& p : r.preempted) {
                    TaskInfo& v = tinfo_.at(p.victim);
                    v.preemptions++;
                    actions.push_back({Action::Kill, p.machine, minfo_[p.machine].address, p.victim, 0, json()});
                    std::string by = task_name(p.by);
                    set_state(v, "pending", "preempted by " + by + " on " + machine_name(p.machine));
                    event("preempt", task_name(p.victim) + " preempted on " + machine_name(p.machine) + " by " + by);
                }
                for (const auto& p : r.placed) {
                    TaskInfo& t = tinfo_.at(p.task);
                    const JobInfo& j = jobs_.at(t.job);
                    t.machine = p.machine;
                    t.attempt++;
                    set_state(t, "starting", "placed on " + machine_name(p.machine) + " (" +
                                                 policy_name(sched_.config().policy) + ")");
                    json body = {{"id", t.id},      {"attempt", t.attempt}, {"job", j.name},
                                 {"package", j.name}, {"cpu", j.cpu},       {"ram_mb", j.ram_mb},
                                 {"workload", j.workload}};
                    actions.push_back({Action::Start, p.machine, minfo_[p.machine].address, t.id, t.attempt, body});
                    event("schedule", task_name(t.id) + " -> " + machine_name(p.machine));
                }
                for (TaskId id : cell_.pending()) {
                    TaskInfo& t = tinfo_.at(id);
                    if (t.reason.rfind("preempted", 0) == 0 || t.reason.rfind("lost", 0) == 0 ||
                        t.reason.rfind("restart", 0) == 0)
                        continue;  // keep the more informative reason
                    t.reason = no_fit_reason(cell_.task(id));
                }
            }
            run_actions(actions);
        }
    }

    std::string no_fit_reason(const Task& t) const {
        int feasible_ignoring_resources = 0;
        for (const auto& m : cell_.machines()) {
            if (!m.up) continue;
            bool ok = true;
            for (const auto& c : t.constraints) {
                auto it = m.attributes.find(c.key);
                if (c.hard && (it == m.attributes.end() || it->second != c.value)) ok = false;
            }
            if (ok && t.limit.fits_in(m.capacity)) feasible_ignoring_resources++;
        }
        if (feasible_ignoring_resources == 0) return "no machine satisfies the constraints / size";
        return "waiting: no machine has enough free resources at this priority";
    }

    // Kills go first so preempted tasks release resources before new ones start.
    void run_actions(std::vector<Action>& actions) {
        std::stable_sort(actions.begin(), actions.end(),
                         [](const Action& a, const Action& b) { return a.kind == Action::Kill && b.kind != Action::Kill; });
        for (const auto& a : actions) {
            httplib::Client cli(a.address);
            cli.set_connection_timeout(1, 0);
            cli.set_read_timeout(2, 0);
            httplib::Result r = a.kind == Action::Kill ? cli.Delete("/tasks/" + std::to_string(a.task))
                                                       : cli.Post("/tasks", a.body.dump(), "application/json");
            if (a.kind == Action::Start && (!r || r->status != 200)) {
                std::lock_guard<std::mutex> g(mu_);
                auto it = tinfo_.find(a.task);
                if (it != tinfo_.end() && it->second.attempt == a.attempt && it->second.state == "starting") {
                    requeue(a.task, "restart: could not reach " + machine_name(a.machine));
                    event("error", "start of " + task_name(a.task) + " on " + machine_name(a.machine) + " failed");
                }
            }
        }
    }

    // ---- Borglet polling ---------------------------------------------------------------------

    void poll_loop() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            std::vector<std::pair<MachineId, std::string>> targets;
            {
                std::lock_guard<std::mutex> g(mu_);
                for (size_t i = 0; i < minfo_.size(); ++i) targets.push_back({static_cast<MachineId>(i), minfo_[i].address});
            }
            // Poll all Borglets in parallel so one dead machine does not slow the others.
            std::vector<std::thread> threads;
            std::vector<std::pair<MachineId, json>> results(targets.size());
            for (size_t i = 0; i < targets.size(); ++i) {
                threads.emplace_back([&, i] {
                    httplib::Client cli(targets[i].second);
                    cli.set_connection_timeout(0, 700000);
                    cli.set_read_timeout(1, 0);
                    auto r = cli.Get("/status");
                    json j = (r && r->status == 200) ? json::parse(r->body, nullptr, false) : json();
                    results[i] = {targets[i].first, j.is_discarded() ? json() : j};
                });
            }
            for (auto& t : threads) t.join();

            std::vector<Action> actions;
            {
                std::lock_guard<std::mutex> g(mu_);
                for (auto& [m, j] : results) {
                    if (j.is_object()) on_status(m, j, actions);
                    else on_missed_poll(m);
                }
            }
            run_actions(actions);
        }
    }

    void on_missed_poll(MachineId m) {
        MachineInfo& mi = minfo_[m];
        if (++mi.misses < kMissesForDown || mi.status == "down") return;
        mi.status = "down";
        mi.cpu_usage = mi.mem_mb = 0;
        cell_.set_machine_up(m, false);
        int moved = 0;
        for (auto& [id, t] : tinfo_) {
            if (t.machine == m && !terminal(t.state) && t.state != "pending") {
                requeue(id, "lost: " + machine_name(m) + " went down");
                moved++;
            }
        }
        event("machine", machine_name(m) + " is DOWN (missed " + std::to_string(kMissesForDown) + " polls); " +
                             std::to_string(moved) + " task(s) rescheduled");
        wake_.notify_all();
    }

    void on_status(MachineId m, const json& s, std::vector<Action>& actions) {
        MachineInfo& mi = minfo_[m];
        Machine& mach = cell_.machine(m);
        mi.misses = 0;
        mi.last_seen = now_s();
        mi.cgroups = s.value("cgroups", false);
        if (mi.status == "down") {
            mi.status = "up";
            cell_.set_machine_up(m, true);
            event("machine", mach.name + " is back UP");
            wake_.notify_all();
        }
        if (s.contains("usage")) {
            mi.cpu_usage = s["usage"].value("cpu", 0.0);
            mi.mem_mb = s["usage"].value("ram", 0.0);
        }
        std::unordered_set<std::string> pk;
        for (const auto& p : s.value("packages", json::array())) pk.insert(p.get<std::string>());
        if (pk != mach.packages) {
            mach.packages = std::move(pk);
            mach.version++;  // package locality is part of the score
        }

        std::set<TaskId> reported;
        for (const auto& r : s.value("tasks", json::array())) {
            TaskId id = r.value("id", 0LL);
            int64_t attempt = r.value("attempt", 0LL);
            std::string st = r.value("state", "");
            auto it = tinfo_.find(id);
            bool ours = it != tinfo_.end() && it->second.machine == m && it->second.attempt == attempt &&
                        !terminal(it->second.state) && it->second.state != "pending";
            if (!ours) {
                // Stray task (e.g. a machine that came back after we moved its work): kill it.
                if (st == "running" || st == "installing") actions.push_back({Action::Kill, m, mi.address, id, 0, json()});
                continue;
            }
            reported.insert(id);
            TaskInfo& t = it->second;
            const JobInfo& j = jobs_.at(t.job);
            std::string detail = r.value("detail", "");
            if (st == "installing" || st == "running") {
                if (t.state != st) {
                    set_state(t, st, detail);
                    if (st == "running") event("task", task_name(id) + " running on " + mach.name);
                }
                t.cpu_usage = r.value("cpu_usage", 0.0);
                t.mem_mb = r.value("mem_mb", 0.0);
            } else if (st == "finished") {
                finish(id, "finished", detail);
                event("task", task_name(id) + " finished on " + mach.name);
            } else if (st == "failed" || st == "oom" || st == "killed") {
                std::string what = st == "oom" ? "OOM-killed" : (st == "killed" ? "killed" : "failed");
                if (j.type == "service") {
                    // Restart with exponential backoff (1, 2, 4, ... 30 s) so a crash loop does not
                    // monopolize the scheduler: the task is held out of the cell until then.
                    t.restarts++;
                    double delay = std::min(30.0, std::pow(2.0, std::min(t.restarts, 6) - 1));
                    Task copy = cell_.task(id);
                    cell_.remove_task(id);
                    backoff_[id] = {now_s() + delay, copy};
                    set_state(t, "pending", "restart #" + std::to_string(t.restarts) + " in " + fmt(delay, 0) +
                                                "s (backoff) after " + what + ": " + detail);
                    event(st == "oom" ? "oom" : "task", task_name(id) + " " + what + " on " + mach.name + " (" + detail +
                                                            "); restarting in " + fmt(delay, 0) + "s");
                } else {
                    finish(id, st, detail);
                    event(st == "oom" ? "oom" : "task", task_name(id) + " " + what + " on " + mach.name + " (" + detail + ")");
                }
                wake_.notify_all();
            }
        }
        // Started tasks the Borglet no longer knows about are lost.
        for (auto& [id, t] : tinfo_) {
            if (t.machine != m || reported.count(id) || terminal(t.state) || t.state == "pending") continue;
            if (now_s() - t.since > kLostAfter) {
                requeue(id, "lost: " + mach.name + " no longer reports it");
                event("task", task_name(id) + " lost on " + mach.name + "; rescheduling");
                wake_.notify_all();
            }
        }
    }

    // ---- JSON views --------------------------------------------------------------------------

    static std::string fmt(double v, int prec = 2) {
        char b[32];
        std::snprintf(b, sizeof b, "%.*f", prec, v);
        return b;
    }

    json state_json() {
        std::lock_guard<std::mutex> g(mu_);
        json machines = json::array();
        Resources cap_total, alloc_total;
        for (const auto& m : cell_.machines()) {
            const MachineInfo& mi = minfo_[m.id];
            Resources alloc = m.total_limits(), prod = m.committed(kProdMin, false);
            if (m.up) {
                cap_total += m.capacity;
                alloc_total += alloc;
            }
            json tasks = json::array();
            for (TaskId t : m.tasks) tasks.push_back(t);
            machines.push_back({{"id", m.id},
                                {"name", m.name},
                                {"address", mi.address},
                                {"status", mi.status},
                                {"cgroups", mi.cgroups},
                                {"capacity", {{"cpu", m.capacity.cpu}, {"ram", m.capacity.ram}}},
                                {"allocated", {{"cpu", alloc.cpu}, {"ram", alloc.ram}}},
                                {"allocated_prod", {{"cpu", prod.cpu}, {"ram", prod.ram}}},
                                {"usage", {{"cpu", mi.cpu_usage}, {"ram", mi.mem_mb}}},
                                {"attributes", m.attributes},
                                {"packages", std::vector<std::string>(m.packages.begin(), m.packages.end())},
                                {"tasks", tasks}});
        }
        json jobs = json::array();
        std::map<std::string, int> totals;
        for (const auto& [id, j] : jobs_) {
            std::map<std::string, int> counts;
            for (TaskId t : j.tasks) counts[tinfo_.at(t).state]++;
            for (auto& [k, v] : counts) totals[k] += v;
            int live = 0;
            for (auto& [k, v] : counts) live += terminal(k) ? 0 : v;
            json cons = json::array();
            for (const auto& c : j.constraints) cons.push_back({{"key", c.key}, {"value", c.value}, {"hard", c.hard}});
            jobs.push_back({{"id", j.id},
                            {"name", j.name},
                            {"user", j.user},
                            {"type", j.type},
                            {"priority", j.priority},
                            {"band", band(j.priority)},
                            {"tasks", j.task_count},
                            {"cpu", j.cpu},
                            {"ram_mb", j.ram_mb},
                            {"workload", j.workload},
                            {"constraints", cons},
                            {"distinct", j.distinct},
                            {"submitted", j.submitted},
                            {"counts", counts},
                            {"done", live == 0}});
        }
        json tasks = json::array();
        for (const auto& [id, t] : tinfo_) {
            const JobInfo& j = jobs_.at(t.job);
            tasks.push_back({{"id", id},
                             {"job", t.job},
                             {"job_name", j.name},
                             {"index", t.index},
                             {"priority", j.priority},
                             {"band", band(j.priority)},
                             {"state", t.state},
                             {"reason", t.reason},
                             {"machine", t.machine},
                             {"machine_name", machine_name(t.machine)},
                             {"restarts", t.restarts},
                             {"preemptions", t.preemptions},
                             {"cpu", j.cpu},
                             {"ram_mb", j.ram_mb},
                             {"cpu_usage", t.cpu_usage},
                             {"mem_mb", t.mem_mb},
                             {"since", t.since}});
        }
        return {{"time", now_s()},
                {"policy", policy_name(sched_.config().policy)},
                {"prod_min_priority", kProdMin},
                {"capacity", {{"cpu", cap_total.cpu}, {"ram", cap_total.ram}}},
                {"allocated", {{"cpu", alloc_total.cpu}, {"ram", alloc_total.ram}}},
                {"task_counts", totals},
                {"scheduler", last_stats_},
                {"machines", machines},
                {"jobs", jobs},
                {"tasks", tasks}};
    }

    json events_json(uint64_t since) {
        std::lock_guard<std::mutex> g(mu_);
        json out = json::array();
        for (const auto& e : events_)
            if (e.seq > since) out.push_back({{"seq", e.seq}, {"time", e.time}, {"kind", e.kind}, {"msg", e.msg}});
        return {{"events", out}, {"last", event_seq_}};
    }

    std::mutex mu_;
    std::condition_variable wake_;
    Cell cell_;
    Scheduler sched_;
    std::vector<MachineInfo> minfo_;  // indexed by MachineId
    std::map<JobId, JobInfo> jobs_;
    std::map<TaskId, TaskInfo> tinfo_;
    std::map<TaskId, std::pair<double, Task>> backoff_;  // crashed service tasks waiting to restart
    std::deque<Event> events_;
    uint64_t event_seq_ = 0;
    JobId next_job_ = 1;
    json last_stats_ = json::object();
};

}  // namespace

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);
    int port = 8080;
    std::string ui = "ui", results = "results";
    Policy policy = Policy::Hybrid;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i], v = argv[i + 1];
        if (a == "--port") port = std::stoi(v);
        else if (a == "--ui") ui = v;
        else if (a == "--results") results = v;
        else if (a == "--policy" && !parse_policy(v, &policy)) {
            std::cerr << "unknown policy " << v << "\n";
            return 2;
        }
    }
    Master(policy).serve(port, ui, results);
    return 0;
}
