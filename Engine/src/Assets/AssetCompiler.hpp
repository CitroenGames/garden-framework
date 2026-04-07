#pragma once

#include "EngineExport.h"
#include "CompiledTextureFormat.hpp"
#include <string>
#include <vector>
#include <functional>

namespace Assets {

struct CompileConfig {
    bool compile_models  = true;
    bool compile_textures = true;
    bool incremental     = true;
    bool generate_mipmaps = true;
    TexCompressionFormat default_color_format = TexCompressionFormat::BC7;
    TexCompressionFormat normal_map_format    = TexCompressionFormat::BC5;
    int  bc7_quality     = 1; // 0 = fast, 1 = balanced, 2 = best
};

struct CompileProgress {
    int total_assets     = 0;
    int completed_assets = 0;
    int skipped_assets   = 0;
    int failed_assets    = 0;
    int models_compiled  = 0;
    int textures_compiled = 0;
    std::string current_asset;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

using CompileProgressCallback = std::function<void(const CompileProgress&)>;

class ENGINE_API AssetCompiler {
public:
    // Compile all assets from source_root into output_root,
    // mirroring directory structure and replacing source formats with .cmesh/.ctex.
    // Non-compilable files are copied as-is.  .meta / .lodbin are skipped.
    static CompileProgress compileAll(
        const std::string& source_root,
        const std::string& output_root,
        const CompileConfig& config,
        CompileProgressCallback progress_cb = nullptr);

    // Compile a single model to .cmesh
    static bool compileModel(
        const std::string& source_path,
        const std::string& output_path,
        const CompileConfig& config);

    // Compile a single texture to .ctex
    static bool compileTexture(
        const std::string& source_path,
        const std::string& output_path,
        const CompileConfig& config,
        bool is_normal_map = false);

    // Check whether the compiled output is still up-to-date vs the source.
    static bool isUpToDate(
        const std::string& source_path,
        const std::string& compiled_path);

private:
    static bool isMeshFile(const std::string& ext);
    static bool isTextureFile(const std::string& ext);
    static bool isIntermediateFile(const std::string& ext);
    static TexCompressionFormat inferTextureFormat(
        const std::string& filename, int channels,
        bool is_normal_map, const CompileConfig& config);
};

} // namespace Assets
