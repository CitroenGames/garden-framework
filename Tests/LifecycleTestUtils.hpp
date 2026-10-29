#pragma once

#include "world.hpp"

// A failed construction can leave a durable unpublished rollback operation.
// Test fixtures must finish and acknowledge it before destroying the world.
inline bool settleTestRollback(world& owner, LifecycleResult rollback)
{
    if (!rollback.operation.valid()) return true;
    while (!rollback.cleanupComplete()) {
        rollback = owner.resumeRetirement(rollback.operation);
        if (!rollback.accepted) return false;
    }
    return owner.acknowledgeRetirement(rollback.operation).cleanupComplete();
}
