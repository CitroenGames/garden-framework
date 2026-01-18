#pragma once

#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <memory>

namespace Threading {

struct JobData;

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void enqueue(JobData* job);
    void enqueuePriority(JobData* job);

    void shutdown();
    void waitForIdle();

    size_t getWorkerCount() const { return m_workers.size(); }
    size_t getPendingJobCount() const;

private:
    void workerThread(size_t worker_id);
    JobData* trySteal(size_t thief_id);
    JobData* getNextJob(size_t worker_id);

    std::vector<std::thread> m_workers;

    struct WorkerQueue {
        std::deque<JobData*> jobs;
        std::mutex mutex;
    };
    std::vector<std::unique_ptr<WorkerQueue>> m_worker_queues;

    std::deque<JobData*> m_global_queue;
    std::mutex m_global_mutex;
    std::condition_variable m_condition;

    std::atomic<bool> m_shutdown{false};
    std::atomic<size_t> m_active_workers{0};
    std::atomic<size_t> m_pending_jobs{0};
};

} // namespace Threading
