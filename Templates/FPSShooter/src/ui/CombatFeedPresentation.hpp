#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

// The current all-client PLAYER_DIED message carries only stable client IDs.
// This client-side presentation cache deliberately models only those fields.
// It is not a replica of FpsCombat::CombatFeedComponent: that server-owned
// feed also has weapon, flags, score deltas, and a publication sequence, none
// of which are present on the current protocol.  Keep that distinction
// explicit until a reviewed feed publication message is added.
namespace UiLogic {

constexpr size_t kCombatFeedPresentationCapacity = 5;
constexpr float kCombatFeedPresentationLifetime = 5.0f;

struct CombatFeedPresentationEntry {
    uint16_t killer_client_id = 0;
    uint16_t victim_client_id = 0;
    uint64_t local_sequence = 0;
    float remaining_seconds = 0.0f;
};

class CombatFeedPresentation {
public:
    // PLAYER_DIED cannot identify stale/replayed records, because it has no
    // sequence.  The local sequence below orders only messages received by
    // this client and must never drive scoreboard or match decisions.
    bool recordPlayerDied(uint16_t killer_client_id, uint16_t victim_client_id)
    {
        if (victim_client_id == 0)
            return false;

        const size_t write_index = (m_first + m_count) % kCombatFeedPresentationCapacity;
        CombatFeedPresentationEntry& entry = m_entries[write_index];
        entry.killer_client_id = killer_client_id;
        entry.victim_client_id = victim_client_id;
        entry.local_sequence = m_next_sequence++;
        entry.remaining_seconds = kCombatFeedPresentationLifetime;
        if (m_count < kCombatFeedPresentationCapacity)
            ++m_count;
        else
            m_first = (m_first + 1) % kCombatFeedPresentationCapacity;
        return true;
    }

    void tick(float delta_seconds)
    {
        if (!(delta_seconds > 0.0f))
            return;

        // Records have equal lifetimes and arrive in receive order, so an
        // expired record can only be removed from the oldest end.
        for (size_t i = 0; i < m_count; ++i)
            entryAtMutable(i)->remaining_seconds -= delta_seconds;
        while (m_count > 0 && entryAtMutable(0)->remaining_seconds <= 0.0f) {
            m_first = (m_first + 1) % kCombatFeedPresentationCapacity;
            --m_count;
        }
    }

    void clear()
    {
        m_first = 0;
        m_count = 0;
        // Keep the local receive sequence monotonic across level/round
        // clears. It is presentation-only and is never an authority token.
    }

    size_t count() const { return m_count; }

    const CombatFeedPresentationEntry* entryAt(size_t index) const
    {
        if (index >= m_count)
            return nullptr;
        return &m_entries[(m_first + index) % kCombatFeedPresentationCapacity];
    }

    std::string buildText() const
    {
        std::string out;
        // Most-recent-first matches the existing HUD layout while the ring
        // preserves its oldest-to-newest receive order for testable state.
        for (size_t offset = 0; offset < m_count; ++offset) {
            const CombatFeedPresentationEntry* entry = entryAt(m_count - 1 - offset);
            if (!entry)
                continue;
            if (!out.empty())
                out += '\n';
            out += displayName(entry->killer_client_id);
            out += " killed ";
            out += displayName(entry->victim_client_id);
        }
        return out;
    }

private:
    static std::string displayName(uint16_t client_id)
    {
        // The wire message has no immutable display name. Do not read remote
        // entity state or fabricate one from an unapproved roster snapshot.
        return client_id == 0 ? "World" : "Player " + std::to_string(client_id);
    }

    CombatFeedPresentationEntry* entryAtMutable(size_t index)
    {
        return index < m_count ? &m_entries[(m_first + index) % kCombatFeedPresentationCapacity] : nullptr;
    }

    std::array<CombatFeedPresentationEntry, kCombatFeedPresentationCapacity> m_entries{};
    size_t m_first = 0;
    size_t m_count = 0;
    uint64_t m_next_sequence = 1;
};

} // namespace UiLogic
