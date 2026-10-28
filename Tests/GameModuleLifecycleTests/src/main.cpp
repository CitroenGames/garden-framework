#include "Plugin/GameModuleHostSession.hpp"
#include "Reflection/ReflectionRegistry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

    fs::path executableDirectory()
    {
#ifdef _WIN32
        char path[32768]{};
        const DWORD length = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
        if (length != 0 && length < sizeof(path)) return fs::path(path).parent_path();
#endif
        return fs::current_path();
    }

    fs::path fixturePath(const std::string& name)
    {
#ifdef _WIN32
        return executableDirectory() / "GameModuleLifecycleFixtures" / (name + ".dll");
#elif defined(__APPLE__)
        return executableDirectory() / "GameModuleLifecycleFixtures" / ("lib" + name + ".dylib");
#else
        return executableDirectory() / "GameModuleLifecycleFixtures" / ("lib" + name + ".so");
#endif
    }

    std::vector<std::string> readEvents(const fs::path& path)
    {
        std::vector<std::string> events;
        std::ifstream input(path);
        for (std::string line; std::getline(input, line);)
            if (!line.empty()) events.push_back(line);
        return events;
    }

    bool equals(const std::vector<std::string>& actual, const std::vector<std::string>& expected)
    {
        return actual == expected;
    }

    class ScopedRecorder final
    {
    public:
        explicit ScopedRecorder(const std::string& name)
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() / ("garden_module_lifecycle_" + name + "_" + std::to_string(stamp) + ".log");
#ifdef _WIN32
            _putenv_s("GARDEN_GAME_MODULE_LIFECYCLE_RECORDER", m_path.string().c_str());
#else
            setenv("GARDEN_GAME_MODULE_LIFECYCLE_RECORDER", m_path.string().c_str(), 1);
#endif
        }

        ~ScopedRecorder()
        {
#ifdef _WIN32
            _putenv_s("GARDEN_GAME_MODULE_LIFECYCLE_RECORDER", "");
#else
            unsetenv("GARDEN_GAME_MODULE_LIFECYCLE_RECORDER");
#endif
            std::error_code ec;
            fs::remove(m_path, ec);
        }

        std::vector<std::string> events() const { return readEvents(m_path); }

    private:
        fs::path m_path;
    };

    struct HostLedger
    {
        std::vector<std::string> events;

        GameModuleHostSession::HostOperations operations()
        {
            return {
                [this]() { events.push_back("host.quiesce"); },
                [this]() { events.push_back("host.release_resources"); },
                [this](const std::string& source) { events.push_back("host.remove:" + source); },
            };
        }
    };

    bool expectLedger(const std::string& name, const HostLedger& ledger)
    {
        return equals(ledger.events, {
            "host.quiesce", "host.release_resources", "host.remove:GameModuleLifecycleFixture"}) ||
            fail(name, "host cleanup was not quiesce -> release -> registration removal");
    }

    bool testStandaloneAndPieClient()
    {
        const std::string name = "GameModuleLifecycle_L1_ClientSequences";
        {
            ScopedRecorder recorder("standalone");
            HostLedger ledger;
            ReflectionRegistry reflection;
            GameModuleHostSession session(ledger.operations());
            if (!session.loadAndRegister(fixturePath("client_recorder").string(), reflection) ||
                !session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() ||
                !session.beginPlaying(false))
                return fail(name, "standalone client did not reach Playing");
            session.updateClient(0.016f);
            session.stop();
            if (!equals(recorder.events(), {"register_components", "client.init", "client.level_loaded", "client.update", "client.shutdown"}))
                return fail(name, "standalone callback sequence was incorrect");
            if (!expectLedger(name, ledger)) return false;
        }
        {
            ScopedRecorder recorder("pie");
            HostLedger ledger;
            ReflectionRegistry reflection;
            GameModuleHostSession session(ledger.operations());
            if (!session.loadAndRegister(fixturePath("client_recorder").string(), reflection) ||
                !session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() ||
                !session.beginPlaying(true))
                return fail(name, "PIE client did not reach Playing");
            session.stop();
            if (!equals(recorder.events(), {"register_components", "client.init", "client.level_loaded", "client.play_start", "client.play_stop", "client.shutdown"}))
                return fail(name, "PIE did not pair play-start and play-stop around shutdown");
            if (!expectLedger(name, ledger)) return false;
        }
        return pass(name);
    }

    bool testDedicatedServer()
    {
        const std::string name = "GameModuleLifecycle_L2_DedicatedServerOrder";
        ScopedRecorder recorder("server");
        HostLedger ledger;
        ReflectionRegistry reflection;
        GameModuleHostSession session(ledger.operations());
        if (!session.loadAndRegister(fixturePath("server_recorder").string(), reflection) ||
            !session.initialiseServer(nullptr) || !session.notifyServerLevelLoaded() || !session.beginPlaying(false))
            return fail(name, "server did not reach Playing");
        session.updateServerPrePhysics(0.016f);
        session.updateServer(0.016f);
        session.stop();
        if (!equals(recorder.events(), {"register_components", "server.init", "server.level_loaded", "server.pre_physics", "server.update", "server.shutdown"}))
            return fail(name, "server callback order was incorrect");
        if (!expectLedger(name, ledger)) return false;
        return pass(name);
    }

    bool testListenServer()
    {
        const std::string name = "GameModuleLifecycle_L3_ListenServerOrder";
        ScopedRecorder recorder("listen");
        HostLedger ledger;
        ReflectionRegistry reflection;
        GameModuleHostSession session(ledger.operations());
        if (!session.loadAndRegister(fixturePath("server_recorder").string(), reflection) ||
            !session.initialiseServer(nullptr) || !session.notifyServerLevelLoaded() ||
            !session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() || !session.beginPlaying(true))
            return fail(name, "listen server did not reach Playing");
        session.updateServer(0.016f);
        session.updateClient(0.016f);
        session.stop();
        if (!equals(recorder.events(), {"register_components", "server.init", "server.level_loaded", "client.init", "client.level_loaded", "client.play_start", "server.update", "client.update", "client.play_stop", "client.shutdown", "server.shutdown"}))
            return fail(name, "listen-server order did not preserve server-first init and client-first shutdown");
        if (!expectLedger(name, ledger)) return false;
        return pass(name);
    }

    bool testFailedInitialisationRecovery()
    {
        const std::string name = "GameModuleLifecycle_L4_FailedInitCleanup";
        for (const std::string& fixture : {std::string("client_init_fails"), std::string("server_init_fails")})
        {
            ScopedRecorder recorder(fixture);
            HostLedger ledger;
            ReflectionRegistry reflection;
            GameModuleHostSession session(ledger.operations());
            if (!session.loadAndRegister(fixturePath(fixture).string(), reflection))
                return fail(name, "failure fixture did not load: " + fixture);
            bool initialised = false;
            if (fixture == "client_init_fails")
            {
                initialised = session.initialiseClient(nullptr);
            }
            else
            {
                // A listen-server failure must release the already-completed
                // client role without dispatching any later level/update hook.
                if (!session.initialiseClient(nullptr))
                    return fail(name, "peer client role did not initialize before server failure");
                initialised = session.initialiseServer(nullptr);
            }
            if (initialised || session.phase() != GameModuleHostSession::Phase::Idle || session.lastFailure().empty())
                return fail(name, "failed initialization left a callable session: " + fixture);
            const std::vector<std::string> expected = fixture == "client_init_fails"
                ? std::vector<std::string>{"register_components", "client.init"}
                : std::vector<std::string>{"register_components", "client.init", "server.init", "client.shutdown"};
            if (!equals(recorder.events(), expected))
                return fail(name, "failed initialization dispatched level/update/shutdown callbacks: " + fixture);
            if (!expectLedger(name, ledger)) return false;
        }

        ScopedRecorder recovery_recorder("recovery");
        HostLedger recovery_ledger;
        ReflectionRegistry recovery_reflection;
        GameModuleHostSession recovery(recovery_ledger.operations());
        if (!recovery.loadAndRegister(fixturePath("client_recorder").string(), recovery_reflection) ||
            !recovery.initialiseClient(nullptr) || !recovery.notifyClientLevelLoaded() || !recovery.beginPlaying(false))
            return fail(name, "valid module could not load after failed initialization cleanup");
        recovery.stop();
        return expectLedger(name, recovery_ledger) ? pass(name) : false;
    }

    bool testEditorFailureFallbackReusesSession()
    {
        const std::string name = "GameModuleLifecycle_EditorPieFallbackReuse";
        ScopedRecorder recorder("editor_fallback");
        HostLedger ledger;
        ReflectionRegistry reflection;
        GameModuleHostSession session(ledger.operations());

        // Editor PIE retains its host-session object while it recreates the
        // play world after a failed module init. Verify that the same session
        // can return from its bounded cleanup to Idle and then issue a fully
        // paired PIE lifecycle for the fallback module.
        if (!session.loadAndRegister(fixturePath("client_init_fails").string(), reflection) ||
            session.initialiseClient(nullptr) || session.phase() != GameModuleHostSession::Phase::Idle)
            return fail(name, "failed PIE candidate did not return its host session to Idle");

        if (!session.loadAndRegister(fixturePath("client_recorder").string(), reflection) ||
            !session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() ||
            !session.beginPlaying(true))
            return fail(name, "fallback PIE candidate did not reach Playing on the reused session");
        session.stop();

        if (!equals(recorder.events(), {
                "register_components", "client.init",
                "register_components", "client.init", "client.level_loaded",
                "client.play_start", "client.play_stop", "client.shutdown"}))
            return fail(name, "fallback module did not receive a complete paired PIE lifecycle");
        if (!equals(ledger.events, {
                "host.quiesce", "host.release_resources", "host.remove:GameModuleLifecycleFixture",
                "host.quiesce", "host.release_resources", "host.remove:GameModuleLifecycleFixture"}))
            return fail(name, "PIE fallback did not clean each candidate before reuse");
        return pass(name);
    }
}

int main()
{
    for (const std::string& fixture : {std::string("client_recorder"), std::string("server_recorder"),
                                       std::string("client_init_fails"), std::string("server_init_fails")})
    {
        if (!fs::exists(fixturePath(fixture)))
        {
            fail("GameModuleLifecycle_FixtureLayout", "missing fixture: " + fixturePath(fixture).string());
            return 1;
        }
    }

    bool ok = true;
    ok = testStandaloneAndPieClient() && ok;
    ok = testDedicatedServer() && ok;
    ok = testListenServer() && ok;
    ok = testFailedInitialisationRecovery() && ok;
    ok = testEditorFailureFallbackReusesSession() && ok;
    return ok ? 0 : 1;
}
