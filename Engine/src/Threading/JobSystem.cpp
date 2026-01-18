#include "JobSystem.hpp"
#include "Utils/Log.hpp"

namespace Threading {

JobSystem& JobSystem::get() {
    static JobSystem instance;
    return instance;
}

JobSystem::~JobSystem() {
    shutdown();
}

bool JobSystem::initialize(size_t num_worker_threads) {
    if (m_initialized.exchange(true)) {
        LOG_ENGINE_WARN("JobSystem: Already initialized");
        return true;
    }

    LOG_ENGINE_INFO("JobSystem: Initializing...");

    m_thread_pool = std::make_unique<ThreadPool>(num_worker_threads);

    LOG_ENGINE_INFO("JobSystem: Initialized with {} worker threads", m_thread_pool->getWorkerCount());
    return true;
}

void JobSystem::shutdown() {
    if (!m_initialized.exchange(false)) {
        return;
    }

    LOG_ENGINE_INFO("JobSystem: Shutting down...");

    if (m_thread_pool) {
        m_thread_pool->shutdown();
        m_thread_pool.reset();
    }

    {
        std::unique_lock<std::shared_mutex> lock(m_jobs_mutex);
        m_jobs.clear();
    }

    {
        std::lock_guard<std::mutex> lock(m_dependents_mutex);
        m_dependents.clear();
    }

    LOG_ENGINE_INFO("JobSystem: Shutdown complete");
}

JobBuilder JobSystem::createJob() {
    return JobBuilder(this);
}

JobHandle JobSystem::submitJob(std::unique_ptr<JobData> job) {
    if (!job || !m_initialized) {
        return INVALID_JOB_HANDLE;
    }

    JobHandle handle = m_next_handle.fetch_add(1, std::memory_order_relaxed);
    job->handle = handle;

    int32_t pending_deps = 0;
    for (JobHandle dep : job->dependencies) {
        JobData* dep_data = getJobData(dep);
        if (dep_data) {
            JobStatus dep_status = dep_data->status.load(std::memory_order_acquire);
            if (dep_status != JobStatus::Completed && dep_status != JobStatus::Failed) {
                addDependent(dep, handle);
                ++pending_deps;
            }
        }
    }

    job->unfinished_dependencies.store(pending_deps, std::memory_order_release);

    JobData* job_ptr = job.get();

    {
        std::unique_lock<std::shared_mutex> lock(m_jobs_mutex);
        m_jobs[handle] = std::move(job);
    }

    if (pending_deps == 0) {
        scheduleIfReady(job_ptr);
    } else {
        job_ptr->status.store(JobStatus::Pending, std::memory_order_release);
    }

    return handle;
}

void JobSystem::scheduleIfReady(JobData* job) {
    if (!job) return;

    job->status.store(JobStatus::Ready, std::memory_order_release);

    if (job->context == JobContext::MainThread) {
        m_main_thread_queue.enqueue(job);
    } else {
        if (job->priority >= JobPriority::High) {
            m_thread_pool->enqueuePriority(job);
        } else {
            m_thread_pool->enqueue(job);
        }
    }
}

void JobSystem::addDependent(JobHandle dependency, JobHandle dependent) {
    std::lock_guard<std::mutex> lock(m_dependents_mutex);
    m_dependents[dependency].push_back(dependent);
}

void JobSystem::notifyJobComplete(JobHandle completed_job) {
    std::vector<JobHandle> dependents_to_check;

    {
        std::lock_guard<std::mutex> lock(m_dependents_mutex);
        auto it = m_dependents.find(completed_job);
        if (it != m_dependents.end()) {
            dependents_to_check = std::move(it->second);
            m_dependents.erase(it);
        }
    }

    for (JobHandle dependent_handle : dependents_to_check) {
        JobData* dependent = getJobData(dependent_handle);
        if (dependent) {
            int32_t remaining = dependent->unfinished_dependencies.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0) {
                scheduleIfReady(dependent);
            }
        }
    }
}

JobData* JobSystem::getJobData(JobHandle handle) const {
    std::shared_lock<std::shared_mutex> lock(m_jobs_mutex);
    auto it = m_jobs.find(handle);
    return (it != m_jobs.end()) ? it->second.get() : nullptr;
}

JobStatus JobSystem::getJobStatus(JobHandle handle) const {
    JobData* job = getJobData(handle);
    return job ? job->status.load(std::memory_order_acquire) : JobStatus::Failed;
}

bool JobSystem::isJobComplete(JobHandle handle) const {
    JobStatus status = getJobStatus(handle);
    return status == JobStatus::Completed || status == JobStatus::Failed;
}

void JobSystem::waitForJob(JobHandle handle) {
    JobData* job = getJobData(handle);
    if (!job) return;

    job->completion_future.wait();
}

void JobSystem::waitForJobs(const std::vector<JobHandle>& handles) {
    for (JobHandle handle : handles) {
        waitForJob(handle);
    }
}

void JobSystem::processMainThreadJobs() {
    size_t processed = m_main_thread_queue.processAll();

    std::vector<JobHandle> completed_jobs;
    {
        std::shared_lock<std::shared_mutex> lock(m_jobs_mutex);
        for (auto& [handle, job] : m_jobs) {
            if (job->status.load(std::memory_order_acquire) == JobStatus::Completed ||
                job->status.load(std::memory_order_acquire) == JobStatus::Failed) {
                completed_jobs.push_back(handle);
            }
        }
    }

    for (JobHandle handle : completed_jobs) {
        notifyJobComplete(handle);
    }
}

void JobSystem::processMainThreadJobs(size_t max_jobs) {
    m_main_thread_queue.processN(max_jobs);

    std::vector<JobHandle> completed_jobs;
    {
        std::shared_lock<std::shared_mutex> lock(m_jobs_mutex);
        for (auto& [handle, job] : m_jobs) {
            if (job->status.load(std::memory_order_acquire) == JobStatus::Completed ||
                job->status.load(std::memory_order_acquire) == JobStatus::Failed) {
                completed_jobs.push_back(handle);
            }
        }
    }

    for (JobHandle handle : completed_jobs) {
        notifyJobComplete(handle);
    }
}

void JobSystem::barrier() {
    if (m_thread_pool) {
        m_thread_pool->waitForIdle();
    }
    while (m_main_thread_queue.hasPending()) {
        m_main_thread_queue.processAll();
    }
}

size_t JobSystem::getWorkerCount() const {
    return m_thread_pool ? m_thread_pool->getWorkerCount() : 0;
}

size_t JobSystem::getPendingJobCount() const {
    size_t count = 0;
    if (m_thread_pool) {
        count += m_thread_pool->getPendingJobCount();
    }
    count += m_main_thread_queue.getPendingCount();
    return count;
}

// JobBuilder implementation
JobBuilder::JobBuilder(JobSystem* system)
    : m_system(system)
    , m_data(std::make_unique<JobData>())
{
}

JobBuilder& JobBuilder::setName(const std::string& name) {
    m_data->name = name;
    return *this;
}

JobBuilder& JobBuilder::setWork(std::function<void()> work) {
    m_data->work = std::move(work);
    return *this;
}

JobBuilder& JobBuilder::setPriority(JobPriority priority) {
    m_data->priority = priority;
    return *this;
}

JobBuilder& JobBuilder::setContext(JobContext context) {
    m_data->context = context;
    return *this;
}

JobBuilder& JobBuilder::dependsOn(JobHandle dependency) {
    if (dependency != INVALID_JOB_HANDLE) {
        m_data->dependencies.push_back(dependency);
    }
    return *this;
}

JobBuilder& JobBuilder::dependsOn(const std::vector<JobHandle>& dependencies) {
    for (JobHandle dep : dependencies) {
        if (dep != INVALID_JOB_HANDLE) {
            m_data->dependencies.push_back(dep);
        }
    }
    return *this;
}

JobBuilder& JobBuilder::onComplete(JobCallback callback) {
    m_data->on_complete = std::move(callback);
    return *this;
}

JobHandle JobBuilder::submit() {
    return m_system->submitJob(std::move(m_data));
}

std::pair<JobHandle, std::shared_future<bool>> JobBuilder::submitWithFuture() {
    auto future = m_data->completion_future;
    JobHandle handle = m_system->submitJob(std::move(m_data));
    return {handle, future};
}

} // namespace Threading
