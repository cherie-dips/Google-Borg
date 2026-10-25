// borglet: the per-machine agent (§3.3). Starts and stops tasks, enforces their CPU/RAM
// limits with cgroup v2, and reports state + usage when the Borgmaster polls it.
//
// HTTP API (called by the Borgmaster):
//   POST   /tasks         start a task {id, attempt, job, package, cpu, ram_mb, workload}
//   DELETE /tasks/<id>    stop a task (SIGTERM, then SIGKILL after a grace period)
//   GET    /status        machine + task state and usage
//   POST   /admin/fail    simulate a machine failure: kill everything, go silent for N seconds
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cgroups.h"
#include "httplib.h"
#include "json.hpp"

int run_worker(int argc, char** argv);

using json = nlohmann::json;
using borglet::Cgroups;

namespace {

double now_s() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string env(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v && *v ? v : def;
}

struct Config {
    std::string name;
    int port = 9090;
    std::string master = "http://localhost:8080";
    std::string advertise;  // how the master reaches us
    double cpu = 1;
    int64_t ram_mb = 512;
    std::map<std::string, std::string> attrs;
    double install_secs = 2;  // simulated package download on first use (§3.2 package locality)
};

struct LocalTask {
    int64_t id = 0;
    int64_t attempt = 0;
    std::string job, package;
    double cpu = 0;
    int64_t ram_mb = 0;
    json workload;

    std::string state;  // installing, running, finished, failed, oom, killed
    std::string detail;
    int exit_code = 0;
    pid_t pid = -1;
    std::string cgroup;
    double start_at = 0, started = 0, ended = 0;
    bool kill_requested = false;
    double kill_deadline = 0;

    uint64_t last_cpu_usec = 0;
    double last_sample = 0;
    double cpu_usage = 0;  // cores
    double mem_mb = 0;

    bool terminal() const { return state == "finished" || state == "failed" || state == "oom" || state == "killed"; }
};

class Borglet {
public:
    Borglet(Config cfg, std::string self_exe) : cfg_(std::move(cfg)), self_exe_(std::move(self_exe)) {}

    void run() {
        std::string why;
        if (cgroups_.init(&why)) std::cout << "borglet: cgroup v2 isolation enabled" << std::endl;
        else std::cout << "borglet: WARNING running without isolation: " << why << std::endl;

        std::thread([this] { monitor_loop(); }).detach();
        std::thread([this] { register_loop(); }).detach();

        httplib::Server svr;
        svr.set_pre_routing_handler([this](const httplib::Request&, httplib::Response& res) {
            if (offline()) {
                res.status = 503;
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        svr.Get("/status", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(status().dump(), "application/json");
        });
        svr.Post("/tasks", [this](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.contains("id")) {
                res.status = 400;
                return;
            }
            start_task(body);
            res.set_content(R"({"ok":true})", "application/json");
        });
        svr.Delete(R"(/tasks/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            kill_task(std::stoll(req.matches[1].str()), "killed by master");
            res.set_content(R"({"ok":true})", "application/json");
        });
        svr.Post("/admin/fail", [this](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body, nullptr, false);
            double secs = body.is_object() ? body.value("seconds", 20.0) : 20.0;
            res.set_content(R"({"ok":true})", "application/json");
            fail(secs);
        });
        svr.Get("/healthz", [](const httplib::Request&, httplib::Response& res) { res.set_content("ok", "text/plain"); });

        std::cout << "borglet " << cfg_.name << " listening on :" << cfg_.port << " (cpu " << cfg_.cpu
                  << ", ram " << cfg_.ram_mb << " MB), master " << cfg_.master << std::endl;
        svr.listen("0.0.0.0", cfg_.port);
    }

private:
    bool offline() const { return now_s() < offline_until_.load(); }

    json status() {
        std::lock_guard<std::mutex> g(mu_);
        last_poll_ = now_s();
        json tasks = json::array();
        double cpu = 0, mem = 0;
        for (const auto& [id, t] : tasks_) {
            tasks.push_back({{"id", t.id}, {"attempt", t.attempt}, {"state", t.state}, {"detail", t.detail},
                             {"exit_code", t.exit_code}, {"pid", t.pid}, {"cpu_usage", t.cpu_usage},
                             {"mem_mb", t.mem_mb}});
            if (!t.terminal()) {
                cpu += t.cpu_usage;
                mem += t.mem_mb;
            }
        }
        return {{"name", cfg_.name},
                {"capacity", {{"cpu", cfg_.cpu}, {"ram", cfg_.ram_mb}}},
                {"attributes", cfg_.attrs},
                {"cgroups", cgroups_.enabled()},
                {"packages", packages_},
                {"usage", {{"cpu", cpu}, {"ram", mem}}},
                {"tasks", tasks}};
    }

    void start_task(const json& b) {
        std::lock_guard<std::mutex> g(mu_);
        int64_t id = b["id"].get<int64_t>();
        int64_t attempt = b.value("attempt", 0LL);
        auto it = tasks_.find(id);
        if (it != tasks_.end()) {
            if (!it->second.terminal() && it->second.attempt == attempt) return;  // duplicate request
            if (!it->second.terminal()) kill_locked(it->second, "replaced by a newer attempt");
            cleanup_locked(it->second);
        }
        LocalTask t;
        t.id = id;
        t.attempt = attempt;
        t.job = b.value("job", "");
        t.package = b.value("package", t.job);
        t.cpu = b.value("cpu", 0.1);
        t.ram_mb = b.value("ram_mb", 64LL);
        t.workload = b.value("workload", json::object());
        bool cached = packages_.count(t.package) > 0;
        t.state = cached ? "running" : "installing";
        t.start_at = now_s() + (cached ? 0 : cfg_.install_secs);
        t.detail = cached ? "package cached" : "installing package " + t.package;
        tasks_[id] = std::move(t);
    }

    void kill_task(int64_t id, const std::string& why) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = tasks_.find(id);
        if (it != tasks_.end()) kill_locked(it->second, why);
    }

    void kill_locked(LocalTask& t, const std::string& why) {
        if (t.terminal()) return;
        t.detail = why;
        if (t.pid <= 0) {  // still installing
            t.state = "killed";
            t.ended = now_s();
            return;
        }
        t.kill_requested = true;
        t.kill_deadline = now_s() + 3;  // grace period after SIGTERM (§2.5)
        ::kill(-t.pid, SIGTERM);
    }

    void cleanup_locked(LocalTask& t) {
        if (t.pid > 0) {
            ::kill(-t.pid, SIGKILL);
            ::waitpid(t.pid, nullptr, 0);
            t.pid = -1;
        }
        cgroups_.remove(t.cgroup);
        t.cgroup.clear();
    }

    void fail(double secs) {
        std::cout << "borglet: simulating machine failure for " << secs << "s" << std::endl;
        offline_until_ = now_s() + secs;
        std::lock_guard<std::mutex> g(mu_);
        for (auto& [id, t] : tasks_) cleanup_locked(t);
        tasks_.clear();
        packages_.clear();  // a "rebooted" machine has a cold package cache
        registered_ = false;
    }

    void launch_locked(LocalTask& t) {
        std::string type = t.workload.value("type", "service");
        std::vector<std::string> args = {self_exe_, "--worker", type};
        double secs = t.workload.value("duration", 0.0);
        if (type == "sleep" || type == "crash") args.push_back(std::to_string(secs));
        else if (type == "cpu") {
            args.push_back(std::to_string(t.workload.value("threads", 1)));
            args.push_back(std::to_string(secs));
        } else if (type == "mem") {
            args.push_back(std::to_string(t.workload.value("mb", 100)));
            args.push_back(std::to_string(secs));
        }

        t.cgroup = cgroups_.create(std::to_string(t.id), t.cpu, t.ram_mb * 1024 * 1024);
        std::string procs = t.cgroup.empty() ? "" : t.cgroup + "/cgroup.procs";
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);

        pid_t pid = ::fork();
        if (pid == 0) {
            ::setpgid(0, 0);
            if (!procs.empty() && !Cgroups::join(procs.c_str())) _exit(126);
            ::execv(self_exe_.c_str(), argv.data());
            _exit(127);
        }
        if (pid < 0) {
            t.state = "failed";
            t.detail = "fork failed";
            t.ended = now_s();
            return;
        }
        ::setpgid(pid, pid);
        t.pid = pid;
        t.state = "running";
        t.started = now_s();
        t.detail = cgroups_.enabled() ? "running in " + t.cgroup : "running (no isolation)";
    }

    void monitor_loop() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::lock_guard<std::mutex> g(mu_);
            double now = now_s();
            for (auto it = tasks_.begin(); it != tasks_.end();) {
                LocalTask& t = it->second;
                if (!t.terminal() && t.pid < 0 && now >= t.start_at) {
                    packages_.insert(t.package);
                    launch_locked(t);
                }
                if (t.pid > 0) {
                    int st = 0;
                    if (::waitpid(t.pid, &st, WNOHANG) == t.pid) on_exit_locked(t, st);
                    else if (t.kill_requested && now > t.kill_deadline) ::kill(-t.pid, SIGKILL);
                }
                if (t.pid > 0 && now - t.last_sample >= 1.0) sample_locked(t, now);
                // Keep finished tasks around long enough for the master to see them.
                if (t.terminal() && now - t.ended > 120) {
                    it = tasks_.erase(it);
                    continue;
                }
                ++it;
            }
        }
    }

    void on_exit_locked(LocalTask& t, int st) {
        Cgroups::Stats s;
        cgroups_.stats(t.cgroup, &s);
        t.pid = -1;
        t.ended = now_s();
        t.cpu_usage = 0;
        t.mem_mb = 0;
        if (t.kill_requested) {
            t.state = "killed";
        } else if (s.oom_kills > 0) {
            t.state = "oom";
            t.detail = "OOM-killed: exceeded memory limit of " + std::to_string(t.ram_mb) + " MB";
        } else if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            t.state = "finished";
            t.detail = "exited 0";
        } else {
            t.state = "failed";
            t.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            t.detail = WIFEXITED(st) ? "exited " + std::to_string(WEXITSTATUS(st))
                                     : "killed by signal " + std::to_string(WTERMSIG(st));
        }
        cgroups_.remove(t.cgroup);
        t.cgroup.clear();
    }

    void sample_locked(LocalTask& t, double now) {
        Cgroups::Stats s;
        if (!cgroups_.stats(t.cgroup, &s)) return;
        if (t.last_sample > 0 && s.cpu_usec >= t.last_cpu_usec)
            t.cpu_usage = (s.cpu_usec - t.last_cpu_usec) / 1e6 / (now - t.last_sample);
        t.last_cpu_usec = s.cpu_usec;
        t.last_sample = now;
        t.mem_mb = s.mem_bytes / (1024.0 * 1024.0);
    }

    void register_loop() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (offline()) continue;
            {
                std::lock_guard<std::mutex> g(mu_);
                // Re-register if the master has not polled us for a while (e.g. it restarted).
                if (registered_ && now_s() - last_poll_ < 10) continue;
            }
            httplib::Client cli(cfg_.master);
            cli.set_connection_timeout(1, 0);
            cli.set_read_timeout(2, 0);
            json body = {{"name", cfg_.name},     {"address", cfg_.advertise}, {"cpu", cfg_.cpu},
                         {"ram", cfg_.ram_mb},    {"attributes", cfg_.attrs}};
            auto r = cli.Post("/api/register", body.dump(), "application/json");
            std::lock_guard<std::mutex> g(mu_);
            if (r && r->status == 200) {
                if (!registered_) std::cout << "borglet: registered with " << cfg_.master << std::endl;
                registered_ = true;
                last_poll_ = now_s();
            }
        }
    }

    Config cfg_;
    std::string self_exe_;
    Cgroups cgroups_;
    std::mutex mu_;
    std::map<int64_t, LocalTask> tasks_;
    std::set<std::string> packages_;
    bool registered_ = false;
    double last_poll_ = 0;
    std::atomic<double> offline_until_{0};
};

std::string self_exe_path(const char* argv0) {
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.string();
    return std::filesystem::absolute(argv0, ec).string();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--worker") return run_worker(argc, argv);
    ::signal(SIGPIPE, SIG_IGN);

    char host[256] = {0};
    ::gethostname(host, sizeof host - 1);
    Config cfg;
    cfg.name = env("BORGLET_NAME", host);
    cfg.port = std::stoi(env("BORGLET_PORT", "9090"));
    cfg.master = env("BORG_MASTER", "http://localhost:8080");
    cfg.cpu = std::stod(env("BORGLET_CPU", "1"));
    cfg.ram_mb = std::stoll(env("BORGLET_RAM_MB", "512"));
    cfg.install_secs = std::stod(env("BORGLET_INSTALL_SECS", "2"));
    std::stringstream attrs(env("BORGLET_ATTRS", ""));
    std::string kv;
    while (std::getline(attrs, kv, ',')) {
        auto eq = kv.find('=');
        if (eq != std::string::npos) cfg.attrs[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    cfg.advertise = env("BORGLET_ADVERTISE", "http://" + cfg.name + ":" + std::to_string(cfg.port));

    Borglet(cfg, self_exe_path(argv[0])).run();
    return 0;
}
