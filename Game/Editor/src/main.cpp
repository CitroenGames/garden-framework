#define SDL_MAIN_HANDLED
#include "EditorApp.hpp"
#include "Graphics/RenderAPI.hpp"
#include <cstring>

static RenderAPIType parseRenderAPI(int argc, char* argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-vulkan") == 0 || strcmp(argv[i], "--vulkan") == 0)
            return RenderAPIType::Vulkan;
#ifdef _WIN32
        if (strcmp(argv[i], "-d3d11") == 0 || strcmp(argv[i], "--d3d11") == 0 ||
            strcmp(argv[i], "-dx11")  == 0 || strcmp(argv[i], "--dx11")  == 0)
            return RenderAPIType::D3D11;
#endif
#ifdef __APPLE__
        if (strcmp(argv[i], "-metal") == 0 || strcmp(argv[i], "--metal") == 0)
            return RenderAPIType::Metal;
#endif
    }
    return DefaultRenderAPI;
}

int main(int argc, char* argv[])
{
    RenderAPIType api_type = parseRenderAPI(argc, argv);

    EditorApp editor;
    if (!editor.initialize(api_type))
        return 1;

    editor.run();
    editor.shutdown();
    return 0;
}
