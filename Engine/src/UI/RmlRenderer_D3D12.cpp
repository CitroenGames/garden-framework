#ifdef _WIN32

#include "RmlRenderer_D3D12.h"
#include "Graphics/D3D12RenderAPI.hpp"
#include "Utils/EnginePaths.hpp"

#include "stb_image.h"

#include <fstream>
#include <cstring>

static std::vector<char> readBinary(const std::string& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) return {};
    size_t sz = static_cast<size_t>(file.tellg());
    std::vector<char> buf(sz);
    file.seekg(0);
    file.read(buf.data(), sz);
    return buf;
}

RmlRenderer_D3D12::RmlRenderer_D3D12() = default;
RmlRenderer_D3D12::~RmlRenderer_D3D12() { Shutdown(); }

bool RmlRenderer_D3D12::Init(D3D12RenderAPI* renderAPI)
{
    m_renderAPI = renderAPI;
    if (!m_renderAPI) return false;

    if (!CreateRootSignatureAndPSOs())
        return false;

    return true;
}

void RmlRenderer_D3D12::Shutdown()
{
    m_geometries.clear();
    for (auto& [k, tex] : m_textures)
    {
        if (m_renderAPI && tex.srvIndex != UINT(-1))
            m_renderAPI->deferSRVFree(tex.srvIndex);
    }
    m_textures.clear();
    m_rootSignature.Reset();
    m_psoTextured.Reset();
    m_psoColor.Reset();
    m_renderAPI = nullptr;
}

bool RmlRenderer_D3D12::CreateRootSignatureAndPSOs()
{
    ID3D12Device* device = m_renderAPI->getDevice();

    // Root signature: [0] Root CBV b0, [1] Descriptor table SRV t0
    D3D12_ROOT_PARAMETER rootParams[2] = {};

    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Linear clamp sampler for UI
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, error;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              serialized.GetAddressOf(), error.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                      IID_PPV_ARGS(m_rootSignature.GetAddressOf()));
    if (FAILED(hr)) return false;

    // Load shaders
    std::string dir = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/d3d12/");
    auto vsBlob = readBinary(dir + "rmlui_vs.dxil");
    if (vsBlob.empty()) return false;
    auto psTexBlob = readBinary(dir + "rmlui_ps_textured.dxil");
    if (psTexBlob.empty()) return false;
    auto psColBlob = readBinary(dir + "rmlui_ps_color.dxil");
    if (psColBlob.empty()) return false;

    // Input layout: position (float2), color (R8G8B8A8_UNORM), texcoord (float2)
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, offsetof(Rml::Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(Rml::Vertex, colour),   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, offsetof(Rml::Vertex, tex_coord), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // Base PSO desc for RmlUI
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vsBlob.data(), vsBlob.size() };
    psoDesc.InputLayout = { layout, 3 };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = FALSE;

    // Premultiplied alpha blend
    auto& rt = psoDesc.BlendState.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D12_BLEND_ONE;
    rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count = 1;

    // Textured PSO
    psoDesc.PS = { psTexBlob.data(), psTexBlob.size() };
    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_psoTextured.GetAddressOf()));
    if (FAILED(hr)) return false;

    // Color-only PSO
    psoDesc.PS = { psColBlob.data(), psColBlob.size() };
    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_psoColor.GetAddressOf()));
    if (FAILED(hr)) return false;

    return true;
}

void RmlRenderer_D3D12::SetViewport(int width, int height)
{
    m_viewportWidth = width;
    m_viewportHeight = height;
}

void RmlRenderer_D3D12::BeginFrame()
{
    m_cbFrameSlot = (m_cbFrameSlot + 1) % kCBFrameSlots;
    m_perFrameCBs[m_cbFrameSlot].clear();
}

Rml::CompiledGeometryHandle RmlRenderer_D3D12::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    ID3D12Device* device = m_renderAPI->getDevice();
    GeometryData geo;
    geo.numIndices = (int)indices.size();

    // Create vertex buffer in upload heap (RmlUI geometry is small and transient)
    {
        size_t size = vertices.size() * sizeof(Rml::Vertex);
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(geo.vertexBuffer.GetAddressOf()))))
            return 0;

        void* mapped = nullptr;
        geo.vertexBuffer->Map(0, nullptr, &mapped);
        memcpy(mapped, vertices.data(), size);
        geo.vertexBuffer->Unmap(0, nullptr);

        geo.vbView.BufferLocation = geo.vertexBuffer->GetGPUVirtualAddress();
        geo.vbView.SizeInBytes = static_cast<UINT>(size);
        geo.vbView.StrideInBytes = sizeof(Rml::Vertex);
    }

    // Create index buffer in upload heap
    {
        size_t size = indices.size() * sizeof(int);
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(geo.indexBuffer.GetAddressOf()))))
            return 0;

        void* mapped = nullptr;
        geo.indexBuffer->Map(0, nullptr, &mapped);
        memcpy(mapped, indices.data(), size);
        geo.indexBuffer->Unmap(0, nullptr);

        geo.ibView.BufferLocation = geo.indexBuffer->GetGPUVirtualAddress();
        geo.ibView.SizeInBytes = static_cast<UINT>(size);
        geo.ibView.Format = DXGI_FORMAT_R32_UINT;
    }

    uintptr_t handle = m_nextGeometryHandle++;
    m_geometries[handle] = std::move(geo);
    return (Rml::CompiledGeometryHandle)handle;
}

void RmlRenderer_D3D12::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    auto it = m_geometries.find((uintptr_t)handle);
    if (it == m_geometries.end()) return;

    const auto& geo = it->second;
    auto* cmdList = m_renderAPI->getCommandList();

    // Set our root signature and PSO
    cmdList->SetGraphicsRootSignature(m_rootSignature.Get());

    if (texture)
    {
        cmdList->SetPipelineState(m_psoTextured.Get());
        auto tex_it = m_textures.find((uintptr_t)texture);
        if (tex_it != m_textures.end())
        {
            cmdList->SetGraphicsRootDescriptorTable(1, m_renderAPI->getSrvAllocator().getGPU(tex_it->second.srvIndex));
        }
    }
    else
    {
        cmdList->SetPipelineState(m_psoColor.Get());
    }

    // Build and upload constant buffer
    float L = 0.0f, R = (float)m_viewportWidth;
    float T = 0.0f, B = (float)m_viewportHeight;

    float ortho[16] = {
        2.0f / (R - L),    0.0f,              0.0f, 0.0f,
        0.0f,              2.0f / (T - B),    0.0f, 0.0f,
        0.0f,              0.0f,              0.5f, 0.0f,
        (L + R) / (L - R), (T + B) / (B - T), 0.5f, 1.0f
    };

    CBufferData cb = {};
    if (m_transformEnabled)
    {
        const float* a = ortho;
        const float* b = m_transform.data();
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                cb.transform[j * 4 + i] = 0.0f;
                for (int k = 0; k < 4; k++)
                    cb.transform[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
            }
    }
    else
    {
        memcpy(cb.transform, ortho, sizeof(ortho));
    }
    cb.translation[0] = translation.x;
    cb.translation[1] = translation.y;

    // Allocate from the frame's upload ring buffer
    // We access it through the render API's internal state
    // For now, create a small upload buffer per draw (simple but not optimal)
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = 256; // 256-byte aligned
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> cbResource;
    if (FAILED(m_renderAPI->getDevice()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                  IID_PPV_ARGS(cbResource.GetAddressOf()))))
        return;
    void* mapped = nullptr;
    cbResource->Map(0, nullptr, &mapped);
    memcpy(mapped, &cb, sizeof(cb));
    cbResource->Unmap(0, nullptr);

    cmdList->SetGraphicsRootConstantBufferView(0, cbResource->GetGPUVirtualAddress());

    // Retain the CB until the GPU is done with the current frame's command list.
    m_perFrameCBs[m_cbFrameSlot].push_back(cbResource);

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, &geo.vbView);
    cmdList->IASetIndexBuffer(&geo.ibView);

    cmdList->DrawIndexedInstanced(geo.numIndices, 1, 0, 0, 0);
}

void RmlRenderer_D3D12::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
{
    m_geometries.erase((uintptr_t)handle);
}

Rml::TextureHandle RmlRenderer_D3D12::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
{
    int w, h, channels;
    unsigned char* data = stbi_load(source.c_str(), &w, &h, &channels, 4);
    if (!data) return 0;

    texture_dimensions.x = w;
    texture_dimensions.y = h;

    // Premultiply alpha
    for (int i = 0; i < w * h; i++)
    {
        unsigned char* p = data + i * 4;
        float a = p[3] / 255.0f;
        p[0] = (unsigned char)(p[0] * a);
        p[1] = (unsigned char)(p[1] * a);
        p[2] = (unsigned char)(p[2] * a);
    }

    auto handle = GenerateTexture(Rml::Span<const Rml::byte>(data, w * h * 4), texture_dimensions);
    stbi_image_free(data);
    return handle;
}

Rml::TextureHandle RmlRenderer_D3D12::GenerateTexture(Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions)
{
    ID3D12Device* device = m_renderAPI->getDevice();
    TextureData tex;
    int w = source_dimensions.x, h = source_dimensions.y;

    // Create texture resource
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;

    if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(tex.texture.GetAddressOf()))))
        return 0;

    // Upload via staging buffer
    UINT64 rowPitch = ((w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) / D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    UINT64 uploadSize = rowPitch * h;

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> uploadBuffer;
    if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(uploadBuffer.GetAddressOf()))))
        return 0;

    uint8_t* mapped = nullptr;
    uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (int y = 0; y < h; y++)
        memcpy(mapped + y * rowPitch, source_data.data() + y * w * 4, w * 4);
    uploadBuffer->Unmap(0, nullptr);

    // Use the render API's upload infrastructure
    // We need a command list for this - create a temporary one
    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence> fence;

    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(cmdAlloc.GetAddressOf()));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(cmdList.GetAddressOf()));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = tex.texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex.texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    m_renderAPI->getCommandQueue()->ExecuteCommandLists(1, lists);

    m_renderAPI->getCommandQueue()->Signal(fence.Get(), 1);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence->GetCompletedValue() < 1)
    {
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);
    }
    CloseHandle(event);

    // Create SRV
    tex.srvIndex = m_renderAPI->getSrvAllocator().allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(tex.texture.Get(), &srvDesc,
                                      m_renderAPI->getSrvAllocator().getCPU(tex.srvIndex));

    uintptr_t handle = m_nextTextureHandle++;
    m_textures[handle] = std::move(tex);
    return (Rml::TextureHandle)handle;
}

void RmlRenderer_D3D12::ReleaseTexture(Rml::TextureHandle texture)
{
    auto it = m_textures.find((uintptr_t)texture);
    if (it != m_textures.end())
    {
        if (m_renderAPI && it->second.srvIndex != UINT(-1))
            m_renderAPI->deferSRVFree(it->second.srvIndex);
        m_textures.erase(it);
    }
}

void RmlRenderer_D3D12::EnableScissorRegion(bool enable)
{
    m_scissorEnabled = enable;
    if (!enable)
    {
        // Reset scissor to full viewport
        auto* cmdList = m_renderAPI->getCommandList();
        D3D12_RECT rect = { 0, 0, static_cast<LONG>(m_viewportWidth), static_cast<LONG>(m_viewportHeight) };
        cmdList->RSSetScissorRects(1, &rect);
    }
}

void RmlRenderer_D3D12::SetScissorRegion(Rml::Rectanglei region)
{
    auto* cmdList = m_renderAPI->getCommandList();
    D3D12_RECT rect;
    rect.left = region.Left();
    rect.top = region.Top();
    rect.right = region.Right();
    rect.bottom = region.Bottom();
    cmdList->RSSetScissorRects(1, &rect);
}

void RmlRenderer_D3D12::SetTransform(const Rml::Matrix4f* transform)
{
    if (transform)
    {
        m_transformEnabled = true;
        m_transform = *transform;
    }
    else
    {
        m_transformEnabled = false;
    }
}

#endif // _WIN32
