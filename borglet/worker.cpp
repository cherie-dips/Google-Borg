// Built-in task programs. The Borglet runs tasks by re-executing itself as
// `borglet --worker <type> ...`, so the image needs no extra tools.
//
//   service              long-running server: ~20 MB resident, light periodic CPU
//   sleep  <secs>        idle, then exit 0
//   cpu    <threads> <secs>  busy-loop threads (cpu.max throttles them to the limit)
//   mem    <mb> <secs>   allocate and touch <mb> MB gradually, then hold (exceeding memory.max -> OOM kill)
//   crash  <secs>        run, then exit 1 (shows restarts of service jobs)
//
// secs <= 0 means "forever".
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

bool expired(Clock::time_point start, double secs) {
    return secs > 0 && std::chrono::duration<double>(Clock::now() - start).count() >= secs;
}

std::vector<char*> touch(size_t mb, std::vector<char*> blocks = {}) {
    for (size_t i = 0; i < mb; ++i) {
        char* p = static_cast<char*>(std::malloc(1 << 20));
        if (!p) break;
        std::memset(p, static_cast<int>(i & 0xff), 1 << 20);
        blocks.push_back(p);
    }
    return blocks;
}

void spin(std::atomic<bool>& stop) {
    volatile uint64_t x = 0;
    while (!stop.load(std::memory_order_relaxed)) x = x * 6364136223846793005ULL + 1;
}

}  // namespace

int run_worker(int argc, char** argv) {
    // argv: borglet --worker <type> [args...]
    if (argc < 3) return 2;
    std::string type = argv[2];
    auto arg = [&](int i, double def) { return argc > i ? std::atof(argv[i]) : def; };
    auto start = Clock::now();

    if (type == "service") {
        auto mem = touch(20);
        while (true) {
            std::atomic<bool> stop{false};
            std::thread t(spin, std::ref(stop));
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            stop = true;
            t.join();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
    if (type == "sleep") {
        double secs = arg(3, 30);
        while (!expired(start, secs)) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return 0;
    }
    if (type == "cpu") {
        int threads = std::max(1, static_cast<int>(arg(3, 1)));
        double secs = arg(4, 30);
        auto mem = touch(10);
        std::atomic<bool> stop{false};
        std::vector<std::thread> ts;
        for (int i = 0; i < threads; ++i) ts.emplace_back(spin, std::ref(stop));
        while (!expired(start, secs)) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        stop = true;
        for (auto& t : ts) t.join();
        return 0;
    }
    if (type == "mem") {
        size_t target = static_cast<size_t>(arg(3, 100));
        double secs = arg(4, 30);
        std::vector<char*> blocks;
        while (blocks.size() < target) {  // ~80 MB/s so the growth is visible in the UI
            blocks = touch(std::min<size_t>(8, target - blocks.size()), std::move(blocks));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        while (!expired(start, secs)) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return 0;
    }
    if (type == "crash") {
        double secs = arg(3, 10);
        auto mem = touch(5);
        while (!expired(start, secs)) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::fprintf(stderr, "worker: simulated crash\n");
        return 1;
    }
    std::fprintf(stderr, "worker: unknown type %s\n", type.c_str());
    return 2;
}
