// Minimal cgroup v2 manager: one cgroup per task with cpu.max and memory.max.
#pragma once

#include <cstdint>
#include <string>

namespace borglet {

class Cgroups {
public:
    // Moves this process out of the root cgroup and enables the cpu+memory controllers for
    // children (cgroup v2 forbids processes in a cgroup that delegates controllers).
    // Returns false when cgroup v2 is missing or read-only; tasks then run without isolation.
    bool init(std::string* why);
    bool enabled() const { return enabled_; }

    // Creates <root>/task-<name> with the given limits; returns its path ("" on failure).
    std::string create(const std::string& name, double cpu_cores, int64_t ram_bytes);
    void remove(const std::string& path);

    struct Stats {
        uint64_t cpu_usec = 0;
        int64_t mem_bytes = 0;
        int oom_kills = 0;
    };
    bool stats(const std::string& path, Stats* out) const;

    // Async-signal-safe: moves the calling process into the cgroup. Used between fork and exec.
    static bool join(const char* procs_path);

private:
    std::string root_ = "/sys/fs/cgroup";
    bool enabled_ = false;
};

}  // namespace borglet
