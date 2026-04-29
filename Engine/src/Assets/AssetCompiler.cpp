#include "AssetCompiler.hpp"
#include "CompiledMeshSerializer.hpp"
#include "CompiledTextureSerializer.hpp"
#include "LODGenerator.hpp"
#include "LODMeshSerializer.hpp"
#include "MeshChunker.hpp"
#include "AssetMetadata.hpp"
#include "AssetMetadataSerializer.hpp"
#include "Utils/FileHash.hpp"
#include "Utils/GltfLoader.hpp"
#include "Utils/ObjLoader.hpp"
#include "Utils/Log.hpp"

#include <bc7enc.h>
#include <rgbcx.h>
#include <stb_image.h>
#include <stb_image_resize2.h>

#include "Threading/JobSystem.hpp"

#include <filesystem>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace fs = std::filesystem;

namespace Assets {

// ================================================================
// Helpers
// ================================================================

static std::once_flag s_encoder_init_flag;

static void ensureEncodersInitialised()
{
    std::call_once(s_encoder_init_flag, []() {
        bc7enc_compress_block_init();
        rgbcx::init();
    });
}

static std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    return out;
}

static std::string replaceExtension(const std::string& path, const std::string& new_ext)
{
    fs::path p(path);
    p.replace_extension(new_ext);
    return p.string();
}

// Check if texture has meaningful alpha (any pixel with alpha < 255)
static bool hasAlpha(const uint8_t* rgba, int w, int h)
{
    int count = w * h;
    for (int i = 0; i < count; ++i) {
        if (rgba[i * 4 + 3] < 255)
            return true;
    }
    return false;
}

// Check if filename hints at a normal map
static bool looksLikeNormalMap(const std::string& filename)
{
    std::string stem = toLower(fs::path(filename).stem().string());
    return stem.find("_normal") != std::string::npos
        || stem.find("_nrm")    != std::string::npos
        || stem.find("_n.")     != std::string::npos
        || (stem.size() >= 2 && stem.substr(stem.size() - 2) == "_n");
}

// Pad dimensions to next multiple of 4 (required for BC compression)
static void padToMultipleOf4(std::vector<uint8_t>& pixels, int& w, int& h)
{
    int new_w = (w + 3) & ~3;
    int new_h = (h + 3) & ~3;
    if (new_w == w && new_h == h)
        return;

    std::vector<uint8_t> padded(new_w * new_h * 4, 0);
    for (int y = 0; y < h; ++y)
        std::memcpy(padded.data() + y * new_w * 4, pixels.data() + y * w * 4, w * 4);

    // Extend right edge
    for (int y = 0; y < h; ++y)
        for (int x = w; x < new_w; ++x)
            std::memcpy(padded.data() + (y * new_w + x) * 4,
                        padded.data() + (y * new_w + (w - 1)) * 4, 4);

    // Extend bottom edge
    for (int y = h; y < new_h; ++y)
        std::memcpy(padded.data() + y * new_w * 4,
                    padded.data() + (h - 1) * new_w * 4, new_w * 4);

    pixels = std::move(padded);
    w = new_w;
    h = new_h;
}

// Compress a single mip level to BC format
static bool compressMip(const uint8_t* rgba, int w, int h,
                        TexCompressionFormat fmt, int quality,
                        std::vector<uint8_t>& out)
{
    uint32_t blocks_x = (w + 3) / 4;
    uint32_t blocks_y = (h + 3) / 4;
    uint32_t block_size = getBlockSize(fmt);
    out.resize(blocks_x * blocks_y * block_size);

    uint8_t block_pixels[4 * 4 * 4]; // 4x4 RGBA

    for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            // Extract 4x4 block
            for (int py = 0; py < 4; ++py) {
                int src_y = std::min(static_cast<int>(by * 4 + py), h - 1);
                for (int px = 0; px < 4; ++px) {
                    int src_x = std::min(static_cast<int>(bx * 4 + px), w - 1);
                    std::memcpy(&block_pixels[(py * 4 + px) * 4],
                                &rgba[(src_y * w + src_x) * 4], 4);
                }
            }

            uint8_t* dst = out.data() + (by * blocks_x + bx) * block_size;

            switch (fmt) {
            case TexCompressionFormat::BC1: {
                uint32_t level = quality == 0 ? 4 : (quality == 1 ? 10 : 18);
                rgbcx::encode_bc1(level, dst, block_pixels, true, false);
                break;
            }
            case TexCompressionFormat::BC3: {
                uint32_t level = quality == 0 ? 4 : (quality == 1 ? 10 : 18);
                rgbcx::encode_bc3(level, dst, block_pixels);
                break;
            }
            case TexCompressionFormat::BC5: {
                rgbcx::encode_bc5(dst, block_pixels, 0, 1, 4);
                break;
            }
            case TexCompressionFormat::BC7: {
                bc7enc_compress_block_params params;
                bc7enc_compress_block_params_init(&params);
                if (quality >= 2) {
                    params.m_uber_level = 4;
                    params.m_max_partitions = BC7ENC_MAX_PARTITIONS;
                } else if (quality == 1) {
                    params.m_uber_level = 1;
                } else {
                    params.m_uber_level = 0;
                    params.m_max_partitions = 16;
                }
                bc7enc_compress_block(dst, block_pixels, &params);
                break;
            }
            default:
                return false;
            }
        }
    }
    return true;
}

// ================================================================
// isMeshFile / isTextureFile / isIntermediateFile
// ================================================================

bool AssetCompiler::isMeshFile(const std::string& ext)
{
    return ext == ".gltf" || ext == ".glb" || ext == ".obj";
}

bool AssetCompiler::isTextureFile(const std::string& ext)
{
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg"
        || ext == ".bmp" || ext == ".tga";
}

bool AssetCompiler::isIntermediateFile(const std::string& ext)
{
    return ext == ".meta" || ext == ".lodbin";
}

// ================================================================
// inferTextureFormat
// ================================================================

TexCompressionFormat AssetCompiler::inferTextureFormat(
    const std::string& filename, int channels,
    bool is_normal_map, const CompileConfig& config)
{
    if (is_normal_map || looksLikeNormalMap(filename))
        return config.normal_map_format;
    return config.default_color_format;
}

// ================================================================
// isUpToDate
// ================================================================

bool AssetCompiler::isUpToDate(const std::string& source_path, const std::string& compiled_path)
{
    if (!fs::exists(compiled_path))
        return false;

    uint64_t source_hash = Utils::hashFile(source_path);
    if (source_hash == 0)
        return false;

    // Try as .cmesh first
    std::string ext = toLower(fs::path(compiled_path).extension().string());
    if (ext == ".cmesh") {
        CmeshHeader header{};
        if (CompiledMeshSerializer::loadHeader(header, compiled_path))
            return header.source_hash == source_hash;
    } else if (ext == ".ctex") {
        CtexHeader header{};
        if (CompiledTextureSerializer::loadHeader(header, compiled_path))
            return header.source_hash == source_hash;
    }
    return false;
}

// ================================================================
// compileTexture
// ================================================================

bool AssetCompiler::compileTexture(
    const std::string& source_path,
    const std::string& output_path,
    const CompileConfig& config,
    bool is_normal_map)
{
    ensureEncodersInitialised();

    // Load source image as RGBA
    int w, h, channels;
    stbi_set_flip_vertically_on_load_thread(false);
    uint8_t* raw = stbi_load(source_path.c_str(), &w, &h, &channels, 4);
    if (!raw) {
        LOG_ENGINE_ERROR("[AssetCompiler] Failed to load texture: {}", source_path);
        return false;
    }

    // Copy into managed buffer
    std::vector<uint8_t> pixels(raw, raw + w * h * 4);
    stbi_image_free(raw);

    // Determine compression format
    bool has_alpha = hasAlpha(pixels.data(), w, h);
    TexCompressionFormat fmt = inferTextureFormat(source_path, channels, is_normal_map, config);

    // BC1 can't store alpha - upgrade to BC3/BC7 if needed
    if (fmt == TexCompressionFormat::BC1 && has_alpha)
        fmt = config.default_color_format == TexCompressionFormat::BC7
            ? TexCompressionFormat::BC7 : TexCompressionFormat::BC3;

    // Pad to multiple of 4 for BC block compression
    if (fmt != TexCompressionFormat::RGBA8)
        padToMultipleOf4(pixels, w, h);

    // Generate mip chain
    int mip_count = 1;
    if (config.generate_mipmaps)
        mip_count = static_cast<int>(std::floor(std::log2(std::max(w, h)))) + 1;

    // For BC formats, stop when a dimension would go below 4
    if (fmt != TexCompressionFormat::RGBA8) {
        int max_mips = 1;
        int mw = w, mh = h;
        while (mw >= 4 && mh >= 4) {
            mw = std::max(1, mw / 2);
            mh = std::max(1, mh / 2);
            if (mw >= 4 && mh >= 4)
                max_mips++;
            else
                break;
        }
        mip_count = std::min(mip_count, max_mips + 1);
        if (mip_count < 1) mip_count = 1;
    }

    CompiledTextureData tex_data{};
    tex_data.header.magic   = CTEX_MAGIC;
    tex_data.header.version = CTEX_VERSION;
    tex_data.header.width   = static_cast<uint32_t>(w);
    tex_data.header.height  = static_cast<uint32_t>(h);
    tex_data.header.format  = fmt;
    tex_data.header.mip_count = static_cast<uint32_t>(mip_count);
    tex_data.header.flags   = CTEX_FLAG_SRGB;
    if (is_normal_map || looksLikeNormalMap(source_path))
        tex_data.header.flags = CTEX_FLAG_NORMAL_MAP;
    tex_data.header.source_hash = Utils::hashFile(source_path);

    tex_data.mip_levels.resize(mip_count);

    // Mip 0 = original (possibly padded) image
    std::vector<uint8_t> current_mip = pixels;
    int mw = w, mh = h;

    for (int i = 0; i < mip_count; ++i) {
        auto& mip = tex_data.mip_levels[i];
        mip.width  = static_cast<uint32_t>(mw);
        mip.height = static_cast<uint32_t>(mh);

        if (fmt == TexCompressionFormat::RGBA8) {
            mip.data = current_mip;
        } else {
            if (!compressMip(current_mip.data(), mw, mh, fmt, config.bc7_quality, mip.data)) {
                LOG_ENGINE_ERROR("[AssetCompiler] BC compression failed for mip {} of {}", i, source_path);
                return false;
            }
        }

        // Generate next mip level
        if (i + 1 < mip_count) {
            int next_w = std::max(1, mw / 2);
            int next_h = std::max(1, mh / 2);

            // For BC, keep multiples of 4
            if (fmt != TexCompressionFormat::RGBA8) {
                next_w = std::max(4, (next_w + 3) & ~3);
                next_h = std::max(4, (next_h + 3) & ~3);
            }

            std::vector<uint8_t> next_mip(next_w * next_h * 4);
            stbir_resize_uint8_linear(
                current_mip.data(), mw, mh, mw * 4,
                next_mip.data(), next_w, next_h, next_w * 4,
                STBIR_RGBA);

            current_mip = std::move(next_mip);
            mw = next_w;
            mh = next_h;
        }
    }

    // Create output directory and write
    fs::create_directories(fs::path(output_path).parent_path());
    if (!CompiledTextureSerializer::save(tex_data, output_path)) {
        LOG_ENGINE_ERROR("[AssetCompiler] Failed to write {}", output_path);
        return false;
    }

    LOG_ENGINE_INFO("[AssetCompiler] Compiled texture: {} -> {} ({}x{}, {} mips, {})",
                    fs::path(source_path).filename().string(),
                    fs::path(output_path).filename().string(),
                    w, h, mip_count,
                    fmt == TexCompressionFormat::BC1 ? "BC1" :
                    fmt == TexCompressionFormat::BC3 ? "BC3" :
                    fmt == TexCompressionFormat::BC5 ? "BC5" :
                    fmt == TexCompressionFormat::BC7 ? "BC7" : "RGBA8");
    return true;
}

// ================================================================
// compileModel
// ================================================================

bool AssetCompiler::compileModel(
    const std::string& source_path,
    const std::string& output_path,
    const CompileConfig& config)
{
    std::string ext = toLower(fs::path(source_path).extension().string());

    // --- Load geometry ---
    vertex* raw_verts = nullptr;
    size_t  raw_vert_count = 0;
    std::vector<size_t> primitive_vertex_counts;
    std::vector<int>    material_indices;
    std::vector<std::string> material_names;
    std::vector<std::string> texture_paths;

    // Material data for .cmesh material references
    std::vector<CompiledMeshData::MaterialRef> mat_refs;

    if (ext == ".gltf" || ext == ".glb") {
        GltfLoaderConfig gltf_cfg;
        gltf_cfg.verbose_logging = false;
        gltf_cfg.generate_normals_if_missing = true;
        gltf_cfg.flip_uvs = true;
        gltf_cfg.triangulate = true;

        // Load geometry only (no render API needed)
        GltfLoadResult result = GltfLoader::loadGltf(source_path, gltf_cfg);
        if (!result.success) {
            LOG_ENGINE_ERROR("[AssetCompiler] Failed to load model: {} - {}", source_path, result.error_message);
            return false;
        }

        raw_verts = result.vertices;
        raw_vert_count = result.vertex_count;
        result.vertices = nullptr; // take ownership
        primitive_vertex_counts = result.primitive_vertex_counts;
        material_indices = result.material_indices;
        material_names = result.material_names;
        texture_paths = result.texture_paths;

        // Extract material properties from the glTF file directly
        // (load materials without a render API just for metadata)
        {
            MaterialLoaderConfig mat_cfg;
            mat_cfg.verbose_logging = false;
            mat_cfg.load_all_textures = false; // don't load pixels, just metadata

            auto mat_names_list = GltfMaterialLoader::getMaterialNames(source_path);
            auto tex_uris = GltfMaterialLoader::getTextureUris(source_path);

            // We can't load full material data without a render API,
            // but we can store names and texture paths from the gltf result
            for (size_t i = 0; i < mat_names_list.size(); ++i) {
                CompiledMeshData::MaterialRef ref;
                ref.name = mat_names_list[i];
                // Store texture paths from the gltf result
                // Map them to .ctex extensions
                mat_refs.push_back(std::move(ref));
            }

            // Associate texture paths with materials using the gltf texture_paths
            for (size_t i = 0; i < texture_paths.size() && i < mat_refs.size(); ++i) {
                if (!texture_paths[i].empty()) {
                    CompiledMeshData::MaterialRef::TextureRef tex_ref;
                    tex_ref.type = static_cast<uint8_t>(TextureType::BASE_COLOR);
                    // Replace texture extension with .ctex
                    tex_ref.path = replaceExtension(texture_paths[i], ".ctex");
                    mat_refs[i].textures.push_back(std::move(tex_ref));
                }
            }
        }
    }
    else if (ext == ".obj") {
        ObjLoaderConfig obj_cfg;
        obj_cfg.verbose_logging = false;
        obj_cfg.triangulate = true;

        ObjLoadResult result = ObjLoader::loadObj(source_path, obj_cfg);
        if (!result.success) {
            LOG_ENGINE_ERROR("[AssetCompiler] Failed to load model: {} - {}", source_path, result.error_message);
            return false;
        }

        raw_verts = result.vertices;
        raw_vert_count = result.vertex_count;
        result.vertices = nullptr; // take ownership
        primitive_vertex_counts.push_back(raw_vert_count);
    }
    else {
        LOG_ENGINE_ERROR("[AssetCompiler] Unsupported model format: {}", ext);
        return false;
    }

    if (!raw_verts || raw_vert_count == 0) {
        LOG_ENGINE_ERROR("[AssetCompiler] Empty model: {}", source_path);
        if (raw_verts) delete[] raw_verts;
        return false;
    }

    // --- Generate/reuse LODs ---
    // Check for existing pre-generated LOD data
    std::string meta_path = AssetMetadataSerializer::getMetaPath(source_path);
    AssetMetadata existing_meta;
    bool have_existing_lods = false;

    if (fs::exists(meta_path)) {
        if (AssetMetadataSerializer::load(existing_meta, meta_path)) {
            uint64_t current_hash = Utils::hashFile(source_path);
            if (current_hash == existing_meta.source_hash && existing_meta.lod_enabled)
                have_existing_lods = true;
        }
    }

    std::vector<LODMeshData> lod_meshes;

    if (have_existing_lods && !existing_meta.lod_levels.empty()) {
        // Reuse pre-generated LODs
        std::string mesh_dir = fs::path(source_path).parent_path().string();
        if (!mesh_dir.empty() && mesh_dir.back() != '/' && mesh_dir.back() != '\\')
            mesh_dir += "/";

        for (size_t i = 0; i < existing_meta.lod_levels.size(); ++i) {
            LODMeshData lod_data;
            if (i == 0) {
                // LOD0 = original (need to generate indexed version via LODGenerator)
            } else {
                const auto& lod_info = existing_meta.lod_levels[i];
                if (!lod_info.file_path.empty()) {
                    std::string lod_path = mesh_dir + lod_info.file_path;
                    if (LODMeshSerializer::load(lod_data, lod_path)) {
                        lod_data.achieved_error = lod_info.target_error;
                        lod_data.achieved_ratio = lod_info.target_ratio;
                    }
                }
            }
            if (i > 0 && !lod_data.vertices.empty())
                lod_meshes.push_back(std::move(lod_data));
        }
    }

    // Always generate LOD0 through the LODGenerator (optimises vertices)
    // and if we don't have existing LODs, generate them all
    {
        // Build submesh info
        std::vector<SubmeshInfo> submeshes;
        size_t current_vertex = 0;
        for (size_t pvc : primitive_vertex_counts) {
            SubmeshInfo sub;
            sub.start_vertex = current_vertex;
            sub.vertex_count = pvc;
            submeshes.push_back(sub);
            current_vertex += pvc;
        }

        AssetMetadata::LODConfig lod_cfg;
        if (have_existing_lods)
            lod_cfg = existing_meta.lod_config;

        // If we reused lodbin files, only generate LOD0
        if (!lod_meshes.empty()) {
            lod_cfg.max_lod_levels = 1;
            lod_cfg.target_ratios = {1.0f};
        }

        LODGenerationInput lod_input;
        lod_input.vertices = raw_verts;
        lod_input.vertex_count = raw_vert_count;
        lod_input.indices = nullptr;
        lod_input.index_count = 0;
        lod_input.config = lod_cfg;
        lod_input.submeshes = std::move(submeshes);

        LODGenerationResult lod_result = LODGenerator::generate(lod_input);
        if (!lod_result.success) {
            LOG_ENGINE_ERROR("[AssetCompiler] LOD generation failed for {}: {}",
                             source_path, lod_result.error_message);
            delete[] raw_verts;
            return false;
        }

        // Merge: LOD0 from generation + LOD1+ from either generation or existing files
        std::vector<LODMeshData> final_lods;
        if (!lod_result.lod_meshes.empty())
            final_lods.push_back(std::move(lod_result.lod_meshes[0])); // LOD0

        if (!lod_meshes.empty()) {
            // Reused from lodbin
            for (auto& l : lod_meshes)
                final_lods.push_back(std::move(l));
        } else {
            // All from generation
            for (size_t i = 1; i < lod_result.lod_meshes.size(); ++i)
                final_lods.push_back(std::move(lod_result.lod_meshes[i]));
        }

        lod_meshes = std::move(final_lods);
    }

    {
        MeshChunkConfig chunk_cfg;
        std::vector<bool> split_submesh(primitive_vertex_counts.size(), true);
        for (size_t i = 0; i < primitive_vertex_counts.size(); ++i) {
            int mat_idx = (i < material_indices.size()) ? material_indices[i] : -1;
            if (mat_idx >= 0 && mat_idx < static_cast<int>(mat_refs.size()))
                split_submesh[i] = mat_refs[mat_idx].alpha_mode != 2;
        }

        for (auto& lod_mesh : lod_meshes)
            lod_mesh = MeshChunker::chunkLODMesh(lod_mesh, chunk_cfg, &split_submesh);
    }

    delete[] raw_verts;
    raw_verts = nullptr;

    // --- Build CompiledMeshData ---
    CompiledMeshData cmesh{};
    cmesh.header.magic   = CMESH_MAGIC;
    cmesh.header.version = CMESH_VERSION;
    cmesh.header.flags   = CMESH_FLAG_HAS_INDICES;
    cmesh.header.source_hash = Utils::hashFile(source_path);

    if (lod_meshes.size() > 1)
        cmesh.header.flags |= CMESH_FLAG_HAS_LODS;
    if (!mat_refs.empty())
        cmesh.header.flags |= CMESH_FLAG_HAS_MATERIALS;

    // Submeshes
    for (size_t i = 0; i < primitive_vertex_counts.size(); ++i) {
        CompiledMeshData::Submesh sub;
        sub.material_index = (i < material_indices.size()) ? static_cast<uint32_t>(material_indices[i]) : 0;
        sub.name = (i < material_names.size() && sub.material_index < material_names.size())
                 ? material_names[sub.material_index] : "";
        cmesh.submeshes.push_back(std::move(sub));
    }
    cmesh.header.submesh_count = static_cast<uint32_t>(cmesh.submeshes.size());

    // Material references
    cmesh.material_refs = std::move(mat_refs);
    cmesh.header.material_ref_count = static_cast<uint32_t>(cmesh.material_refs.size());

    // LOD levels
    float default_thresholds[] = { 0.0f, 0.3f, 0.15f, 0.05f, 0.02f };
    for (size_t i = 0; i < lod_meshes.size(); ++i) {
        CompiledMeshData::LODLevel lod;
        lod.vertices = std::move(lod_meshes[i].vertices);
        lod.indices  = std::move(lod_meshes[i].indices);
        lod.achieved_error = lod_meshes[i].achieved_error;
        lod.achieved_ratio = lod_meshes[i].achieved_ratio;
        lod.submesh_ranges = std::move(lod_meshes[i].submesh_ranges);

        if (have_existing_lods && i < existing_meta.lod_levels.size())
            lod.screen_threshold = existing_meta.lod_levels[i].screen_threshold;
        else
            lod.screen_threshold = (i < 5) ? default_thresholds[i] : 0.01f;

        cmesh.lod_levels.push_back(std::move(lod));
    }
    cmesh.header.lod_count = static_cast<uint32_t>(cmesh.lod_levels.size());

    // AABB from LOD0
    if (!cmesh.lod_levels.empty() && !cmesh.lod_levels[0].vertices.empty()) {
        const auto& verts = cmesh.lod_levels[0].vertices;
        glm::vec3 bmin(verts[0].vx, verts[0].vy, verts[0].vz);
        glm::vec3 bmax = bmin;
        for (const auto& v : verts) {
            bmin.x = std::min(bmin.x, v.vx); bmin.y = std::min(bmin.y, v.vy); bmin.z = std::min(bmin.z, v.vz);
            bmax.x = std::max(bmax.x, v.vx); bmax.y = std::max(bmax.y, v.vy); bmax.z = std::max(bmax.z, v.vz);
        }
        cmesh.header.aabb_min[0] = bmin.x; cmesh.header.aabb_min[1] = bmin.y; cmesh.header.aabb_min[2] = bmin.z;
        cmesh.header.aabb_max[0] = bmax.x; cmesh.header.aabb_max[1] = bmax.y; cmesh.header.aabb_max[2] = bmax.z;
    }

    // Write .cmesh
    fs::create_directories(fs::path(output_path).parent_path());
    if (!CompiledMeshSerializer::save(cmesh, output_path)) {
        LOG_ENGINE_ERROR("[AssetCompiler] Failed to write {}", output_path);
        return false;
    }

    LOG_ENGINE_INFO("[AssetCompiler] Compiled model: {} -> {} ({} LODs, {} verts, {} tris)",
                    fs::path(source_path).filename().string(),
                    fs::path(output_path).filename().string(),
                    cmesh.header.lod_count,
                    cmesh.lod_levels.empty() ? 0 : cmesh.lod_levels[0].vertices.size(),
                    cmesh.lod_levels.empty() ? 0 : cmesh.lod_levels[0].indices.size() / 3);
    return true;
}

// ================================================================
// compileAll
// ================================================================

// ----------------------------------------------------------------
// Thread-safe progress tracking for parallel compileAll
// ----------------------------------------------------------------

struct SharedCompileProgress {
    std::atomic<int> total_assets{0};
    std::atomic<int> completed_assets{0};
    std::atomic<int> skipped_assets{0};
    std::atomic<int> failed_assets{0};
    std::atomic<int> models_compiled{0};
    std::atomic<int> textures_compiled{0};

    std::mutex string_mutex;
    std::string current_asset;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    CompileProgressCallback callback;

    void notifyProgress(const std::string& asset_name) {
        {
            std::lock_guard<std::mutex> lock(string_mutex);
            current_asset = asset_name;
        }
        if (callback)
            callback(takeSnapshot());
    }

    CompileProgress takeSnapshot() {
        CompileProgress p;
        p.total_assets     = total_assets.load(std::memory_order_relaxed);
        p.completed_assets = completed_assets.load(std::memory_order_relaxed);
        p.skipped_assets   = skipped_assets.load(std::memory_order_relaxed);
        p.failed_assets    = failed_assets.load(std::memory_order_relaxed);
        p.models_compiled  = models_compiled.load(std::memory_order_relaxed);
        p.textures_compiled = textures_compiled.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(string_mutex);
        p.current_asset = current_asset;
        p.errors   = errors;
        p.warnings = warnings;
        return p;
    }

    void addError(const std::string& msg) {
        std::lock_guard<std::mutex> lock(string_mutex);
        errors.push_back(msg);
    }

    void addWarning(const std::string& msg) {
        std::lock_guard<std::mutex> lock(string_mutex);
        warnings.push_back(msg);
    }
};

struct AssetWorkItem {
    std::string source_path;
    std::string output_path;
    std::string relative_path;
    fs::path    source_fs_path; // original path for copy-as-is
    enum Type { Model, Texture, Copy } type;
    bool is_normal_map = false;
};

CompileProgress AssetCompiler::compileAll(
    const std::string& source_root,
    const std::string& output_root,
    const CompileConfig& config,
    CompileProgressCallback progress_cb)
{
    CompileProgress progress;

    if (!fs::exists(source_root) || !fs::is_directory(source_root)) {
        progress.errors.push_back("Source directory not found: " + source_root);
        LOG_ENGINE_ERROR("[AssetCompiler] Source directory not found: {}", source_root);
        return progress;
    }

    LOG_ENGINE_INFO("[AssetCompiler] compileAll: source='{}' output='{}'", source_root, output_root);

    // --- Collect work items ---
    std::vector<AssetWorkItem> work_items;

    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(
             source_root, fs::directory_options::skip_permission_denied, ec))
    {
        if (ec) {
            LOG_ENGINE_ERROR("[AssetCompiler] Directory iteration error: {}", ec.message());
            break;
        }
        if (!entry.is_regular_file()) continue;

        std::string ext = toLower(entry.path().extension().string());
        if (isIntermediateFile(ext)) continue;

        fs::path relative = fs::relative(entry.path(), source_root, ec);
        if (ec) { ec.clear(); continue; }

        std::string src_path = entry.path().string();
        std::replace(src_path.begin(), src_path.end(), '\\', '/');

        AssetWorkItem item;
        item.source_path   = src_path;
        item.relative_path = relative.string();
        item.source_fs_path = entry.path();

        if (config.compile_models && isMeshFile(ext)) {
            fs::path out = fs::path(output_root) / relative;
            out.replace_extension(".cmesh");
            item.output_path = out.string();
            std::replace(item.output_path.begin(), item.output_path.end(), '\\', '/');
            item.type = AssetWorkItem::Model;
            LOG_ENGINE_TRACE("[AssetCompiler] Queued model: {} -> {}", item.relative_path, item.output_path);
        }
        else if (config.compile_textures && isTextureFile(ext)) {
            fs::path out = fs::path(output_root) / relative;
            out.replace_extension(".ctex");
            item.output_path = out.string();
            std::replace(item.output_path.begin(), item.output_path.end(), '\\', '/');
            item.type = AssetWorkItem::Texture;
            item.is_normal_map = looksLikeNormalMap(src_path);
            LOG_ENGINE_TRACE("[AssetCompiler] Queued texture: {} -> {}", item.relative_path, item.output_path);
        }
        else {
            fs::path out = fs::path(output_root) / relative;
            item.output_path = out.string();
            item.type = AssetWorkItem::Copy;
            LOG_ENGINE_TRACE("[AssetCompiler] Queued copy: {} -> {}", item.relative_path, item.output_path);
        }

        work_items.push_back(std::move(item));
    }

    LOG_ENGINE_INFO("[AssetCompiler] Collected {} work items ({} from '{}')",
                    work_items.size(), source_root, output_root);

    // --- Set up shared progress ---
    auto shared = std::make_shared<SharedCompileProgress>();
    shared->total_assets.store(static_cast<int>(work_items.size()), std::memory_order_relaxed);
    shared->callback = progress_cb;

    // --- Dispatch jobs in parallel ---
    std::vector<Threading::JobHandle> handles;
    handles.reserve(work_items.size());

    // Copy config by value so jobs don't hold a dangling reference
    // (compileAll receives config as const&, which may alias a temporary)
    CompileConfig cfg = config;

    for (size_t i = 0; i < work_items.size(); ++i) {
        // Capture work item by value — the job runs asynchronously on a worker thread
        AssetWorkItem item = work_items[i];

        auto handle = Threading::JobSystem::get().createJob()
            .setName("Compile: " + item.relative_path)
            .setPriority(Threading::JobPriority::Normal)
            .setContext(Threading::JobContext::Worker)
            .setWork([item, cfg, shared]() {
                shared->notifyProgress(item.relative_path);

                if (item.type == AssetWorkItem::Model) {
                    if (cfg.incremental && isUpToDate(item.source_path, item.output_path)) {
                        LOG_ENGINE_TRACE("[AssetCompiler] Skipped (up-to-date): {}", item.relative_path);
                        shared->skipped_assets.fetch_add(1, std::memory_order_relaxed);
                        shared->completed_assets.fetch_add(1, std::memory_order_relaxed);
                        if (shared->callback) shared->callback(shared->takeSnapshot());
                        return;
                    }

                    std::error_code local_ec;
                    fs::create_directories(fs::path(item.output_path).parent_path(), local_ec);

                    if (compileModel(item.source_path, item.output_path, cfg)) {
                        LOG_ENGINE_INFO("[AssetCompiler] Compiled model: {}", item.relative_path);
                        shared->models_compiled.fetch_add(1, std::memory_order_relaxed);
                        shared->completed_assets.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        LOG_ENGINE_ERROR("[AssetCompiler] FAILED model: {}", item.relative_path);
                        shared->failed_assets.fetch_add(1, std::memory_order_relaxed);
                        shared->addError("Failed to compile model: " + item.relative_path);
                    }
                }
                else if (item.type == AssetWorkItem::Texture) {
                    if (cfg.incremental && isUpToDate(item.source_path, item.output_path)) {
                        LOG_ENGINE_TRACE("[AssetCompiler] Skipped (up-to-date): {}", item.relative_path);
                        shared->skipped_assets.fetch_add(1, std::memory_order_relaxed);
                        shared->completed_assets.fetch_add(1, std::memory_order_relaxed);
                        if (shared->callback) shared->callback(shared->takeSnapshot());
                        return;
                    }

                    std::error_code local_ec;
                    fs::create_directories(fs::path(item.output_path).parent_path(), local_ec);

                    if (compileTexture(item.source_path, item.output_path, cfg, item.is_normal_map)) {
                        LOG_ENGINE_INFO("[AssetCompiler] Compiled texture: {}", item.relative_path);
                        shared->textures_compiled.fetch_add(1, std::memory_order_relaxed);
                        shared->completed_assets.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        LOG_ENGINE_ERROR("[AssetCompiler] FAILED texture: {}", item.relative_path);
                        shared->failed_assets.fetch_add(1, std::memory_order_relaxed);
                        shared->addError("Failed to compile texture: " + item.relative_path);
                    }
                }
                else {
                    // Copy as-is
                    std::error_code local_ec;
                    fs::create_directories(fs::path(item.output_path).parent_path(), local_ec);
                    if (local_ec) {
                        LOG_ENGINE_ERROR("[AssetCompiler] Failed to create dir for copy: {} - {}",
                                         item.output_path, local_ec.message());
                    }
                    fs::copy_file(item.source_fs_path, item.output_path,
                                  fs::copy_options::overwrite_existing, local_ec);
                    if (local_ec) {
                        LOG_ENGINE_ERROR("[AssetCompiler] Failed to copy: {} -> {} - {}",
                                         item.source_path, item.output_path, local_ec.message());
                        shared->addWarning("Failed to copy: " + item.relative_path + " - " + local_ec.message());
                    } else {
                        LOG_ENGINE_INFO("[AssetCompiler] Copied: {} -> {}", item.relative_path, item.output_path);
                    }
                    shared->completed_assets.fetch_add(1, std::memory_order_relaxed);
                }

                if (shared->callback) shared->callback(shared->takeSnapshot());
            })
            .submit();

        handles.push_back(handle);
    }

    LOG_ENGINE_INFO("[AssetCompiler] Dispatched {} jobs, waiting for completion...", handles.size());

    // --- Wait for all jobs to complete ---
    Threading::JobSystem::get().waitForJobs(handles);

    // --- Return aggregated result ---
    CompileProgress result = shared->takeSnapshot();
    LOG_ENGINE_INFO("[AssetCompiler] Done: {} completed, {} models, {} textures, {} skipped, {} failed",
                    result.completed_assets, result.models_compiled, result.textures_compiled,
                    result.skipped_assets, result.failed_assets);
    return result;
}

} // namespace Assets
