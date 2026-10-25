#!/usr/bin/env python3
"""End-to-end test of the Plan B mini cell, driven through the Borgmaster's REST API.

    docker compose up -d --build
    python3 tests/e2e_cell.py [http://localhost:8080]

It removes every job in the cell first, so don't run it against a cell you care about.
Checks: spreading, filling the cell, preemption (only lower-priority tasks, lowest first),
cgroup OOM kills, machine failure -> rescheduling -> recovery.
"""
import json
import sys
import time
import urllib.error
import urllib.request

BASE = sys.argv[1] if len(sys.argv) > 1 else "http://localhost:8080"
failures = 0


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read() or b"{}")


def state():
    return call("GET", "/api/state")


def events(since=0):
    return call("GET", f"/api/events?since={since}")["events"]


def last_event():
    return call("GET", f"/api/events?since={2**62}")["last"]


def submit(name, priority, tasks, cpu, ram_mb, workload, type_="service", distinct=False):
    job = {"name": name, "user": "e2e", "type": type_, "priority": priority, "tasks": tasks,
           "cpu": cpu, "ram_mb": ram_mb, "workload": workload, "distinct": distinct}
    return call("POST", "/api/jobs", job)["id"]


def remove_all_jobs():
    for j in state()["jobs"]:
        for _ in range(2):  # first DELETE kills a live job, the second removes it
            try:
                call("DELETE", f"/api/jobs/{j['id']}")
            except urllib.error.HTTPError:
                pass


def check(cond, desc):
    global failures
    print(f"  {'ok  ' if cond else 'FAIL'} {desc}")
    failures += not cond


def wait_for(desc, pred, timeout=30):
    t0 = time.time()
    while time.time() - t0 < timeout:
        s = state()
        if pred(s):
            print(f"  ok   {desc} ({time.time() - t0:.1f}s)")
            return s
        time.sleep(0.5)
    print(f"  FAIL {desc} (timed out after {timeout}s)")
    remove_all_jobs()
    sys.exit(1)


def tasks_of(s, name):
    return [t for t in s["tasks"] if t["job_name"] == name]


def running(s, name):
    return [t for t in tasks_of(s, name) if t["state"] == "running"]


def machine(s, mid):
    return next(m for m in s["machines"] if m["id"] == mid)


def main():
    print(f"e2e test against {BASE}")
    remove_all_jobs()
    s = wait_for("at least 3 machines up", lambda s: sum(m["status"] == "up" for m in s["machines"]) >= 3)
    cgroups = all(m["cgroups"] for m in s["machines"] if m["status"] == "up")

    print("placement")
    submit("e2e-web", 200, 3, 0.25, 64, {"type": "service"}, distinct=True)
    s = wait_for("3 prod tasks running on 3 different machines",
                 lambda s: len(running(s, "e2e-web")) == 3 and len({t["machine"] for t in running(s, "e2e-web")}) == 3)

    # Fill every remaining CPU slot with 0.25-core batch tasks (all machine sizes are multiples of 0.25).
    free = s["capacity"]["cpu"] - s["allocated"]["cpu"]
    n = int(free / 0.25 + 1e-6)
    # With cgroups the batch tasks busy-loop, so we can check that cpu.max throttles them.
    work = {"type": "cpu", "threads": 1, "duration": 600} if cgroups else {"type": "sleep", "duration": 600}
    submit("e2e-batch", 100, n, 0.25, 32, work, type_="batch")
    s = wait_for(f"{n} batch tasks fill the cell", lambda s: len(running(s, "e2e-batch")) == n)
    check(abs(s["allocated"]["cpu"] - s["capacity"]["cpu"]) < 1e-6, "cell CPU fully allocated")

    print("preemption")
    mark = last_event()  # the event log outlives jobs; only look at what this submission causes
    submit("e2e-prod", 250, 2, 0.5, 64, {"type": "service"}, distinct=True)
    s = wait_for("higher-priority job runs in a full cell", lambda s: len(running(s, "e2e-prod")) == 2)
    pre = [e for e in events(mark) if e["kind"] == "preempt" and "by e2e-prod" in e["msg"]]
    check(len(pre) == 4, f"exactly 4 batch tasks preempted to make room (got {len(pre)})")
    check(all(e["msg"].startswith("e2e-batch/") for e in pre), "only lower-priority tasks were preempted")
    check(all(t["state"] == "running" for t in tasks_of(s, "e2e-web")), "prod tasks were not preempted")
    victims = [t for t in tasks_of(s, "e2e-batch") if t["state"] == "pending"]
    check(len(victims) == 4 and all(t["reason"].startswith("preempted") for t in victims),
          "victims are back in the pending queue")

    if cgroups:
        print("isolation")
        submit("e2e-oom", 300, 1, 0.1, 32, {"type": "mem", "mb": 128, "duration": 60}, type_="batch")
        wait_for("task allocating 128 MB with a 32 MB limit is OOM-killed",
                 lambda s: any(t["state"] == "oom" for t in tasks_of(s, "e2e-oom")))
        s = wait_for("busy-looping batch tasks report CPU usage",
                     lambda s: running(s, "e2e-batch") and all(t["cpu_usage"] > 0.05 for t in running(s, "e2e-batch")))
        burners = running(s, "e2e-batch")
        top = max(t["cpu_usage"] for t in burners)
        avg = sum(t["cpu_usage"] for t in burners) / len(burners)
        check(top <= 0.25 * 1.15 + 0.02, f"cpu.max caps each 0.25-core busy loop (max {top:.2f} cores)")
        check(avg >= 0.2, f"busy loops get their full share (avg {avg:.2f} cores)")
    else:
        print("isolation: skipped (Borglets run without cgroups)")

    print("machine failure")
    s = state()
    task = running(s, "e2e-web")[0]
    mid, tid = task["machine"], task["id"]
    call("POST", f"/api/machines/{mid}/fail")
    wait_for(f"{machine(s, mid)['name']} marked DOWN after missed polls",
             lambda s: machine(s, mid)["status"] == "down", timeout=15)
    wait_for("its prod task is rescheduled onto another machine",
             lambda s: any(t["id"] == tid and t["state"] == "running" and t["machine"] != mid for t in s["tasks"]),
             timeout=20)
    wait_for("machine comes back UP after the outage", lambda s: machine(s, mid)["status"] == "up", timeout=45)

    remove_all_jobs()
    print("PASSED" if failures == 0 else f"FAILED ({failures} check(s))")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
