#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12RenderAPI.hpp"
#include "D3D12Mesh.hpp"
#include "Components/mesh.hpp"
#include "Graphics/RenderCommandBuffer.hpp"
#include "Utils/Log.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <future>
#include <thread>

#include "stb_image.h"

// ============================================================================
// Texture Management
// ============================================================================

TextureHandle D3D12RenderAPI::loadTexture(const std::string& filename, bool invert_y, bool generate_mipmaps)
{
    int width, height, channels;
    stbi_set_flip_vertically_on_load(invert_y);
    uint8_t* pixels = stbi_load(filename.c_str(), &width, &height, &channels, 4);
    stbi_set_flip_vertically_on_load(false);

    if (!pixels)
    {
        LOG_ENGINE_ERROR("Failed to load texture: {}", filename);
        return INVALID_TEXTURE;
    }

    TextureHandle handle = loadTextureFromMemory(pixels, width, height, 4, false, generate_mipmaps);
    stbi_image_free(pixels);
    return handle;
}

std::vector<uint8_t> D3D12RenderAPI::generateMipLevel(const uint8_t* src, int srcWidth, int srcHeight,
                                                        int channels, int& outWidth, int& outHeight)
{
    outWidth = std::max(1, srcWidth / 2);
    outHeight = std::max(1, srcHeight / 2);
    std::vector<uint8_t> result(outWidth * outHeight * channels);

    for (int y = 0; y < outHeight; y++)
    {
        for (int x = 0; x < outWidth; x++)
        {
            int sx = x * 2, sy = y * 2;
            for (int c = 0; c < channels; c++)
            {
                int sum = 0;
                int count = 0;
                auto sample = [&](int px, int py) {
                    if (px < srcWidth && py < srcHeight)
                    {
                        sum += src[(py * srcWidth + px) * channels + c];
                        count++;
                    }
                };
                sample(sx, sy);
                sample(sx + 1, sy);
                sample(sx, sy + 1);
                sample(sx + 1, sy + 1);
                result[(y * outWidth + x) * channels + c] = static_cast<uint8_t>(sum / count);
            }
        }
    }
    return result;
}

TextureHandle D3D12RenderAPI::loadTextureFromMemory(const uint8_t* pixels, int width, int height,
                                                     int channels, bool flip_vertically, bool generate_mipmaps)
{
    if (!pixels || width <= 0 || height <= 0) return INVALID_TEXTURE;

    // Convert to RGBA if needed
    std::vector<uint8_t> rgbaData;
    const uint8_t* srcData = pixels;
    if (channels != 4)
    {
        rgbaData.resize(width * height * 4);
        for (int i = 0; i < width * height; i++)
        {
            if (channels == 1)
            {
                rgbaData[i * 4 + 0] = pixels[i];
                rgbaData[i * 4 + 1] = pixels[i];
                rgbaData[i * 4 + 2] = pixels[i];
                rgbaData[i * 4 + 3] = 255;
            }
            else if (channels == 3)
            {
                rgbaData[i * 4 + 0] = pixels[i * 3 + 0];
                rgbaData[i * 4 + 1] = pixels[i * 3 + 1];
                rgbaData[i * 4 + 2] = pixels[i * 3 + 2];
                rgbaData[i * 4 + 3] = 255;
            }
        }
        srcData = rgbaData.data();
        channels = 4;
    }

    // Flip vertically if needed
    std::vector<uint8_t> flippedData;
    if (flip_vertically)
    {
        flippedData.resize(width * height * 4);
        int rowBytes = width * 4;
        for (int y = 0; y < height; y++)
            memcpy(&flippedData[y * rowBytes], &srcData[(height - 1 - y) * rowBytes], rowBytes);
        srcData = flippedData.data();
    }

    // Calculate mip levels
    int mipLevels = 1;
    if (generate_mipmaps)
    {
        int w = width, h = height;
        while (w > 1 || h > 1)
        {
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
            mipLevels++;
        }
    }

    // Create texture resource
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = static_cast<UINT16>(mipLevels);
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;

    D3D12Texture tex;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(tex.resource.GetAddressOf()));
    if (FAILED(hr)) return INVALID_TEXTURE;

    // Generate all mip levels on CPU
    struct MipData
    {
        std::vector<uint8_t> pixels;
        int width, height;
    };
    std::vector<MipData> mips(mipLevels);
    mips[0].pixels.assign(srcData, srcData + width * height * 4);
    mips[0].width = width;
    mips[0].height = height;

    for (int i = 1; i < mipLevels; i++)
    {
        int mw, mh;
        mips[i].pixels = generateMipLevel(mips[i - 1].pixels.data(), mips[i - 1].width, mips[i - 1].height, 4, mw, mh);
        mips[i].width = mw;
        mips[i].height = mh;
    }

    // Calculate total upload buffer size
    size_t totalUploadSize = 0;
    for (int i = 0; i < mipLevels; i++)
    {
        UINT64 rowPitch = AlignUp(mips[i].width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        totalUploadSize += rowPitch * mips[i].height;
    }
    totalUploadSize = AlignUp(totalUploadSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    // Create upload buffer
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = totalUploadSize + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * mipLevels;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> uploadBuffer;
    hr = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(uploadBuffer.GetAddressOf()));
    if (FAILED(hr)) return INVALID_TEXTURE;

    // Map and copy mip data
    uint8_t* mapped = nullptr;
    uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));

    auto* copyCmdList = m_copyQueue.getCommandList();

    size_t uploadOffset = 0;
    for (int i = 0; i < mipLevels; i++)
    {
        uploadOffset = AlignUp(uploadOffset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        UINT64 rowPitch = AlignUp(mips[i].width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

        // Copy row by row to respect pitch alignment
        for (int y = 0; y < mips[i].height; y++)
        {
            memcpy(mapped + uploadOffset + y * rowPitch,
                   mips[i].pixels.data() + y * mips[i].width * 4,
                   mips[i].width * 4);
        }

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = tex.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = uploadBuffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = uploadOffset;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = mips[i].width;
        src.PlacedFootprint.Footprint.Height = mips[i].height;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

        copyCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        uploadOffset += rowPitch * mips[i].height;
    }

    uploadBuffer->Unmap(0, nullptr);

    // Track staging buffer and schedule transition on graphics queue
    m_copyQueue.retainStagingBuffer(std::move(uploadBuffer));
    m_copyQueue.addPendingTransition(tex.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Create SRV
    tex.srvIndex = m_srvAllocator.allocate();
    if (tex.srvIndex == UINT(-1))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to allocate SRV for texture ({}x{}, {} mips)", width, height, mipLevels);
        return INVALID_TEXTURE;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = mipLevels;
    device->CreateShaderResourceView(tex.resource.Get(), &srvDesc,
                                      m_srvAllocator.getCPU(tex.srvIndex));

    tex.width = width;
    tex.height = height;

    TextureHandle handle = nextTextureHandle++;
    UINT srvIdx = tex.srvIndex;
    textures[handle] = std::move(tex);
    LOG_ENGINE_TRACE("[D3D12] Loaded texture #{}: {}x{}, {} mips, SRV index {}",
                      handle, width, height, mipLevels, srvIdx);
    return handle;
}

TextureHandle D3D12RenderAPI::loadCompressedTexture(int width, int height, uint32_t format, int mip_count,
                                                     const std::vector<const uint8_t*>& mip_data,
                                                     const std::vector<size_t>& mip_sizes,
                                                     const std::vector<std::pair<int,int>>& mip_dimensions)
{
    if (mip_count <= 0 || mip_data.empty()) return INVALID_TEXTURE;

    DXGI_FORMAT dxgiFormat;
    UINT blockSize = 0;
    bool isBC = false;
    switch (format) {
    case 0: dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case 1: dxgiFormat = DXGI_FORMAT_BC1_UNORM; blockSize = 8; isBC = true; break;
    case 2: dxgiFormat = DXGI_FORMAT_BC3_UNORM; blockSize = 16; isBC = true; break;
    case 3: dxgiFormat = DXGI_FORMAT_BC5_UNORM; blockSize = 16; isBC = true; break;
    case 4: dxgiFormat = DXGI_FORMAT_BC7_UNORM; blockSize = 16; isBC = true; break;
    default: return INVALID_TEXTURE;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = static_cast<UINT16>(mip_count);
    texDesc.Format = dxgiFormat;
    texDesc.SampleDesc.Count = 1;

    D3D12Texture tex;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(tex.resource.GetAddressOf()));
    if (FAILED(hr)) return INVALID_TEXTURE;

    // Calculate total upload buffer size
    size_t totalUploadSize = 0;
    for (int i = 0; i < mip_count; i++) {
        UINT64 rowPitch;
        if (isBC) {
            UINT blockWidth = (mip_dimensions[i].first + 3) / 4;
            rowPitch = AlignUp(static_cast<size_t>(blockWidth) * blockSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            UINT blockHeight = (mip_dimensions[i].second + 3) / 4;
            totalUploadSize = AlignUp(totalUploadSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            totalUploadSize += rowPitch * blockHeight;
        } else {
            rowPitch = AlignUp(static_cast<size_t>(mip_dimensions[i].first) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            totalUploadSize = AlignUp(totalUploadSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
            totalUploadSize += rowPitch * mip_dimensions[i].second;
        }
    }
    totalUploadSize = AlignUp(totalUploadSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    // Create upload buffer
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = totalUploadSize + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * mip_count;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> uploadBuffer;
    hr = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(uploadBuffer.GetAddressOf()));
    if (FAILED(hr)) return INVALID_TEXTURE;

    uint8_t* mapped = nullptr;
    uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));

    auto* copyCmdList = m_copyQueue.getCommandList();

    size_t uploadOffset = 0;
    for (int i = 0; i < mip_count; i++) {
        uploadOffset = AlignUp(uploadOffset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

        int mw = mip_dimensions[i].first;
        int mh = mip_dimensions[i].second;
        UINT64 rowPitch;
        int numRows;

        if (isBC) {
            UINT blockWidth = (mw + 3) / 4;
            numRows = (mh + 3) / 4;
            UINT srcRowBytes = blockWidth * blockSize;
            rowPitch = AlignUp(static_cast<size_t>(srcRowBytes), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

            // Copy row-of-blocks by row-of-blocks
            const uint8_t* src = mip_data[i];
            for (int row = 0; row < numRows; row++) {
                memcpy(mapped + uploadOffset + row * rowPitch,
                       src + row * srcRowBytes, srcRowBytes);
            }
        } else {
            numRows = mh;
            rowPitch = AlignUp(static_cast<size_t>(mw) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            const uint8_t* src = mip_data[i];
            for (int y = 0; y < mh; y++) {
                memcpy(mapped + uploadOffset + y * rowPitch,
                       src + y * mw * 4, mw * 4);
            }
        }

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = tex.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;

        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = uploadBuffer.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Offset = uploadOffset;
        srcLoc.PlacedFootprint.Footprint.Format = dxgiFormat;
        srcLoc.PlacedFootprint.Footprint.Width = mw;
        srcLoc.PlacedFootprint.Footprint.Height = mh;
        srcLoc.PlacedFootprint.Footprint.Depth = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

        copyCmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
        uploadOffset += rowPitch * numRows;
    }

    uploadBuffer->Unmap(0, nullptr);

    // Track staging buffer and schedule transition on graphics queue
    m_copyQueue.retainStagingBuffer(std::move(uploadBuffer));
    m_copyQueue.addPendingTransition(tex.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Create SRV
    tex.srvIndex = m_srvAllocator.allocate();
    if (tex.srvIndex == UINT(-1))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to allocate SRV for compressed texture ({}x{}, {} mips, format {})",
                          width, height, mip_count, format);
        return INVALID_TEXTURE;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = dxgiFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = mip_count;
    device->CreateShaderResourceView(tex.resource.Get(), &srvDesc,
                                      m_srvAllocator.getCPU(tex.srvIndex));

    tex.width = width;
    tex.height = height;

    TextureHandle handle = nextTextureHandle++;
    textures[handle] = std::move(tex);
    LOG_ENGINE_TRACE("[D3D12] loadCompressedTexture: handle {} ({}x{}, {} mips, format {})",
                      handle, width, height, mip_count, format);
    return handle;
}

void D3D12RenderAPI::bindTexture(TextureHandle texture)
{
    if (texture == currentBoundTexture) return;

    auto it = textures.find(texture);
    if (it != textures.end())
    {
        commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(it->second.srvIndex));
        currentBoundTexture = texture;
    }
    else
    {
        unbindTexture();
    }
}

void D3D12RenderAPI::unbindTexture()
{
    if (defaultTexture != INVALID_TEXTURE)
        bindTexture(defaultTexture);
    currentBoundTexture = INVALID_TEXTURE;
}

void D3D12RenderAPI::deleteTexture(TextureHandle texture)
{
    auto it = textures.find(texture);
    if (it != textures.end())
    {
        // Flush GPU once to ensure it's done with this texture, then release.
        // Callers deleting many textures should batch their own flushGPU() call
        // and use the internal erase path to avoid repeated stalls.
        if (!device_lost)
            flushGPU();
        if (it->second.srvIndex != UINT(-1))
            m_srvAllocator.free(it->second.srvIndex);
        textures.erase(it);
    }
}

// ============================================================================
// Mesh Rendering
// ============================================================================

void D3D12RenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0) return;
    if (device_lost) return;

    // Lazy GPU upload
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU(this);
    }

    D3D12Mesh* gpuMesh = dynamic_cast<D3D12Mesh*>(m.gpu_mesh);
    if (!gpuMesh || !gpuMesh->isUploaded()) return;

    if (in_shadow_pass)
    {
        // Shadow pass: just update shadow CBuffer and draw
        updateShadowCBuffer(lightSpaceMatrices[currentCascade], current_model_matrix);

        commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
        if (gpuMesh->isIndexed())
        {
            commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
            commandList->DrawIndexedInstanced(static_cast<UINT>(gpuMesh->getIndexCount()), 1, 0, 0, 0);
        }
        else
        {
            commandList->DrawInstanced(static_cast<UINT>(gpuMesh->getVertexCount()), 1, 0, 0);
        }
        return;
    }

    // Normal pass
    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }

    bool useTexture = (m.texture != INVALID_TEXTURE);
    updatePerObjectCBuffer(state.color, useTexture);

    // Upload light data once per frame, reuse cached address for subsequent meshes
    if (m_cachedLightCBAddr == 0)
    {
        m_cachedLightCBAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(LightCBuffer), &current_lights);
        if (m_cachedLightCBAddr == 0) return;
    }
    commandList->SetGraphicsRootConstantBufferView(4, m_cachedLightCBAddr);

    // Select and bind PSO
    bool unlit = !state.lighting || !lighting_enabled;
    ID3D12PipelineState* pso = selectPSO(state, unlit);
    if (pso != last_bound_pso)
    {
        commandList->SetPipelineState(pso);
        last_bound_pso = pso;
    }

    // Bind texture
    if (useTexture)
        bindTexture(m.texture);
    else if (defaultTexture != INVALID_TEXTURE)
        bindTexture(defaultTexture);

    // Bind default PBR textures (root params 5-8)
    if (m_defaultMetallicRoughnessTexture.srvIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(5, m_srvAllocator.getGPU(m_defaultMetallicRoughnessTexture.srvIndex));
    if (m_defaultNormalTexture.srvIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(6, m_srvAllocator.getGPU(m_defaultNormalTexture.srvIndex));
    if (m_defaultOcclusionTexture.srvIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(7, m_srvAllocator.getGPU(m_defaultOcclusionTexture.srvIndex));
    if (m_defaultEmissiveTexture.srvIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(8, m_srvAllocator.getGPU(m_defaultEmissiveTexture.srvIndex));

    // Draw
    commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
    if (gpuMesh->isIndexed())
    {
        commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
        commandList->DrawIndexedInstanced(static_cast<UINT>(gpuMesh->getIndexCount()), 1, 0, 0, 0);
    }
    else
    {
        commandList->DrawInstanced(static_cast<UINT>(gpuMesh->getVertexCount()), 1, 0, 0);
    }
}

void D3D12RenderAPI::renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state)
{
    if (!m.visible || !m.is_valid || vertex_count == 0) return;
    if (device_lost) return;

    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
        const_cast<mesh&>(m).uploadToGPU(this);

    D3D12Mesh* gpuMesh = dynamic_cast<D3D12Mesh*>(m.gpu_mesh);
    if (!gpuMesh || !gpuMesh->isUploaded()) return;

    if (in_shadow_pass)
    {
        updateShadowCBuffer(lightSpaceMatrices[currentCascade], current_model_matrix);
        commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
        if (gpuMesh->isIndexed())
        {
            commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
            commandList->DrawIndexedInstanced(static_cast<UINT>(vertex_count), 1,
                                               static_cast<UINT>(start_vertex), 0, 0);
        }
        else
        {
            commandList->DrawInstanced(static_cast<UINT>(vertex_count), 1,
                                        static_cast<UINT>(start_vertex), 0);
        }
        return;
    }

    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }

    updatePerObjectCBuffer(state.color, true);

    // Upload light data once per frame, reuse cached address for subsequent meshes
    if (m_cachedLightCBAddr == 0)
    {
        m_cachedLightCBAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(LightCBuffer), &current_lights);
        if (m_cachedLightCBAddr == 0) return;
    }
    commandList->SetGraphicsRootConstantBufferView(4, m_cachedLightCBAddr);

    bool unlit = !state.lighting || !lighting_enabled;
    ID3D12PipelineState* pso = selectPSO(state, unlit);
    if (pso != last_bound_pso)
    {
        commandList->SetPipelineState(pso);
        last_bound_pso = pso;
    }

    commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
    if (gpuMesh->isIndexed())
    {
        commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
        commandList->DrawIndexedInstanced(static_cast<UINT>(vertex_count), 1,
                                           static_cast<UINT>(start_vertex), 0, 0);
    }
    else
    {
        commandList->DrawInstanced(static_cast<UINT>(vertex_count), 1,
                                    static_cast<UINT>(start_vertex), 0);
    }
}

// ============================================================================
// Command Buffer Replay (Multicore Rendering)
// ============================================================================

void D3D12RenderAPI::flushAndReopenCommandList()
{
    if (!m_commandListOpen || device_lost) return;

    flushBarriers();
    commandList->Close();
    m_commandListOpen = false;

    ID3D12CommandList* lists[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(1, lists);

    // Reopen immediately for subsequent work (postamble, UI, etc.)
    // Don't wait for GPU -- we just need the CPU-side cmd list open again.
    // The command allocator is still valid for this frame.
    commandList->Reset(m_frameContexts[m_frameIndex].commandAllocator.Get(), nullptr);
    m_commandListOpen = true;

    // Restore essential state on the reopened command list
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    if (m_currentRT.valid)
    {
        commandList->OMSetRenderTargets(1, &m_currentRT.rtvHandle, FALSE, &m_currentRT.dsvHandle);
        commandList->RSSetViewports(1, &m_currentRT.viewport);
        commandList->RSSetScissorRects(1, &m_currentRT.scissor);
    }

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (m_shadowSRVIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(3, m_srvAllocator.getGPU(m_shadowSRVIndex));
    if (m_pointLightsSRVIndex[m_frameIndex] != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(9, m_srvAllocator.getGPU(m_pointLightsSRVIndex[m_frameIndex]));
    if (m_spotLightsSRVIndex[m_frameIndex] != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(10, m_srvAllocator.getGPU(m_spotLightsSRVIndex[m_frameIndex]));

    // Restore PBR texture bindings (root params 5-8)
    if (m_defaultMetallicRoughnessTexture.srvIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(5, m_srvAllocator.getGPU(m_defaultMetallicRoughnessTexture.srvIndex));
    if (m_defaultNormalTexture.srvIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(6, m_srvAllocator.getGPU(m_defaultNormalTexture.srvIndex));
    if (m_defaultOcclusionTexture.srvIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(7, m_srvAllocator.getGPU(m_defaultOcclusionTexture.srvIndex));
    if (m_defaultEmissiveTexture.srvIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(8, m_srvAllocator.getGPU(m_defaultEmissiveTexture.srvIndex));

    // Force rebind of PSO and texture
    last_bound_pso = nullptr;
    currentBoundTexture = INVALID_TEXTURE;
    global_cbuffer_dirty = true;
}

static constexpr size_t D3D12_PARALLEL_REPLAY_THRESHOLD = 512;

void D3D12RenderAPI::replayCommandBufferParallel(const RenderCommandBuffer& cmds)
{
    if (cmds.empty() || device_lost) return;

    // Fall back to single-threaded for small buffers or if pool isn't initialized
    if (cmds.size() < D3D12_PARALLEL_REPLAY_THRESHOLD || m_commandListPool.capacity() == 0)
    {
        replayCommandBuffer(cmds);
        return;
    }

    if (!m_currentRT.valid)
    {
        replayCommandBuffer(cmds);
        return;
    }

    // Ensure global CBuffer is uploaded before workers reference it
    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }

    // Upload light data once (workers will reference this address)
    if (m_cachedLightCBAddr == 0)
    {
        m_cachedLightCBAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(LightCBuffer), &current_lights);
        if (m_cachedLightCBAddr == 0)
        {
            replayCommandBuffer(cmds);
            return;
        }
    }

    // Cache the global CBuffer address for workers
    D3D12GlobalCBuffer globalCB = {};
    globalCB.view = view_matrix;
    globalCB.projection = projection_matrix;
    for (int i = 0; i < NUM_CASCADES; i++)
        globalCB.lightSpaceMatrices[i] = lightSpaceMatrices[i];
    globalCB.cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                        cascadeSplitDistances[2], cascadeSplitDistances[3]);
    globalCB.cascadeSplit4 = cascadeSplitDistances[NUM_CASCADES];
    globalCB.lightDir = current_light_direction;
    globalCB.cascadeCount = NUM_CASCADES;
    globalCB.lightAmbient = current_light_ambient;
    globalCB.lightDiffuse = current_light_diffuse;
    globalCB.debugCascades = debugCascades ? 1 : 0;
    globalCB.shadowMapTexelSize = glm::vec2(1.0f / static_cast<float>(currentShadowSize));

    // Flush main command list (preamble complete)
    flushAndReopenCommandList();

    // Determine chunk count based on available command lists
    uint32_t max_workers = m_commandListPool.capacity();
    uint32_t num_workers = std::min(max_workers,
        static_cast<uint32_t>((cmds.size() + D3D12_PARALLEL_REPLAY_THRESHOLD - 1) / D3D12_PARALLEL_REPLAY_THRESHOLD));
    num_workers = std::max(num_workers, 1u);

    size_t chunk_size = (cmds.size() + num_workers - 1) / num_workers;

    // Acquire worker command lists
    struct WorkerData
    {
        D3D12CommandListPool::Entry* entry = nullptr;
        size_t start = 0;
        size_t end = 0;
    };
    std::vector<WorkerData> workers(num_workers);

    for (uint32_t w = 0; w < num_workers; w++)
    {
        workers[w].entry = m_commandListPool.acquire(nullptr);
        if (!workers[w].entry)
        {
            num_workers = w;
            break;
        }
        workers[w].start = w * chunk_size;
        workers[w].end = std::min(workers[w].start + chunk_size, cmds.size());
    }

    if (num_workers == 0)
    {
        // Pool exhausted, fall back
        replayCommandBuffer(cmds);
        return;
    }

    // Capture shared state for workers
    auto rootSig = m_rootSignature.Get();
    auto srvHeap = m_srvHeap.Get();
    auto rtState = m_currentRT;
    auto shadowSRVIdx = m_shadowSRVIndex;
    auto defaultTex = defaultTexture;
    auto frameIdx = m_frameIndex;
    auto lightCBAddr = m_cachedLightCBAddr;
    auto* uploadBuf = &m_cbUploadBuffer[m_frameIndex];
    auto* srvAlloc = &m_srvAllocator;
    auto* textureMap = &textures;
    auto pbrMetallicRoughnessSRV = m_defaultMetallicRoughnessTexture.srvIndex;
    auto pbrNormalSRV = m_defaultNormalTexture.srvIndex;
    auto pbrOcclusionSRV = m_defaultOcclusionTexture.srvIndex;
    auto pbrEmissiveSRV = m_defaultEmissiveTexture.srvIndex;

    // Launch parallel replay on worker threads
    std::vector<std::future<void>> futures;
    futures.reserve(num_workers);

    for (uint32_t w = 0; w < num_workers; w++)
    {
        futures.push_back(std::async(std::launch::async,
            [&, w, rootSig, srvHeap, rtState, shadowSRVIdx, defaultTex, lightCBAddr, uploadBuf, srvAlloc, textureMap,
             pbrMetallicRoughnessSRV, pbrNormalSRV, pbrOcclusionSRV, pbrEmissiveSRV]()
            {
                auto* cmdList = workers[w].entry->cmdList.Get();

                // Set up command list state (must be done independently per list)
                ID3D12DescriptorHeap* heaps[] = { srvHeap };
                cmdList->SetDescriptorHeaps(1, heaps);
                cmdList->SetGraphicsRootSignature(rootSig);
                cmdList->OMSetRenderTargets(1, &rtState.rtvHandle, FALSE, &rtState.dsvHandle);
                cmdList->RSSetViewports(1, &rtState.viewport);
                cmdList->RSSetScissorRects(1, &rtState.scissor);
                cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                // Upload global CBuffer for this command list
                auto globalAddr = uploadBuf->allocate(sizeof(D3D12GlobalCBuffer), &globalCB);
                if (globalAddr != 0)
                    cmdList->SetGraphicsRootConstantBufferView(0, globalAddr);

                // Bind light CBuffer
                cmdList->SetGraphicsRootConstantBufferView(4, lightCBAddr);

                // Bind shadow map
                if (shadowSRVIdx != UINT(-1))
                    cmdList->SetGraphicsRootDescriptorTable(3, srvAlloc->getGPU(shadowSRVIdx));

                // Bind default PBR textures (root params 5-8)
                if (pbrMetallicRoughnessSRV != UINT(-1))
                    cmdList->SetGraphicsRootDescriptorTable(5, srvAlloc->getGPU(pbrMetallicRoughnessSRV));
                if (pbrNormalSRV != UINT(-1))
                    cmdList->SetGraphicsRootDescriptorTable(6, srvAlloc->getGPU(pbrNormalSRV));
                if (pbrOcclusionSRV != UINT(-1))
                    cmdList->SetGraphicsRootDescriptorTable(7, srvAlloc->getGPU(pbrOcclusionSRV));
                if (pbrEmissiveSRV != UINT(-1))
                    cmdList->SetGraphicsRootDescriptorTable(8, srvAlloc->getGPU(pbrEmissiveSRV));

                // Replay this worker's chunk of commands
                ID3D12PipelineState* workerLastPSO = nullptr;
                TextureHandle workerLastTex = INVALID_TEXTURE;

                for (size_t i = workers[w].start; i < workers[w].end; i++)
                {
                    const auto& cmd = cmds[i];
                    if (!cmd.gpu_mesh || !cmd.gpu_mesh->isUploaded()) continue;

                    D3D12Mesh* gpuMesh = dynamic_cast<D3D12Mesh*>(cmd.gpu_mesh);
                    if (!gpuMesh || !gpuMesh->isUploaded()) continue;

                    // Per-object CBuffer
                    D3D12PerObjectCBuffer objCB = {};
                    objCB.model = cmd.model_matrix;
                    glm::mat3 normalMat3 = glm::mat3(cmd.model_matrix);
                    float det = glm::determinant(normalMat3);
                    if (std::abs(det) > 1e-6f)
                        objCB.normalMatrix = glm::mat4(glm::transpose(glm::inverse(normalMat3)));
                    else
                        objCB.normalMatrix = glm::mat4(1.0f);
                    objCB.color = cmd.color;
                    objCB.useTexture = cmd.use_texture ? 1 : 0;
                    objCB.alphaCutoff = cmd.alpha_cutoff;
                    objCB.metallic = cmd.metallic;
                    objCB.roughness = cmd.roughness;
                    objCB.emissive = cmd.emissive;
                    objCB.hasMetallicRoughnessMap = 0;
                    objCB.hasNormalMap = 0;
                    objCB.hasOcclusionMap = 0;
                    objCB.hasEmissiveMap = 0;

                    auto objAddr = uploadBuf->allocate(sizeof(objCB), &objCB);
                    if (objAddr == 0) continue;
                    cmdList->SetGraphicsRootConstantBufferView(1, objAddr);

                    // Select PSO (or honor caller-set GBuffer override).
                    ID3D12PipelineState* pso = m_replayPSOOverride;
                    if (!pso)
                    {
                        RenderState rs;
                        rs.blend_mode = cmd.pso_key.blend;
                        rs.cull_mode = cmd.pso_key.cull;
                        rs.lighting = cmd.pso_key.lighting;
                        bool unlit = !cmd.pso_key.lighting;
                        pso = selectPSO(rs, unlit);
                    }
                    if (pso != workerLastPSO)
                    {
                        cmdList->SetPipelineState(pso);
                        workerLastPSO = pso;
                    }

                    // Bind texture
                    TextureHandle texToBind = cmd.use_texture && cmd.texture != INVALID_TEXTURE
                                              ? cmd.texture : defaultTex;
                    if (texToBind != workerLastTex && texToBind != INVALID_TEXTURE)
                    {
                        auto it = textureMap->find(texToBind);
                        if (it != textureMap->end())
                            cmdList->SetGraphicsRootDescriptorTable(2, srvAlloc->getGPU(it->second.srvIndex));
                        workerLastTex = texToBind;
                    }

                    // Draw
                    cmdList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
                    if (gpuMesh->isIndexed())
                    {
                        cmdList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
                        if (cmd.vertex_count > 0)
                            cmdList->DrawIndexedInstanced(static_cast<UINT>(cmd.vertex_count), 1,
                                                          static_cast<UINT>(cmd.start_vertex), 0, 0);
                        else
                            cmdList->DrawIndexedInstanced(static_cast<UINT>(gpuMesh->getIndexCount()), 1, 0, 0, 0);
                    }
                    else
                    {
                        if (cmd.vertex_count > 0)
                            cmdList->DrawInstanced(static_cast<UINT>(cmd.vertex_count), 1,
                                                    static_cast<UINT>(cmd.start_vertex), 0);
                        else
                            cmdList->DrawInstanced(static_cast<UINT>(gpuMesh->getVertexCount()), 1, 0, 0);
                    }
                }

                // Close this worker's command list
                cmdList->Close();
            }));
    }

    // Wait for all workers to finish recording
    for (auto& f : futures)
        f.get();

    // Submit all worker command lists at once
    auto activeLists = m_commandListPool.getActiveCommandLists();
    if (!activeLists.empty())
    {
        commandQueue->ExecuteCommandLists(static_cast<UINT>(activeLists.size()), activeLists.data());

        // Capture the fence value that covers this pool submission. The pool
        // can't be reset until the GPU is past this. The per-frame-slot fence
        // alone isn't enough — with NUM_FRAMES_IN_FLIGHT=2 it lags this one
        // by a full frame, leaving pool work from frame N-1 in flight when
        // frame N+1's ensureCommandListOpen calls resetAll().
        m_fenceValue++;
        commandQueue->Signal(m_fence.Get(), m_fenceValue);
        m_commandListPool.setLastSubmissionFence(m_fenceValue);
    }
}

void D3D12RenderAPI::replayCommandBuffer(const RenderCommandBuffer& cmds)
{
    if (cmds.empty() || device_lost) return;

    for (const auto& cmd : cmds)
    {
        if (!cmd.gpu_mesh || !cmd.gpu_mesh->isUploaded()) continue;

        D3D12Mesh* gpuMesh = dynamic_cast<D3D12Mesh*>(cmd.gpu_mesh);
        if (!gpuMesh || !gpuMesh->isUploaded()) continue;

        if (cmd.pso_key.shadow)
        {
            // Shadow pass draw: select pipeline based on alpha test
            if (cmd.pso_key.alpha_test && m_psoShadowAlphaTest)
            {
                commandList->SetPipelineState(m_psoShadowAlphaTest.Get());
                // Alpha-test shadow needs per-object CB (for alphaCutoff) and texture
                glm::mat4 saved_model = current_model_matrix;
                current_model_matrix = cmd.model_matrix;
                updatePerObjectCBuffer(glm::vec3(1.0f), true, cmd.alpha_cutoff);
                current_model_matrix = saved_model;
                if (cmd.use_texture && cmd.texture != INVALID_TEXTURE)
                    bindTexture(cmd.texture);
            }
            updateShadowCBuffer(lightSpaceMatrices[currentCascade], cmd.model_matrix);

            commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
            if (gpuMesh->isIndexed())
            {
                commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
                if (cmd.vertex_count > 0)
                    commandList->DrawIndexedInstanced(static_cast<UINT>(cmd.vertex_count), 1,
                                                      static_cast<UINT>(cmd.start_vertex), 0, 0);
                else
                    commandList->DrawIndexedInstanced(static_cast<UINT>(gpuMesh->getIndexCount()), 1, 0, 0, 0);
            }
            else
            {
                if (cmd.vertex_count > 0)
                    commandList->DrawInstanced(static_cast<UINT>(cmd.vertex_count), 1,
                                                static_cast<UINT>(cmd.start_vertex), 0);
                else
                    commandList->DrawInstanced(static_cast<UINT>(gpuMesh->getVertexCount()), 1, 0, 0);
            }
            // Restore opaque shadow PSO for next draw
            if (cmd.pso_key.alpha_test && m_psoShadowAlphaTest)
                commandList->SetPipelineState(m_psoShadow.Get());
            continue;
        }

        if (cmd.pso_key.depth_only)
        {
            // Depth prepass draw: select PSO based on alpha test
            if (cmd.pso_key.alpha_test) {
                RenderState depth_rs;
                depth_rs.alpha_test = true;
                depth_rs.cull_mode = cmd.pso_key.cull;
                ID3D12PipelineState* pso = selectPSO(depth_rs, false);
                if (pso != last_bound_pso) {
                    commandList->SetPipelineState(pso);
                    last_bound_pso = pso;
                }
                // Alpha-test depth prepass needs texture + global CB for view/projection
                if (global_cbuffer_dirty) { updateGlobalCBuffer(); global_cbuffer_dirty = false; }
                if (cmd.use_texture && cmd.texture != INVALID_TEXTURE)
                    bindTexture(cmd.texture);
            }
            // Update per-object CBuffer
            glm::mat4 saved_model = current_model_matrix;
            current_model_matrix = cmd.model_matrix;
            updatePerObjectCBuffer(glm::vec3(1.0f), cmd.pso_key.alpha_test && cmd.use_texture, cmd.alpha_cutoff);
            current_model_matrix = saved_model;

            commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
            if (gpuMesh->isIndexed())
            {
                commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
                if (cmd.vertex_count > 0)
                    commandList->DrawIndexedInstanced(static_cast<UINT>(cmd.vertex_count), 1,
                                                      static_cast<UINT>(cmd.start_vertex), 0, 0);
                else
                    commandList->DrawIndexedInstanced(static_cast<UINT>(gpuMesh->getIndexCount()), 1, 0, 0, 0);
            }
            else
            {
                if (cmd.vertex_count > 0)
                    commandList->DrawInstanced(static_cast<UINT>(cmd.vertex_count), 1,
                                                static_cast<UINT>(cmd.start_vertex), 0);
                else
                    commandList->DrawInstanced(static_cast<UINT>(gpuMesh->getVertexCount()), 1, 0, 0);
            }
            continue;
        }

        // Main pass draw
        if (global_cbuffer_dirty)
        {
            updateGlobalCBuffer();
            global_cbuffer_dirty = false;
        }

        // Upload per-object CBuffer with the command's model matrix
        {
            glm::mat4 saved_model = current_model_matrix;
            current_model_matrix = cmd.model_matrix;
            updatePerObjectCBuffer(cmd.color, cmd.use_texture, cmd.alpha_cutoff);
            current_model_matrix = saved_model;
        }

        // Upload light data once per frame
        if (m_cachedLightCBAddr == 0)
        {
            m_cachedLightCBAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(LightCBuffer), &current_lights);
            if (m_cachedLightCBAddr == 0) continue;
        }
        commandList->SetGraphicsRootConstantBufferView(4, m_cachedLightCBAddr);

        // Select PSO from PSOKey (or honor caller-set GBuffer override).
        ID3D12PipelineState* pso = m_replayPSOOverride;
        if (!pso)
        {
            RenderState rs;
            rs.blend_mode = cmd.pso_key.blend;
            rs.cull_mode = cmd.pso_key.cull;
            rs.lighting = cmd.pso_key.lighting;
            rs.alpha_test = cmd.pso_key.alpha_test;
            bool unlit = !cmd.pso_key.lighting;
            pso = selectPSO(rs, unlit);
        }
        if (pso != last_bound_pso)
        {
            commandList->SetPipelineState(pso);
            last_bound_pso = pso;
        }

        // Bind texture
        if (cmd.use_texture && cmd.texture != INVALID_TEXTURE)
            bindTexture(cmd.texture);
        else if (defaultTexture != INVALID_TEXTURE)
            bindTexture(defaultTexture);

        // Bind default PBR textures (root params 5-8)
        if (m_defaultMetallicRoughnessTexture.srvIndex != UINT(-1))
            commandList->SetGraphicsRootDescriptorTable(5, m_srvAllocator.getGPU(m_defaultMetallicRoughnessTexture.srvIndex));
        if (m_defaultNormalTexture.srvIndex != UINT(-1))
            commandList->SetGraphicsRootDescriptorTable(6, m_srvAllocator.getGPU(m_defaultNormalTexture.srvIndex));
        if (m_defaultOcclusionTexture.srvIndex != UINT(-1))
            commandList->SetGraphicsRootDescriptorTable(7, m_srvAllocator.getGPU(m_defaultOcclusionTexture.srvIndex));
        if (m_defaultEmissiveTexture.srvIndex != UINT(-1))
            commandList->SetGraphicsRootDescriptorTable(8, m_srvAllocator.getGPU(m_defaultEmissiveTexture.srvIndex));

        // Draw
        commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
        if (gpuMesh->isIndexed())
        {
            commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
            if (cmd.vertex_count > 0)
                commandList->DrawIndexedInstanced(static_cast<UINT>(cmd.vertex_count), 1,
                                                  static_cast<UINT>(cmd.start_vertex), 0, 0);
            else
                commandList->DrawIndexedInstanced(static_cast<UINT>(gpuMesh->getIndexCount()), 1, 0, 0, 0);
        }
        else
        {
            if (cmd.vertex_count > 0)
                commandList->DrawInstanced(static_cast<UINT>(cmd.vertex_count), 1,
                                            static_cast<UINT>(cmd.start_vertex), 0);
            else
                commandList->DrawInstanced(static_cast<UINT>(gpuMesh->getVertexCount()), 1, 0, 0);
        }
    }
}

void D3D12RenderAPI::setDeferredEnabled(bool enabled)
{
    m_useDeferred = enabled;
}

bool D3D12RenderAPI::isDeferredActive() const
{
    return m_useDeferred
        && lighting_enabled
        && m_gbufferPass.isInitialized()
        && m_deferredLightingPass.isInitialized();
}

void D3D12RenderAPI::submitDeferredOpaqueCommands(const RenderCommandBuffer& cmds)
{
    m_deferredOpaqueCmds = cmds;
}

void D3D12RenderAPI::submitDeferredTransparentCommands(const RenderCommandBuffer& cmds)
{
    m_deferredTransparentCmds = cmds;
}

bool D3D12RenderAPI::createDeferredLightBuffers()
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    auto createStructuredBuffer = [&](SIZE_T stride, ComPtr<ID3D12Resource>& outRes,
                                      void*& outMapped, UINT& outSrvIndex,
                                      const wchar_t* debugName) -> bool
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width            = stride * MAX_LIGHTS_DEFERRED;
        desc.Height           = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(outRes.GetAddressOf()));
        if (FAILED(hr)) return false;
        outRes->SetName(debugName);

        if (FAILED(outRes->Map(0, nullptr, &outMapped))) return false;
        memset(outMapped, 0, desc.Width);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                  = DXGI_FORMAT_UNKNOWN;
        srvDesc.Buffer.FirstElement        = 0;
        srvDesc.Buffer.NumElements         = MAX_LIGHTS_DEFERRED;
        srvDesc.Buffer.StructureByteStride = static_cast<UINT>(stride);
        srvDesc.Buffer.Flags               = D3D12_BUFFER_SRV_FLAG_NONE;

        outSrvIndex = m_srvAllocator.allocate();
        if (outSrvIndex == UINT(-1)) return false;
        device->CreateShaderResourceView(outRes.Get(), &srvDesc,
                                         m_srvAllocator.getCPU(outSrvIndex));
        return true;
    };

    for (int i = 0; i < NUM_FRAMES_IN_FLIGHT; ++i)
    {
        if (!createStructuredBuffer(sizeof(GPUPointLight), m_pointLightsSB[i],
                                    m_pointLightsSBMapped[i], m_pointLightsSRVIndex[i],
                                    L"DeferredPointLightsSB"))
            return false;
        if (!createStructuredBuffer(sizeof(GPUSpotLight), m_spotLightsSB[i],
                                    m_spotLightsSBMapped[i], m_spotLightsSRVIndex[i],
                                    L"DeferredSpotLightsSB"))
            return false;
    }
    return true;
}

void D3D12RenderAPI::uploadLightBuffers(const GPUPointLight* pts, int ptCount,
                                        const GPUSpotLight* spts, int spCount)
{
    if (ptCount > MAX_LIGHTS_DEFERRED) ptCount = MAX_LIGHTS_DEFERRED;
    if (spCount > MAX_LIGHTS_DEFERRED) spCount = MAX_LIGHTS_DEFERRED;
    if (ptCount < 0) ptCount = 0;
    if (spCount < 0) spCount = 0;

    if (m_pointLightsSBMapped[m_frameIndex] && pts && ptCount > 0)
        memcpy(m_pointLightsSBMapped[m_frameIndex], pts, sizeof(GPUPointLight) * ptCount);
    if (m_spotLightsSBMapped[m_frameIndex] && spts && spCount > 0)
        memcpy(m_spotLightsSBMapped[m_frameIndex], spts, sizeof(GPUSpotLight) * spCount);

    m_numPointLights = ptCount;
    m_numSpotLights  = spCount;
}

void D3D12RenderAPI::renderDebugLines(const vertex* vertices, size_t vertex_count)
{
    if (!vertices || vertex_count < 2 || device_lost) return;
    if (in_shadow_pass) return;
    if (!m_psoDebugLines) return;

    // Deferred path: buffer the vertices and let the RG TransparentForward pass
    // draw them after the lighting pass. If we drew now, the lighting pass's
    // full-screen write would wipe them (albedo/emissive are 0 at non-geometry
    // pixels) and shipped colors would be black.
    if (isDeferredActive()) {
        m_deferredDebugLineVertices.insert(m_deferredDebugLineVertices.end(),
                                           vertices, vertices + vertex_count);
        return;
    }

    renderDebugLinesDirect(vertices, vertex_count);
}

void D3D12RenderAPI::renderDebugLinesDirect(const vertex* vertices, size_t vertex_count)
{
    if (!vertices || vertex_count < 2 || device_lost) return;
    if (!m_psoDebugLines) return;

    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }

    // Upload vertex data to the upload ring buffer (it's in upload heap, can be used as VB)
    size_t dataSize = vertex_count * sizeof(vertex);
    auto vbAddr = m_cbUploadBuffer[m_frameIndex].allocate(dataSize, vertices);
    if (vbAddr == 0) return; // Ring buffer full

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = vbAddr;
    vbv.SizeInBytes = static_cast<UINT>(dataSize);
    vbv.StrideInBytes = sizeof(vertex);

    // Bind debug line PSO
    if (m_psoDebugLines.Get() != last_bound_pso)
    {
        commandList->SetPipelineState(m_psoDebugLines.Get());
        last_bound_pso = m_psoDebugLines.Get();
    }

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    commandList->IASetVertexBuffers(0, 1, &vbv);

    // Save model matrix
    glm::mat4 saved_model = current_model_matrix;
    current_model_matrix = glm::mat4(1.0f);

    // Batch draw by color
    size_t i = 0;
    while (i < vertex_count)
    {
        glm::vec3 color(vertices[i].nx, vertices[i].ny, vertices[i].nz);
        size_t batch_start = i;

        while (i < vertex_count &&
               vertices[i].nx == color.r &&
               vertices[i].ny == color.g &&
               vertices[i].nz == color.b)
        {
            i++;
        }

        updatePerObjectCBuffer(color, false);
        commandList->DrawInstanced(static_cast<UINT>(i - batch_start), 1,
                                    static_cast<UINT>(batch_start), 0);
    }

    current_model_matrix = saved_model;

    // Restore triangle topology for subsequent mesh draws
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    last_bound_pso = nullptr; // Force rebind next mesh draw
}

// ============================================================================
// Depth Prepass
// ============================================================================

void D3D12RenderAPI::beginDepthPrepass()
{
    in_depth_prepass = true;
    commandList->SetPipelineState(m_psoDepthPrepass.Get());
    last_bound_pso = m_psoDepthPrepass.Get();

    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }
}

void D3D12RenderAPI::endDepthPrepass()
{
    in_depth_prepass = false;
    last_bound_pso = nullptr;
}

void D3D12RenderAPI::renderMeshDepthOnly(const mesh& m)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0) return;
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
        const_cast<mesh&>(m).uploadToGPU(this);

    D3D12Mesh* gpuMesh = dynamic_cast<D3D12Mesh*>(m.gpu_mesh);
    if (!gpuMesh || !gpuMesh->isUploaded()) return;

    updatePerObjectCBuffer(glm::vec3(1.0f), false);

    commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
    if (gpuMesh->isIndexed())
    {
        commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
        commandList->DrawIndexedInstanced(static_cast<UINT>(gpuMesh->getIndexCount()), 1, 0, 0, 0);
    }
    else
    {
        commandList->DrawInstanced(static_cast<UINT>(gpuMesh->getVertexCount()), 1, 0, 0);
    }
}

IGPUMesh* D3D12RenderAPI::createMesh()
{
    D3D12Mesh* mesh = new D3D12Mesh();
    mesh->setD3D12Handles(device.Get(), commandQueue.Get(),
                          m_uploadCmdAllocator.Get(), m_uploadCmdList.Get(),
                          m_uploadFence.Get(), m_uploadFenceEvent,
                          &m_uploadFenceValue,
                          this);
    return mesh;
}
