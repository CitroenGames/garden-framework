#define SDL_MAIN_HANDLED
#include "EditorApp.hpp"
#include "EditorConfig.hpp"
#include "Graphics/RenderAPI.hpp"
#include <cstring>
#include <string>
#include <optional>

// Returns a specific backend if the user passed a CLI flag, or nullopt to use config.
static std::optional<RenderAPIType> parseRenderAPICLI(int argc, char* argv[])
{
    for (int i = 1; i < argc; i++)
    {
#ifndef __APPLE__
        if (strcmp(argv[i], "-vulkan") == 0 || strcmp(argv[i], "--vulkan") == 0)
            return RenderAPIType::Vulkan;
#endif
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
    return std::nullopt;
}

static std::string parseProjectPath(int argc, char* argv[])
{
    for (int i = 1; i < argc - 1; i++)
    {
        if (strcmp(argv[i], "--project") == 0)
            return argv[i + 1];
    }
    return "";
}

int main(int argc, char* argv[])
{
    // Load persistent editor config (lives next to exe, shared across projects)
    EditorConfig editor_config;
    editor_config.load();

    // CLI flags override the saved config
    auto cli_api = parseRenderAPICLI(argc, argv);
    RenderAPIType api_type = cli_api.value_or(editor_config.render_backend);

    std::string project_path = parseProjectPath(argc, argv);

    EditorApp editor;
    editor.setEditorConfig(&editor_config);
    if (!project_path.empty())
        editor.setProjectPath(project_path);
    if (!editor.initialize(api_type))
        return 1;

    editor.run();
    editor.shutdown();
    return 0;
}
