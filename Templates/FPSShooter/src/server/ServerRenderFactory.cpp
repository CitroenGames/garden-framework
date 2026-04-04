#include "Graphics/RenderAPI.hpp"
#include "Graphics/HeadlessRenderAPI.hpp"

IRenderAPI* CreateRenderAPI(RenderAPIType type)
{
    return new HeadlessRenderAPI();
}
