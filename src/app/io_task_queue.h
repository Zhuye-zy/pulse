#pragma once
#include <deque>
#include <functional>
#include <algorithm>

namespace pulse::app {
// Protected by WorkerPool's mutex. Serial jobs retain submission order while
// unrelated I/O can continue on the other workers.
class IoTaskQueue {
public:
    struct Job { std::function<void()> task; bool serial = false; };
    void Push(std::function<void()> task, bool serial) { jobs_.push_back({std::move(task), serial}); }
    bool Ready() const {
        return std::any_of(jobs_.begin(), jobs_.end(), [&](const Job& job) {
            return !job.serial || !serial_active_;
        });
    }
    Job Pop() {
        const auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& job) {
            return !job.serial || !serial_active_;
        });
        if (it == jobs_.end()) return {};
        Job job = std::move(*it);
        jobs_.erase(it);
        if (job.serial) serial_active_ = true;
        return job;
    }
    void Complete(const Job& job) { if (job.serial) serial_active_ = false; }
private:
    std::deque<Job> jobs_;
    bool serial_active_ = false;
};
}
