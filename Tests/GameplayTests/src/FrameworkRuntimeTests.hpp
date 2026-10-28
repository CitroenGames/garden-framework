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

class MovingState final : public GameFramework::GameStateBase
{
public:
    explicit MovingState(int& shutdown_count) : shutdown_count(shutdown_count) {}
    void shutdown() override
    {
        ++shutdown_count;
        GameStateBase::shutdown();
    }
private:
    int& shutdown_count;
};

bool testFrameworkWorldMoveLifecycle()
{
    const std::string name = "Framework world move lifecycle";
    int source_shutdowns = 0;
    int destination_shutdowns = 0;
    int reused_shutdowns = 0;
    int assignment_source_shutdowns = 0;
    world source;
    auto mode = std::make_unique<GameFramework::GameModeBase>();
    auto* mode_ptr = mode.get();
    auto state = std::make_unique<MovingState>(source_shutdowns);
    auto* state_ptr = state.get();
    source.setGameState(std::move(state));
    source.setAuthorityGameMode(std::move(mode));
    if (!source.initializeGameplayFramework("move-map", "") )
        return fail(name, "source did not initialize");
    source.startGameplayFramework();
    state_ptr->tick(0.5f);
    world moved(std::move(source));
    if (moved.getAuthorityGameMode() != mode_ptr || moved.getGameState() != state_ptr ||
        mode_ptr->getWorld() != &moved || state_ptr->getWorld() != &moved ||
        !moved.isGameplayFrameworkInitialized() || !moved.hasGameplayFrameworkStarted() ||
        state_ptr->getServerWorldTimeSeconds() < 0.5)
        return fail(name, "move construction did not preserve and rebind state");
    source.startGameplayFramework();
    source.tickGameplayFramework(0.1f);
    if (source.initializeGameplayFramework() || source.getGameState() || source.getAuthorityGameMode())
        return fail(name, "moved-from framework was not empty");
    source.setGameState(std::make_unique<MovingState>(reused_shutdowns));
    source.startGameplayFramework();
    source.tickGameplayFramework(0.1f);
    if (!source.hasGameplayFrameworkStarted() || source.getGameState()->getWorld() != &source)
        return fail(name, "moved-from state could not be reused");
    source.clearGameplayFramework();
    source.setAuthorityGameMode(std::make_unique<GameFramework::GameModeBase>());
    if (!source.getAuthorityGameMode() || source.getAuthorityGameMode()->getWorld() != &source)
        return fail(name, "moved-from mode setter failed");
    source.clearGameplayFramework();

    world destination;
    destination.setGameState(std::make_unique<MovingState>(destination_shutdowns));
    destination.startGameplayFramework();
    destination = std::move(moved);
    if (destination_shutdowns != 1 || destination.getAuthorityGameMode() != mode_ptr ||
        destination.getGameState() != state_ptr || mode_ptr->getWorld() != &destination ||
        state_ptr->getWorld() != &destination || !destination.hasGameplayFrameworkStarted() ||
        source_shutdowns != 0)
        return fail(name, "move assignment lost state or skipped destination shutdown");
    moved.setGameState(std::make_unique<MovingState>(assignment_source_shutdowns));
    moved.startGameplayFramework();
    moved.tickGameplayFramework(0.1f);
    if (!moved.hasGameplayFrameworkStarted() || moved.getGameState()->getWorld() != &moved)
        return fail(name, "move-assigned source could not be reused");
    moved.clearGameplayFramework();
    destination.tickGameplayFramework(0.25f);
    if (state_ptr->getServerWorldTimeSeconds() < 0.75)
        return fail(name, "moved state did not continue ticking");
    destination = std::move(destination);
    if (destination.getGameState() != state_ptr || destination_shutdowns != 1)
        return fail(name, "self move assignment changed framework");
    destination.clearGameplayFramework();
    if (source_shutdowns != 1 || reused_shutdowns != 1 || assignment_source_shutdowns != 1)
        return fail(name, "framework shutdown count changed");
    return pass(name);
}

}
