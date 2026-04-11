#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12RenderAPI.hpp"
#include "D3D12Mesh.hpp"
#include "Components/mesh.hpp"
#include "Utils/Log.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

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
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
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

    m_uploadCmdAllocator->Reset();
    m_uploadCmdList->Reset(m_uploadCmdAllocator.Get(), nullptr);

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

        m_uploadCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        uploadOffset += rowPitch * mips[i].height;
    }

    uploadBuffer->Unmap(0, nullptr);

    // Transition to shader resource
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_uploadCmdList->ResourceBarrier(1, &barrier);

    executeUploadCommandList();

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
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
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

    m_uploadCmdAllocator->Reset();
    m_uploadCmdList->Reset(m_uploadCmdAllocator.Get(), nullptr);

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

        m_uploadCmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
        uploadOffset += rowPitch * numRows;
    }

    uploadBuffer->Unmap(0, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_uploadCmdList->ResourceBarrier(1, &barrier);

    executeUploadCommandList();

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
        flushGPU(); // Ensure GPU is done with this texture before releasing
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

void D3D12RenderAPI::renderDebugLines(const vertex* vertices, size_t vertex_count)
{
    if (!vertices || vertex_count < 2 || device_lost) return;
    if (in_shadow_pass) return;
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
                          &m_uploadFenceValue);
    return mesh;
}
