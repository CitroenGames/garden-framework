#include "D3D12PSOCache.hpp"
#include "Utils/Log.hpp"
#include <fstream>

bool D3D12PSOCache::loadFromDisk(ID3D12Device* device, const std::string& cachePath)
{
    m_device = device;

    // Check if device supports pipeline libraries (ID3D12Device1)
    ComPtr<ID3D12Device1> device1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(device1.GetAddressOf()))))
    {
        LOG_ENGINE_TRACE("[D3D12 PSOCache] Device does not support ID3D12Device1, PSO caching disabled");
        return false;
    }

    // Try to load existing cache from disk
    std::ifstream file(cachePath, std::ios::binary | std::ios::ate);
    if (file.is_open())
    {
        size_t size = static_cast<size_t>(file.tellg());
        if (size > 0)
        {
            std::vector<char> blob(size);
            file.seekg(0);
            file.read(blob.data(), size);
            file.close();

            HRESULT hr = device1->CreatePipelineLibrary(blob.data(), blob.size(),
                                                         IID_PPV_ARGS(m_library.GetAddressOf()));
            if (SUCCEEDED(hr))
            {
                LOG_ENGINE_INFO("[D3D12 PSOCache] Loaded pipeline cache ({} KB)", size / 1024);
                return true;
            }

            // Cache is stale (driver update, shader change, etc.) — create empty
            LOG_ENGINE_TRACE("[D3D12 PSOCache] Cache stale or incompatible, will rebuild");
        }
    }

    // Create empty library
    HRESULT hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(m_library.GetAddressOf()));
    if (FAILED(hr))
    {
        LOG_ENGINE_WARN("[D3D12 PSOCache] Failed to create empty pipeline library (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    LOG_ENGINE_TRACE("[D3D12 PSOCache] Created empty pipeline library (first run)");
    return true;
}

void D3D12PSOCache::saveToDisk(const std::string& cachePath)
{
    if (!m_library) return;

    SIZE_T blobSize = m_library->GetSerializedSize();
    if (blobSize == 0) return;

    std::vector<char> blob(blobSize);
    HRESULT hr = m_library->Serialize(blob.data(), blobSize);
    if (FAILED(hr))
    {
        LOG_ENGINE_WARN("[D3D12 PSOCache] Failed to serialize pipeline library");
        return;
    }

    std::ofstream file(cachePath, std::ios::binary);
    if (file.is_open())
    {
        file.write(blob.data(), blobSize);
        LOG_ENGINE_INFO("[D3D12 PSOCache] Saved pipeline cache ({} KB)", blobSize / 1024);
    }
}

ComPtr<ID3D12PipelineState> D3D12PSOCache::loadGraphicsPSO(const wchar_t* name,
                                                             const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    if (!m_library) return nullptr;

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = m_library->LoadGraphicsPipeline(name, &desc, IID_PPV_ARGS(pso.GetAddressOf()));
    if (SUCCEEDED(hr))
        return pso;

    return nullptr; // Not in cache
}

void D3D12PSOCache::storePSO(const wchar_t* name, ID3D12PipelineState* pso)
{
    if (!m_library || !pso) return;

    HRESULT hr = m_library->StorePipeline(name, pso);
    if (FAILED(hr))
    {
        LOG_ENGINE_TRACE("[D3D12 PSOCache] Failed to store PSO '{}' (may already exist)", "pso");
    }
}
