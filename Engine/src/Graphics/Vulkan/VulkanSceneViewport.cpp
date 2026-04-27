#include "VulkanSceneViewport.hpp"
#include "VulkanRenderAPI.hpp"

VulkanSceneViewport::VulkanSceneViewport(VulkanRenderAPI* api, int width, int height)
    : m_api(api)
    , m_width(width > 0 ? width : 1)
    , m_height(height > 0 ? height : 1)
{
    if (m_api)
        m_pie_id = m_api->createPIEViewport(m_width, m_height);
}

VulkanSceneViewport::~VulkanSceneViewport()
{
    if (m_api && m_pie_id >= 0)
        m_api->destroyPIEViewport(m_pie_id);
}

void VulkanSceneViewport::resize(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (w == m_width && h == m_height) return;
    m_width  = w;
    m_height = h;
    if (m_api && m_pie_id >= 0)
        m_api->setPIEViewportSize(m_pie_id, w, h);
}

uint64_t VulkanSceneViewport::getOutputTextureID() const
{
    if (!m_api || m_pie_id < 0) return 0;
    return m_api->getPIEViewportTextureID(m_pie_id);
}
