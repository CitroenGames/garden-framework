#pragma once

#include <atomic>
#include <cassert>

// Frame phase tracking for debug-mode assertions against data races.
// In release builds all methods are no-ops with zero overhead.
//
// Usage:
//   FrameSync::get().setPhase(FramePhase::ParallelRecord);
//   // ... launch worker threads ...
//   FrameSync::get().setPhase(FramePhase::Replay);
//
// Elsewhere (e.g. before mutating ECS registry):
//   FrameSync::get().assertNotInParallelRecord();

enum class FramePhase : uint8_t
{
    GameLogic,       // Safe to modify ECS, add/remove entities
    PreRender,       // Main thread prep: mesh upload, LOD selection, BVH build
    ParallelRecord,  // Worker threads reading ECS — NO structural writes
    Replay,          // GPU command submission — main thread only
    Present          // Swap chain present
};

class FrameSync
{
public:
    static FrameSync& get()
    {
        static FrameSync instance;
        return instance;
    }

    void setPhase(FramePhase phase)
    {
#ifndef NDEBUG
        m_phase.store(phase, std::memory_order_release);
#else
        (void)phase;
#endif
    }

    FramePhase getPhase() const
    {
#ifndef NDEBUG
        return m_phase.load(std::memory_order_acquire);
#else
        return FramePhase::GameLogic;
#endif
    }

    // Assert that we are NOT in the parallel recording phase.
    // Call this before any ECS structural modification (emplace, remove, destroy).
    void assertNotInParallelRecord() const
    {
#ifndef NDEBUG
        assert(m_phase.load(std::memory_order_acquire) != FramePhase::ParallelRecord
               && "ECS structural modification during parallel recording is a data race!");
#endif
    }

    // Assert that we ARE in a phase where ECS writes are safe.
    void assertWriteAllowed() const
    {
#ifndef NDEBUG
        FramePhase p = m_phase.load(std::memory_order_acquire);
        assert((p == FramePhase::GameLogic || p == FramePhase::Present)
               && "ECS writes are only safe during GameLogic or Present phases!");
#endif
    }

private:
    FrameSync() = default;
    FrameSync(const FrameSync&) = delete;
    FrameSync& operator=(const FrameSync&) = delete;

#ifndef NDEBUG
    std::atomic<FramePhase> m_phase{FramePhase::GameLogic};
#endif
};
