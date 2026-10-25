// A Borg cell: the machines plus every task that is pending or running on them.
#pragma once

#include <unordered_map>
#include <vector>

#include "borg/model.h"

namespace borg {

class Cell {
public:
    MachineId add_machine(const std::string& name, Resources capacity,
                          std::map<std::string, std::string> attributes = {});
    // Adds a pending task. If t.id < 0 a fresh id is assigned. Returns the id.
    TaskId add_task(Task t);
    // Unplaces the task if needed and forgets it.
    void remove_task(TaskId id);

    void place(TaskId id, MachineId m);
    // Takes the task off its machine and puts it back in the pending state.
    void unplace(TaskId id);
    void set_machine_up(MachineId m, bool up);

    Task& task(TaskId id) { return tasks_.at(id); }
    const Task& task(TaskId id) const { return tasks_.at(id); }
    bool has_task(TaskId id) const { return tasks_.count(id) != 0; }
    Machine& machine(MachineId m) { return machines_.at(m); }
    const Machine& machine(MachineId m) const { return machines_.at(m); }

    std::vector<Machine>& machines() { return machines_; }
    const std::vector<Machine>& machines() const { return machines_; }
    const std::unordered_map<TaskId, Task>& tasks() const { return tasks_; }

    std::vector<TaskId> pending() const;
    size_t pending_count() const;
    size_t running_count() const { return tasks_.size() - pending_count(); }

private:
    std::vector<Machine> machines_;
    std::unordered_map<TaskId, Task> tasks_;
    TaskId next_id_ = 1;
};

}  // namespace borg
