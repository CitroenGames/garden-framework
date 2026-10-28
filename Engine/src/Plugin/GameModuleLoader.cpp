#include "GameModuleLoader.hpp"
#include "GameFramework/GameModeRegistry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#else
#   include <dlfcn.h>
#   include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
    constexpr std::string_view kGenerationMarkerName = ".garden-module-hot-owner";
    constexpr std::string_view kGenerationMarkerContents = "Garden GameModuleLoader generation root v1\n";

    std::atomic_uint64_t g_generationOrdinal{0};
    std::atomic_uint64_t g_sessionOrdinal{0};

    uint64_t currentProcessId()
    {
#ifdef _WIN32
        return static_cast<uint64_t>(GetCurrentProcessId());
#else
        return static_cast<uint64_t>(getpid());
#endif
    }

    int moduleApiRevision(int32_t version)
    {
        return (version >> 24) & 0xff;
    }

    int moduleCompilerFamily(int32_t version)
    {
        return (version >> 20) & 0x0f;
    }

    int moduleCompilerVersion(int32_t version)
    {
        return (version >> 8) & 0x0fff;
    }

    int moduleStlDebugLevel(int32_t version)
    {
        return (version >> 4) & 0x0f;
    }

    int moduleDebugRuntime(int32_t version)
    {
        return version & 0x0f;
    }

    void printModuleVersionFields(const char* label, int32_t version)
    {
        fprintf(stderr, "%s=%d (api_revision=%d compiler_family=%d compiler_version=%d stl_debug_level=%d debug_runtime=%d)",
                label,
                version,
                moduleApiRevision(version),
                moduleCompilerFamily(version),
                moduleCompilerVersion(version),
                moduleStlDebugLevel(version),
                moduleDebugRuntime(version));
    }

    void printModuleVersionDifferences(int32_t dll_version, int32_t engine_version)
    {
        const char* separator = "";
        fprintf(stderr, " differing fields: ");
        const auto print_if_different = [&](const char* name, int dll_value, int engine_value)
        {
            if (dll_value == engine_value)
                return;
            fprintf(stderr, "%s%s(%d != %d)", separator, name, dll_value, engine_value);
            separator = ", ";
        };

        print_if_different("api_revision", moduleApiRevision(dll_version), moduleApiRevision(engine_version));
        print_if_different("compiler_family", moduleCompilerFamily(dll_version), moduleCompilerFamily(engine_version));
        print_if_different("compiler_version", moduleCompilerVersion(dll_version), moduleCompilerVersion(engine_version));
        print_if_different("stl_debug_level", moduleStlDebugLevel(dll_version), moduleStlDebugLevel(engine_version));
        print_if_different("debug_runtime", moduleDebugRuntime(dll_version), moduleDebugRuntime(engine_version));
    }

    void* platformLoad(const std::string& path)
    {
#ifdef _WIN32
        return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
        return dlopen(path.c_str(), RTLD_NOW);
#endif
    }

    void platformUnload(void* handle)
    {
        if (!handle) return;
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif
    }

    void* platformGetProc(void* handle, const char* name)
    {
        if (!handle) return nullptr;
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
        return dlsym(handle, name);
#endif
    }

    const char* platformGetError()
    {
#ifdef _WIN32
        static thread_local char buf[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, GetLastError(),
                       0, buf, sizeof(buf), nullptr);
        return buf;
#else
        return dlerror();
#endif
    }

    fs::path normalizedAbsolutePath(const std::string& value)
    {
        std::error_code ec;
        fs::path absolute = fs::absolute(fs::path(value), ec);
        return ec ? fs::path(value).lexically_normal() : absolute.lexically_normal();
    }

    bool isReparseBacked(const fs::path& path)
    {
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
        std::error_code ec;
        return fs::is_symlink(fs::symlink_status(path, ec));
#endif
    }

    struct SourceStamp
    {
        uintmax_t size = 0;
        fs::file_time_type write_time{};
    };

    bool captureRegularSourceStamp(const fs::path& path, SourceStamp& stamp, std::string& error)
    {
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(path, ec);
        if (ec || !fs::is_regular_file(status) || isReparseBacked(path))
        {
            error = "source is missing, non-regular, or reparse-backed";
            return false;
        }

        stamp.size = fs::file_size(path, ec);
        if (ec)
        {
            error = "could not read source size: " + ec.message();
            return false;
        }
        stamp.write_time = fs::last_write_time(path, ec);
        if (ec)
        {
            error = "could not read source write time: " + ec.message();
            return false;
        }
        return true;
    }

    bool sameSourceStamp(const SourceStamp& left, const SourceStamp& right)
    {
        return left.size == right.size && left.write_time == right.write_time;
    }

    bool markerIsValid(const fs::path& root)
    {
        std::ifstream marker(root / kGenerationMarkerName, std::ios::binary);
        std::string contents((std::istreambuf_iterator<char>(marker)), std::istreambuf_iterator<char>());
        return contents == kGenerationMarkerContents;
    }

    bool generationPathIsOwned(const fs::path& root, const fs::path& generation)
    {
        if (root.empty() || generation.empty() || generation.parent_path().lexically_normal() != root.lexically_normal())
            return false;
        if (generation.filename().string().rfind("generation-", 0) != 0 || isReparseBacked(root) || isReparseBacked(generation))
            return false;
        return markerIsValid(root);
    }

    bool removeOwnedGeneration(const fs::path& root, const fs::path& generation,
                               const std::vector<std::string>& owned_files)
    {
        if (generation.empty()) return true;
        if (!generationPathIsOwned(root, generation))
        {
            fprintf(stderr, "[GameModule] Refusing cleanup outside a validated owned generation: '%s'\n",
                    generation.string().c_str());
            return false;
        }

        std::error_code ec;
        std::vector<fs::path> expected;
        expected.reserve(owned_files.size());
        for (const std::string& owned_file : owned_files)
        {
            const fs::path file(owned_file);
            if (file.parent_path().lexically_normal() != generation.lexically_normal() || isReparseBacked(file))
            {
                fprintf(stderr, "[GameModule] Refusing cleanup with a non-direct owned file: '%s'\n", owned_file.c_str());
                return false;
            }
            expected.push_back(file.lexically_normal());
        }

        for (fs::directory_iterator it(generation, ec), end; !ec && it != end; it.increment(ec))
        {
            const fs::path entry = it->path().lexically_normal();
            const fs::file_status status = fs::symlink_status(entry, ec);
            if (ec || !fs::is_regular_file(status) || isReparseBacked(entry) ||
                std::find(expected.begin(), expected.end(), entry) == expected.end())
            {
                fprintf(stderr, "[GameModule] Refusing cleanup of generation with an unexpected entry: '%s'\n",
                        entry.string().c_str());
                return false;
            }
        }
        if (ec)
        {
            fprintf(stderr, "[GameModule] Could not inspect owned generation '%s': %s\n",
                    generation.string().c_str(), ec.message().c_str());
            return false;
        }

        for (const fs::path& file : expected)
        {
            fs::remove(file, ec);
            if (ec)
            {
                fprintf(stderr, "[GameModule] Retaining owned generation after failed file cleanup '%s': %s\n",
                        file.string().c_str(), ec.message().c_str());
                return false;
            }
        }
        fs::remove(generation, ec);
        if (ec)
        {
            fprintf(stderr, "[GameModule] Retaining owned generation after failed directory cleanup '%s': %s\n",
                    generation.string().c_str(), ec.message().c_str());
            return false;
        }
        return true;
    }

    void removeEmptyOwnedRoot(const fs::path& root)
    {
        if (root.empty() || !markerIsValid(root) || isReparseBacked(root)) return;
        std::error_code ec;
        size_t entries = 0;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        {
            ++entries;
        }
        if (!ec && entries == 1)
        {
            fs::remove(root / kGenerationMarkerName, ec);
            if (!ec) fs::remove(root, ec);
        }
        if (ec)
        {
            fprintf(stderr, "[GameModule] Retaining owned generation root '%s': %s\n",
                    root.string().c_str(), ec.message().c_str());
        }
    }

    bool ensureOwnedRoot(const fs::path& root, std::string& error)
    {
        std::error_code ec;
        fs::create_directories(root, ec);
        if (ec || isReparseBacked(root))
        {
            error = ec ? ec.message() : "generation root is reparse-backed";
            return false;
        }

        const fs::path marker = root / kGenerationMarkerName;
        if (!fs::exists(marker, ec))
        {
            std::ofstream output(marker, std::ios::binary | std::ios::trunc);
            output << kGenerationMarkerContents;
            output.close();
            if (!output)
            {
                error = "could not create ownership marker";
                return false;
            }
        }
        if (!markerIsValid(root))
        {
            error = "generation root ownership marker is invalid";
            return false;
        }
        return true;
    }

    struct CandidateExports
    {
        void* get_api_version = nullptr;
        void* get_game_name = nullptr;
        void* game_init = nullptr;
        void* game_shutdown = nullptr;
        void* register_components = nullptr;
        void* game_update = nullptr;
        void* on_level_loaded = nullptr;
        void* on_play_start = nullptr;
        void* on_play_stop = nullptr;
        void* server_init = nullptr;
        void* server_shutdown = nullptr;
        void* server_pre_physics = nullptr;
        void* server_update = nullptr;
        void* server_on_level_loaded = nullptr;
        void* server_on_client_connected = nullptr;
        void* server_on_client_disconnected = nullptr;
    };

    bool resolveCandidateExports(void* handle, const std::string& source_path, CandidateExports& exports)
    {
        exports.get_api_version = platformGetProc(handle, "gardenGetAPIVersion");
        exports.get_game_name = platformGetProc(handle, "gardenGetGameName");
        exports.game_init = platformGetProc(handle, "gardenGameInit");
        exports.game_shutdown = platformGetProc(handle, "gardenGameShutdown");
        exports.register_components = platformGetProc(handle, "gardenRegisterComponents");
        exports.game_update = platformGetProc(handle, "gardenGameUpdate");
        exports.on_level_loaded = platformGetProc(handle, "gardenOnLevelLoaded");
        exports.on_play_start = platformGetProc(handle, "gardenOnPlayStart");
        exports.on_play_stop = platformGetProc(handle, "gardenOnPlayStop");
        exports.server_init = platformGetProc(handle, "gardenServerInit");
        exports.server_shutdown = platformGetProc(handle, "gardenServerShutdown");
        exports.server_pre_physics = platformGetProc(handle, "gardenServerPrePhysics");
        exports.server_update = platformGetProc(handle, "gardenServerUpdate");
        exports.server_on_level_loaded = platformGetProc(handle, "gardenServerOnLevelLoaded");
        exports.server_on_client_connected = platformGetProc(handle, "gardenServerOnClientConnected");
        exports.server_on_client_disconnected = platformGetProc(handle, "gardenServerOnClientDisconnected");

        const std::pair<const char*, void*> required[] = {
            {"gardenGetAPIVersion", exports.get_api_version},
            {"gardenGetGameName", exports.get_game_name},
            {"gardenGameInit", exports.game_init},
            {"gardenGameShutdown", exports.game_shutdown},
            {"gardenRegisterComponents", exports.register_components},
            {"gardenGameUpdate", exports.game_update},
        };
        for (const auto& [name, pointer] : required)
        {
            if (!pointer)
            {
                fprintf(stderr, "[GameModule] Module '%s' is missing required client export '%s'\n",
                        source_path.c_str(), name);
                return false;
            }
        }
        return true;
    }

    bool validateCandidateVersion(const CandidateExports& exports, const std::string& source_path, int32_t& version)
    {
        const auto get_api_version = reinterpret_cast<int32_t(*)()>(exports.get_api_version);
        version = get_api_version();
        if (version == GARDEN_MODULE_API_VERSION) return true;

        if (moduleApiRevision(version) == 0)
        {
            fprintf(stderr, "[GameModule] API/ABI mismatch for '%s': DLL=%d (legacy API token), ",
                    source_path.c_str(), version);
        }
        else
        {
            fprintf(stderr, "[GameModule] API/ABI mismatch for '%s': ", source_path.c_str());
            printModuleVersionFields("DLL", version);
            fprintf(stderr, ", ");
        }
        printModuleVersionFields("Engine", GARDEN_MODULE_API_VERSION);
        printModuleVersionDifferences(version, GARDEN_MODULE_API_VERSION);
        fprintf(stderr, "\n");
        return false;
    }

    class PreparedModule final
    {
    public:
        explicit PreparedModule(fs::path root) : generation_root(std::move(root)) {}
        ~PreparedModule() { reset(); }
        PreparedModule(const PreparedModule&) = delete;
        PreparedModule& operator=(const PreparedModule&) = delete;

        void reset()
        {
            platformUnload(handle);
            handle = nullptr;
            if (!generation_path.empty())
                removeOwnedGeneration(generation_root, generation_path, owned_files);
            generation_path.clear();
            owned_files.clear();
            loaded_path.clear();
        }

        void* releaseHandle() { void* value = handle; handle = nullptr; return value; }
        std::string releaseGenerationPath()
        {
            std::string value = generation_path.string();
            generation_path.clear();
            return value;
        }
        std::vector<std::string> releaseOwnedFiles() { return std::move(owned_files); }

        fs::path generation_root;
        fs::path generation_path;
        fs::path source_path;
        fs::path loaded_path;
        std::vector<std::string> owned_files;
        GameModuleContentIdentity identity;
        void* handle = nullptr;
    };

    // Streaming SHA-256, with the full digest retained for module identity.
    class ImageSha256
    {
    public:
        void update(const char* data, size_t size)
        {
            m_length += static_cast<uint64_t>(size);
            for (size_t i = 0; i < size; ++i)
            {
                m_block[m_used++] = static_cast<uint8_t>(data[i]);
                if (m_used == 64) { transform(); m_used = 0; }
            }
        }

        std::array<uint8_t, 32> finish()
        {
            const uint64_t bits = m_length * 8;
            m_block[m_used++] = 0x80;
            if (m_used > 56)
            {
                while (m_used < 64) m_block[m_used++] = 0;
                transform();
                m_used = 0;
            }
            while (m_used < 56) m_block[m_used++] = 0;
            for (int i = 7; i >= 0; --i) m_block[m_used++] = static_cast<uint8_t>(bits >> (i * 8));
            transform();
            std::array<uint8_t, 32> result{};
            for (size_t i = 0; i < 8; ++i)
                for (size_t j = 0; j < 4; ++j)
                    result[i * 4 + j] = static_cast<uint8_t>(m_state[i] >> (24 - j * 8));
            return result;
        }

    private:
        static uint32_t rotate(uint32_t value, unsigned bits) { return (value >> bits) | (value << (32 - bits)); }
        void transform()
        {
            static constexpr uint32_t k[64] = {
                0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
                0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
                0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
                0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
                0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
                0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
                0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
                0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
            };
            uint32_t w[64]{};
            for (size_t i = 0; i < 16; ++i)
                w[i] = (uint32_t(m_block[i*4]) << 24) | (uint32_t(m_block[i*4+1]) << 16) |
                       (uint32_t(m_block[i*4+2]) << 8) | uint32_t(m_block[i*4+3]);
            for (size_t i = 16; i < 64; ++i)
            {
                const uint32_t s0 = rotate(w[i-15],7) ^ rotate(w[i-15],18) ^ (w[i-15] >> 3);
                const uint32_t s1 = rotate(w[i-2],17) ^ rotate(w[i-2],19) ^ (w[i-2] >> 10);
                w[i] = w[i-16] + s0 + w[i-7] + s1;
            }
            uint32_t a=m_state[0], b=m_state[1], c=m_state[2], d=m_state[3];
            uint32_t e=m_state[4], f=m_state[5], g=m_state[6], h=m_state[7];
            for (size_t i = 0; i < 64; ++i)
            {
                const uint32_t s1 = rotate(e,6) ^ rotate(e,11) ^ rotate(e,25);
                const uint32_t choice = (e & f) ^ (~e & g);
                const uint32_t t1 = h + s1 + choice + k[i] + w[i];
                const uint32_t s0 = rotate(a,2) ^ rotate(a,13) ^ rotate(a,22);
                const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                const uint32_t t2 = s0 + majority;
                h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
            }
            m_state[0]+=a; m_state[1]+=b; m_state[2]+=c; m_state[3]+=d;
            m_state[4]+=e; m_state[5]+=f; m_state[6]+=g; m_state[7]+=h;
        }
        uint32_t m_state[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                               0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        std::array<uint8_t, 64> m_block{};
        size_t m_used = 0;
        uint64_t m_length = 0;
    };

    // Compare complete streams and hash exactly the bytes of the owned image.
    // Failure, including an I/O error or a different length, rejects the copy.
    bool verifyOwnedImage(const fs::path& source, const fs::path& image, std::array<uint8_t, 32>& digest)
    {
        std::ifstream original(source, std::ios::binary);
        std::ifstream owned(image, std::ios::binary);
        if (!original || !owned) return false;
        std::array<char, 65536> a{}, b{};
        ImageSha256 hash;
        for (;;)
        {
            original.read(a.data(), static_cast<std::streamsize>(a.size()));
            owned.read(b.data(), static_cast<std::streamsize>(b.size()));
            const std::streamsize count = owned.gcount();
            if (original.gcount() != count || !std::equal(a.begin(), a.begin() + count, b.begin()) ||
                original.bad() || owned.bad()) return false;
            hash.update(b.data(), static_cast<size_t>(count));
            if (original.eof() || owned.eof())
            {
                if (!original.eof() || !owned.eof()) return false;
                digest = hash.finish();
                return true;
            }
            if (!original || !owned) return false;
        }
    }

    enum class CandidatePreparation
    {
        Success,
        SourceRefused,
        CopyFailed,
        CandidateRefused,
    };

    CandidatePreparation prepareOwnedCandidate(const std::string& source_value, PreparedModule& candidate)
    {
        candidate.source_path = normalizedAbsolutePath(source_value);
        SourceStamp before{};
        std::string error;
        if (!captureRegularSourceStamp(candidate.source_path, before, error))
        {
            fprintf(stderr, "[GameModule] Refusing module source '%s': %s\n",
                    candidate.source_path.string().c_str(), error.c_str());
            return CandidatePreparation::SourceRefused;
        }
        if (!ensureOwnedRoot(candidate.generation_root, error))
        {
            fprintf(stderr, "[GameModule] Could not prepare owned generation root '%s': %s\n",
                    candidate.generation_root.string().c_str(), error.c_str());
            return CandidatePreparation::CopyFailed;
        }

        const uint64_t ordinal = g_generationOrdinal.fetch_add(1, std::memory_order_relaxed);
        candidate.generation_path = candidate.generation_root / ("generation-" + std::to_string(ordinal));
        std::error_code ec;
        fs::create_directory(candidate.generation_path, ec);
        if (ec)
        {
            fprintf(stderr, "[GameModule] Could not create owned generation '%s': %s\n",
                    candidate.generation_path.string().c_str(), ec.message().c_str());
            candidate.generation_path.clear();
            return CandidatePreparation::CopyFailed;
        }

        const fs::path copied_image = candidate.generation_path /
            (candidate.source_path.stem().string() + "_generation_" + std::to_string(ordinal) + candidate.source_path.extension().string());
        const fs::path partial_image = copied_image.string() + ".copying";
        fs::copy_file(candidate.source_path, partial_image, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            fprintf(stderr, "[GameModule] Could not copy module source '%s': %s\n",
                    candidate.source_path.string().c_str(), ec.message().c_str());
            ec.clear();
            fs::remove(partial_image, ec);
            candidate.reset();
            return CandidatePreparation::CopyFailed;
        }

        SourceStamp after{};
        if (!verifyOwnedImage(candidate.source_path, partial_image, candidate.identity.image_sha256) ||
            !captureRegularSourceStamp(candidate.source_path, after, error) || !sameSourceStamp(before, after))
        {
            fprintf(stderr, "[GameModule] Deferred module load because source changed while copied: '%s'\n",
                    candidate.source_path.string().c_str());
            std::error_code cleanup_error;
            fs::remove(partial_image, cleanup_error);
            candidate.reset();
            return CandidatePreparation::CopyFailed;
        }
        fs::rename(partial_image, copied_image, ec);
        if (ec)
        {
            fprintf(stderr, "[GameModule] Could not publish owned module copy '%s': %s\n",
                    copied_image.string().c_str(), ec.message().c_str());
            ec.clear();
            fs::remove(partial_image, ec);
            candidate.reset();
            return CandidatePreparation::CopyFailed;
        }
        candidate.loaded_path = copied_image;
        candidate.owned_files.push_back(copied_image.string());

        const fs::path source_pdb = candidate.source_path.parent_path() / (candidate.source_path.stem().string() + ".pdb");
        if (fs::is_regular_file(source_pdb, ec) && !isReparseBacked(source_pdb))
        {
            const fs::path copied_pdb = candidate.generation_path /
                (candidate.source_path.stem().string() + "_generation_" + std::to_string(ordinal) + ".pdb");
            const fs::path partial_pdb = copied_pdb.string() + ".copying";
            ec.clear();
            fs::copy_file(source_pdb, partial_pdb, fs::copy_options::overwrite_existing, ec);
            if (!ec) fs::rename(partial_pdb, copied_pdb, ec);
            if (ec)
            {
                fprintf(stderr, "[GameModule] Could not copy optional PDB '%s': %s\n",
                        source_pdb.string().c_str(), ec.message().c_str());
                fs::remove(partial_pdb, ec);
            }
            else
            {
                candidate.owned_files.push_back(copied_pdb.string());
            }
        }

        candidate.handle = platformLoad(candidate.loaded_path.string());
        if (!candidate.handle)
        {
            fprintf(stderr, "[GameModule] Failed to load candidate DLL '%s': %s\n",
                    candidate.loaded_path.string().c_str(), platformGetError());
            candidate.reset();
            return CandidatePreparation::CandidateRefused;
        }
        CandidateExports exports;
        if (!resolveCandidateExports(candidate.handle, candidate.source_path.string(), exports) ||
            !validateCandidateVersion(exports, candidate.source_path.string(), candidate.identity.api_abi_version))
        {
            candidate.reset();
            return CandidatePreparation::CandidateRefused;
        }
        return CandidatePreparation::Success;
    }

}

GameModuleLoader::GameModuleLoader()
{
    const uint64_t session = g_sessionOrdinal.fetch_add(1, std::memory_order_relaxed);
    m_generation_root = (fs::temp_directory_path() / "GardenGameModuleHot" /
        (std::to_string(currentProcessId()) + "-" + std::to_string(session))).lexically_normal().string();
}

GameModuleLoader::~GameModuleLoader()
{
    unload();
}

GameModuleLoader::GameModuleLoader(GameModuleLoader&& other) noexcept
{
    moveFrom(std::move(other));
}

GameModuleLoader& GameModuleLoader::operator=(GameModuleLoader&& other) noexcept
{
    if (this != &other)
    {
        unload();
        moveFrom(std::move(other));
    }
    return *this;
}

void GameModuleLoader::moveFrom(GameModuleLoader&& other) noexcept
{
    m_handle = other.m_handle;
    m_source_path = std::move(other.m_source_path);
    m_loaded_path = std::move(other.m_loaded_path);
    m_generation_root = std::move(other.m_generation_root);
    m_generation_path = std::move(other.m_generation_path);
    m_owned_generation_files = std::move(other.m_owned_generation_files);
    m_content_identity = std::move(other.m_content_identity);
    m_fnGetAPIVersion = other.m_fnGetAPIVersion;
    m_fnGetGameName = other.m_fnGetGameName;
    m_fnGameInit = other.m_fnGameInit;
    m_fnGameShutdown = other.m_fnGameShutdown;
    m_fnRegisterComponents = other.m_fnRegisterComponents;
    m_fnGameUpdate = other.m_fnGameUpdate;
    m_fnOnLevelLoaded = other.m_fnOnLevelLoaded;
    m_fnOnPlayStart = other.m_fnOnPlayStart;
    m_fnOnPlayStop = other.m_fnOnPlayStop;
    m_fnServerInit = other.m_fnServerInit;
    m_fnServerShutdown = other.m_fnServerShutdown;
    m_fnServerPrePhysics = other.m_fnServerPrePhysics;
    m_fnServerUpdate = other.m_fnServerUpdate;
    m_fnServerOnLevelLoaded = other.m_fnServerOnLevelLoaded;
    m_fnServerOnClientConnected = other.m_fnServerOnClientConnected;
    m_fnServerOnClientDisconnected = other.m_fnServerOnClientDisconnected;

    other.m_handle = nullptr;
    other.m_source_path.clear();
    other.m_loaded_path.clear();
    other.m_generation_path.clear();
    other.m_owned_generation_files.clear();
    other.m_content_identity = {};
    other.clearFunctionPointers();
}

bool GameModuleLoader::loadReloadCandidate(const std::string& dll_path)
{
    if (m_handle || dll_path.empty()) return false;

    PreparedModule candidate(m_generation_root);
    if (prepareOwnedCandidate(dll_path, candidate) != CandidatePreparation::Success)
    {
        // A reload candidate is never allowed to load the source image in
        // place: an editor/compiler may still be writing it while A runs.
        removeEmptyOwnedRoot(m_generation_root);
        return false;
    }

    m_handle = candidate.releaseHandle();
    m_source_path = candidate.source_path.string();
    m_loaded_path = candidate.loaded_path.string();
    m_generation_path = candidate.releaseGenerationPath();
    m_owned_generation_files = candidate.releaseOwnedFiles();
    m_content_identity = std::move(candidate.identity);
    if (resolveFunctions()) return true;

    unload();
    return false;
}

bool GameModuleLoader::load(const std::string& dll_path)
{
    if (m_handle)
        unload();

    PreparedModule candidate(m_generation_root);
    if (prepareOwnedCandidate(dll_path, candidate) != CandidatePreparation::Success)
    {
        fprintf(stderr, "[GameModule] Refusing startup load without an owned generation copy: '%s'\n",
                dll_path.c_str());
        removeEmptyOwnedRoot(m_generation_root);
        return false;
    }

    m_handle = candidate.releaseHandle();
    m_source_path = candidate.source_path.string();
    m_loaded_path = candidate.loaded_path.string();
    m_generation_path = candidate.releaseGenerationPath();
    m_owned_generation_files = candidate.releaseOwnedFiles();
    m_content_identity = std::move(candidate.identity);

    if (!resolveFunctions())
    {
        unload();
        return false;
    }

    printf("[GameModule] Loaded '%s' (game: %s, server: %s)\n",
           m_source_path.c_str(), getGameName(), hasServerSupport() ? "yes" : "no");
    return true;
}

void GameModuleLoader::unload()
{
    if (!m_handle) return;

    const std::string game_name = getGameName();
    GameFramework::GameModeRegistry::get().unregisterBySource(game_name);
    clearFunctionPointers();
    platformUnload(m_handle);
    m_handle = nullptr;

    if (!m_generation_path.empty())
        removeOwnedGeneration(m_generation_root, m_generation_path, m_owned_generation_files);
    m_source_path.clear();
    m_loaded_path.clear();
    m_generation_path.clear();
    m_owned_generation_files.clear();
    m_content_identity = {};
    removeEmptyOwnedRoot(m_generation_root);
}

bool GameModuleLoader::reload(const std::string& dll_path)
{
    if (!m_handle)
        return load(dll_path);

    PreparedModule candidate(m_generation_root);
    const CandidatePreparation prepared = prepareOwnedCandidate(dll_path, candidate);
    if (prepared != CandidatePreparation::Success)
    {
        fprintf(stderr, "[GameModule] Reload deferred; active module remains loaded: '%s'\n", m_source_path.c_str());
        removeEmptyOwnedRoot(m_generation_root);
        return false;
    }

    printf("[GameModule] Promoting validated candidate for '%s'...\n", candidate.source_path.string().c_str());
    unload();
    m_handle = candidate.releaseHandle();
    m_source_path = candidate.source_path.string();
    m_loaded_path = candidate.loaded_path.string();
    m_generation_path = candidate.releaseGenerationPath();
    m_owned_generation_files = candidate.releaseOwnedFiles();
    m_content_identity = std::move(candidate.identity);

    if (!resolveFunctions())
    {
        unload();
        return false;
    }
    printf("[GameModule] Reloaded '%s' (game: %s, server: %s)\n",
           m_source_path.c_str(), getGameName(), hasServerSupport() ? "yes" : "no");
    return true;
}

bool GameModuleLoader::resolveFunctions()
{
    CandidateExports exports;
    if (!resolveCandidateExports(m_handle, m_source_path, exports)) return false;

    m_fnGetAPIVersion = reinterpret_cast<FnGetAPIVersion>(exports.get_api_version);
    m_fnGetGameName = reinterpret_cast<FnGetGameName>(exports.get_game_name);
    m_fnGameInit = reinterpret_cast<FnGameInit>(exports.game_init);
    m_fnGameShutdown = reinterpret_cast<FnGameShutdown>(exports.game_shutdown);
    m_fnRegisterComponents = reinterpret_cast<FnRegisterComponents>(exports.register_components);
    m_fnGameUpdate = reinterpret_cast<FnGameUpdate>(exports.game_update);
    m_fnOnLevelLoaded = reinterpret_cast<FnOnLevelLoaded>(exports.on_level_loaded);
    m_fnOnPlayStart = reinterpret_cast<FnOnPlayStart>(exports.on_play_start);
    m_fnOnPlayStop = reinterpret_cast<FnOnPlayStop>(exports.on_play_stop);
    m_fnServerInit = reinterpret_cast<FnServerInit>(exports.server_init);
    m_fnServerShutdown = reinterpret_cast<FnServerShutdown>(exports.server_shutdown);
    m_fnServerPrePhysics = reinterpret_cast<FnServerPrePhysics>(exports.server_pre_physics);
    m_fnServerUpdate = reinterpret_cast<FnServerUpdate>(exports.server_update);
    m_fnServerOnLevelLoaded = reinterpret_cast<FnServerOnLevelLoaded>(exports.server_on_level_loaded);
    m_fnServerOnClientConnected = reinterpret_cast<FnServerOnClientConnected>(exports.server_on_client_connected);
    m_fnServerOnClientDisconnected = reinterpret_cast<FnServerOnClientDisconnected>(exports.server_on_client_disconnected);
    return true;
}

void GameModuleLoader::clearFunctionPointers()
{
    m_fnGetAPIVersion = nullptr;
    m_fnGetGameName = nullptr;
    m_fnGameInit = nullptr;
    m_fnGameShutdown = nullptr;
    m_fnRegisterComponents = nullptr;
    m_fnGameUpdate = nullptr;
    m_fnOnLevelLoaded = nullptr;
    m_fnOnPlayStart = nullptr;
    m_fnOnPlayStop = nullptr;
    m_fnServerInit = nullptr;
    m_fnServerShutdown = nullptr;
    m_fnServerPrePhysics = nullptr;
    m_fnServerUpdate = nullptr;
    m_fnServerOnLevelLoaded = nullptr;
    m_fnServerOnClientConnected = nullptr;
    m_fnServerOnClientDisconnected = nullptr;
}

int32_t GameModuleLoader::getAPIVersion() { return m_fnGetAPIVersion ? m_fnGetAPIVersion() : -1; }
const char* GameModuleLoader::getGameName() { return m_fnGetGameName ? m_fnGetGameName() : "Unknown"; }
bool GameModuleLoader::init(EngineServices* services) { return m_fnGameInit ? m_fnGameInit(services) : false; }
void GameModuleLoader::shutdown() { if (m_fnGameShutdown) m_fnGameShutdown(); }
void GameModuleLoader::registerComponents(ReflectionRegistry* registry) { if (m_fnRegisterComponents) m_fnRegisterComponents(registry); }
void GameModuleLoader::update(float delta_time) { if (m_fnGameUpdate) m_fnGameUpdate(delta_time); }
void GameModuleLoader::onLevelLoaded() { if (m_fnOnLevelLoaded) m_fnOnLevelLoaded(); }
void GameModuleLoader::onPlayStart() { if (m_fnOnPlayStart) m_fnOnPlayStart(); }
void GameModuleLoader::onPlayStop() { if (m_fnOnPlayStop) m_fnOnPlayStop(); }
bool GameModuleLoader::serverInit(EngineServices* services) { return m_fnServerInit ? m_fnServerInit(services) : false; }
void GameModuleLoader::serverShutdown() { if (m_fnServerShutdown) m_fnServerShutdown(); }
void GameModuleLoader::serverPrePhysics(float fixed_delta) { if (m_fnServerPrePhysics) m_fnServerPrePhysics(fixed_delta); }
void GameModuleLoader::serverUpdate(float delta_time) { if (m_fnServerUpdate) m_fnServerUpdate(delta_time); }
void GameModuleLoader::serverOnLevelLoaded() { if (m_fnServerOnLevelLoaded) m_fnServerOnLevelLoaded(); }
void GameModuleLoader::serverOnClientConnected(uint16_t client_id) { if (m_fnServerOnClientConnected) m_fnServerOnClientConnected(client_id); }
void GameModuleLoader::serverOnClientDisconnected(uint16_t client_id) { if (m_fnServerOnClientDisconnected) m_fnServerOnClientDisconnected(client_id); }
