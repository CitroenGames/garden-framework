#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <memory>

namespace Assets {

enum class AssetType : uint8_t {
    Unknown = 0,
    Mesh,
    Texture,
    Material,
    Model,
    Shader,
    Sound,
    Animation
};

enum class LoadState : uint8_t {
    NotLoaded,
    Queued,
    LoadingIO,
    Parsing,
    Processing,
    UploadingGPU,
    Ready,
    Failed
};

enum class LoadPriority : uint8_t {
    Background = 0,
    Normal = 1,
    High = 2,
    Immediate = 3
};

struct AssetError {
    std::string message;
    std::string file_path;
    int error_code = 0;

    AssetError() = default;
    AssetError(const std::string& msg, const std::string& path = "", int code = 0)
        : message(msg), file_path(path), error_code(code) {}
};

struct MeshAssetData;
struct TextureAssetData;
struct MaterialAssetData;
struct ModelAssetData;

using AssetData = std::variant<
    std::monostate,
    std::shared_ptr<MeshAssetData>,
    std::shared_ptr<TextureAssetData>,
    std::shared_ptr<MaterialAssetData>,
    std::shared_ptr<ModelAssetData>
>;

inline const char* assetTypeToString(AssetType type) {
    switch (type) {
        case AssetType::Mesh: return "Mesh";
        case AssetType::Texture: return "Texture";
        case AssetType::Material: return "Material";
        case AssetType::Model: return "Model";
        case AssetType::Shader: return "Shader";
        case AssetType::Sound: return "Sound";
        case AssetType::Animation: return "Animation";
        default: return "Unknown";
    }
}

inline const char* loadStateToString(LoadState state) {
    switch (state) {
        case LoadState::NotLoaded: return "NotLoaded";
        case LoadState::Queued: return "Queued";
        case LoadState::LoadingIO: return "LoadingIO";
        case LoadState::Parsing: return "Parsing";
        case LoadState::Processing: return "Processing";
        case LoadState::UploadingGPU: return "UploadingGPU";
        case LoadState::Ready: return "Ready";
        case LoadState::Failed: return "Failed";
        default: return "Unknown";
    }
}

} // namespace Assets
