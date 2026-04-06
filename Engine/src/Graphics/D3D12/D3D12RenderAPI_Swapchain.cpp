#include "D3D12RenderAPI.hpp"
#include "Utils/Log.hpp"

// ============================================================================
// Swap Chain
// ============================================================================

bool D3D12RenderAPI::createSwapChain()
{
    LOG_ENGINE_TRACE("[D3D12] Creating swap chain ({}x{}, {} buffers)...",
                      viewport_width, viewport_height, NUM_BACK_BUFFERS);
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = viewport_width;
    scd.Height = viewport_height;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = NUM_BACK_BUFFERS;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = dxgiFactory->CreateSwapChainForHwnd(
        commandQueue.Get(), hwnd, &scd, nullptr, nullptr,
        swapChain1.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("CreateSwapChainForHwnd failed");
        return false;
    }

    hr = swapChain1.As(&swapChain);
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to query IDXGISwapChain3");
        return false;
    }

    // Disable Alt+Enter fullscreen toggle
    dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    m_backBufferIndex = swapChain->GetCurrentBackBufferIndex();
    return true;
}
