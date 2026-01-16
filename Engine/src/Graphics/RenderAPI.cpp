#include "RenderAPI.hpp"
#include "OpenGLRenderAPI.hpp"
#include "VulkanRenderAPI.hpp"

// Factory implementation
IRenderAPI* CreateRenderAPI(RenderAPIType type)
{
    switch (type)
    {
    case RenderAPIType::OpenGL:
        return new OpenGLRenderAPI();
    case RenderAPIType::Vulkan:
        return new VulkanRenderAPI();
    default:
        return nullptr;
    }
}
