#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// Caches compiled PSO microcode to disk using ID3D12PipelineLibrary.
// On first launch, PSOs are compiled normally and stored in the library.
// On subsequent launches, PSOs load from cache in microseconds instead of milliseconds.
// Cache is invalidated automatically when shader bytecodes change.
class D3D12PSOCache
{
public:
    // Load cache from disk. Returns false if no cache exists or it's stale.
    bool loadFromDisk(ID3D12Device* device, const std::string& cachePath);

    // Save cache to disk. Call on shutdown.
    void saveToDisk(const std::string& cachePath);

    // Try to load a cached PSO. Returns nullptr if not in cache.
    ComPtr<ID3D12PipelineState> loadGraphicsPSO(const wchar_t* name,
                                                 const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);

    // Store a newly created PSO in the library for future caching.
    void storePSO(const wchar_t* name, ID3D12PipelineState* pso);

    bool isAvailable() const { return m_library != nullptr; }

private:
    ComPtr<ID3D12PipelineLibrary> m_library;
    ID3D12Device* m_device = nullptr;
};
