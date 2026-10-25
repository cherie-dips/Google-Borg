#include "cgroups.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace borglet {

namespace {

bool write_file(const std::string& path, const std::string& value) {
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    ssize_t n = ::write(fd, value.data(), value.size());
    ::close(fd);
    return n == static_cast<ssize_t>(value.size());
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

bool Cgroups::init(std::string* why) {
    std::string controllers = read_file(root_ + "/cgroup.controllers");
    if (controllers.empty()) {
        *why = "cgroup v2 not available (" + root_ + "/cgroup.controllers missing)";
        return false;
    }
    if (controllers.find("cpu") == std::string::npos || controllers.find("memory") == std::string::npos) {
        *why = "cpu/memory controllers not delegated: " + controllers;
        return false;
    }
    // Move every process in the root of our cgroup namespace into a leaf for the agent.
    std::string agent = root_ + "/borglet-agent";
    ::mkdir(agent.c_str(), 0755);
    std::istringstream procs(read_file(root_ + "/cgroup.procs"));
    std::string pid;
    while (std::getline(procs, pid))
        if (!pid.empty()) write_file(agent + "/cgroup.procs", pid);
    if (!write_file(root_ + "/cgroup.subtree_control", "+cpu +memory")) {
        *why = std::string("cannot enable +cpu +memory in subtree_control: ") + std::strerror(errno) +
               " (is the container privileged with a private cgroup namespace?)";
        return false;
    }
    enabled_ = true;
    return true;
}

std::string Cgroups::create(const std::string& name, double cpu_cores, int64_t ram_bytes) {
    if (!enabled_) return "";
    std::string path = root_ + "/task-" + name;
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return "";
    const long period = 100000;
    long quota = std::max(1000L, static_cast<long>(std::lround(cpu_cores * period)));
    write_file(path + "/cpu.max", std::to_string(quota) + " " + std::to_string(period));
    write_file(path + "/memory.max", std::to_string(ram_bytes));
    write_file(path + "/memory.swap.max", "0");   // OOM-kill instead of swapping
    write_file(path + "/memory.oom.group", "1");  // kill the whole task, not one thread
    return path;
}

void Cgroups::remove(const std::string& path) {
    if (path.empty()) return;
    // The kernel may need a moment to finish tearing down the last process.
    for (int i = 0; i < 10; ++i) {
        if (::rmdir(path.c_str()) == 0 || errno == ENOENT) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool Cgroups::stats(const std::string& path, Stats* out) const {
    if (path.empty()) return false;
    std::istringstream cpu(read_file(path + "/cpu.stat"));
    std::string key;
    uint64_t val;
    while (cpu >> key >> val)
        if (key == "usage_usec") out->cpu_usec = val;
    std::string mem = read_file(path + "/memory.current");
    if (!mem.empty()) out->mem_bytes = std::stoll(mem);
    std::istringstream ev(read_file(path + "/memory.events"));
    while (ev >> key >> val)
        if (key == "oom_kill") out->oom_kills = static_cast<int>(val);
    return true;
}

bool Cgroups::join(const char* procs_path) {
    int fd = ::open(procs_path, O_WRONLY);
    if (fd < 0) return false;
    bool ok = ::write(fd, "0", 1) == 1;
    ::close(fd);
    return ok;
}

}  // namespace borglet
