#pragma once

#include "Graphics/SceneViewport.hpp"

class VulkanRenderAPI;

// Vulkan SceneViewport — for now this is a thin wrapper around the existing
// PIE-viewport infrastructure on the Vulkan side. The wrapper allocates a PIE
// viewport on construction and frees it on destruction; resize / texture-ID
// queries forward to the legacy PIE methods. A future cleanup pass can fold
// PIEViewportTarget into this class directly (mirroring D3D12SceneViewport).
class VulkanSceneViewport : public SceneViewport
{
public:
    VulkanSceneViewport(VulkanRenderAPI* api, int width, int height);
    ~VulkanSceneViewport() override;

    int  width()  const override { return m_width; }
    int  height() const override { return m_height; }
    void resize(int w, int h) override;

    uint64_t getOutputTextureID() const override;
    bool     outputsToBackBuffer() const override { return false; }

    // VulkanRenderAPI::setEditorViewport reads this to map the active editor
    // viewport pointer back to the PIE id stored in the legacy infrastructure.
    int pieId() const { return m_pie_id; }

private:
    VulkanRenderAPI* m_api = nullptr;
    int m_pie_id = -1;
    int m_width  = 0;
    int m_height = 0;
};
