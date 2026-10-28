#pragma once

namespace
{
struct FrameworkCounts { int init = 0, start = 0, tick = 0, shutdown = 0; };

class LifecycleState final : public GameFramework::GameStateBase
{
public:
    explicit LifecycleState(FrameworkCounts& counts) : counts(counts) {}
    void initialize(world*) override { ++counts.init; }
    void handleBeginPlay() override { ++counts.start; }
    void tick(float) override { ++counts.tick; }
    void shutdown() override { ++counts.shutdown; }
private:
    FrameworkCounts& counts;
};

class LifecycleMode final : public GameFramework::GameModeBase
{
public:
    explicit LifecycleMode(FrameworkCounts& counts, bool fail_init = false) : counts(counts), fail_init(fail_init) {}
    void initGame(world&, const std::string&, const std::string&, std::string& error) override
    { ++counts.init; if (fail_init) error = "fixture error"; }
    void startPlay() override { ++counts.start; }
    void tick(float) override { ++counts.tick; }
private:
    FrameworkCounts& counts;
    bool fail_init;
};

bool testFrameworkRuntimeLifecycle()
{
    const std::string name = "Framework runtime lifecycle";
    FrameworkCounts mode_counts, state_counts;
    world w;
    if (w.initializeGameplayFramework() || w.isGameplayFrameworkInitialized())
        return fail(name, "empty framework initialized");
    w.setGameState(std::make_unique<LifecycleState>(state_counts));
    w.setAuthorityGameMode(std::make_unique<LifecycleMode>(mode_counts));
    w.startGameplayFramework();
    w.startGameplayFramework();
    w.tickGameplayFramework(0.1f);
    if (!w.isGameplayFrameworkInitialized() || !w.hasGameplayFrameworkStarted() ||
        mode_counts.init != 1 || mode_counts.start != 1 || mode_counts.tick != 1 ||
        state_counts.init || state_counts.start || state_counts.tick)
        return fail(name, "authority dispatch or idempotence changed");
    w.shutdownGameplayFramework();
    w.shutdownGameplayFramework();
    if (state_counts.shutdown != 1 || w.isGameplayFrameworkInitialized() || w.hasGameplayFrameworkStarted())
        return fail(name, "shutdown dispatch or flags changed");
    w.clearGameplayFramework();
    if (w.getAuthorityGameMode() || w.getGameState())
        return fail(name, "clear did not release framework objects");
    w.setGameState(std::make_unique<LifecycleState>(state_counts));
    w.tickGameplayFramework(0.1f);
    w.startGameplayFramework();
    if (state_counts.init != 1 || state_counts.tick != 1 || state_counts.start != 1 || !w.hasGameplayFrameworkStarted())
        return fail(name, "client state dispatch changed");
    w.clearGameplayFramework();
    if (state_counts.shutdown != 2)
        return fail(name, "clear did not shut down state");
    w.setAuthorityGameMode(std::make_unique<LifecycleMode>(mode_counts, true));
    w.startGameplayFramework();
    if (w.isGameplayFrameworkInitialized() || w.hasGameplayFrameworkStarted() || mode_counts.start != 1)
        return fail(name, "failed initialization started framework");
    return pass(name);
}

class ResetState final : public GameFramework::GameStateBase
{
public:
    explicit ResetState(int& shutdowns) : shutdowns(shutdowns) {}
    void shutdown() override { ++shutdowns; GameStateBase::shutdown(); }
private:
    int& shutdowns;
};

bool testFrameworkWorldResetLifecycle()
{
    const std::string name = "Framework world reset lifecycle";
    int shutdowns = 0;
    world w;
    world* const address = &w;
    const auto old_entity = w.createScopedEntity();
    w.setGameState(std::make_unique<ResetState>(shutdowns));
    w.startGameplayFramework();
    if (!w.hasGameplayFrameworkStarted())
        return fail(name, "framework did not start");
    w.resetWorld();
    if (&w != address || w.isEntityValid(old_entity) || w.getGameState() ||
        w.hasGameplayFrameworkStarted() || shutdowns != 1)
        return fail(name, "reset did not retire the prior world state");
    w.setGameState(std::make_unique<ResetState>(shutdowns));
    w.startGameplayFramework();
    if (!w.hasGameplayFrameworkStarted() || w.getGameState()->getWorld() != address)
        return fail(name, "framework did not restart at the original world address");
    w.clearGameplayFramework();
    if (shutdowns != 2)
        return fail(name, "restarted framework did not shut down");
    return pass(name);
}

}
