#include "borg/cell.h"

#include <algorithm>
#include <stdexcept>

namespace borg {

MachineId Cell::add_machine(const std::string& name, Resources capacity,
                            std::map<std::string, std::string> attributes) {
    Machine m;
    m.id = static_cast<MachineId>(machines_.size());
    m.name = name;
    m.capacity = capacity;
    m.attributes = std::move(attributes);
    machines_.push_back(std::move(m));
    return machines_.back().id;
}

TaskId Cell::add_task(Task t) {
    if (t.id < 0) t.id = next_id_++;
    else next_id_ = std::max(next_id_, t.id + 1);
    t.state = TaskState::Pending;
    t.machine = -1;
    TaskId id = t.id;
    tasks_[id] = std::move(t);
    return id;
}

void Cell::remove_task(TaskId id) {
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return;
    if (it->second.machine >= 0) unplace(id);
    tasks_.erase(it);
}

void Cell::place(TaskId id, MachineId mid) {
    Task& t = tasks_.at(id);
    if (t.machine >= 0) throw std::logic_error("task already placed");
    Machine& m = machines_.at(mid);
    m.tasks.push_back(id);
    m.limit_by_prio[t.priority] += t.limit;
    m.resv_by_prio[t.priority] += t.reservation;
    m.job_count[t.job]++;
    m.version++;
    t.machine = mid;
    t.state = TaskState::Running;
}

void Cell::unplace(TaskId id) {
    Task& t = tasks_.at(id);
    if (t.machine < 0) return;
    Machine& m = machines_.at(t.machine);
    auto it = std::find(m.tasks.begin(), m.tasks.end(), id);
    if (it != m.tasks.end()) {
        *it = m.tasks.back();
        m.tasks.pop_back();
    }
    auto drop = [&](std::map<int, Resources>& by_prio, const Resources& r) {
        auto p = by_prio.find(t.priority);
        p->second -= r;
        if (p->second.cpu < kEps && p->second.ram < kEps) by_prio.erase(p);
    };
    drop(m.limit_by_prio, t.limit);
    drop(m.resv_by_prio, t.reservation);
    if (--m.job_count[t.job] <= 0) m.job_count.erase(t.job);
    m.version++;
    t.machine = -1;
    t.state = TaskState::Pending;
}

void Cell::set_machine_up(MachineId mid, bool up) {
    Machine& m = machines_.at(mid);
    if (m.up != up) {
        m.up = up;
        m.version++;
    }
}

std::vector<TaskId> Cell::pending() const {
    std::vector<TaskId> out;
    for (const auto& [id, t] : tasks_)
        if (t.state == TaskState::Pending) out.push_back(id);
    return out;
}

size_t Cell::pending_count() const {
    size_t n = 0;
    for (const auto& [id, t] : tasks_) n += t.state == TaskState::Pending;
    return n;
}

}  // namespace borg
