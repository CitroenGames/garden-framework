#include "Plugin/GameModuleLoader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
    bool fail(const std::string& name, const std::string& reason)
    {
        std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
        return false;
    }

    bool pass(const std::string& name)
    {
        std::cout << "[PASS] " << name << std::endl;
        return true;
    }

    std::vector<std::string> readEvents(const fs::path& path)
    {
        std::vector<std::string> events;
        std::ifstream file(path);
        for (std::string line; std::getline(file, line);)
        {
            if (!line.empty()) events.push_back(line);
        }
        return events;
    }

    bool contains(const std::string& text, const std::string& expected)
    {
        return text.find(expected) != std::string::npos;
    }

    bool hasExactly(const std::vector<std::string>& actual, std::vector<std::string> expected)
    {
        std::vector<std::string> sorted_actual = actual;
        std::sort(sorted_actual.begin(), sorted_actual.end());
        std::sort(expected.begin(), expected.end());
        return sorted_actual == expected;
    }

    fs::path executableDirectory()
    {
#ifdef _WIN32
        char executable_path[32768]{};
        const DWORD length = GetModuleFileNameA(nullptr, executable_path, static_cast<DWORD>(sizeof(executable_path)));
        if (length != 0 && length < sizeof(executable_path))
            return fs::path(executable_path).parent_path();
#endif
        return fs::current_path();
    }

    class ScopedRecorder final
    {
    public:
        explicit ScopedRecorder(const std::string& label)
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() /
                ("garden_game_module_contract_" + label + "_" + std::to_string(stamp) + ".log");
#ifdef _WIN32
            _putenv_s("GARDEN_GAME_MODULE_CONTRACT_RECORDER", m_path.string().c_str());
#else
            setenv("GARDEN_GAME_MODULE_CONTRACT_RECORDER", m_path.string().c_str(), 1);
#endif
        }

        ~ScopedRecorder()
        {
#ifdef _WIN32
            _putenv_s("GARDEN_GAME_MODULE_CONTRACT_RECORDER", "");
#else
            unsetenv("GARDEN_GAME_MODULE_CONTRACT_RECORDER");
#endif
            std::error_code ec;
            fs::remove(m_path, ec);
        }

        std::vector<std::string> events() const { return readEvents(m_path); }

    private:
        fs::path m_path;
    };

    class StderrCapture final
    {
    public:
        StderrCapture()
        {
            std::fflush(stderr);
#ifdef _WIN32
            m_saved_stderr = _dup(_fileno(stderr));
#else
            m_saved_stderr = dup(fileno(stderr));
#endif
            m_capture = std::tmpfile();
            if (m_saved_stderr < 0 || !m_capture) return;
#ifdef _WIN32
            m_active = _dup2(_fileno(m_capture), _fileno(stderr)) == 0;
#else
            m_active = dup2(fileno(m_capture), fileno(stderr)) == 0;
#endif
        }

        ~StderrCapture()
        {
            restore();
            if (m_capture) std::fclose(m_capture);
        }

        std::string finish()
        {
            restore();
            if (!m_capture) return {};
            std::fflush(m_capture);
            std::rewind(m_capture);
            std::string output;
            char buffer[512];
            while (const size_t count = std::fread(buffer, 1, sizeof(buffer), m_capture))
                output.append(buffer, count);
            std::fclose(m_capture);
            m_capture = nullptr;
            return output;
        }

    private:
        void restore()
        {
            if (m_active)
            {
                std::fflush(stderr);
#ifdef _WIN32
                _dup2(m_saved_stderr, _fileno(stderr));
#else
                dup2(m_saved_stderr, fileno(stderr));
#endif
                m_active = false;
            }
            if (m_saved_stderr >= 0)
            {
#ifdef _WIN32
                _close(m_saved_stderr);
#else
                close(m_saved_stderr);
#endif
                m_saved_stderr = -1;
            }
        }

        int m_saved_stderr = -1;
        FILE* m_capture = nullptr;
        bool m_active = false;
    };

    struct ContractPaths
    {
        fs::path fixture_directory;

        fs::path fixture(const std::string& name) const
        {
#ifdef _WIN32
            return fixture_directory / (name + ".dll");
#elif defined(__APPLE__)
            return fixture_directory / ("lib" + name + ".dylib");
#else
            return fixture_directory / ("lib" + name + ".so");
#endif
        }
    };

    bool loadAccepted(GameModuleLoader& loader, const fs::path& fixture, std::string& error)
    {
        StderrCapture capture;
        const bool loaded = loader.load(fixture.string());
        error = capture.finish();
        if (loaded && contains(error, "Refusing cleanup of generation with an unexpected entry"))
        {
            error = "accepted load reported unexpected generation cleanup: " + error;
            return false;
        }
        return loaded;
    }

    bool verifyRecovery(const ContractPaths& paths, const std::string& name, std::string& reason)
    {
        ScopedRecorder recorder(name + "_recovery");
        GameModuleLoader loader;
        std::string error;
        if (!loadAccepted(loader, paths.fixture("client_complete"), error))
        {
            reason = "following client_complete load failed: " + error;
            return false;
        }
        if (!loader.isLoaded())
        {
            reason = "following client_complete load did not retain a module handle";
            return false;
        }
        loader.unload();
        return true;
    }

    size_t directChildCount(const fs::path& directory)
    {
        std::error_code ec;
        size_t count = 0;
        for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
            ++count;
        return ec ? static_cast<size_t>(-1) : count;
    }

    bool testReloadCandidateRefusalKeepsActive(const ContractPaths& paths)
    {
        const std::string name = "GameModuleLifecycle_CandidateRefusalKeepsActive";
        ScopedRecorder recorder("candidate_refusal");
        GameModuleLoader loader;
        std::string error;
        if (!loadAccepted(loader, paths.fixture("client_complete"), error)) return fail(name, error);

        const fs::path active_image = loader.getLoadedImagePath();
        const fs::path generation_root = loader.getOwnedGenerationRoot();
        if (!fs::exists(active_image) || active_image.parent_path().parent_path() != generation_root)
            return fail(name, "initial load did not use a private owned generation");

        loader.update(0.016f);
        StderrCapture capture;
        if (loader.reload(paths.fixture("client_missing_game_update").string()))
            return fail(name, "candidate with a missing required export was promoted");
        const std::string reload_error = capture.finish();
        if (!contains(reload_error, "Reload deferred") || !contains(reload_error, "gardenGameUpdate"))
            return fail(name, "refused candidate did not report a precise deferred reload: " + reload_error);
        if (!loader.isLoaded() || loader.getLoadedImagePath() != active_image.string() || !fs::exists(active_image))
            return fail(name, "candidate refusal changed or released the active module image");

        loader.update(0.016f);
        if (!hasExactly(recorder.events(), {"client.update", "client.update"}))
            return fail(name, "active module did not continue receiving callbacks after candidate refusal");
        if (directChildCount(generation_root) != 2)
            return fail(name, "refused candidate left an owned generation behind");

        loader.unload();
        if (fs::exists(generation_root)) return fail(name, "normal unload left its private generation root behind");
        if (!fs::exists(paths.fixture("client_complete"))) return fail(name, "normal unload removed the source module");
        return pass(name);
    }

    bool testReloadPromotesValidatedGeneration(const ContractPaths& paths)
    {
        const std::string name = "GameModuleLifecycle_CandidatePromotion";
        ScopedRecorder recorder("candidate_promotion");
        GameModuleLoader loader;
        std::string error;
        if (!loadAccepted(loader, paths.fixture("client_complete"), error)) return fail(name, error);
        const fs::path previous_image = loader.getLoadedImagePath();
        const fs::path generation_root = loader.getOwnedGenerationRoot();

        if (!loader.reload(paths.fixture("client_lifecycle_recorder").string()))
            return fail(name, "valid candidate was not promoted");
        const fs::path promoted_image = loader.getLoadedImagePath();
        if (!fs::exists(promoted_image) || promoted_image == previous_image || fs::exists(previous_image))
            return fail(name, "candidate promotion did not exchange exactly one owned generation");
        if (directChildCount(generation_root) != 2)
            return fail(name, "successful promotion retained an inactive owned generation");

        loader.onLevelLoaded();
        loader.onPlayStart();
        loader.onPlayStop();
        if (!hasExactly(recorder.events(), {"client.level_loaded", "client.play_start", "client.play_stop"}))
            return fail(name, "promoted candidate did not receive its lifecycle callbacks");
        loader.unload();
        if (fs::exists(generation_root)) return fail(name, "promoted generation root remained after unload");
        return pass(name);
    }

    bool testOwnedGenerationCycles(const ContractPaths& paths)
    {
        const std::string name = "GameModuleLifecycle_OwnedGenerationCycles";
        GameModuleLoader loader;
        std::string error;
        if (!loadAccepted(loader, paths.fixture("client_complete"), error)) return fail(name, error);
        const fs::path source = paths.fixture("client_complete");
        const fs::path generation_root = loader.getOwnedGenerationRoot();
        fs::path previous = loader.getLoadedImagePath();

        for (int cycle = 0; cycle != 20; ++cycle)
        {
            if (!loader.reload(source.string())) return fail(name, "valid cycle candidate was refused at cycle " + std::to_string(cycle));
            const fs::path current = loader.getLoadedImagePath();
            if (current == previous || !fs::exists(current) || fs::exists(previous))
                return fail(name, "cycle did not release the prior image at cycle " + std::to_string(cycle));
            if (directChildCount(generation_root) != 2)
                return fail(name, "owned generation count grew at cycle " + std::to_string(cycle));
            previous = current;
        }

        loader.unload();
        if (fs::exists(generation_root)) return fail(name, "repeat cycle cleanup left a private generation root behind");
        if (!fs::exists(source)) return fail(name, "repeat cycle cleanup removed the source module");
        return pass(name);
    }

    bool testSourceImageCopyFailureIsRejected(const ContractPaths& paths)
    {
        const std::string name = "GameModuleContract_SourceImageFallbackRetired";
#ifdef _WIN32
        const fs::path source = paths.fixture("client_complete");
        const HANDLE source_lock = CreateFileA(source.string().c_str(), GENERIC_READ, 0, nullptr,
                                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (source_lock == INVALID_HANDLE_VALUE)
            return fail(name, "could not create exclusive source-image fixture lock");

        ScopedRecorder recorder("source_copy_failure");
        GameModuleLoader loader;
        const fs::path generation_root = loader.getOwnedGenerationRoot();
        StderrCapture capture;
        const bool loaded = loader.load(source.string());
        const std::string error = capture.finish();
        CloseHandle(source_lock);

        if (loaded || loader.isLoaded())
            return fail(name, "loader accepted a source image after owned-copy failure");
        if (!recorder.events().empty())
            return fail(name, "refused source image executed a module callback");
        if (!contains(error, "Could not copy module source") ||
            !contains(error, "Refusing startup load without an owned generation copy") ||
            !contains(error, source.string()))
            return fail(name, "copy failure was not reported as an owned-generation rejection: " + error);
        if (contains(error, "loading source in place") || contains(error, "source fallback"))
            return fail(name, "retired source-image fallback diagnostic was emitted: " + error);
        if (!fs::exists(source) || fs::exists(generation_root))
            return fail(name, "copy failure changed the source fixture or retained an empty generation root");

        GameModuleLoader active_loader;
        std::string active_load_error;
        if (!loadAccepted(active_loader, source, active_load_error))
            return fail(name, "could not prepare the active owned generation: " + active_load_error);
        const fs::path active_image = active_loader.getLoadedImagePath();
        active_loader.update(0.016f);

        const HANDLE reload_source_lock = CreateFileA(source.string().c_str(), GENERIC_READ, 0, nullptr,
                                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reload_source_lock == INVALID_HANDLE_VALUE)
        {
            active_loader.unload();
            return fail(name, "could not lock the source image for candidate-copy refusal");
        }
        StderrCapture reload_capture;
        const bool reloaded = active_loader.reload(source.string());
        const std::string reload_error = reload_capture.finish();
        CloseHandle(reload_source_lock);

        if (reloaded || !active_loader.isLoaded() || active_loader.getLoadedImagePath() != active_image.string() ||
            !fs::exists(active_image))
        {
            active_loader.unload();
            return fail(name, "owned-copy candidate failure changed the active generation");
        }
        if (!contains(reload_error, "Could not copy module source") || !contains(reload_error, "Reload deferred") ||
            contains(reload_error, "loading source in place") || contains(reload_error, "source fallback"))
        {
            active_loader.unload();
            return fail(name, "candidate-copy failure did not preserve the active generation: " + reload_error);
        }
        active_loader.update(0.016f);
        if (!hasExactly(recorder.events(), {"client.update", "client.update"}))
        {
            active_loader.unload();
            return fail(name, "active generation did not remain callable after candidate-copy refusal");
        }
        active_loader.unload();
#else
        (void)paths;
#endif
        return pass(name);
    }

    bool testRequiredExportRejections(const ContractPaths& paths)
    {
        const std::vector<std::pair<std::string, std::string>> cases = {
            {"client_missing_get_api_version", "gardenGetAPIVersion"},
            {"client_missing_get_game_name", "gardenGetGameName"},
            {"client_missing_game_init", "gardenGameInit"},
            {"client_missing_game_shutdown", "gardenGameShutdown"},
            {"client_missing_register_components", "gardenRegisterComponents"},
            {"client_missing_game_update", "gardenGameUpdate"},
        };

        for (const auto& [fixture_name, missing_export] : cases)
        {
            const std::string name = "GameModuleContract_RequiredExport_" + missing_export;
            ScopedRecorder recorder(fixture_name);
            GameModuleLoader loader;
            StderrCapture capture;
            const bool loaded = loader.load(paths.fixture(fixture_name).string());
            const std::string error = capture.finish();
            if (loaded || loader.isLoaded())
                return fail(name, "fixture with a missing core export was accepted");
            if (!recorder.events().empty())
                return fail(name, "refused module executed a callback");
            if (!contains(error, paths.fixture(fixture_name).string()) || !contains(error, missing_export))
                return fail(name, "diagnostic did not name the source fixture and missing export: " + error);
            std::string recovery_error;
            if (!verifyRecovery(paths, fixture_name, recovery_error)) return fail(name, recovery_error);
        }
        return pass("GameModuleContract_RequiredExportRejections");
    }

    bool testAbiRejections(const ContractPaths& paths)
    {
        const std::vector<std::pair<std::string, std::string>> cases = {
            {"abi_revision_mismatch", "api_revision"},
            {"abi_compiler_family_mismatch", "compiler_family"},
            {"abi_compiler_version_mismatch", "compiler_version"},
            {"abi_stl_debug_level_mismatch", "stl_debug_level"},
            {"abi_debug_runtime_mismatch", "debug_runtime"},
        };

        for (const auto& [fixture_name, field] : cases)
        {
            const std::string name = "GameModuleContract_Abi_" + field;
            ScopedRecorder recorder(fixture_name);
            GameModuleLoader loader;
            StderrCapture capture;
            const bool loaded = loader.load(paths.fixture(fixture_name).string());
            const std::string error = capture.finish();
            if (loaded || loader.isLoaded()) return fail(name, "ABI-mismatched fixture was accepted");
            if (!recorder.events().empty()) return fail(name, "refused ABI fixture executed a callback");
            if (!contains(error, paths.fixture(fixture_name).string()) || !contains(error, "DLL=") ||
                !contains(error, "Engine=") || !contains(error, field))
                return fail(name, "diagnostic did not name fixture, packed tokens, and differing field: " + error);
            std::string recovery_error;
            if (!verifyRecovery(paths, fixture_name, recovery_error)) return fail(name, recovery_error);
        }
        return pass("GameModuleContract_AbiRejections");
    }

    bool testClientOptionalHooks(const ContractPaths& paths)
    {
        for (const std::string& fixture_name : {std::string("client_complete"), std::string("client_lifecycle_absent")})
        {
            const std::string name = "GameModuleContract_ClientOptionalHooks_" + fixture_name;
            ScopedRecorder recorder(fixture_name);
            GameModuleLoader loader;
            std::string error;
            if (!loadAccepted(loader, paths.fixture(fixture_name), error)) return fail(name, error);
            if (!loader.isLoaded() || loader.hasServerSupport()) return fail(name, "client-only module reported the wrong load state");
            if (!loader.init(nullptr) || loader.serverInit(nullptr)) return fail(name, "client/server initialization returned an unexpected result");
            loader.registerComponents(nullptr);
            loader.update(0.016f);
            loader.onLevelLoaded();
            loader.onPlayStart();
            loader.onPlayStop();
            loader.serverShutdown();
            loader.serverPrePhysics(0.016f);
            loader.serverUpdate(0.016f);
            loader.serverOnLevelLoaded();
            loader.serverOnClientConnected(1);
            loader.serverOnClientDisconnected(1);
            loader.shutdown();
            if (!hasExactly(recorder.events(), {"client.init", "client.register_components", "client.update", "client.shutdown"}))
                return fail(name, "optional hooks were dispatched or required hooks were not recorded");
            loader.unload();
            std::string recovery_error;
            if (!verifyRecovery(paths, fixture_name, recovery_error)) return fail(name, recovery_error);
        }
        return pass("GameModuleContract_ClientOptionalHooks");
    }

    bool testClientLifecycleRecorder(const ContractPaths& paths)
    {
        const std::string name = "GameModuleContract_ClientLifecycleRecorder";
        ScopedRecorder recorder("client_lifecycle_recorder");
        GameModuleLoader loader;
        std::string error;
        if (!loadAccepted(loader, paths.fixture("client_lifecycle_recorder"), error)) return fail(name, error);
        loader.onLevelLoaded();
        loader.onPlayStart();
        loader.onPlayStop();
        if (!hasExactly(recorder.events(), {"client.level_loaded", "client.play_start", "client.play_stop"}))
            return fail(name, "lifecycle wrappers did not dispatch exactly once");
        loader.unload();
        std::string recovery_error;
        if (!verifyRecovery(paths, "client_lifecycle_recorder", recovery_error)) return fail(name, recovery_error);
        return pass(name);
    }

    bool testServerShapes(const ContractPaths& paths)
    {
        struct Case
        {
            const char* fixture;
            bool has_server_support;
            std::vector<std::string> expected;
        };
        const std::vector<Case> cases = {
            {"server_none", false, {}},
            {"server_init_only", true, {"server.init"}},
            {"server_partial", true, {"server.init", "server.update", "server.client_connected"}},
            {"server_callbacks_without_init", false, {"server.update", "server.client_connected"}},
            {"server_complete_recorder", true, {"server.init", "server.shutdown", "server.pre_physics", "server.update", "server.level_loaded", "server.client_connected", "server.client_disconnected"}},
        };

        for (const Case& test_case : cases)
        {
            const std::string name = "GameModuleContract_Server_" + std::string(test_case.fixture);
            ScopedRecorder recorder(test_case.fixture);
            GameModuleLoader loader;
            std::string error;
            if (!loadAccepted(loader, paths.fixture(test_case.fixture), error)) return fail(name, error);
            if (loader.hasServerSupport() != test_case.has_server_support)
                return fail(name, "server capability marker had the wrong value");
            const bool server_init = loader.serverInit(nullptr);
            if (server_init != test_case.has_server_support)
                return fail(name, "server init result did not match the server capability marker");
            loader.serverShutdown();
            loader.serverPrePhysics(0.016f);
            loader.serverUpdate(0.016f);
            loader.serverOnLevelLoaded();
            loader.serverOnClientConnected(3);
            loader.serverOnClientDisconnected(3);
            if (!hasExactly(recorder.events(), test_case.expected))
                return fail(name, "present and absent server wrappers did not follow the fixture export set");
            loader.unload();
            std::string recovery_error;
            if (!verifyRecovery(paths, test_case.fixture, recovery_error)) return fail(name, recovery_error);
        }
        return pass("GameModuleContract_ServerShapes");
    }
}

int main()
{
    const ContractPaths paths{executableDirectory() / "GameModuleContractFixtures"};
    const std::vector<std::string> fixtures = {
        "client_complete", "client_missing_get_api_version", "client_missing_get_game_name",
        "client_missing_game_init", "client_missing_game_shutdown", "client_missing_register_components",
        "client_missing_game_update", "client_lifecycle_absent", "client_lifecycle_recorder", "server_none",
        "server_init_only", "server_partial", "server_callbacks_without_init", "server_complete_recorder",
        "abi_revision_mismatch", "abi_compiler_family_mismatch", "abi_compiler_version_mismatch",
        "abi_stl_debug_level_mismatch", "abi_debug_runtime_mismatch",
    };
    for (const std::string& fixture : fixtures)
    {
        if (!fs::exists(paths.fixture(fixture)))
        {
            fail("GameModuleContract_FixtureLayout", "missing fixture: " + paths.fixture(fixture).string());
            return 1;
        }
    }

    bool ok = true;
    ok = testRequiredExportRejections(paths) && ok;
    ok = testAbiRejections(paths) && ok;
    ok = testClientOptionalHooks(paths) && ok;
    ok = testClientLifecycleRecorder(paths) && ok;
    ok = testServerShapes(paths) && ok;
    ok = testReloadCandidateRefusalKeepsActive(paths) && ok;
    ok = testReloadPromotesValidatedGeneration(paths) && ok;
    ok = testOwnedGenerationCycles(paths) && ok;
    ok = testSourceImageCopyFailureIsRejected(paths) && ok;
    return ok ? 0 : 1;
}
