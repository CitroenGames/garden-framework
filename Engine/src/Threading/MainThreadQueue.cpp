#include "MainThreadQueue.hpp"
#include "Job.hpp"
#include "Utils/Log.hpp"

namespace Threading {

void MainThreadQueue::enqueue(JobData* job) {
    if (!job) return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(job);
    }
    m_pending_count.fetch_add(1, std::memory_order_relaxed);
}

size_t MainThreadQueue::processAll() {
    std::deque<JobData*> jobs_to_process;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        jobs_to_process.swap(m_queue);
    }

    size_t count = jobs_to_process.size();
    m_pending_count.store(0, std::memory_order_relaxed);

    for (JobData* job : jobs_to_process) {
        processJob(job);
    }

    return count;
}

size_t MainThreadQueue::processN(size_t max_jobs) {
    std::deque<JobData*> jobs_to_process;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t count = std::min(max_jobs, m_queue.size());
        for (size_t i = 0; i < count; ++i) {
            jobs_to_process.push_back(m_queue.front());
            m_queue.pop_front();
        }
    }

    size_t count = jobs_to_process.size();
    m_pending_count.fetch_sub(count, std::memory_order_relaxed);

    for (JobData* job : jobs_to_process) {
        processJob(job);
    }

    return count;
}

bool MainThreadQueue::hasPending() const {
    return m_pending_count.load(std::memory_order_relaxed) > 0;
}

size_t MainThreadQueue::getPendingCount() const {
    return m_pending_count.load(std::memory_order_relaxed);
}

void MainThreadQueue::processJob(JobData* job) {
    if (!job) return;

    job->status.store(JobStatus::Running, std::memory_order_release);

    bool success = true;
    try {
        if (job->work) {
            job->work();
        }
    } catch (const std::exception& e) {
        LOG_ENGINE_ERROR("MainThreadQueue: Job '{}' threw exception: {}", job->name, e.what());
        success = false;
    } catch (...) {
        LOG_ENGINE_ERROR("MainThreadQueue: Job '{}' threw unknown exception", job->name);
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
}

} // namespace Threading
