#pragma once

#include <atomic>
#include <vector>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <cstdint>

namespace Threading {

class JobSystem;

enum class JobPriority : uint8_t {
    Low = 0,
    Normal = 1,
    High = 2,
    Critical = 3
};

enum class JobContext : uint8_t {
    Worker,
    MainThread
};

enum class JobStatus : uint8_t {
    Pending,
    Ready,
    Running,
    Completed,
    Failed
};

using JobHandle = uint64_t;
constexpr JobHandle INVALID_JOB_HANDLE = 0;

using JobCallback = std::function<void(JobHandle, bool success)>;

struct JobData {
    std::string name;
    std::function<void()> work;
    JobPriority priority = JobPriority::Normal;
    JobContext context = JobContext::Worker;

    std::vector<JobHandle> dependencies;
    JobCallback on_complete;

    std::atomic<JobStatus> status{JobStatus::Pending};
    std::atomic<int32_t> unfinished_dependencies{0};
    std::atomic<int32_t> ref_count{1};

    std::promise<bool> completion_promise;
    std::shared_future<bool> completion_future;

    JobHandle handle = INVALID_JOB_HANDLE;

    JobData() {
        completion_future = completion_promise.get_future().share();
    }

    JobData(JobData&& other) noexcept
        : name(std::move(other.name))
        , work(std::move(other.work))
        , priority(other.priority)
        , context(other.context)
        , dependencies(std::move(other.dependencies))
        , on_complete(std::move(other.on_complete))
        , status(other.status.load())
        , unfinished_dependencies(other.unfinished_dependencies.load())
        , ref_count(other.ref_count.load())
        , completion_promise(std::move(other.completion_promise))
        , completion_future(std::move(other.completion_future))
        , handle(other.handle)
    {}

    JobData(const JobData&) = delete;
    JobData& operator=(const JobData&) = delete;
    JobData& operator=(JobData&&) = delete;
};

class JobBuilder {
public:
    explicit JobBuilder(JobSystem* system);

    JobBuilder& setName(const std::string& name);
    JobBuilder& setWork(std::function<void()> work);
    JobBuilder& setPriority(JobPriority priority);
    JobBuilder& setContext(JobContext context);
    JobBuilder& dependsOn(JobHandle dependency);
    JobBuilder& dependsOn(const std::vector<JobHandle>& dependencies);
    JobBuilder& onComplete(JobCallback callback);

    JobHandle submit();
    std::pair<JobHandle, std::shared_future<bool>> submitWithFuture();

private:
    JobSystem* m_system;
    std::unique_ptr<JobData> m_data;
};

} // namespace Threading
