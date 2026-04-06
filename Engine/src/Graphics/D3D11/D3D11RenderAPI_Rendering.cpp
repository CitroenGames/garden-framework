#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D11RenderAPI.hpp"
#include "D3D11Mesh.hpp"
#include "Components/mesh.hpp"
#include "Utils/Log.hpp"
#include "Utils/Vertex.hpp"
#include <algorithm>
#include <cstring>

void D3D11RenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0)
        return;

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU(this);
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
            return;
    }

    D3D11Mesh* d3dMesh = static_cast<D3D11Mesh*>(m.gpu_mesh);

    if (in_shadow_pass)
    {
        // Shadow pass - only update per-object shadow CBuffer
        // Shaders, layout, and CB slot are already bound in beginCascade()
        updateShadowCBuffer(lightSpaceMatrices[currentCascade], current_model_matrix);
    }
    else
    {
        // Flush global CBuffer if dirty
        if (global_cbuffer_dirty)
        {
            updateGlobalCBuffer();
            global_cbuffer_dirty = false;
        }

        // Update per-object data
        bool useTexture = m.texture_set && m.texture != INVALID_TEXTURE;
        updatePerObjectCBuffer(state.color, useTexture);

        // Bind shaders with state tracking
        ID3D11VertexShader* desired_vs;
        ID3D11PixelShader* desired_ps;
        if (state.lighting && lighting_enabled)
        {
            desired_vs = basicVertexShader.Get();
            desired_ps = basicPixelShader.Get();
        }
        else
        {
            desired_vs = unlitVertexShader.Get();
            desired_ps = unlitPixelShader.Get();
        }
        if (!desired_vs || !desired_ps) return;

        if (desired_vs != last_bound_vs)
        {
            context->VSSetShader(desired_vs, nullptr, 0);
            last_bound_vs = desired_vs;
        }
        if (desired_ps != last_bound_ps)
        {
            context->PSSetShader(desired_ps, nullptr, 0);
            last_bound_ps = desired_ps;
        }

        // Input layout with state tracking
        if (basicInputLayout.Get() != last_bound_layout)
        {
            context->IASetInputLayout(basicInputLayout.Get());
            last_bound_layout = basicInputLayout.Get();
        }

        // Bind texture (early-out if already bound handled inside bindTexture)
        bindTexture(useTexture ? m.texture : defaultTexture);

        applyRenderState(state);
    }

    // Bind vertex buffer with state tracking
    ID3D11Buffer* vb = d3dMesh->getVertexBuffer();
    if (vb != last_bound_vb)
    {
        d3dMesh->bind();
        last_bound_vb = vb;
    }

    if (d3dMesh->isIndexed())
    {
        context->DrawIndexed(static_cast<UINT>(d3dMesh->getIndexCount()), 0, 0);
    }
    else
    {
        context->Draw(static_cast<UINT>(d3dMesh->getVertexCount()), 0);
    }
}

void D3D11RenderAPI::renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0 || vertex_count == 0)
        return;

    // Validate range
    if (start_vertex + vertex_count > m.vertices_len)
    {
        vertex_count = m.vertices_len - start_vertex;
        if (vertex_count == 0) return;
    }

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU(this);
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
            return;
    }

    D3D11Mesh* d3dMesh = static_cast<D3D11Mesh*>(m.gpu_mesh);

    if (in_shadow_pass)
    {
        // Shadow pass - only update per-object shadow CBuffer
        updateShadowCBuffer(lightSpaceMatrices[currentCascade], current_model_matrix);
    }
    else
    {
        // Flush global CBuffer if dirty
        if (global_cbuffer_dirty)
        {
            updateGlobalCBuffer();
            global_cbuffer_dirty = false;
        }

        // For multi-material rendering, texture is already bound by the caller
        updatePerObjectCBuffer(state.color, true);

        // Bind shaders with state tracking
        ID3D11VertexShader* desired_vs;
        ID3D11PixelShader* desired_ps;
        if (state.lighting && lighting_enabled)
        {
            desired_vs = basicVertexShader.Get();
            desired_ps = basicPixelShader.Get();
        }
        else
        {
            desired_vs = unlitVertexShader.Get();
            desired_ps = unlitPixelShader.Get();
        }
        if (!desired_vs || !desired_ps) return;

        if (desired_vs != last_bound_vs)
        {
            context->VSSetShader(desired_vs, nullptr, 0);
            last_bound_vs = desired_vs;
        }
        if (desired_ps != last_bound_ps)
        {
            context->PSSetShader(desired_ps, nullptr, 0);
            last_bound_ps = desired_ps;
        }

        // Input layout with state tracking
        if (basicInputLayout.Get() != last_bound_layout)
        {
            context->IASetInputLayout(basicInputLayout.Get());
            last_bound_layout = basicInputLayout.Get();
        }

        applyRenderState(state);
    }

    // Bind vertex buffer with state tracking
    ID3D11Buffer* vb = d3dMesh->getVertexBuffer();
    if (vb != last_bound_vb)
    {
        d3dMesh->bind();
        last_bound_vb = vb;
    }

    if (d3dMesh->isIndexed())
    {
        context->DrawIndexed(static_cast<UINT>(vertex_count), static_cast<UINT>(start_vertex), 0);
    }
    else
    {
        context->Draw(static_cast<UINT>(vertex_count), static_cast<UINT>(start_vertex));
    }
}

void D3D11RenderAPI::renderDebugLines(const vertex* vertices, size_t vertex_count)
{
    if (!vertices || vertex_count < 2 || device_lost) return;

    // Ensure dynamic vertex buffer is large enough
    if (!debugLineVB || debugLineVBCapacity < vertex_count)
    {
        debugLineVB.Reset();
        debugLineVBCapacity = std::max(vertex_count, size_t(1024));

        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = static_cast<UINT>(debugLineVBCapacity * sizeof(vertex));
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = device->CreateBuffer(&bd, nullptr, debugLineVB.GetAddressOf());
        if (FAILED(hr)) return;
    }

    // Upload all vertices
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (!mapBuffer(debugLineVB.Get(), mapped)) return;
    memcpy(mapped.pData, vertices, vertex_count * sizeof(vertex));
    context->Unmap(debugLineVB.Get(), 0);

    // Flush global CBuffer if dirty (need view/projection)
    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }

    // Set up state for line rendering
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    context->VSSetShader(unlitVertexShader.Get(), nullptr, 0);
    context->PSSetShader(unlitPixelShader.Get(), nullptr, 0);
    context->IASetInputLayout(basicInputLayout.Get());

    UINT stride = sizeof(vertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, debugLineVB.GetAddressOf(), &stride, &offset);

    // Rebind constant buffers (skybox rendering replaces slot 0 with skyboxCBuffer)
    context->VSSetConstantBuffers(0, 1, globalCBuffer.GetAddressOf());
    context->VSSetConstantBuffers(1, 1, perObjectCBuffer.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, globalCBuffer.GetAddressOf());
    context->PSSetConstantBuffers(1, 1, perObjectCBuffer.GetAddressOf());

    // No culling, no blending, depth test with write
    context->RSSetState(rasterizerCullNone.Get());
    context->OMSetBlendState(blendStateNone.Get(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(depthStateLessEqual.Get(), 0);

    // Save model matrix
    glm::mat4 saved_model = current_model_matrix;
    current_model_matrix = glm::mat4(1.0f);

    // Batch draw by color (both endpoints of a line share the same color in nx,ny,nz)
    size_t i = 0;
    while (i < vertex_count)
    {
        glm::vec3 color(vertices[i].nx, vertices[i].ny, vertices[i].nz);
        size_t batch_start = i;

        // Scan forward while vertices share this color
        while (i < vertex_count &&
               vertices[i].nx == color.r &&
               vertices[i].ny == color.g &&
               vertices[i].nz == color.b)
        {
            i++;
        }

        updatePerObjectCBuffer(color, false);
        context->Draw(static_cast<UINT>(i - batch_start), static_cast<UINT>(batch_start));
    }

    // Restore model matrix and topology
    current_model_matrix = saved_model;
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Invalidate state tracking (we changed shaders, layout, VB, rasterizer, etc.)
    last_bound_vs = nullptr;
    last_bound_ps = nullptr;
    last_bound_layout = nullptr;
    last_bound_vb = nullptr;
    last_bound_rasterizer = nullptr;
    last_bound_blend = nullptr;
    last_bound_depth = nullptr;
}

// --- Depth Prepass ---

void D3D11RenderAPI::beginDepthPrepass()
{
    in_depth_prepass = true;

    // Bind basic vertex shader with null pixel shader (depth-only)
    context->VSSetShader(basicVertexShader.Get(), nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);
    context->IASetInputLayout(basicInputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Disable color writes
    context->OMSetBlendState(blendStateColorWriteDisabled.Get(), nullptr, 0xffffffff);

    // Depth write enabled with LESS_EQUAL test
    context->OMSetDepthStencilState(depthStateLessEqual.Get(), 0);

    // Bind constant buffers for vertex shader
    context->VSSetConstantBuffers(0, 1, globalCBuffer.GetAddressOf());
    context->VSSetConstantBuffers(1, 1, perObjectCBuffer.GetAddressOf());

    // Flush global CBuffer if dirty
    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }

    // Update state tracking
    last_bound_vs = basicVertexShader.Get();
    last_bound_ps = nullptr;
    last_bound_layout = basicInputLayout.Get();
    last_bound_blend = blendStateColorWriteDisabled.Get();
    last_bound_depth = depthStateLessEqual.Get();
    last_bound_vb = nullptr;
}

void D3D11RenderAPI::endDepthPrepass()
{
    in_depth_prepass = false;
    use_equal_depth = true;

    // Restore color writes
    context->OMSetBlendState(blendStateNone.Get(), nullptr, 0xffffffff);
    last_bound_blend = blendStateNone.Get();

    // Switch to read-only depth (LESS_EQUAL, no depth write) for main lit pass.
    // Using LESS_EQUAL instead of EQUAL avoids precision mismatches across
    // D3D11 drivers that can cause fragments to fail a strict EQUAL test.
    context->OMSetDepthStencilState(depthStateReadOnly.Get(), 0);
    last_bound_depth = depthStateReadOnly.Get();

    // Reset shader state tracking so main pass rebinds lit shaders
    last_bound_vs = nullptr;
    last_bound_ps = nullptr;
    last_bound_layout = nullptr;
    last_bound_vb = nullptr;
}

void D3D11RenderAPI::renderMeshDepthOnly(const mesh& m)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0)
        return;

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU(this);
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
            return;
    }

    D3D11Mesh* d3dMesh = static_cast<D3D11Mesh*>(m.gpu_mesh);

    // Update per-object CBuffer with model matrix only (color/texture irrelevant)
    updatePerObjectCBuffer(glm::vec3(1.0f), false);

    // Bind vertex buffer with state tracking
    ID3D11Buffer* vb = d3dMesh->getVertexBuffer();
    if (vb != last_bound_vb)
    {
        d3dMesh->bind();
        last_bound_vb = vb;
    }

    if (d3dMesh->isIndexed())
    {
        context->DrawIndexed(static_cast<UINT>(d3dMesh->getIndexCount()), 0, 0);
    }
    else
    {
        context->Draw(static_cast<UINT>(d3dMesh->getVertexCount()), 0);
    }
}
