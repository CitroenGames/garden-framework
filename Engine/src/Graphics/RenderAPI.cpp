#include "RenderAPI.hpp"
#include "OpenGLRenderAPI.hpp"
#include "VulkanRenderAPI.hpp"
#include "HeadlessRenderAPI.hpp"
#ifdef _WIN32
#include "D3D11RenderAPI.hpp"
#endif

// Factory implementation
IRenderAPI* CreateRenderAPI(RenderAPIType type)
{
    switch (type)
    {
    case RenderAPIType::OpenGL:
        return new OpenGLRenderAPI();
    case RenderAPIType::Vulkan:
        return new VulkanRenderAPI();
#ifdef _WIN32
    case RenderAPIType::D3D11:
        return new D3D11RenderAPI();
#endif
    case RenderAPIType::Headless:
        return new HeadlessRenderAPI();
    default:
        return nullptr;
    }
}
