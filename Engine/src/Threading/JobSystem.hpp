#pragma once

#include "Job.hpp"
#include "ThreadPool.hpp"
#include "MainThreadQueue.hpp"
#include <unordered_map>
#include <shared_mutex>
#include <memory>

namespace Threading {

class JobSystem {
public:
    static JobSystem& get();

    bool initialize(size_t num_worker_threads = 0);
    void shutdown();
    bool isInitialized() const { return m_initialized.load(std::memory_order_acquire); }

    JobBuilder createJob();

    JobHandle submitJob(std::unique_ptr<JobData> job);

    JobStatus getJobStatus(JobHandle handle) const;
    bool isJobComplete(JobHandle handle) const;

    void waitForJob(JobHandle handle);
    void waitForJobs(const std::vector<JobHandle>& handles);

    void processMainThreadJobs();
    void processMainThreadJobs(size_t max_jobs);

    void barrier();

    size_t getWorkerCount() const;
    size_t getPendingJobCount() const;

private:
    JobSystem() = default;
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    void notifyJobComplete(JobHandle completed_job);
    void scheduleIfReady(JobData* job);
    JobData* getJobData(JobHandle handle) const;
    void addDependent(JobHandle dependency, JobHandle dependent);

    std::unique_ptr<ThreadPool> m_thread_pool;
    MainThreadQueue m_main_thread_queue;

    std::unordered_map<JobHandle, std::unique_ptr<JobData>> m_jobs;
    mutable std::shared_mutex m_jobs_mutex;

    std::unordered_map<JobHandle, std::vector<JobHandle>> m_dependents;
    std::mutex m_dependents_mutex;

    std::atomic<JobHandle> m_next_handle{1};
    std::atomic<bool> m_initialized{false};
};

} // namespace Threading
