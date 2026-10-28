#include "Plugin/GameModuleHostSession.hpp"
#include "ECS/ComponentStore.hpp"
#include "ECS/ComponentOperations.hpp"
#include "world.hpp"
#include "../fixtures/LifecycleQueryComponents.hpp"
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
#else
#include <dlfcn.h>
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
        bool server_reload_safe_point = true;
        size_t connected_server_clients = 0;

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
                [this]() {
                    return GameModuleHostSession::ServerReloadState{
                        server_reload_safe_point, connected_server_clients};
                },
                [](const garden::ecs::ComponentOwnerId&) { return true; }, // fixture owns no asynchronous callbacks
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
            "host.release_resources", "host.remove:GameModuleLifecycleFixture"}) ||
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
            if (!session.stop()) return fail(name, "session stop refused");
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
            if (!session.stop()) return fail(name, "session stop refused");
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
        if (!session.stop()) return fail(name, "session stop refused");
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
        if (!session.stop()) return fail(name, "session stop refused");
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
                ? std::vector<std::string>{"register_components", "client.init", "client.init.rollback_callback"}
                : std::vector<std::string>{"register_components", "client.init", "server.init", "server.init.rollback_callback", "client.shutdown"};
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
        if (!recovery.stop()) return fail(name, "recovery stop refused");
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
        if (!session.stop()) return fail(name, "session stop refused");

        if (!equals(recorder.events(), {
                "register_components", "client.init", "client.init.rollback_callback",
                "register_components", "client.init", "client.level_loaded",
                "client.play_start", "client.play_stop", "client.shutdown"}))
            return fail(name, "fallback module did not receive a complete paired PIE lifecycle");
        if (!equals(ledger.events, {
                "host.quiesce", "host.release_callbacks:GameModuleLifecycleFixture",
                "host.release_resources", "host.remove:GameModuleLifecycleFixture",
                "host.quiesce", "host.release_callbacks:GameModuleLifecycleFixture",
                "host.release_resources", "host.remove:GameModuleLifecycleFixture"}))
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
        if (!session.stop()) return fail(name, "session stop refused");

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
            "host.release_resources", "host.remove:GameModuleLifecycleFixture"};
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
        if (!session.stop()) return fail(name, "session stop refused");
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
        if (!session.stop()) return fail(name, "session stop refused");
        if (session.loader().isLoaded() || ledger.released_resource_count != 3 ||
            ledger.removed_registration_count != 3)
            return fail(name, "recovery did not release each generation exactly once");
        return pass(name);
    }

    bool testContentIdentity()
    {
        const std::string name = "GameModuleLifecycle_ContentIdentity";
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path directory = fs::temp_directory_path() / ("garden_identity_" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(directory, ec);
        if (ec) return fail(name, "could not create temporary directory");
        struct Cleanup { fs::path path; ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); } } cleanup{directory};
        const fs::path copy = directory / fixturePath("generation_a").filename();
        fs::copy_file(fixturePath("generation_a"), copy, ec);
        if (ec) return fail(name, "could not copy fixture");

        GameModuleLoader first, second, different, rejected;
        if (first.getComponentOwnerId() || !first.load(fixturePath("generation_a").string()) ||
            !second.load(copy.string()) || !first.getComponentOwnerId() || !second.getComponentOwnerId())
            return fail(name, "duplicate images did not load with identity");
        const garden::ecs::ComponentOwnerId identity = *first.getComponentOwnerId();
        if (first.getLoadedImagePath() == second.getLoadedImagePath() ||
            identity != *second.getComponentOwnerId() ||
            identity.kind != garden::ecs::ComponentOwnerKind::Module ||
            identity.apiAbiVersion != GARDEN_MODULE_API_VERSION ||
            identity == garden::ecs::ComponentOwnerId::engineCore())
            return fail(name, "duplicate owned images or accepted ABI differ");

        garden::ecs::ComponentOwnerId other_abi = identity;
        ++other_abi.apiAbiVersion;
        if (identity == other_abi) return fail(name, "ABI is absent from identity comparison");

        for (size_t i = 0; i < identity.imageSha256.size(); ++i)
        {
            auto other_digest = identity;
            other_digest.imageSha256[i] ^= 1;
            if (identity == other_digest) return fail(name, "digest byte is absent from identity comparison");
        }

        // Appended overlay leaves the loadable image intact while changing its bytes.
        { std::ofstream out(copy, std::ios::binary | std::ios::app); out.put('\x5a'); if (!out) return fail(name, "could not modify fixture copy"); }
        if (!different.load(copy.string()) || !different.getComponentOwnerId() ||
            identity == *different.getComponentOwnerId())
            return fail(name, "different image bytes did not change identity");
        if (rejected.load(fixturePath("candidate_abi_mismatch").string()) || rejected.getComponentOwnerId() ||
            rejected.load(fixturePath("candidate_missing_game_update").string()) || rejected.getComponentOwnerId() ||
            rejected.load((directory / "missing.dll").string()) || rejected.getComponentOwnerId())
            return fail(name, "rejected candidate exposed an identity");

        GameModuleLoader moved(std::move(second));
        if (second.getComponentOwnerId() || !moved.getComponentOwnerId() || *moved.getComponentOwnerId() != identity)
            return fail(name, "identity did not follow image ownership on move");
        GameModuleLoader assigned;
        if (!assigned.replaceWith(std::move(moved))) return fail(name, "move replacement refused");
        if (moved.getComponentOwnerId() || !assigned.getComponentOwnerId() || *assigned.getComponentOwnerId() != identity)
            return fail(name, "identity did not follow image ownership on move assignment");
        if (first.unload()) return fail(name, "provider unloaded while borrowed");
        if (!assigned.unload() || !first.unload()) return fail(name, "retry after borrower release failed");
        if (first.getComponentOwnerId()) return fail(name, "unloaded image retained an identity");
        if (!different.unload()) return fail(name, "different provider cleanup refused");
        return pass(name);
    }

    bool testModuleComponentRegistration()
    {
        using namespace garden::ecs;
        const std::string name = "GameModuleLifecycle_RealModuleComponentRegistration";
        ScopedRecorder recorder("ecs_pending_borrower");
        GameModuleLoader provider, borrower;
        if (!provider.load(fixturePath("ecs_provider").string()) ||
            !borrower.load(fixturePath("ecs_provider").string()))
            return fail(name, "provider or pending borrower load failed");
        if (borrower.registerEcsComponents() || borrower.init(nullptr) || !recorder.events().empty())
            return fail(name, "pending borrower activated or dispatched a callback");
        if (!provider.registerEcsComponents() || !borrower.registerEcsComponents())
            return fail(name, "provider registration did not make the borrower ready");
        const auto owner = *provider.getComponentOwnerId();
        garden::ecs::ComponentDescriptor query{{"tests.lifecycle.ModuleFixtureComponent", 1}, owner,
            sizeof(int), alignof(int), ComponentDefaultConstruct, {}};
        ComponentId id;
        auto& catalog = ComponentCatalog::instance();
        if (catalog.resolve(query, id) != ComponentCatalogResult::Success)
            return fail(name, "registered module descriptor did not resolve");
        ComponentLease lease;
        if (catalog.acquire(id, ComponentPinKind::Operation, lease) != ComponentCatalogResult::Success ||
            !lease.descriptor() || lease.descriptor()->ownerId != owner ||
            !lease.descriptor()->operations.defaultConstruct || !lease.descriptor()->operations.destroy)
            return fail(name, "module operation lease or injected owner missing");
        alignas(int) unsigned char storage[sizeof(int)];
        if (!lease.descriptor()->operations.defaultConstruct(storage) || *reinterpret_cast<int*>(storage) != 73)
            return fail(name, "provider operation did not construct its value");
        lease.descriptor()->operations.destroy(storage);
        const auto image = provider.getLoadedImagePath();
        if (!borrower.unload() || provider.unload() || !fs::exists(image))
            return fail(name, "provider image retired with an outstanding operation lease");
        lease.reset();
        if (!provider.unload() || fs::exists(image) ||
            catalog.resolve(query, id) != ComponentCatalogResult::NotFound)
            return fail(name, "provider did not retire its descriptor after lease release");
        return pass(name);
    }

    bool testCrossDllMixedQuery()
    {
        using namespace garden::ecs;
        const std::string name = "GameModuleLifecycle_CrossDllMixedQuery";
        ScopedRecorder recorder("ecs_mixed_query");
        auto& catalog = ComponentCatalog::instance();
        garden::ecs::ComponentDescriptor engineDescriptor{ComponentType<EngineQueryComponent>::key,
            ComponentOwnerId::engineCore(), sizeof(EngineQueryComponent), alignof(EngineQueryComponent),
            componentCapabilities<EngineQueryComponent>(), makeComponentOperations<EngineQueryComponent>()};
        ComponentId engineId{};
        if (catalog.registerComponent(engineDescriptor, engineId) != ComponentCatalogResult::Success)
            return fail(name, "engine component registration failed");

        GameModuleLoader provider;
        if (!provider.load(fixturePath("ecs_provider").string()) ||
            !provider.registerEcsComponents() || !provider.isComponentProvider())
            return fail(name, "provider load or registration failed");
        const fs::path image = provider.getLoadedImagePath();
        const garden::ecs::ComponentDescriptor moduleDescriptor{ComponentType<ModuleFixtureComponent>::key,
            *provider.getComponentOwnerId(), sizeof(ModuleFixtureComponent), alignof(ModuleFixtureComponent),
            ComponentDefaultConstruct, {}};
        ComponentId moduleId{};
        if (catalog.resolve(moduleDescriptor, moduleId) != ComponentCatalogResult::Success)
            return fail(name, "module descriptor did not resolve");

        // Declared after the loader so every failure destroys values before the DLL is retired.
        {
            world w;
            const auto both = w.createScopedEntity();
            const auto engineOnly = w.createScopedEntity();
            if (!w.isEntityValid(both) || !w.isEntityValid(engineOnly) ||
                w.add<ModuleFixtureComponent>(both) != ComponentStoreResult::Success ||
                w.add<EngineQueryComponent>(both, EngineQueryComponent{12}) != ComponentStoreResult::Success ||
                w.add<EngineQueryComponent>(engineOnly, EngineQueryComponent{99}) != ComponentStoreResult::Success)
                return fail(name, "could not create query values");

#ifdef _WIN32
            HMODULE handle = LoadLibraryA(image.string().c_str());
            if (!handle) return fail(name, "could not find provider fixture image");
            auto query = reinterpret_cast<bool(*)(world*, EntityHandle)>(GetProcAddress(handle, "fixtureQueryMixed"));
            FreeLibrary(handle);
#else
            void* handle = dlopen(image.string().c_str(), RTLD_NOW);
            if (!handle) return fail(name, "could not find provider fixture image");
            auto query = reinterpret_cast<bool(*)(world*, EntityHandle)>(dlsym(handle, "fixtureQueryMixed"));
            dlclose(handle);
#endif
            EngineQueryComponent* engine = nullptr;
            if (!query || !query(&w, both) ||
                w.get<EngineQueryComponent>(both, engine) != ComponentStoreResult::Success ||
                !engine || engine->value != 85)
                return fail(name, "provider mixed query did not join the registered values");
            if (provider.unload() || !provider.isLoaded() || !fs::exists(image) || !recorder.events().empty())
                return fail(name, "provider retired while the module value was live");
            if (w.destroyEntity(both) != ComponentStoreResult::Success ||
                !equals(recorder.events(), {"ecs.component.destroy"}) || !provider.isLoaded() || !fs::exists(image))
                return fail(name, "entity destruction did not invoke the module destructor while loaded");
        }
        if (!provider.unload() || provider.isLoaded() || fs::exists(image) ||
            catalog.resolve(moduleDescriptor, moduleId) != ComponentCatalogResult::NotFound)
            return fail(name, "provider did not retire after entity destruction");
        return pass(name);
    }

    bool testStoredModuleComponentRetirement()
    {
        using namespace garden::ecs;
        const std::string name = "GameModuleLifecycle_StoredModuleComponentRetirement";
        for (bool explicitRemoval : {true, false})
        {
            ScopedRecorder recorder(explicitRemoval ? "ecs_store_remove" : "ecs_store_destroy");
            GameModuleLoader provider;
            if (!provider.load(fixturePath("ecs_provider").string()) ||
                !provider.registerEcsComponents() || !provider.isComponentProvider())
                return fail(name, "provider load or registration failed");
            const fs::path image = provider.getLoadedImagePath();
            const garden::ecs::ComponentDescriptor query{{"tests.lifecycle.ModuleFixtureComponent", 1},
                *provider.getComponentOwnerId(), sizeof(int), alignof(int), ComponentDefaultConstruct, {}};
            ComponentId id;
            auto& catalog = ComponentCatalog::instance();
            if (catalog.resolve(query, id) != ComponentCatalogResult::Success)
                return fail(name, "module descriptor did not resolve");
#ifdef _WIN32
            HMODULE handle = LoadLibraryA(image.string().c_str());
            if (!handle) return fail(name, "could not find provider fixture image");
            auto add = reinterpret_cast<ComponentStoreResult(*)(ComponentStore*, ComponentId, ComponentEntityKey)>(
                GetProcAddress(handle, "fixtureStoreAdd"));
            FreeLibrary(handle);
#else
            void* handle = dlopen(image.string().c_str(), RTLD_NOW);
            if (!handle) return fail(name, "could not find provider fixture image");
            auto add = reinterpret_cast<ComponentStoreResult(*)(ComponentStore*, ComponentId, ComponentEntityKey)>(
                dlsym(handle, "fixtureStoreAdd"));
            dlclose(handle);
#endif
            if (!add) return fail(name, "fixture store insertion export missing");
            const ComponentEntityKey entity{42, 1};
            {
                ComponentStore store;
                void* value = nullptr;
                if (add(&store, id, entity) != ComponentStoreResult::Success ||
                    store.get(id, entity, value) != ComponentStoreResult::Success ||
                    !value || *static_cast<int*>(value) != 73)
                    return fail(name, "module value was not constructed in the EngineCore store");
                if (provider.unload() || !provider.isLoaded() || !fs::exists(image) ||
                    !recorder.events().empty())
                    return fail(name, "live store value did not retain the provider image");
                if (explicitRemoval && store.removeEntity(entity) != ComponentStoreResult::Success)
                    return fail(name, "explicit entity removal failed after unload refusal");
                if (explicitRemoval && !equals(recorder.events(), {"ecs.component.destroy"}))
                    return fail(name, "explicit removal did not run the module destructor");
            }
            if (!provider.isLoaded() || !fs::exists(image) ||
                !equals(recorder.events(), {"ecs.component.destroy"}))
                return fail(name, "module destructor did not run once while the provider remained loaded");
            if (!provider.unload() || provider.isLoaded() || fs::exists(image) ||
                catalog.resolve(query, id) != ComponentCatalogResult::NotFound)
                return fail(name, "provider did not retire after the stored value was destroyed");
        }
        return pass(name);
    }

    bool testModuleComponentBatchFailures()
    {
        using namespace garden::ecs;
        const std::string name = "GameModuleLifecycle_ModuleComponentBatchRollback";
        auto& catalog = ComponentCatalog::instance();
        for (const char* fixture : {"ecs_bad_batch", "ecs_export_false", "ecs_sticky_failure"})
        {
            GameModuleLoader loader;
            if (!loader.load(fixturePath(fixture).string()))
                return fail(name, std::string("fixture load failed: ") + fixture);
            garden::ecs::ComponentDescriptor query{{"tests.lifecycle.ModuleFixtureComponent", 1}, *loader.getComponentOwnerId(),
                sizeof(int), alignof(int), ComponentDefaultConstruct, {}};
            ComponentId id;
            if (loader.registerEcsComponents() || catalog.resolve(query, id) != ComponentCatalogResult::NotFound)
                return fail(name, std::string("failed export published a partial batch: ") + fixture);
            GameModuleLoader rejectedBorrower;
            if (rejectedBorrower.load(fixturePath(fixture).string()) || rejectedBorrower.isLoaded())
                return fail(name, std::string("failed generation accepted a borrower: ") + fixture);
            if (!loader.unload()) return fail(name, std::string("failed fixture did not unload: ") + fixture);
        }
        return pass(name);
    }

    bool testProviderRetirement()
    {
        using namespace garden::ecs;
        const std::string name = "GameModuleLifecycle_ProviderRetirement";
        GameModuleLoader provider, borrower, replacement;
        if (!provider.load(fixturePath("generation_a").string()) ||
            !borrower.load(fixturePath("generation_a").string()) ||
            !provider.isComponentProvider() || borrower.isComponentProvider())
            return fail(name, "provider and borrower were not distinguished");
        const auto owner = *provider.getComponentOwnerId();
        const auto image = provider.getLoadedImagePath();
#ifdef _WIN32
        HMODULE imageHandle = LoadLibraryA(image.c_str());
        if (!imageHandle) return fail(name, "could not acquire provider fixture exports");
        auto construct = reinterpret_cast<bool(*)(void*) noexcept>(GetProcAddress(imageHandle, "fixtureConstruct"));
        auto destroy = reinterpret_cast<void(*)(void*) noexcept>(GetProcAddress(imageHandle, "fixtureDestroy"));
        FreeLibrary(imageHandle);
#else
        void* imageHandle = dlopen(image.c_str(), RTLD_NOW);
        if (!imageHandle) return fail(name, "could not acquire provider fixture exports");
        auto construct = reinterpret_cast<bool(*)(void*) noexcept>(dlsym(imageHandle, "fixtureConstruct"));
        auto destroy = reinterpret_cast<void(*)(void*) noexcept>(dlsym(imageHandle, "fixtureDestroy"));
        dlclose(imageHandle);
#endif
        if (!construct || !destroy) return fail(name, "fixture operation exports missing");
        garden::ecs::ComponentDescriptor descriptor{{"fixture.provider", 1}, owner, sizeof(int), alignof(int),
            ComponentDefaultConstruct, {construct, destroy, nullptr, nullptr}};
        auto& catalog = ComponentCatalog::instance();
        ComponentId id;
        if (catalog.registerComponent(descriptor, id) != ComponentCatalogResult::Success)
            return fail(name, "provider descriptor was not registered");
        ComponentLease operation, instance;
        if (catalog.acquire(id, ComponentPinKind::Operation, operation) != ComponentCatalogResult::Success ||
            catalog.acquire(id, ComponentPinKind::LiveInstance, instance) != ComponentCatalogResult::Success)
            return fail(name, "could not pin provider callbacks");
        int value = 0;
        if (!operation.descriptor()->operations.defaultConstruct(&value) || value != 73)
            return fail(name, "provider callback did not execute");
        if (provider.unload() || !fs::exists(image) || replacement.load(fixturePath("generation_a").string()))
            return fail(name, "retiring provider accepted unload or new attachment");
        ComponentId blocked;
        if (catalog.registerComponent(descriptor, blocked) != ComponentCatalogResult::InUse)
            return fail(name, "retirement did not block new registration");
        if (!borrower.unload() || provider.unload()) return fail(name, "leases did not prevent retirement");
        operation.descriptor()->operations.destroy(&value);
        operation.reset();
        if (provider.unload()) return fail(name, "live instance did not prevent retirement");
        instance.reset();
        if (!provider.unload() || fs::exists(image) ||
            catalog.acquire(id, ComponentPinKind::Operation, operation) != ComponentCatalogResult::StaleId)
            return fail(name, "retry failed to retire catalog before releasing provider");
        return pass(name);
    }

    void throwCleanupForTest() { throw 17; }

    bool testCleanupExceptionAfterUnload()
    {
        const std::string name = "GameModuleLifecycle_CleanupExceptionAfterUnload";
        GameModuleLoader provider, borrower;
        if (!provider.load(fixturePath("generation_a").string()) ||
            !borrower.load(fixturePath("generation_a").string())) return fail(name, "load failed");
        const auto providerRoot = provider.getOwnedGenerationRoot();
        const auto borrowerRoot = borrower.getOwnedGenerationRoot();
        struct Restore {
            std::string providerRoot, borrowerRoot;
            ~Restore() {
                GameModuleLoader::setCleanupTestHook(nullptr);
                std::error_code ec;
                fs::remove_all(providerRoot, ec);
                fs::remove_all(borrowerRoot, ec);
            }
        } restore{providerRoot, borrowerRoot};
        GameModuleLoader::setCleanupTestHook(throwCleanupForTest);
        if (!borrower.unload() || borrower.isLoaded() || !borrower.unload())
            return fail(name, "cleanup exception left borrower attached or unload retry unsafe");
        if (!provider.unload() || provider.isLoaded() || !provider.unload())
            return fail(name, "cleanup exception left provider attached or unload retry unsafe");
        GameModuleLoader::setCleanupTestHook(nullptr);
        GameModuleLoader replacement;
        if (!replacement.load(fixturePath("generation_a").string()) || !replacement.isComponentProvider() ||
            !replacement.unload()) return fail(name, "old generation bookkeeping survived cleanup exception");
        return pass(name);
    }

    bool testOrphanedProviderRetry()
    {
        using namespace garden::ecs;
        const std::string name = "GameModuleLifecycle_OrphanedProviderRetry";
        GameModuleLoader borrower;
        std::string image;
        {
            GameModuleLoader provider;
            if (!provider.load(fixturePath("generation_a").string()) ||
                !borrower.load(fixturePath("generation_a").string())) return fail(name, "load failed");
            image = provider.getLoadedImagePath();
            // This fixture has no host resources; explicit unload asserts cleanup.
            if (provider.unload()) return fail(name, "provider retired while borrowed");
        }
        if (!fs::exists(image) || !borrower.unload() || !fs::exists(image))
            return fail(name, "orphan provider image was not retained");
        // This fixture has no host-owned resources. Real hosts must drain them first.
        if (!GameModuleLoader::retryRetiredGenerations() || fs::exists(image))
            return fail(name, "orphan retry did not release provider");
        return pass(name);
    }

    bool testOrphanedPinnedProviderRetry()
    {
        using namespace garden::ecs;
        const std::string name = "GameModuleLifecycle_OrphanedPinnedProviderRetry";
        ComponentLease lease;
        std::string image;
        {
            GameModuleLoader provider;
            if (!provider.load(fixturePath("generation_a").string())) return fail(name, "load failed");
            image = provider.getLoadedImagePath();
            garden::ecs::ComponentDescriptor descriptor{{"fixture.orphan.pin", 1}, *provider.getComponentOwnerId(),
                sizeof(int), alignof(int), 0, {nullptr, +[](void*) noexcept {}, nullptr, nullptr}};
            ComponentId id;
            auto& catalog = ComponentCatalog::instance();
            if (catalog.registerComponent(descriptor, id) != ComponentCatalogResult::Success ||
                catalog.acquire(id, ComponentPinKind::LiveInstance, lease) != ComponentCatalogResult::Success ||
                provider.unload()) return fail(name, "pin did not refuse explicit retirement");
        }
        if (!fs::exists(image) || GameModuleLoader::retryRetiredGenerations())
            return fail(name, "orphan retry ignored a live lease");
        lease.reset();
        if (!GameModuleLoader::retryRetiredGenerations() || fs::exists(image))
            return fail(name, "orphan did not retire after lease release");
        return pass(name);
    }

    void testDestroy(void*) noexcept {}

    bool testInvalidSourceRetiresLoadedImage()
    {
        const std::string name = "GameModuleLifecycle_InvalidSourceRetiresImage";
        HostLedger ledger;
        ReflectionRegistry reflection;
        GameModuleHostSession session(ledger.operations());
#ifdef _WIN32
        _putenv_s("GARDEN_TEST_INVALID_MODULE_NAME", "1");
#else
        setenv("GARDEN_TEST_INVALID_MODULE_NAME", "1", 1);
#endif
        const bool loaded = session.loadAndRegister(fixturePath("generation_a").string(), reflection);
#ifdef _WIN32
        _putenv_s("GARDEN_TEST_INVALID_MODULE_NAME", "");
#else
        unsetenv("GARDEN_TEST_INVALID_MODULE_NAME");
#endif
        if (loaded || session.loader().isLoaded() || session.phase() != GameModuleHostSession::Phase::Idle)
            return fail(name, "failed source validation retained a loaded image");
        return pass(name);
    }

    bool testHostRetirementRetry()
    {
        using namespace garden::ecs;
        const std::string name = "GameModuleLifecycle_HostRetirementRetry";
        ScopedRecorder recorder("host_retirement");
        HostLedger ledger;
        ReflectionRegistry reflection;
        GameModuleHostSession session(ledger.operations());
        GameModuleLoader borrower;
        if (!session.loadAndRegister(fixturePath("generation_a").string(), reflection) ||
            !session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() ||
            !session.beginPlaying(false) || !borrower.load(fixturePath("generation_a").string()))
            return fail(name, "sessions did not start");
        borrower.registerComponents(&reflection);
        if (!equals(recorder.events(), {"A.register_components", "A.client.init", "A.client.level_loaded"}))
            return fail(name, "borrower registered reflection callbacks");
        garden::ecs::ComponentDescriptor descriptor{{"fixture.host", 1}, *session.loader().getComponentOwnerId(),
            sizeof(int), alignof(int), 0, {nullptr, testDestroy, nullptr, nullptr}};
        ComponentId id;
        auto& catalog = ComponentCatalog::instance();
        ComponentLease lease;
        if (catalog.registerComponent(descriptor, id) != ComponentCatalogResult::Success ||
            catalog.acquire(id, ComponentPinKind::LiveInstance, lease) != ComponentCatalogResult::Success)
            return fail(name, "could not pin host owner");
        if (session.stop() || session.phase() != GameModuleHostSession::Phase::Playing || !ledger.events.empty())
            return fail(name, "borrower refusal performed teardown");
        if (!borrower.unload() || session.stop() || session.phase() != GameModuleHostSession::Phase::Teardown)
            return fail(name, "lease refusal did not retain teardown state");
        const auto events = recorder.events();
        const auto hostEvents = ledger.events;
        lease.reset();
        if (!session.stop() || recorder.events() != events || ledger.events != hostEvents ||
            !expectLedger(name, ledger)) return fail(name, "retry repeated teardown or did not finish");
        return pass(name);
    }

    bool testHostCallbackProofRefusal()
    {
        const std::string name = "GameModuleLifecycle_HostCallbackProof";
        ScopedRecorder recorder("callback_proof");
        HostLedger ledger;
        ReflectionRegistry reflection;
        auto operations = ledger.operations();
        bool drained = false;
        operations.module_callbacks_drained = [&](const garden::ecs::ComponentOwnerId&) { return drained; };
        GameModuleHostSession session(std::move(operations));
        if (!session.loadAndRegister(fixturePath("generation_a").string(), reflection) ||
            !session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() || !session.beginPlaying(false))
            return fail(name, "session did not start");
        if (session.stop() || session.phase() != GameModuleHostSession::Phase::Teardown ||
            ledger.events != std::vector<std::string>{"host.quiesce", "host.release_callbacks:GameModuleLifecycleFixture", "host.release_resources"} ||
            !session.loader().isLoaded()) return fail(name, "unproven callbacks did not refuse safely");
        drained = true;
        if (!session.stop() || !expectLedger(name, ledger)) return fail(name, "drained retry failed or repeated quiesce");
        return pass(name);
    }

    bool testDestructorQuarantinesUnprovenCallbacks()
    {
        const std::string name = "GameModuleLifecycle_DestructorQuarantine";
        const auto directory = fs::temp_directory_path() / ("garden_quarantine_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::create_directories(directory, ec);
        const auto source = directory / fixturePath("generation_a").filename();
        fs::copy_file(fixturePath("generation_a"), source, ec);
        if (ec) return fail(name, "could not copy fixture");
        { std::ofstream out(source, std::ios::binary | std::ios::app); out.put('Q'); }
        HostLedger ledger;
        ReflectionRegistry reflection;
        auto operations = ledger.operations();
        operations.module_callbacks_drained = [](const garden::ecs::ComponentOwnerId&) { return false; };
        std::string image;
        bool (*construct)(void*) noexcept = nullptr;
        {
            GameModuleHostSession session(std::move(operations));
            if (!session.loadAndRegister(source.string(), reflection) ||
                !session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() || !session.beginPlaying(false))
                return fail(name, "fixture did not start");
            image = session.loader().getLoadedImagePath();
#ifdef _WIN32
            auto handle = LoadLibraryA(image.c_str());
            if (!handle) return fail(name, "could not inspect fixture");
            construct = reinterpret_cast<bool(*)(void*) noexcept>(GetProcAddress(handle, "fixtureConstruct"));
            FreeLibrary(handle);
#else
            auto handle = dlopen(image.c_str(), RTLD_NOW);
            if (!handle) return fail(name, "could not inspect fixture");
            construct = reinterpret_cast<bool(*)(void*) noexcept>(dlsym(handle, "fixtureConstruct"));
            dlclose(handle);
#endif
            if (!construct || session.stop()) return fail(name, "host cleanup proof unexpectedly passed");
        }
        int value = 0;
        if (!fs::exists(image) || !construct(&value) || value != 73 ||
            GameModuleLoader::retryRetiredGenerations() || !fs::exists(image))
            return fail(name, "destructor unmapped or made unproven cleanup retryable");
        fs::remove_all(directory, ec);
        return pass(name);
    }

    bool testModuleEventBusRefusesUnload()
    {
        const std::string name = "GameModuleLifecycle_ModuleEventBusRefusesUnload";
        ScopedRecorder recorder("module_event");
        const auto directory = fs::temp_directory_path() / ("garden_module_event_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::create_directories(directory, ec);
        const auto source = directory / fixturePath("generation_a").filename();
        fs::copy_file(fixturePath("generation_a"), source, ec);
        if (ec) return fail(name, "could not copy fixture");
        { std::ofstream out(source, std::ios::binary | std::ios::app); out.put('E'); }
        std::string image;
        void (*fire)() = nullptr;
        {
            GameModuleLoader loader;
            if (!loader.load(source.string())) return fail(name, "fixture load failed");
            image = loader.getLoadedImagePath();
#ifdef _WIN32
            auto handle = LoadLibraryA(image.c_str());
            if (!handle) return fail(name, "could not inspect fixture");
            fire = reinterpret_cast<void(*)()>(GetProcAddress(handle, "fixtureFireEvent"));
            FreeLibrary(handle);
            _putenv_s("GARDEN_TEST_MODULE_EVENT", "1");
#else
            auto handle = dlopen(image.c_str(), RTLD_NOW);
            if (!handle) return fail(name, "could not inspect fixture");
            fire = reinterpret_cast<void(*)()>(dlsym(handle, "fixtureFireEvent"));
            dlclose(handle);
            setenv("GARDEN_TEST_MODULE_EVENT", "1", 1);
#endif
            const bool initialized = loader.registerEcsComponents() && loader.init(nullptr);
#ifdef _WIN32
            _putenv_s("GARDEN_TEST_MODULE_EVENT", "");
#else
            unsetenv("GARDEN_TEST_MODULE_EVENT");
#endif
            if (!fire || !initialized || loader.unload())
                return fail(name, "module EventBus registration did not refuse unload");
        }
        fire();
        const auto events = recorder.events();
        if (!fs::exists(image) || events.empty() || events.back() != "A.event.callback" ||
            GameModuleLoader::retryRetiredGenerations())
            return fail(name, "event callback or image did not survive refused retirement");
        fs::remove_all(directory, ec);
        return pass(name);
    }

    bool testRejectedCandidateEventBusRetention()
    {
        const std::string name = "GameModuleLifecycle_RejectedCandidateEventBusRetention";
        ScopedRecorder recorder("rejected_event");
        GameModuleLoader loader;
#ifdef _WIN32
        _putenv_s("GARDEN_TEST_MODULE_METADATA_EVENT", "1");
#else
        setenv("GARDEN_TEST_MODULE_METADATA_EVENT", "1", 1);
#endif
        const bool accepted = loader.load(fixturePath("candidate_abi_mismatch").string());
#ifdef _WIN32
        _putenv_s("GARDEN_TEST_MODULE_METADATA_EVENT", "");
#else
        unsetenv("GARDEN_TEST_MODULE_METADATA_EVENT");
#endif
        if (accepted || loader.isLoaded()) return fail(name, "ABI mismatch was accepted");
        std::error_code ec;
        fs::path image;
        for (fs::recursive_directory_iterator it(loader.getOwnedGenerationRoot(), ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == fixturePath("candidate_abi_mismatch").extension()) image = it->path();
        if (ec || image.empty()) return fail(name, "hazardous rejected candidate was not retained");
#ifdef _WIN32
        auto handle = LoadLibraryA(image.string().c_str());
        if (!handle) return fail(name, "retained image was not accessible");
        auto fire = reinterpret_cast<void(*)()>(GetProcAddress(handle, "fixtureFireEvent"));
        FreeLibrary(handle);
#else
        auto handle = dlopen(image.string().c_str(), RTLD_NOW);
        if (!handle) return fail(name, "retained image was not accessible");
        auto fire = reinterpret_cast<void(*)()>(dlsym(handle, "fixtureFireEvent"));
        dlclose(handle);
#endif
        if (!fire) return fail(name, "retained event export missing");
        fire();
        const auto events = recorder.events();
        if (std::find(events.begin(), events.end(), "B.event.callback") == events.end())
            return fail(name, "retained event listener was not callable");
        return pass(name);
    }

    bool testBareLoaderDestructorQuarantines()
    {
        const std::string name = "GameModuleLifecycle_BareLoaderDestructorQuarantine";
        const auto directory = fs::temp_directory_path() / ("garden_bare_quarantine_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        fs::create_directories(directory, ec);
        const auto source = directory / fixturePath("generation_a").filename();
        fs::copy_file(fixturePath("generation_a"), source, ec);
        if (ec) return fail(name, "could not copy fixture");
        { std::ofstream out(source, std::ios::binary | std::ios::app); out.put('B'); }
        std::string image;
        bool (*construct)(void*) noexcept = nullptr;
        {
            GameModuleLoader loader;
            if (!loader.load(source.string())) return fail(name, "fixture load failed");
            image = loader.getLoadedImagePath();
#ifdef _WIN32
            auto handle = LoadLibraryA(image.c_str());
            if (!handle) return fail(name, "could not inspect fixture");
            construct = reinterpret_cast<bool(*)(void*) noexcept>(GetProcAddress(handle, "fixtureConstruct"));
            FreeLibrary(handle);
#else
            auto handle = dlopen(image.c_str(), RTLD_NOW);
            if (!handle) return fail(name, "could not inspect fixture");
            construct = reinterpret_cast<bool(*)(void*) noexcept>(dlsym(handle, "fixtureConstruct"));
            dlclose(handle);
#endif
            if (!construct) return fail(name, "missing fixture operation");
        }
        int value = 0;
        if (!fs::exists(image) || !construct(&value) || value != 73 ||
            GameModuleLoader::retryRetiredGenerations() || !fs::exists(image))
            return fail(name, "bare loader destructor did not quarantine the image");
        fs::remove_all(directory, ec);
        return pass(name);
    }

    bool testSameOwnerReloadRefusal()
    {
        const std::string name = "GameModuleLifecycle_SameOwnerReloadRefusal";
        ScopedRecorder recorder("same_owner_reload");
        HostLedger ledger;
        ReflectionRegistry reflection;
        GameModuleHostSession session(ledger.operations());
        if (!session.loadAndRegister(fixturePath("generation_a").string(), reflection) ||
            !session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() || !session.beginPlaying(false))
            return fail(name, "session did not start");
        const auto image = session.loader().getLoadedImagePath();
        if (session.reloadCandidate(fixturePath("generation_a").string(), reflection, nullptr) ||
            session.phase() != GameModuleHostSession::Phase::Playing ||
            session.loader().getLoadedImagePath() != image || !ledger.events.empty())
            return fail(name, "same-owner reload promoted or tore down the provider");
        if (!session.stop()) return fail(name, "provider did not retire after candidate release");
        return pass(name);
    }

    bool testMoveReplacementRefusal()
    {
        const std::string name = "GameModuleLifecycle_MoveReplacementRefusal";
        GameModuleLoader provider, borrower, incoming;
        if (!provider.load(fixturePath("generation_a").string()) ||
            !borrower.load(fixturePath("generation_a").string()) ||
            !incoming.load(fixturePath("generation_b").string())) return fail(name, "load failed");
        const auto image = provider.getLoadedImagePath();
        const auto incomingImage = incoming.getLoadedImagePath();
        if (provider.replaceWith(std::move(incoming)) || provider.getLoadedImagePath() != image ||
            incoming.getLoadedImagePath() != incomingImage || !fs::exists(image))
            return fail(name, "busy replacement lost either image");
        if (!borrower.unload() || !provider.replaceWith(std::move(incoming)) ||
            provider.getLoadedImagePath() != incomingImage || !provider.unload())
            return fail(name, "replacement did not succeed after borrower release");
        return pass(name);
    }

    bool testCandidatePromotion()
    {
        const std::string name = "GameModuleLifecycle_L6_CandidatePromotion";
        ScopedRecorder recorder("candidate_promotion");
        HostLedger ledger;
        ReflectionRegistry reflection;
        GameModuleHostSession session(ledger.operations());

        if (!session.loadAndRegister(fixturePath("generation_a").string(), reflection) ||
            !session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() ||
            !session.beginPlaying(false))
            return fail(name, "generation A did not reach Playing");

        const fs::path generation_a_image = session.loader().getLoadedImagePath();
        session.updateClient(0.016f);
        if (!session.reloadCandidate(fixturePath("generation_b").string(), reflection, nullptr))
            return fail(name, "validated generation B did not promote");
        const fs::path generation_b_image = session.loader().getLoadedImagePath();
        if (session.phase() != GameModuleHostSession::Phase::Playing ||
            generation_a_image == generation_b_image || fs::exists(generation_a_image) ||
            !fs::exists(generation_b_image))
            return fail(name, "candidate promotion did not atomically exchange the active generation");

        session.updateClient(0.016f);
        if (!session.stop()) return fail(name, "session stop refused");
        if (!equals(recorder.events(), {
                "A.register_components", "A.client.init", "A.client.level_loaded", "A.client.update",
                "A.client.shutdown", "B.register_components", "B.client.init", "B.client.level_loaded",
                "B.client.update", "B.client.shutdown"}))
            return fail(name, "candidate B received a callback before A teardown or lifecycle registration was duplicated");
        if (ledger.released_resource_count != 2 || ledger.removed_registration_count != 2 ||
            session.loader().isLoaded())
            return fail(name, "candidate promotion did not clean each generation exactly once");
        return pass(name);
    }

    bool testCandidateRefusalKeepsActive()
    {
        const std::string name = "GameModuleLifecycle_L7_CandidateRefusalKeepsA";
        for (const std::string& rejected_candidate : {
                std::string("candidate_missing_game_update"), std::string("candidate_abi_mismatch")})
        {
            ScopedRecorder recorder(rejected_candidate);
            HostLedger ledger;
            ReflectionRegistry reflection;
            GameModuleHostSession session(ledger.operations());
            if (!session.loadAndRegister(fixturePath("generation_a").string(), reflection) ||
                !session.initialiseClient(nullptr) || !session.notifyClientLevelLoaded() ||
                !session.beginPlaying(false))
                return fail(name, "generation A did not reach Playing before " + rejected_candidate);

            const fs::path generation_a_image = session.loader().getLoadedImagePath();
            session.updateClient(0.016f);
            if (session.reloadCandidate(fixturePath(rejected_candidate).string(), reflection, nullptr))
                return fail(name, "rejected candidate was promoted: " + rejected_candidate);
            if (session.phase() != GameModuleHostSession::Phase::Playing || !session.loader().isLoaded() ||
                session.loader().getLoadedImagePath() != generation_a_image.string() || !fs::exists(generation_a_image))
                return fail(name, "candidate refusal changed active generation A: " + rejected_candidate);

            session.updateClient(0.016f);
            if (!session.stop()) return fail(name, "session stop refused");
            if (!equals(recorder.events(), {
                    "A.register_components", "A.client.init", "A.client.level_loaded", "A.client.update",
                    "A.client.update", "A.client.shutdown"}))
                return fail(name, "candidate refusal stopped A or dispatched candidate code: " + rejected_candidate);
            if (ledger.released_resource_count != 1 || ledger.removed_registration_count != 1)
                return fail(name, "candidate refusal tore down A before explicit stop: " + rejected_candidate);
        }
        return pass(name);
    }

    bool testConnectedServerReloadRefusalKeepsActive()
    {
        const std::string name = "GameModuleLifecycle_L10_ConnectedServerReloadRefusal";
        ScopedRecorder recorder("connected_server_reload");
        HostLedger ledger;
        ledger.connected_server_clients = 1;
        ReflectionRegistry reflection;
        GameModuleHostSession session(ledger.operations());

        if (!session.loadAndRegister(fixturePath("server_recorder").string(), reflection) ||
            !session.initialiseServer(nullptr) || !session.notifyServerLevelLoaded() ||
            !session.beginPlaying(false))
            return fail(name, "server did not reach Playing");

        const fs::path active_image = session.loader().getLoadedImagePath();
        session.updateServer(0.016f);
        if (session.reloadCandidate(fixturePath("server_recorder").string(), reflection, nullptr))
            return fail(name, "reload promoted while an authoritative client was connected");

        if (session.phase() != GameModuleHostSession::Phase::Playing || !session.loader().isLoaded() ||
            session.loader().getLoadedImagePath() != active_image.string() || !fs::exists(active_image) ||
            session.lastFailure().find("1 client(s) remain connected") == std::string::npos ||
            !ledger.events.empty())
            return fail(name, "connected-client refusal changed the active server or began teardown");

        session.updateServer(0.016f);
        if (!session.stop()) return fail(name, "session stop refused");
        if (!equals(recorder.events(), {
                "register_components", "server.init", "server.level_loaded", "server.update",
                "server.update", "server.shutdown"}))
            return fail(name, "active server was not callable after reload refusal");
        if (!expectLedger(name, ledger)) return false;
        return pass(name);
    }

}

int main()
{
    for (const std::string& fixture : {std::string("client_recorder"), std::string("server_recorder"),
                                       std::string("client_init_fails"), std::string("server_init_fails"),
                                       std::string("owned_teardown_recorder"), std::string("owned_teardown_init_fails"),
                                       std::string("generation_a"), std::string("generation_b"),
                                       std::string("candidate_missing_game_update"), std::string("candidate_abi_mismatch"),
                                       std::string("ecs_provider"), std::string("ecs_bad_batch"),
                                       std::string("ecs_export_false"), std::string("ecs_sticky_failure")})
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
    ok = testContentIdentity() && ok;
    ok = testModuleComponentRegistration() && ok;
    ok = testCrossDllMixedQuery() && ok;
    ok = testStoredModuleComponentRetirement() && ok;
    ok = testModuleComponentBatchFailures() && ok;
    ok = testProviderRetirement() && ok;
    ok = testCleanupExceptionAfterUnload() && ok;
    ok = testOrphanedProviderRetry() && ok;
    ok = testOrphanedPinnedProviderRetry() && ok;
    ok = testHostRetirementRetry() && ok;
    ok = testInvalidSourceRetiresLoadedImage() && ok;
    ok = testHostCallbackProofRefusal() && ok;
    ok = testSameOwnerReloadRefusal() && ok;
    ok = testMoveReplacementRefusal() && ok;
    ok = testCandidatePromotion() && ok;
    ok = testCandidateRefusalKeepsActive() && ok;
    ok = testConnectedServerReloadRefusalKeepsActive() && ok;
    ok = testDestructorQuarantinesUnprovenCallbacks() && ok;
    ok = testBareLoaderDestructorQuarantines() && ok;
    ok = testModuleEventBusRefusesUnload() && ok;
    ok = testRejectedCandidateEventBusRetention() && ok;
    return ok ? 0 : 1;
}
