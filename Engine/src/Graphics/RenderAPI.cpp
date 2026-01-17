#include "RenderAPI.hpp"
#include "OpenGLRenderAPI.hpp"
#include "VulkanRenderAPI.hpp"
#include "HeadlessRenderAPI.hpp"

// Factory implementation
IRenderAPI* CreateRenderAPI(RenderAPIType type)
{
    switch (type)
    {
    case RenderAPIType::OpenGL:
        return new OpenGLRenderAPI();
    case RenderAPIType::Vulkan:
        return new VulkanRenderAPI();
    case RenderAPIType::Headless:
        return new HeadlessRenderAPI();
    default:
        return nullptr;
    }
}
