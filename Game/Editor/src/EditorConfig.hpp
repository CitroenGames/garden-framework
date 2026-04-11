#pragma once

#include "Graphics/RenderAPI.hpp"
#include "Utils/EnginePaths.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>

// Persistent editor configuration stored next to the editor executable.
// Settings here apply across all projects.
class EditorConfig
{
public:
    // The render backend to use (platform-dependent options)
    RenderAPIType render_backend = DefaultRenderAPI;

    // UI scale factor (1.0 = 100%, 1.25 = 125%, etc.)
    float ui_scale = 1.0f;

    // Returns the path to editorconfig.cfg next to the executable
    static std::filesystem::path getConfigPath()
    {
        return EnginePaths::getExecutableDir() / "editorconfig.cfg";
    }

    // Load settings from editorconfig.cfg. Missing file or keys use defaults.
    void load()
    {
        std::ifstream file(getConfigPath());
        if (!file.is_open())
            return;

        std::string line;
        while (std::getline(file, line))
        {
            // Skip empty lines and comments
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos)
                continue;
            if (line[start] == '/' && line.size() > start + 1 && line[start + 1] == '/')
                continue;

            std::string trimmed = line.substr(start);
            size_t eq = trimmed.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = trimmed.substr(0, eq);
            std::string value = trimmed.substr(eq + 1);

            // Trim whitespace
            auto trim = [](std::string& s) {
                size_t a = s.find_first_not_of(" \t");
                size_t b = s.find_last_not_of(" \t");
                s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
            };
            trim(key);
            trim(value);

            if (key == "render_backend")
                render_backend = parseBackend(value);
            else if (key == "ui_scale")
                ui_scale = std::clamp(std::stof(value), 0.5f, 3.0f);
        }
    }

    // Save current settings to editorconfig.cfg
    void save() const
    {
        std::ofstream file(getConfigPath());
        if (!file.is_open())
            return;

        file << "// Garden Editor configuration\n";
        file << "// This file is saved next to the editor executable and persists across projects.\n";
        file << "// Changes to render_backend take effect on next launch.\n\n";
        file << "render_backend = " << backendToString(render_backend) << "\n";
        file << "ui_scale = " << ui_scale << "\n";
    }

    // Platform-available backends (for UI display)
    static std::vector<RenderAPIType> availableBackends()
    {
        std::vector<RenderAPIType> list;
#ifdef _WIN32
        list.push_back(RenderAPIType::D3D12);
        list.push_back(RenderAPIType::Vulkan);
#elif defined(__APPLE__)
        list.push_back(RenderAPIType::Metal);
#else
        list.push_back(RenderAPIType::Vulkan);
#endif
        return list;
    }

    static const char* backendToString(RenderAPIType type)
    {
        switch (type)
        {
        case RenderAPIType::Vulkan:   return "vulkan";
        case RenderAPIType::D3D12:    return "d3d12";
        case RenderAPIType::Metal:    return "metal";
        case RenderAPIType::Headless: return "headless";
        }
        return "vulkan";
    }

    static const char* backendDisplayName(RenderAPIType type)
    {
        switch (type)
        {
        case RenderAPIType::Vulkan:   return "Vulkan";
        case RenderAPIType::D3D12:    return "Direct3D 12";
        case RenderAPIType::Metal:    return "Metal";
        case RenderAPIType::Headless: return "Headless";
        }
        return "Unknown";
    }

private:
    static RenderAPIType parseBackend(const std::string& str)
    {
        std::string lower = str;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "vulkan")   return RenderAPIType::Vulkan;
        if (lower == "d3d12" || lower == "dx12" || lower == "direct3d12")
            return RenderAPIType::D3D12;
        if (lower == "metal")    return RenderAPIType::Metal;
        if (lower == "headless") return RenderAPIType::Headless;

        return DefaultRenderAPI;
    }
};
