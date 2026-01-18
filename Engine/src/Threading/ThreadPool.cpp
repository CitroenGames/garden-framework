#include "ThreadPool.hpp"
#include "Job.hpp"
#include "Utils/Log.hpp"
#include <algorithm>
#include <random>

namespace Threading {

ThreadPool::ThreadPool(size_t num_threads) {
    if (num_threads == 0) {
        num_threads = std::max(1u, std::thread::hardware_concurrency() - 1);
    }

    LOG_ENGINE_INFO("ThreadPool: Starting {} worker threads", num_threads);

    m_worker_queues.resize(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        m_worker_queues[i] = std::make_unique<WorkerQueue>();
    }

    for (size_t i = 0; i < num_threads; ++i) {
        m_workers.emplace_back(&ThreadPool::workerThread, this, i);
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::enqueue(JobData* job) {
    if (!job || m_shutdown) return;

    m_pending_jobs.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(m_global_mutex);
        m_global_queue.push_back(job);
    }
    m_condition.notify_one();
}

void ThreadPool::enqueuePriority(JobData* job) {
    if (!job || m_shutdown) return;

    m_pending_jobs.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(m_global_mutex);
        m_global_queue.push_front(job);
    }
    m_condition.notify_one();
}

void ThreadPool::shutdown() {
    if (m_shutdown.exchange(true)) {
        return;
    }

    LOG_ENGINE_INFO("ThreadPool: Shutting down...");

    m_condition.notify_all();

    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    m_workers.clear();
    LOG_ENGINE_INFO("ThreadPool: Shutdown complete");
}

void ThreadPool::waitForIdle() {
    while (m_pending_jobs.load(std::memory_order_relaxed) > 0 ||
           m_active_workers.load(std::memory_order_relaxed) > 0) {
        std::this_thread::yield();
    }
}

size_t ThreadPool::getPendingJobCount() const {
    return m_pending_jobs.load(std::memory_order_relaxed);
}

JobData* ThreadPool::trySteal(size_t thief_id) {
    size_t num_queues = m_worker_queues.size();
    if (num_queues <= 1) return nullptr;

    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, num_queues - 1);

    for (size_t attempts = 0; attempts < num_queues; ++attempts) {
        size_t victim_id = dist(rng);
        if (victim_id == thief_id) continue;

        auto& victim_queue = m_worker_queues[victim_id];
        std::unique_lock<std::mutex> lock(victim_queue->mutex, std::try_to_lock);
        if (lock.owns_lock() && !victim_queue->jobs.empty()) {
            JobData* job = victim_queue->jobs.back();
            victim_queue->jobs.pop_back();
            return job;
        }
    }

    return nullptr;
}

JobData* ThreadPool::getNextJob(size_t worker_id) {
    // Try local queue first
    auto& local_queue = m_worker_queues[worker_id];
    {
        std::lock_guard<std::mutex> lock(local_queue->mutex);
        if (!local_queue->jobs.empty()) {
            JobData* job = local_queue->jobs.front();
            local_queue->jobs.pop_front();
            return job;
        }
    }

    // Try global queue
    {
        std::lock_guard<std::mutex> lock(m_global_mutex);
        if (!m_global_queue.empty()) {
            JobData* job = m_global_queue.front();
            m_global_queue.pop_front();
            return job;
        }
    }

    // Try work stealing
    return trySteal(worker_id);
}

void ThreadPool::workerThread(size_t worker_id) {
    LOG_ENGINE_TRACE("ThreadPool: Worker {} started", worker_id);

    while (!m_shutdown) {
        JobData* job = nullptr;

        // First try to get a job without waiting
        job = getNextJob(worker_id);

        if (!job) {
            // No job available, wait for notification
            std::unique_lock<std::mutex> lock(m_global_mutex);
            m_condition.wait(lock, [this]() {
                return m_shutdown || !m_global_queue.empty();
            });

            if (m_shutdown) {
                break;
            }

            // Try to grab a job from global queue while we have the lock
            if (!m_global_queue.empty()) {
                job = m_global_queue.front();
                m_global_queue.pop_front();
            }
        }

        if (job) {
            m_active_workers.fetch_add(1, std::memory_order_relaxed);

            job->status.store(JobStatus::Running, std::memory_order_release);

            bool success = true;
            try {
                if (job->work) {
                    job->work();
                }
            } catch (const std::exception& e) {
                LOG_ENGINE_ERROR("ThreadPool: Job '{}' threw exception: {}", job->name, e.what());
                success = false;
            } catch (...) {
                LOG_ENGINE_ERROR("ThreadPool: Job '{}' threw unknown exception", job->name);
                success = false;
            }

            job->status.store(success ? JobStatus::Completed : JobStatus::Failed,
                             std::memory_order_release);

            try {
                job->completion_promise.set_value(success);
            } catch (...) {
            }

            if (job->on_complete) {
                try {
                    job->on_complete(job->handle, success);
                } catch (...) {
                }
            }

            m_pending_jobs.fetch_sub(1, std::memory_order_relaxed);
            m_active_workers.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    LOG_ENGINE_TRACE("ThreadPool: Worker {} exiting", worker_id);
}

} // namespace Threading
