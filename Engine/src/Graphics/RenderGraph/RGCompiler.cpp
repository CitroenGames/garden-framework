#include "RGCompiler.hpp"
#include <algorithm>
#include <queue>
#include <unordered_map>
#include <cmath>

RGCompileResult RGCompiler::compile(std::vector<RGPassNode>& passes,
                                    std::vector<RGResourceNode>& resources,
                                    uint32_t refWidth, uint32_t refHeight)
{
    RGCompileResult result;

    // Step 1: Resolve relative dimensions
    resolveDimensions(resources, refWidth, refHeight);

    // Step 2+3: Topological sort
    if (!topologicalSort(passes, resources, result.executionOrder))
        return result; // cycle detected

    // Step 4: Cull unused passes
    cullPasses(passes, resources, result.executionOrder);

    // Remove culled passes from execution order
    std::vector<uint32_t> filteredOrder;
    filteredOrder.reserve(result.executionOrder.size());
    for (uint32_t idx : result.executionOrder)
    {
        if (!passes[idx].culled)
            filteredOrder.push_back(idx);
    }
    result.executionOrder = std::move(filteredOrder);

    // Step 5: Compute resource lifetimes
    computeLifetimes(passes, resources, result.executionOrder);

    // Step 6: Schedule barriers
    result.barriers = scheduleBarriers(passes, resources, result.executionOrder);

    result.valid = true;
    return result;
}

void RGCompiler::resolveDimensions(std::vector<RGResourceNode>& resources,
                                   uint32_t refWidth, uint32_t refHeight)
{
    for (auto& res : resources)
    {
        if (res.desc.scaleFactor > 0.0f && refWidth > 0 && refHeight > 0)
        {
            res.desc.width  = static_cast<uint32_t>(std::ceil(refWidth  * res.desc.scaleFactor));
            res.desc.height = static_cast<uint32_t>(std::ceil(refHeight * res.desc.scaleFactor));
            // Ensure at least 1x1
            if (res.desc.width  == 0) res.desc.width  = 1;
            if (res.desc.height == 0) res.desc.height = 1;
        }
    }
}

bool RGCompiler::topologicalSort(const std::vector<RGPassNode>& passes,
                                 const std::vector<RGResourceNode>& resources,
                                 std::vector<uint32_t>& outOrder)
{
    uint32_t passCount = static_cast<uint32_t>(passes.size());
    if (passCount == 0) return true;

    // Build a map: resource index -> last writer pass index
    // and resource index -> list of reader pass indices
    std::unordered_map<uint16_t, uint32_t> lastWriter;
    std::vector<std::vector<uint32_t>> adjacency(passCount);
    std::vector<uint32_t> inDegree(passCount, 0);

    // First pass: find writers
    for (uint32_t pi = 0; pi < passCount; ++pi)
    {
        for (const auto& access : passes[pi].accesses)
        {
            if (access.isWrite)
                lastWriter[access.handle.index] = pi;
        }
    }

    // Second pass: build edges in declaration order. Tracks three dependency kinds:
    //   RAW — reader depends on most recent writer of the same resource.
    //   WAW — writer depends on most recent writer of the same resource.
    //   WAR — writer depends on every reader recorded since the last write.
    // Without WAR, a pass that writes a resource can be scheduled before passes
    // that read the same resource, leaving the resource in a read-state at graph end.
    lastWriter.clear();
    std::unordered_map<uint16_t, std::vector<uint32_t>> readersSinceWrite;

    for (uint32_t pi = 0; pi < passCount; ++pi)
    {
        // Reads: RAW dependency on previous writer; record reader for future WAR.
        for (const auto& access : passes[pi].accesses)
        {
            if (!access.isWrite)
            {
                auto it = lastWriter.find(access.handle.index);
                if (it != lastWriter.end() && it->second != pi)
                {
                    adjacency[it->second].push_back(pi);
                    inDegree[pi]++;
                }
                readersSinceWrite[access.handle.index].push_back(pi);
            }
        }

        // Writes: WAW dep on previous writer, WAR deps on every intermediate reader.
        for (const auto& access : passes[pi].accesses)
        {
            if (access.isWrite)
            {
                auto itw = lastWriter.find(access.handle.index);
                if (itw != lastWriter.end() && itw->second != pi)
                {
                    adjacency[itw->second].push_back(pi);
                    inDegree[pi]++;
                }
                auto itr = readersSinceWrite.find(access.handle.index);
                if (itr != readersSinceWrite.end())
                {
                    for (uint32_t readerPi : itr->second)
                    {
                        if (readerPi != pi)
                        {
                            adjacency[readerPi].push_back(pi);
                            inDegree[pi]++;
                        }
                    }
                    itr->second.clear();
                }
                lastWriter[access.handle.index] = pi;
            }
        }
    }

    // Kahn's algorithm
    std::queue<uint32_t> q;
    for (uint32_t i = 0; i < passCount; ++i)
    {
        if (inDegree[i] == 0)
            q.push(i);
    }

    outOrder.clear();
    outOrder.reserve(passCount);

    while (!q.empty())
    {
        uint32_t curr = q.front();
        q.pop();
        outOrder.push_back(curr);

        for (uint32_t next : adjacency[curr])
        {
            if (--inDegree[next] == 0)
                q.push(next);
        }
    }

    return outOrder.size() == passCount; // false if cycle
}

void RGCompiler::cullPasses(std::vector<RGPassNode>& passes,
                            const std::vector<RGResourceNode>& resources,
                            const std::vector<uint32_t>& executionOrder)
{
    // Build: for each resource, which passes read it
    std::unordered_map<uint16_t, std::vector<uint32_t>> resourceReaders;
    for (uint32_t pi = 0; pi < static_cast<uint32_t>(passes.size()); ++pi)
    {
        for (const auto& access : passes[pi].accesses)
        {
            if (!access.isWrite)
                resourceReaders[access.handle.index].push_back(pi);
        }
    }

    // Iteratively cull: a pass is cullable if:
    // - It has no side effects
    // - None of its written resources are read by a non-culled pass
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (auto& pass : passes)
        {
            if (pass.culled || pass.hasSideEffect)
                continue;

            bool hasLiveOutput = false;
            for (const auto& access : pass.accesses)
            {
                if (!access.isWrite)
                    continue;

                auto it = resourceReaders.find(access.handle.index);
                if (it == resourceReaders.end())
                    continue;

                for (uint32_t readerIdx : it->second)
                {
                    if (!passes[readerIdx].culled)
                    {
                        hasLiveOutput = true;
                        break;
                    }
                }
                if (hasLiveOutput) break;
            }

            if (!hasLiveOutput)
            {
                pass.culled = true;
                changed = true;
            }
        }
    }
}

void RGCompiler::computeLifetimes(const std::vector<RGPassNode>& passes,
                                  std::vector<RGResourceNode>& resources,
                                  const std::vector<uint32_t>& executionOrder)
{
    for (auto& res : resources)
    {
        res.firstWrite = UINT32_MAX;
        res.lastRead = 0;
    }

    for (uint32_t orderIdx = 0; orderIdx < static_cast<uint32_t>(executionOrder.size()); ++orderIdx)
    {
        const auto& pass = passes[executionOrder[orderIdx]];
        if (pass.culled) continue;

        for (const auto& access : pass.accesses)
        {
            if (access.handle.index >= resources.size())
                continue;

            auto& res = resources[access.handle.index];
            if (access.isWrite && orderIdx < res.firstWrite)
                res.firstWrite = orderIdx;
            if (!access.isWrite && orderIdx > res.lastRead)
                res.lastRead = orderIdx;
        }
    }
}

std::vector<RGBarrier> RGCompiler::scheduleBarriers(
    const std::vector<RGPassNode>& passes,
    const std::vector<RGResourceNode>& resources,
    const std::vector<uint32_t>& executionOrder)
{
    std::vector<RGBarrier> barriers;

    // Track the current usage of each resource
    std::unordered_map<uint16_t, RGResourceUsage> currentUsage;

    // Initialize imported resources to their import usage
    for (uint16_t ri = 0; ri < static_cast<uint16_t>(resources.size()); ++ri)
    {
        if (resources[ri].imported)
            currentUsage[ri] = resources[ri].importUsage;
    }

    for (uint32_t orderIdx = 0; orderIdx < static_cast<uint32_t>(executionOrder.size()); ++orderIdx)
    {
        uint32_t passIdx = executionOrder[orderIdx];
        const auto& pass = passes[passIdx];
        if (pass.culled) continue;

        for (const auto& access : pass.accesses)
        {
            auto it = currentUsage.find(access.handle.index);
            RGResourceUsage prevUsage = (it != currentUsage.end())
                ? it->second
                : RGResourceUsage::Undefined;

            if (prevUsage != access.usage)
            {
                barriers.push_back({access.handle, prevUsage, access.usage, orderIdx});
            }

            // Update current usage (writes set the "output" state)
            currentUsage[access.handle.index] = access.usage;
        }
    }

    return barriers;
}
