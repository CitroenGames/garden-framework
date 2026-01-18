#pragma once

#include <deque>
#include <mutex>
#include <atomic>
#include <cstddef>

namespace Threading {

struct JobData;

class MainThreadQueue {
public:
    MainThreadQueue() = default;
    ~MainThreadQueue() = default;

    MainThreadQueue(const MainThreadQueue&) = delete;
    MainThreadQueue& operator=(const MainThreadQueue&) = delete;

    void enqueue(JobData* job);

    size_t processAll();
    size_t processN(size_t max_jobs);

    bool hasPending() const;
    size_t getPendingCount() const;

private:
    void processJob(JobData* job);

    std::deque<JobData*> m_queue;
    mutable std::mutex m_mutex;
    std::atomic<size_t> m_pending_count{0};
};

} // namespace Threading
