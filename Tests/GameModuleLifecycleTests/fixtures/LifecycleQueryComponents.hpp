#pragma once

#include "ECS/ComponentOperations.hpp"

struct EngineQueryComponent {
    int value = 0;
};

struct ModuleFixtureComponent {
    int value = 73;
    ~ModuleFixtureComponent() noexcept;
};

namespace garden::ecs {
template<> struct ComponentType<EngineQueryComponent> {
    static constexpr ComponentKey key{"tests.lifecycle.EngineQueryComponent", 1};
    static constexpr uint32_t capabilities = ComponentDefaultConstruct | ComponentCopyConstruct;
};
template<> struct ComponentType<ModuleFixtureComponent> {
    static constexpr ComponentKey key{"tests.lifecycle.ModuleFixtureComponent", 1};
    static constexpr uint32_t capabilities = ComponentDefaultConstruct;
};
}
