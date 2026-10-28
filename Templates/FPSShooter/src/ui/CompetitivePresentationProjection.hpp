#pragma once

// Template-local conversion from one validated, recipient-filtered snapshot to
// scalar HUD sources. It has no network, ECS, or RML side effects: the cache
// remains the sole receipt owner and every source is rebuilt from its current
// replacement snapshot.
#include "shared/CompetitivePresentationCache.hpp"
#include "ui/UiLogic.hpp"
#include <utility>

namespace UiLogic {

inline ScoreboardPlayerState scoreboardPlayerStateFromPresentation(
    uint8_t state)
{
    switch (static_cast<CompetitivePresentationRosterState>(state)) {
    case CompetitivePresentationRosterState::Awaiting:
        return ScoreboardPlayerState::Awaiting;
    case CompetitivePresentationRosterState::Alive:
        return ScoreboardPlayerState::Alive;
    case CompetitivePresentationRosterState::Dead:
        return ScoreboardPlayerState::Dead;
    case CompetitivePresentationRosterState::Spectating:
        return ScoreboardPlayerState::Spectating;
    }
    return ScoreboardPlayerState::Awaiting;
}

inline ScoreboardPresentationSource projectCompetitivePresentationScoreboard(
    const CompetitivePresentationSnapshotCache& cache,
    bool connected,
    const ClientCompetitiveState* competitive)
{
    ScoreboardPresentationSource source;
    source.connected = connected;
    source.competitive = competitive;

    const CompetitivePresentationSnapshotMessage* snapshot = cache.snapshot();
    if (!snapshot)
        return source;

    // A snapshot contains the complete currently permitted roster. Never add
    // a locally derived row: omitted data must disappear on replacement.
    source.has_authoritative_roster = true;
    source.approved_rows.reserve(snapshot->roster_count);

    for (uint8_t index = 0; index < snapshot->roster_count; ++index) {
        const CompetitivePresentationRosterRow& row = snapshot->roster[index];
        ScoreboardRowInput input;
        input.name = row.display_name;
        input.kills = row.kills;
        input.deaths = row.deaths;
        input.side = Competitive::teamFromId(row.team);
        input.is_local = row.stable_player_id == snapshot->recipient_stable_player_id;
        input.is_bot = row.is_bot != 0;
        input.state = scoreboardPlayerStateFromPresentation(row.state);
        input.alive = input.state == ScoreboardPlayerState::Alive;
        // `money_visible` is an explicit server privacy grant. Team equality
        // is only a second defensive guard in buildScoreboardRow.
        input.money_available = row.money_visible != 0;
        input.money = input.money_available ? row.money : 0;
        input.ping_available = true;
        input.ping = row.ping_ms;
        source.approved_rows.push_back(std::move(input));
    }
    return source;
}

inline RadarPresentationSource projectCompetitivePresentationRadar(
    const CompetitivePresentationSnapshotCache& cache,
    bool connected,
    const ClientCompetitiveState* competitive,
    const RadarMapTransform& map)
{
    RadarPresentationSource source;
    source.connected = connected;
    source.competitive = competitive;
    source.map = map;

    const CompetitivePresentationSnapshotMessage* snapshot = cache.snapshot();
    if (!snapshot)
        return source;

    source.local_stable_id = snapshot->recipient_stable_player_id;

    auto teamForStableId = [snapshot](uint32_t stable_id) {
        for (uint8_t index = 0; index < snapshot->roster_count; ++index) {
            const CompetitivePresentationRosterRow& row = snapshot->roster[index];
            if (row.stable_player_id == stable_id)
                return Competitive::teamFromId(row.team);
        }
        return Competitive::FpsTeam::None;
    };
    auto aliveForStableId = [snapshot](uint32_t stable_id) {
        for (uint8_t index = 0; index < snapshot->roster_count; ++index) {
            const CompetitivePresentationRosterRow& row = snapshot->roster[index];
            if (row.stable_player_id == stable_id)
                return row.state == static_cast<uint8_t>(CompetitivePresentationRosterState::Alive);
        }
        return false;
    };

    for (uint8_t index = 0; index < snapshot->radar_count; ++index) {
        const CompetitivePresentationRadarRecord& record = snapshot->radar[index];
        const CompetitivePresentationRadarKind kind =
            static_cast<CompetitivePresentationRadarKind>(record.kind);
        RadarApprovedLocation location;
        location.stable_id = record.stable_record_id;
        location.team = teamForStableId(record.stable_record_id);
        location.alive = aliveForStableId(record.stable_record_id);
        location.world_x = record.position.x;
        location.world_z = record.position.z;
        switch (kind) {
        case CompetitivePresentationRadarKind::Self:
            location.kind = RadarLocationKind::LocalSelf;
            source.approved_locations.push_back(location);
            break;
        case CompetitivePresentationRadarKind::Teammate:
            location.kind = RadarLocationKind::ApprovedTeammate;
            source.approved_locations.push_back(location);
            break;
        case CompetitivePresentationRadarKind::Objective:
            // D10 is self/teammates only. Objective status remains an
            // independent HUD field and receives no map position here.
            break;
        }
    }
    // This flag is intentionally tied to an accepted snapshot rather than
    // generic replicated transforms. Empty records clear all prior contacts.
    source.has_authoritative_team_locations = true;
    return source;
}

} // namespace UiLogic
