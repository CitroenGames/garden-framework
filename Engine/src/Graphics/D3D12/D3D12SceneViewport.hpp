#pragma once

#include "Graphics/SceneViewport.hpp"
#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class D3D12RenderAPI;

// D3D12 implementation of SceneViewport. Owns HDR + depth + (optionally) LDR
// textures and their descriptors. Resource destruction is routed through the
// owning D3D12RenderAPI's deferred-release ring so resizing or destroying a
// viewport mid-frame doesn't invalidate any in-flight command list.
class D3D12SceneViewport : public SceneViewport
{
public:
    // outputsToBackBuffer=true means the LDR output is imported (not owned)
    // and must be rebound each frame via rebindBackBuffer(). Use for the
    // standalone-client path where the final image lands on the swap chain.
    D3D12SceneViewport(D3D12RenderAPI* api, int w, int h, bool outputsToBackBuffer);
    ~D3D12SceneViewport() override;

    int  width()  const override { return m_width; }
    int  height() const override { return m_height; }
    void resize(int w, int h) override;

    uint64_t getOutputTextureID() const override;
    bool     outputsToBackBuffer() const override { return m_outputsToBackBuffer; }

    // D3D12-specific accessors — used by render-graph builders and the API.
    ID3D12Resource* getHDR()    const { return m_hdr.Get(); }
    ID3D12Resource* getDepth()  const { return m_depth.Get(); }
    ID3D12Resource* getOutput() const { return m_output.Get(); }

    UINT getHDRRTV()    const { return m_hdrRTV; }
    UINT getHDRSRV()    const { return m_hdrSRV; }
    UINT getDepthDSV()  const { return m_depthDSV; }
    UINT getDepthSRV()  const { return m_depthSRV; }
    UINT getOutputRTV() const { return m_outputRTV; }
    UINT getOutputSRV() const { return m_outputSRV; }  // UINT(-1) when outputsToBackBuffer

    // Only valid when outputsToBackBuffer() is true. Called by D3D12RenderAPI
    // each frame with whichever swap-chain buffer is current. The supplied
    // RTV is owned by the render API, not this viewport.
    void rebindBackBuffer(ID3D12Resource* backBuffer, UINT backBufferRTV);

private:
    void createOwned(int w, int h);
    void releaseOwned();

    D3D12RenderAPI* m_api = nullptr;
    int  m_width  = 0;
    int  m_height = 0;
    bool m_outputsToBackBuffer = false;

    ComPtr<ID3D12Resource> m_hdr;     // R16G16B16A16_FLOAT, RENDER_TARGET
    ComPtr<ID3D12Resource> m_depth;   // R24G8_TYPELESS,     DEPTH_WRITE
    ComPtr<ID3D12Resource> m_output;  // R8G8B8A8_UNORM,     PIXEL_SHADER_RESOURCE. null if outputsToBackBuffer.

    UINT m_hdrRTV    = UINT(-1);
    UINT m_hdrSRV    = UINT(-1);
    UINT m_depthDSV  = UINT(-1);
    UINT m_depthSRV  = UINT(-1);
    UINT m_outputRTV = UINT(-1);
    UINT m_outputSRV = UINT(-1);
};
