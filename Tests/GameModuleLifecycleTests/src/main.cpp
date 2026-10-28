#include "Plugin/GameModuleHostSession.hpp"
#include "Reflection/ReflectionRegistry.hpp"
#include "GameFramework/GameModeRegistry.hpp"

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
        struct CallbackToken
        {
            std::string source_id;
            std::string kind;
            bool active = true;
        };

        std::vector<std::string> events;
        std::vector<CallbackToken> callback_tokens;
        size_t released_callback_count = 0;
        size_t released_resource_count = 0;
        size_t removed_registration_count = 0;
        size_t dispatched_callback_count = 0;

        void addCallbackToken(std::string source_id, std::string kind)
        {
            callback_tokens.push_back({std::move(source_id), std::move(kind), true});
        }

        void dispatchCallbacks()
        {
            for (const CallbackToken& token : callback_tokens)
                if (token.active)
                    ++dispatched_callback_count;
        }

        GameModuleHostSession::HostOperations operations()
        {
            return {
                [this]() { events.push_back("host.quiesce"); },
                [this]() {
                    ++released_resource_count;
                    events.push_back("host.release_resources");
                },
                [this](const std::string& source) {
                    ++removed_registration_count;
                    events.push_back("host.remove:" + source);
                },
                [this](const std::string& source) {
                    events.push_back("host.release_callbacks:" + source);
                    for (CallbackToken& token : callback_tokens)
                    {
                        if (token.active && token.source_id == source)
                        {
                            token.active = false;
                            ++released_callback_count;
                            events.push_back("host.cancel:" + token.kind);
                        }
                    }
                },
            };
        }

        size_t activeCallbackCount() const
        {
            return static_cast<size_t>(std::count_if(callback_tokens.begin(), callback_tokens.end(),
                [](const CallbackToken& token) { return token.active; }));
        }
    };

    bool expectLedger(const std::string& name, const HostLedger& ledger)
    {
        return equals(ledger.events, {
            "host.quiesce", "host.release_callbacks:GameModuleLifecycleFixture",
            "host.remove:GameModuleLifecycleFixture", "host.release_resources"}) ||
            fail(name, "host cleanup was not quiesce -> callback/factory removal -> resource release");
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
                "host.quiesce", "host.release_callbacks:GameModuleLifecycleFixture",
                "host.remove:GameModuleLifecycleFixture", "host.release_resources",
                "host.quiesce", "host.release_callbacks:GameModuleLifecycleFixture",
                "host.remove:GameModuleLifecycleFixture", "host.release_resources"}))
            return fail(name, "PIE fallback did not clean each candidate before reuse");
        return pass(name);
    }

    bool testOwnedTeardownAndNoPostUnloadDispatch()
    {
        const std::string name = "GameModuleLifecycle_L5_OwnedTeardown";
        constexpr const char* source = "GameModuleLifecycleFixture";
        constexpr const char* factory = "GameModuleLifecycleOwnedFactory";
        ScopedRecorder recorder("owned_teardown");
        HostLedger ledger;
        ReflectionRegistry reflection;
        GameModuleHostSession session(ledger.operations());

        if (!session.loadAndRegister(fixturePath("owned_teardown_recorder").string(), reflection) ||
            !GameFramework::GameModeRegistry::get().hasGameMode(factory))
            return fail(name, "fixture factory was not registered with the module source");

        // These stand in for event, job, and UI subscriptions owned by the
        // host. The session must cancel them before any DLL callback can be
        // shut down or the image closed.
        ledger.addCallbackToken(source, "event");
        ledger.addCallbackToken(source, "job");
        ledger.addCallbackToken(source, "ui");
        ledger.dispatchCallbacks();
        if (ledger.dispatched_callback_count != 3 || ledger.activeCallbackCount() != 3)
            return fail(name, "host did not retain each owned callback token before teardown");

        if (!session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() ||
            !session.beginPlaying(false))
            return fail(name, "owned teardown fixture did not reach Playing");
        session.updateClient(0.016f);
        session.stop();

        if (session.loader().isLoaded() || session.phase() != GameModuleHostSession::Phase::Idle ||
            ledger.activeCallbackCount() != 0 || ledger.released_callback_count != 3 ||
            ledger.released_resource_count != 1 || ledger.removed_registration_count != 1)
            return fail(name, "teardown left a callback, module image, or host resource active");
        if (GameFramework::GameModeRegistry::get().hasGameMode(factory))
            return fail(name, "module factory survived source-scoped teardown");

        const size_t dispatches_before_post_unload_tick = ledger.dispatched_callback_count;
        const std::vector<std::string> events_before_post_unload_tick = recorder.events();
        ledger.dispatchCallbacks();
        session.updateClient(0.016f);
        if (ledger.dispatched_callback_count != dispatches_before_post_unload_tick ||
            recorder.events() != events_before_post_unload_tick)
            return fail(name, "post-unload host tick dispatched fixture code");

        const std::vector<std::string> expected = {
            "register_components", "factory.registered", "client.init", "client.level_loaded",
            "client.update", "client.shutdown"};
        if (!equals(recorder.events(), expected))
            return fail(name, "fixture callbacks were not bounded by teardown");
        const std::vector<std::string> expected_ledger = {
            "host.quiesce", "host.release_callbacks:GameModuleLifecycleFixture",
            "host.cancel:event", "host.cancel:job", "host.cancel:ui",
            "host.remove:GameModuleLifecycleFixture", "host.release_resources"};
        if (!equals(ledger.events, expected_ledger))
            return fail(name, "owned callback cancellation did not precede module shutdown");
        return pass(name);
    }

    bool testPostTeardownInitFailureRecovery()
    {
        const std::string name = "GameModuleLifecycle_L8_PostTeardownInitFailure";
        constexpr const char* source = "GameModuleLifecycleFixture";
        constexpr const char* factory = "GameModuleLifecycleOwnedFactory";
        ScopedRecorder recorder("post_teardown_failure");
        HostLedger ledger;
        ReflectionRegistry reflection;
        GameModuleHostSession session(ledger.operations());

        // A is a valid running generation. This slice models the post-A
        // promotion point: once A is stopped, an init failure in B must leave
        // no active callbacks/image/factory before C is attempted.
        if (!session.loadAndRegister(fixturePath("client_recorder").string(), reflection) ||
            !session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() ||
            !session.beginPlaying(false))
            return fail(name, "initial generation A did not reach Playing");
        session.updateClient(0.016f);
        session.stop();
        if (session.loader().isLoaded() || session.phase() != GameModuleHostSession::Phase::Idle)
            return fail(name, "generation A did not unload before candidate B");

        if (!session.loadAndRegister(fixturePath("owned_teardown_init_fails").string(), reflection) ||
            !GameFramework::GameModeRegistry::get().hasGameMode(factory))
            return fail(name, "candidate B did not load and register its owned factory");
        ledger.addCallbackToken(source, "event");
        ledger.addCallbackToken(source, "job");
        ledger.addCallbackToken(source, "ui");
        if (session.initialiseClient(nullptr) || session.phase() != GameModuleHostSession::Phase::Idle ||
            session.loader().isLoaded() || session.lastFailure().empty())
            return fail(name, "candidate B init failure left a callable module session");
        if (ledger.activeCallbackCount() != 0 || ledger.released_callback_count != 3 ||
            GameFramework::GameModeRegistry::get().hasGameMode(factory))
            return fail(name, "candidate B partial cleanup retained a callback or factory");

        const size_t post_failure_dispatches = ledger.dispatched_callback_count;
        const std::vector<std::string> post_failure_events = recorder.events();
        ledger.dispatchCallbacks();
        session.updateClient(0.016f);
        if (ledger.dispatched_callback_count != post_failure_dispatches || recorder.events() != post_failure_events)
            return fail(name, "failed candidate B was dispatched after unloading");

        if (!session.loadAndRegister(fixturePath("client_recorder").string(), reflection) ||
            !session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() ||
            !session.beginPlaying(false))
            return fail(name, "valid generation C could not recover from B's bounded cleanup");
        session.stop();
        if (session.loader().isLoaded() || ledger.released_resource_count != 3 ||
            ledger.removed_registration_count != 3)
            return fail(name, "recovery did not release each generation exactly once");
        return pass(name);
    }

}

int main()
{
    for (const std::string& fixture : {std::string("client_recorder"), std::string("server_recorder"),
                                       std::string("client_init_fails"), std::string("server_init_fails"),
                                       std::string("owned_teardown_recorder"), std::string("owned_teardown_init_fails")})
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
    ok = testOwnedTeardownAndNoPostUnloadDispatch() && ok;
    ok = testPostTeardownInitFailureRecovery() && ok;
    return ok ? 0 : 1;
}
