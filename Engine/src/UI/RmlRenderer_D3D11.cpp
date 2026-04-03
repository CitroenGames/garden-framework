#ifdef _WIN32

#include "RmlRenderer_D3D11.h"
#include "Graphics/D3D11RenderAPI.hpp"
#include <d3dcompiler.h>

#include "stb_image.h"

static const char* s_shaderSource = R"(
cbuffer RmlCB : register(b0) {
    matrix uTransform;
    float2 uTranslation;
    float2 _pad;
};

struct VSInput {
    float2 position : POSITION;
    float4 color    : COLOR;
    float2 texcoord : TEXCOORD0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
    float2 texcoord : TEXCOORD0;
};

Texture2D rmlTexture : register(t0);
SamplerState rmlSampler : register(s0);

PSInput VSMain(VSInput input) {
    PSInput output;
    float2 p = input.position + uTranslation;
    output.position = mul(uTransform, float4(p, 0.0, 1.0));
    output.color = input.color;
    output.texcoord = input.texcoord;
    return output;
}

float4 PSTextured(PSInput input) : SV_TARGET {
    return input.color * rmlTexture.Sample(rmlSampler, input.texcoord);
}

float4 PSColor(PSInput input) : SV_TARGET {
    return input.color;
}
)";

RmlRenderer_D3D11::RmlRenderer_D3D11() = default;
RmlRenderer_D3D11::~RmlRenderer_D3D11() { Shutdown(); }

bool RmlRenderer_D3D11::Init(D3D11RenderAPI* renderAPI)
{
    m_device = renderAPI->getDevice();
    m_context = renderAPI->getDeviceContext();
    if (!m_device || !m_context)
        return false;

    if (!CreateShaders())
        return false;
    if (!CreateStates())
        return false;

    // Constant buffer
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(CBufferData);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device->CreateBuffer(&cbd, nullptr, m_constantBuffer.GetAddressOf())))
        return false;

    return true;
}

void RmlRenderer_D3D11::Shutdown()
{
    m_geometries.clear();
    m_textures.clear();
    m_vertexShader.Reset();
    m_psTextured.Reset();
    m_psColor.Reset();
    m_inputLayout.Reset();
    m_blendState.Reset();
    m_rasterizerScissor.Reset();
    m_rasterizerNoScissor.Reset();
    m_depthStencilState.Reset();
    m_sampler.Reset();
    m_constantBuffer.Reset();
    m_device = nullptr;
    m_context = nullptr;
}

bool RmlRenderer_D3D11::CreateShaders()
{
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG;
#endif

    // Vertex shader
    ComPtr<ID3DBlob> vsBlob, errBlob;
    if (FAILED(D3DCompile(s_shaderSource, strlen(s_shaderSource), "rmlui.hlsl", nullptr, nullptr,
            "VSMain", "vs_5_0", flags, 0, vsBlob.GetAddressOf(), errBlob.GetAddressOf())))
        return false;
    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, m_vertexShader.GetAddressOf())))
        return false;

    // Input layout: position (float2), color (R8G8B8A8_UNORM), texcoord (float2)
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(Rml::Vertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, offsetof(Rml::Vertex, colour),   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(Rml::Vertex, tex_coord),D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(m_device->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), m_inputLayout.GetAddressOf())))
        return false;

    // Textured pixel shader
    ComPtr<ID3DBlob> psTexBlob;
    if (FAILED(D3DCompile(s_shaderSource, strlen(s_shaderSource), "rmlui.hlsl", nullptr, nullptr,
            "PSTextured", "ps_5_0", flags, 0, psTexBlob.GetAddressOf(), errBlob.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(m_device->CreatePixelShader(psTexBlob->GetBufferPointer(), psTexBlob->GetBufferSize(), nullptr, m_psTextured.GetAddressOf())))
        return false;

    // Color-only pixel shader
    ComPtr<ID3DBlob> psColBlob;
    if (FAILED(D3DCompile(s_shaderSource, strlen(s_shaderSource), "rmlui.hlsl", nullptr, nullptr,
            "PSColor", "ps_5_0", flags, 0, psColBlob.GetAddressOf(), errBlob.ReleaseAndGetAddressOf())))
        return false;
    if (FAILED(m_device->CreatePixelShader(psColBlob->GetBufferPointer(), psColBlob->GetBufferSize(), nullptr, m_psColor.GetAddressOf())))
        return false;

    return true;
}

bool RmlRenderer_D3D11::CreateStates()
{
    // Blend state: premultiplied alpha (Src=ONE, Dst=ONE_MINUS_SRC_ALPHA)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m_device->CreateBlendState(&bd, m_blendState.GetAddressOf())))
        return false;

    // Rasterizer states
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    if (FAILED(m_device->CreateRasterizerState(&rd, m_rasterizerScissor.GetAddressOf())))
        return false;
    rd.ScissorEnable = FALSE;
    if (FAILED(m_device->CreateRasterizerState(&rd, m_rasterizerNoScissor.GetAddressOf())))
        return false;

    // Depth stencil: disabled
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    if (FAILED(m_device->CreateDepthStencilState(&dsd, m_depthStencilState.GetAddressOf())))
        return false;

    // Sampler: linear clamp
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(m_device->CreateSamplerState(&sd, m_sampler.GetAddressOf())))
        return false;

    return true;
}

void RmlRenderer_D3D11::SetViewport(int width, int height)
{
    m_viewportWidth = width;
    m_viewportHeight = height;
}

void RmlRenderer_D3D11::UpdateConstantBuffer(Rml::Vector2f translation)
{
    // Build orthographic projection (LH, top-left origin)
    float L = 0.0f, R = (float)m_viewportWidth;
    float T = 0.0f, B = (float)m_viewportHeight;

    // Column-major orthographic matrix
    float ortho[16] = {
        2.0f / (R - L),    0.0f,              0.0f, 0.0f,
        0.0f,              2.0f / (T - B),    0.0f, 0.0f,
        0.0f,              0.0f,              0.5f, 0.0f,
        (L + R) / (L - R), (T + B) / (B - T), 0.5f, 1.0f
    };

    CBufferData cb = {};

    if (m_transformEnabled)
    {
        // Multiply ortho * m_transform (both column-major)
        const float* a = ortho;
        const float* b = m_transform.data();
        for (int i = 0; i < 4; i++)
        {
            for (int j = 0; j < 4; j++)
            {
                cb.transform[j * 4 + i] = 0.0f;
                for (int k = 0; k < 4; k++)
                    cb.transform[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
            }
        }
    }
    else
    {
        memcpy(cb.transform, ortho, sizeof(ortho));
    }

    cb.translation[0] = translation.x;
    cb.translation[1] = translation.y;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        memcpy(mapped.pData, &cb, sizeof(cb));
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }
}

Rml::CompiledGeometryHandle RmlRenderer_D3D11::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    GeometryData geo;
    geo.numIndices = (int)indices.size();

    // Vertex buffer
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = (UINT)(vertices.size() * sizeof(Rml::Vertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = {};
    vsd.pSysMem = vertices.data();
    if (FAILED(m_device->CreateBuffer(&vbd, &vsd, geo.vertexBuffer.GetAddressOf())))
        return 0;

    // Index buffer
    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = (UINT)(indices.size() * sizeof(int));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd = {};
    isd.pSysMem = indices.data();
    if (FAILED(m_device->CreateBuffer(&ibd, &isd, geo.indexBuffer.GetAddressOf())))
        return 0;

    uintptr_t handle = m_nextGeometryHandle++;
    m_geometries[handle] = geo;
    return (Rml::CompiledGeometryHandle)handle;
}

void RmlRenderer_D3D11::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    auto it = m_geometries.find((uintptr_t)handle);
    if (it == m_geometries.end())
        return;

    const auto& geo = it->second;

    // Save current D3D11 state we're about to change
    ComPtr<ID3D11RasterizerState> prevRS;
    ComPtr<ID3D11BlendState> prevBS;
    FLOAT prevBlendFactor[4];
    UINT prevSampleMask;
    ComPtr<ID3D11DepthStencilState> prevDSS;
    UINT prevStencilRef;
    m_context->RSGetState(prevRS.GetAddressOf());
    m_context->OMGetBlendState(prevBS.GetAddressOf(), prevBlendFactor, &prevSampleMask);
    m_context->OMGetDepthStencilState(prevDSS.GetAddressOf(), &prevStencilRef);

    // Set our pipeline state
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);

    if (texture)
    {
        auto tex_it = m_textures.find((uintptr_t)texture);
        if (tex_it != m_textures.end())
        {
            ID3D11ShaderResourceView* srv = tex_it->second.srv.Get();
            m_context->PSSetShaderResources(0, 1, &srv);
        }
        m_context->PSSetShader(m_psTextured.Get(), nullptr, 0);
    }
    else
    {
        m_context->PSSetShader(m_psColor.Get(), nullptr, 0);
    }

    ID3D11SamplerState* sampler = m_sampler.Get();
    m_context->PSSetSamplers(0, 1, &sampler);

    float blendFactor[4] = { 0, 0, 0, 0 };
    m_context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);
    m_context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);

    if (m_scissorEnabled)
        m_context->RSSetState(m_rasterizerScissor.Get());
    else
        m_context->RSSetState(m_rasterizerNoScissor.Get());

    // Update constant buffer with projection + transform + translation
    UpdateConstantBuffer(translation);
    ID3D11Buffer* cb = m_constantBuffer.Get();
    m_context->VSSetConstantBuffers(0, 1, &cb);

    // Bind geometry
    UINT stride = sizeof(Rml::Vertex);
    UINT offset = 0;
    ID3D11Buffer* vb = geo.vertexBuffer.Get();
    m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    m_context->IASetIndexBuffer(geo.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);

    // Draw
    m_context->DrawIndexed(geo.numIndices, 0, 0);

    // Restore previous state
    m_context->RSSetState(prevRS.Get());
    m_context->OMSetBlendState(prevBS.Get(), prevBlendFactor, prevSampleMask);
    m_context->OMSetDepthStencilState(prevDSS.Get(), prevStencilRef);
}

void RmlRenderer_D3D11::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
{
    m_geometries.erase((uintptr_t)handle);
}

Rml::TextureHandle RmlRenderer_D3D11::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
{
    int w, h, channels;
    unsigned char* data = stbi_load(source.c_str(), &w, &h, &channels, 4);
    if (!data)
        return 0;

    texture_dimensions.x = w;
    texture_dimensions.y = h;

    // Premultiply alpha (stb_image loads straight alpha)
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

Rml::TextureHandle RmlRenderer_D3D11::GenerateTexture(Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions)
{
    TextureData tex;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = source_dimensions.x;
    desc.Height = source_dimensions.y;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA srd = {};
    srd.pSysMem = source_data.data();
    srd.SysMemPitch = source_dimensions.x * 4;

    if (FAILED(m_device->CreateTexture2D(&desc, &srd, tex.texture.GetAddressOf())))
        return 0;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    if (FAILED(m_device->CreateShaderResourceView(tex.texture.Get(), &srvDesc, tex.srv.GetAddressOf())))
        return 0;

    uintptr_t handle = m_nextTextureHandle++;
    m_textures[handle] = tex;
    return (Rml::TextureHandle)handle;
}

void RmlRenderer_D3D11::ReleaseTexture(Rml::TextureHandle texture)
{
    m_textures.erase((uintptr_t)texture);
}

void RmlRenderer_D3D11::EnableScissorRegion(bool enable)
{
    m_scissorEnabled = enable;
}

void RmlRenderer_D3D11::SetScissorRegion(Rml::Rectanglei region)
{
    D3D11_RECT rect;
    rect.left = region.Left();
    rect.top = region.Top();
    rect.right = region.Right();
    rect.bottom = region.Bottom();
    m_context->RSSetScissorRects(1, &rect);
}

void RmlRenderer_D3D11::SetTransform(const Rml::Matrix4f* transform)
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
