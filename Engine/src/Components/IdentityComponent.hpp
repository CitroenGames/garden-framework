#pragma once

#include "EngineExport.h"
#include "Reflection/Reflector.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

// Stable 128-bit entity identity (entity_lifecycle.md §2). {0,0} = null.
struct EntityGuid
{
    uint64_t hi = 0;
    uint64_t lo = 0;

    bool isNull() const { return hi == 0 && lo == 0; }
    bool operator==(const EntityGuid& o) const { return hi == o.hi && lo == o.lo; }
    bool operator!=(const EntityGuid& o) const { return !(*this == o); }
};

struct GuidHash
{
    size_t operator()(const EntityGuid& g) const noexcept
    {
        // splitmix-style mix of both halves
        uint64_t x = g.hi + 0x9e3779b97f4a7c15ull + g.lo;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        return static_cast<size_t>(x ^ (x >> 31));
    }
};

inline std::string guidToString(const EntityGuid& g)
{
    char buf[40]{};
    std::snprintf(buf, sizeof(buf), "%016llx-%016llx",
                  static_cast<unsigned long long>(g.hi),
                  static_cast<unsigned long long>(g.lo));
    return std::string(buf);
}

inline bool guidFromString(const std::string& s, EntityGuid& out)
{
    const size_t dash = s.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= s.size())
        return false;
    auto parse_hex = [](const std::string& part, uint64_t& value) {
        value = 0;
        for (char c : part)
        {
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<uint64_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<uint64_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<uint64_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    };
    EntityGuid parsed{};
    if (!parse_hex(s.substr(0, dash), parsed.hi) || !parse_hex(s.substr(dash + 1), parsed.lo))
        return false;
    out = parsed;
    return true;
}

// splitmix64 step (deterministic GUID generation, entity_lifecycle.md §2).
inline uint64_t splitmix64Next(uint64_t& state)
{
    state += 0x9e3779b97f4a7c15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Serializable reference value (replaces raw entt::entity in reflected fields).
struct EntityRef
{
    EntityGuid guid;

    bool isNull() const { return guid.isNull(); }
    bool operator==(const EntityRef& o) const { return guid == o.guid; }
    bool operator!=(const EntityRef& o) const { return !(*this == o); }
};

template<>
struct DeducePropertyType<EntityGuid>
{
    static constexpr auto value = EPropertyType::String;
};

// EntityRef serializes as its GUID hex string (null GUID = no reference).
template<>
struct DeducePropertyType<EntityRef>
{
    static constexpr auto value = EPropertyType::EntityRef;
};

struct IdentityComponent
{
    EntityGuid guid;

    static void reflect(Reflector<IdentityComponent>& r)
    {
        r.display("Identity").category("Core").removable(false);
        r.field<&IdentityComponent::guid>("guid")
            .visible().tooltip("Stable entity GUID (hi-lo hex)").category("Identity");
    }
};

// GUID of an entity, or a null ref when it has no IdentityComponent.
inline EntityRef entityRefOf(const entt::registry& r, entt::entity e)
{
    if (e == entt::null || !r.valid(e))
        return EntityRef{};
    const auto* id = r.try_get<IdentityComponent>(e);
    return id ? EntityRef{id->guid} : EntityRef{};
}

inline EntityGuid generateEntityGuid()
{
    static std::atomic<uint64_t> state{[] {
        std::random_device rd;
        uint64_t s = (static_cast<uint64_t>(rd()) << 32) ^ rd();
        s ^= static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return s != 0 ? s : 0x9e3779b97f4a7c15ull;
    }()};
    uint64_t local = state.fetch_add(0x9e3779b97f4a7c15ull, std::memory_order_relaxed);
    EntityGuid guid{};
    do
    {
        guid.hi = splitmix64Next(local);
        guid.lo = splitmix64Next(local);
        if (guid.isNull())
            guid.lo = 1;
    } while (guid.isNull());
    return guid;
}

// Like entityRefOf, but assigns a fresh GUID first when missing so a
// migrated EntityRef field never silently drops a live reference.
inline EntityRef ensureEntityRef(entt::registry& r, entt::entity e)
{
    if (e == entt::null || !r.valid(e))
        return EntityRef{};
    if (auto* id = r.try_get<IdentityComponent>(e))
        return EntityRef{id->guid};
    const EntityGuid guid = generateEntityGuid();
    r.emplace<IdentityComponent>(e, IdentityComponent{guid});
    return EntityRef{guid};
}

// Resolve a GUID ref by scanning IdentityComponents (small registries;
// the world-owned retirement/index service is the indexed path when installed).
// Duplicate live owners are ambiguous and must not resolve.
inline entt::entity resolveEntityRef(const entt::registry& r, EntityRef ref)
{
    if (ref.isNull())
        return entt::null;
    entt::entity result = entt::null;
    for (auto [e, id] : r.view<IdentityComponent>().each())
    {
        if (id.guid != ref.guid || !r.valid(e))
            continue;
        if (result != entt::null)
            return entt::null;
        result = e;
    }
    return result;
}
