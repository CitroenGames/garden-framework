#pragma once

#include "shared/CombatProtocol.hpp"
#include "shared/CompetitivePresentationCache.hpp"
#include "ui/UiLogic.hpp"

#include <limits>
#include <string>
#include <vector>

bool testCompetitiveProtocolStatePublication()
{
    const std::string name = "CompetitiveProtocol_StatePublication";

    if (competitiveMessageAudience(CompetitiveMessageType::ROUND_STATE) !=
            CompetitiveMessageAudience::AllConnectedClients ||
        competitiveMessageAudience(CompetitiveMessageType::ECONOMY_STATE) !=
            CompetitiveMessageAudience::OwningClient ||
        competitiveMessageAudience(CompetitiveMessageType::BUY_RESULT) !=
            CompetitiveMessageAudience::OwningClient ||
        competitiveMessageAudience(CompetitiveMessageType::TEAM_RESULT) !=
            CompetitiveMessageAudience::OwningClient ||
        competitiveMessageAudience(CompetitiveMessageType::BUY_REQUEST) !=
            CompetitiveMessageAudience::Server) {
        return fail(name, "competitive message audience contract changed");
    }

    RoundStateMessage round;
    round.phase = static_cast<uint8_t>(Competitive::RoundPhase::Live);
    round.phase_time_remaining = 91.25f;
    round.round_number = 14;
    round.wins_attackers = 8;
    round.wins_defenders = 5;
    round.bomb_planted = 1;
    round.last_reason = static_cast<uint8_t>(Competitive::RoundEndReason::BombDefused);

    Net::BitWriter round_writer;
    CompetitiveSerializer::serialize(round_writer, round);
    if (round_writer.getByteSize() != 21)
        return fail(name, "ROUND_STATE v3 payload layout changed");
    Net::BitReader round_reader(round_writer.getData(), round_writer.getByteSize());
    RoundStateMessage received_round;
    if (!CompetitiveSerializer::deserialize(round_reader, received_round) ||
        received_round.phase != round.phase ||
        received_round.phase_time_remaining != round.phase_time_remaining ||
        received_round.round_number != round.round_number ||
        received_round.wins_attackers != round.wins_attackers ||
        received_round.wins_defenders != round.wins_defenders ||
        received_round.bomb_planted != round.bomb_planted) {
        return fail(name, "round broadcast lost authoritative state");
    }

    UiLogic::ClientCompetitiveState client;
    UiLogic::applyRoundState(client, received_round.phase, received_round.phase_time_remaining,
                             received_round.round_number, received_round.wins_attackers,
                             received_round.wins_defenders, received_round.bomb_planted != 0,
                             received_round.phase_deadline_tick, received_round.server_tick,
                             received_round.last_reason);
    if (!client.has_round_state || client.phase != Competitive::RoundPhase::Live ||
        client.round_number != 14 || client.score_attackers != 8 ||
        client.score_defenders != 5 || !client.bomb_planted ||
        client.last_round_end_reason != Competitive::RoundEndReason::BombDefused) {
        return fail(name, "client round handler did not retain the server snapshot");
    }

    EconomyStateMessage economy;
    economy.balance = 7350;
    economy.loss_streak = 3;
    Net::BitWriter economy_writer;
    CompetitiveSerializer::serialize(economy_writer, economy);
    // ECONOMY_STATE is exactly type + wallet balance + loss streak.  Keeping
    // this fixed payload prevents team, loadout, and inventory fields from
    // leaking through wallet publication.
    if (economy_writer.getByteSize() != 6 ||
        economy_writer.getData()[0] != static_cast<uint8_t>(CompetitiveMessageType::ECONOMY_STATE) ||
        !isCompetitiveOwnerOnlyMessage(economy_writer.getData()[0])) {
        return fail(name, "economy payload audience or private-field boundary changed");
    }
    Net::BitReader economy_reader(economy_writer.getData(), economy_writer.getByteSize());
    EconomyStateMessage received_economy;
    if (!CompetitiveSerializer::deserialize(economy_reader, received_economy) ||
        received_economy.balance != economy.balance ||
        received_economy.loss_streak != economy.loss_streak) {
        return fail(name, "owner economy handler did not receive its wallet state");
    }
    UiLogic::applyEconomyState(client, received_economy.balance, received_economy.loss_streak);
    if (!client.has_economy || client.money != 7350 || client.loss_streak != 3)
        return fail(name, "client economy state was not corrected from the server");

    TeamResultMessage team;
    team.result = static_cast<uint8_t>(Competitive::TeamSelectResult::Ok);
    team.team = Competitive::teamId(Competitive::FpsTeam::Defenders);
    Net::BitWriter team_writer;
    CompetitiveSerializer::serialize(team_writer, team);
    Net::BitReader team_reader(team_writer.getData(), team_writer.getByteSize());
    TeamResultMessage received_team;
    UiLogic::UiState ui;
    if (!CompetitiveSerializer::deserialize(team_reader, received_team) ||
        !UiLogic::applyTeamResult(ui, static_cast<Competitive::TeamSelectResult>(received_team.result),
                                  Competitive::teamFromId(received_team.team), client.local_team) ||
        client.local_team != Competitive::FpsTeam::Defenders) {
        return fail(name, "client team handler did not accept the owner correction");
    }

    BuyRequestMessage request;
    request.request_seq = 0x12345678u;
    request.item_id = 42;
    Net::BitWriter request_writer;
    CompetitiveSerializer::serialize(request_writer, request);
    Net::BitReader server_reader(request_writer.getData(), request_writer.getByteSize());
    BuyRequestMessage received_request;
    if (!CompetitiveSerializer::deserialize(server_reader, received_request) ||
        received_request.request_seq != request.request_seq ||
        received_request.item_id != request.item_id) {
        return fail(name, "server buy handler wire decode was offset by the message type");
    }

    Net::BitWriter truncated_buy;
    truncated_buy.writeByte(static_cast<uint8_t>(CompetitiveMessageType::BUY_REQUEST));
    truncated_buy.writeUInt32(1);
    Net::BitReader truncated_buy_reader(truncated_buy.getData(), truncated_buy.getByteSize());
    if (CompetitiveSerializer::deserialize(truncated_buy_reader, received_request)) {
        return fail(name, "truncated buy request reached server semantics");
    }
    Net::BitWriter trailing_buy;
    CompetitiveSerializer::serialize(trailing_buy, request);
    trailing_buy.writeByte(0xff);
    Net::BitReader trailing_buy_reader(trailing_buy.getData(), trailing_buy.getByteSize());
    if (CompetitiveSerializer::deserialize(trailing_buy_reader, received_request)) {
        return fail(name, "buy request with trailing payload reached server semantics");
    }

    TeamSelectMessage team_request;
    team_request.desired_team = Competitive::teamId(Competitive::FpsTeam::Attackers);
    team_request.auto_assign = 0;
    if (!isValidCompetitiveBuyRequest(request) || isValidCompetitiveBuyRequest(BuyRequestMessage{}) ||
        !isValidCompetitiveTeamSelectRequest(team_request)) {
        return fail(name, "valid competitive client-request bounds were rejected");
    }
    team_request.desired_team = 4;
    if (isValidCompetitiveTeamSelectRequest(team_request)) {
        return fail(name, "out-of-range team enum passed client-request validation");
    }
    team_request.desired_team = Competitive::teamId(Competitive::FpsTeam::Attackers);
    Net::BitWriter trailing_team;
    CompetitiveSerializer::serialize(trailing_team, team_request);
    trailing_team.writeByte(0xff);
    Net::BitReader trailing_team_reader(trailing_team.getData(), trailing_team.getByteSize());
    TeamSelectMessage received_team_request;
    if (CompetitiveSerializer::deserialize(trailing_team_reader, received_team_request)) {
        return fail(name, "team request with trailing payload reached server semantics");
    }

    Net::BitReader wrong_reader(economy_writer.getData(), economy_writer.getByteSize());
    RoundStateMessage wrong_type;
    if (CompetitiveSerializer::deserialize(wrong_reader, wrong_type))
        return fail(name, "round handler accepted an owner-only economy message");

    return pass(name);
}

bool testCompetitivePresentationSnapshotProtocol()
{
    const std::string name = "CompetitivePresentationSnapshot_Protocol";
    CompetitivePresentationSnapshotMessage snapshot;
    snapshot.match_instance_id = 42;
    snapshot.presentation_revision = 9;
    snapshot.source_server_tick = 600;
    snapshot.recipient_team = Competitive::teamId(Competitive::FpsTeam::Defenders);
    snapshot.recipient_stable_player_id = 20;
    snapshot.phase = static_cast<uint8_t>(Competitive::RoundPhase::Live);
    snapshot.phase_deadline_tick = 1200;
    snapshot.round_number = 7;
    snapshot.wins_attackers = 3;
    snapshot.wins_defenders = 4;
    snapshot.objective_state = static_cast<uint8_t>(Competitive::BombState::Planted);
    snapshot.objective_site = 1;
    snapshot.objective_deadline_tick = 900;
    snapshot.roster_count = 2;
    snapshot.roster[0] = {10, "alpha", Competitive::teamId(Competitive::FpsTeam::Attackers),
        static_cast<uint8_t>(CompetitivePresentationRosterState::Alive), 3, 1, 0, 0, 0, 18};
    snapshot.roster[1] = {20, "BOT 2", Competitive::teamId(Competitive::FpsTeam::Defenders),
        static_cast<uint8_t>(CompetitivePresentationRosterState::Dead), 1, 4, 1, 1, 4200, 0};
    // A dead local recipient has no Self radar record. Its durable recipient
    // identity still identifies the local roster row for the scoreboard.
    snapshot.radar_count = 1;
    snapshot.radar[0] = {std::numeric_limits<uint32_t>::max(),
        static_cast<uint8_t>(CompetitivePresentationRadarKind::Objective), glm::vec3(4, 5, 6)};

    Net::BitWriter writer;
    if (!CompetitiveSerializer::serialize(writer, snapshot) || writer.getByteSize() > kCompetitivePresentationMaxPayloadBytes)
        return fail(name, "valid bounded snapshot did not serialize");
    Net::BitReader reader(writer.getData(), writer.getByteSize());
    CompetitivePresentationSnapshotMessage received;
    if (!CompetitiveSerializer::deserialize(reader, received) ||
        received.match_instance_id != snapshot.match_instance_id ||
        received.presentation_revision != snapshot.presentation_revision ||
        received.recipient_stable_player_id != snapshot.recipient_stable_player_id ||
        received.roster_count != 2 || received.roster[1].money != 4200 ||
        received.radar_count != 1 || received.radar[0].stable_record_id != std::numeric_limits<uint32_t>::max())
        return fail(name, "valid snapshot round trip lost durable presentation state");

    CompetitivePresentationSnapshotMessage invalid = snapshot;
    invalid.roster[1].stable_player_id = invalid.roster[0].stable_player_id;
    Net::BitWriter invalid_writer;
    if (CompetitiveSerializer::serialize(invalid_writer, invalid))
        return fail(name, "duplicate stable roster id serialized");
    invalid = snapshot;
    invalid.roster[0].display_name = "bad\nname";
    if (CompetitiveSerializer::serialize(invalid_writer, invalid))
        return fail(name, "unsafe roster name serialized");
    invalid = snapshot;
    invalid.roster[0].money_visible = 0;
    invalid.roster[0].money = 1;
    if (CompetitiveSerializer::serialize(invalid_writer, invalid))
        return fail(name, "hidden money serialized with a value");
    invalid = snapshot;
    invalid.recipient_stable_player_id = 0;
    if (CompetitiveSerializer::serialize(invalid_writer, invalid))
        return fail(name, "snapshot without recipient identity serialized");
    invalid = snapshot;
    invalid.radar_count = kCompetitivePresentationMaxRadarRecords + 1;
    if (CompetitiveSerializer::serialize(invalid_writer, invalid))
        return fail(name, "oversize radar row count serialized");

    std::vector<uint8_t> malformed(writer.getData(), writer.getData() + writer.getByteSize());
    malformed[1] = kCompetitivePresentationSchemaVersion + 1;
    Net::BitReader unknown_version_reader(malformed.data(), malformed.size());
    if (CompetitiveSerializer::deserialize(unknown_version_reader, received))
        return fail(name, "unknown presentation schema version deserialized");
    malformed.assign(writer.getData(), writer.getData() + writer.getByteSize());
    malformed.push_back(0xff);
    Net::BitReader trailing_reader(malformed.data(), malformed.size());
    if (CompetitiveSerializer::deserialize(trailing_reader, received))
        return fail(name, "trailing presentation payload deserialized");

    if (!isCompetitivePresentationRevisionNewer(1, std::numeric_limits<uint32_t>::max()) ||
        isCompetitivePresentationRevisionNewer(std::numeric_limits<uint32_t>::max(), 1) ||
        isCompetitivePresentationRevisionNewer(5, 5))
        return fail(name, "wrapped presentation revision ordering changed");
    if (!competitivePresentationAllowsTeamPrivateData(2, false, 2) ||
        competitivePresentationAllowsTeamPrivateData(2, false, 1) ||
        competitivePresentationAllowsTeamPrivateData(2, true, 2) ||
        competitivePresentationAllowsTeamPrivateData(3, false, 2))
        return fail(name, "recipient wallet and radar privacy policy changed");
    Competitive::CompetitiveWorldContext first_context;
    Competitive::CompetitiveWorldContext second_context;
    first_context.configure(Competitive::ModeConfig{});
    second_context.configure(Competitive::ModeConfig{});
    const uint32_t first_player_id = first_context.presentationPlayerId(11);
    if (first_context.presentation_match_instance_id == 0 ||
        second_context.presentation_match_instance_id == 0 ||
        first_context.presentation_match_instance_id == second_context.presentation_match_instance_id ||
        first_player_id == 0 || first_player_id != first_context.presentationPlayerId(11) ||
        first_player_id == first_context.presentationPlayerId(12))
        return fail(name, "match-scoped presentation identities were not stable");
    return pass(name);
}

bool testCompetitivePresentationSnapshotCache()
{
    const std::string name = "CompetitivePresentationSnapshot_Cache";
    CompetitivePresentationSnapshotMessage initial;
    initial.match_instance_id = 9;
    initial.presentation_revision = 7;
    initial.recipient_team = Competitive::teamId(Competitive::FpsTeam::Defenders);
    initial.recipient_stable_player_id = 10;
    initial.roster_count = 1;
    initial.roster[0] = {10, "alpha", Competitive::teamId(Competitive::FpsTeam::Defenders),
        static_cast<uint8_t>(CompetitivePresentationRosterState::Alive), 0, 0, 0, 1, 800, 0};
    initial.radar_count = 1;
    initial.radar[0] = {10, static_cast<uint8_t>(CompetitivePresentationRadarKind::Self), glm::vec3(1, 2, 3)};

    CompetitivePresentationSnapshotCache cache;
    if (cache.accept(initial) != CompetitivePresentationSnapshotAcceptResult::AcceptedInitial ||
        !cache.hasSnapshot() || cache.snapshot()->presentation_revision != 7)
        return fail(name, "initial valid snapshot did not create bounded cache state");

    CompetitivePresentationSnapshotMessage heartbeat = initial;
    heartbeat.source_server_tick = 8;
    heartbeat.phase_deadline_tick = 120;
    heartbeat.roster[0].ping_ms = 37;
    heartbeat.radar[0].position = glm::vec3(4, 5, 6);
    if (cache.accept(heartbeat) != CompetitivePresentationSnapshotAcceptResult::AcceptedHeartbeat ||
        cache.snapshot()->source_server_tick != heartbeat.source_server_tick ||
        cache.snapshot()->radar[0].position != heartbeat.radar[0].position)
        return fail(name, "same-revision heartbeat did not refresh transient presentation state");

    CompetitivePresentationSnapshotMessage duplicate_heartbeat = heartbeat;
    if (cache.accept(duplicate_heartbeat) != CompetitivePresentationSnapshotAcceptResult::RejectedStaleRevision)
        return fail(name, "duplicate same-revision heartbeat replaced accepted state");

    CompetitivePresentationSnapshotMessage older_heartbeat = heartbeat;
    older_heartbeat.source_server_tick = 7;
    if (cache.accept(older_heartbeat) != CompetitivePresentationSnapshotAcceptResult::RejectedStaleRevision)
        return fail(name, "older same-revision heartbeat replaced accepted state");

    CompetitivePresentationSnapshotMessage forged_same_revision = heartbeat;
    forged_same_revision.source_server_tick = 9;
    forged_same_revision.roster[0].money = 801;
    if (cache.accept(forged_same_revision) != CompetitivePresentationSnapshotAcceptResult::RejectedStaleRevision ||
        cache.snapshot()->roster[0].money != heartbeat.roster[0].money)
        return fail(name, "same-revision heartbeat replaced durable roster state");

    CompetitivePresentationSnapshotMessage malformed = initial;
    malformed.roster[0].display_name = "bad\nname";
    if (cache.accept(malformed) != CompetitivePresentationSnapshotAcceptResult::RejectedInvalid ||
        cache.snapshot()->presentation_revision != initial.presentation_revision)
        return fail(name, "malformed snapshot mutated accepted state");

    CompetitivePresentationSnapshotMessage stale = initial;
    stale.presentation_revision = 6;
    if (cache.accept(stale) != CompetitivePresentationSnapshotAcceptResult::RejectedStaleRevision ||
        cache.snapshot()->presentation_revision != initial.presentation_revision)
        return fail(name, "stale revision replaced accepted snapshot");

    CompetitivePresentationSnapshotMessage newer = initial;
    newer.presentation_revision = 8;
    newer.roster_count = 0;
    newer.radar_count = 0;
    if (cache.accept(newer) != CompetitivePresentationSnapshotAcceptResult::AcceptedNewerRevision ||
        cache.snapshot()->roster_count != 0 || cache.snapshot()->radar_count != 0)
        return fail(name, "new revision did not replace missing roster or radar rows");

    CompetitivePresentationSnapshotMessage new_instance = newer;
    new_instance.match_instance_id = 10;
    new_instance.presentation_revision = 1;
    if (cache.accept(new_instance) != CompetitivePresentationSnapshotAcceptResult::AcceptedNewerInstance ||
        cache.snapshot()->match_instance_id != new_instance.match_instance_id)
        return fail(name, "newer instance did not reset prior durable snapshot");

    if (cache.accept(newer) != CompetitivePresentationSnapshotAcceptResult::RejectedStaleInstance ||
        cache.snapshot()->match_instance_id != new_instance.match_instance_id)
        return fail(name, "late prior instance resurrected cached data");

    CompetitivePresentationSnapshotCache wrap_cache;
    CompetitivePresentationSnapshotMessage before_tick_wrap = initial;
    before_tick_wrap.source_server_tick = std::numeric_limits<uint32_t>::max();
    if (wrap_cache.accept(before_tick_wrap) != CompetitivePresentationSnapshotAcceptResult::AcceptedInitial)
        return fail(name, "pre-wrap initial heartbeat fixture was rejected");
    CompetitivePresentationSnapshotMessage after_tick_wrap = before_tick_wrap;
    after_tick_wrap.source_server_tick = 1;
    if (wrap_cache.accept(after_tick_wrap) != CompetitivePresentationSnapshotAcceptResult::AcceptedHeartbeat ||
        wrap_cache.snapshot()->source_server_tick != after_tick_wrap.source_server_tick)
        return fail(name, "same-revision heartbeat tick wrap was not accepted");

    cache.clear();
    if (cache.hasSnapshot() || cache.snapshot() != nullptr)
        return fail(name, "teardown clear retained presentation state");
    return pass(name);
}

bool testCompetitivePresentationDurableFingerprint()
{
    const std::string name = "CompetitivePresentation_DurableFingerprint";
    CompetitivePresentationSnapshotMessage baseline;
    baseline.match_instance_id = 9;
    baseline.presentation_revision = 7;
    baseline.source_server_tick = 100;
    baseline.recipient_team = Competitive::teamId(Competitive::FpsTeam::Defenders);
    baseline.recipient_stable_player_id = 10;
    baseline.phase = static_cast<uint8_t>(Competitive::RoundPhase::Freeze);
    baseline.phase_deadline_tick = 200;
    baseline.round_number = 4;
    baseline.roster_count = 1;
    baseline.roster[0] = {10, "alpha", Competitive::teamId(Competitive::FpsTeam::Defenders),
        static_cast<uint8_t>(CompetitivePresentationRosterState::Alive), 2, 1, 0, 1, 800, 18};
    baseline.radar_count = 1;
    baseline.radar[0] = {10, static_cast<uint8_t>(CompetitivePresentationRadarKind::Self), glm::vec3(1, 2, 3)};

    baseline.objective_deadline_tick = 300;
    baseline.channel_kind = 1;
    baseline.channel_actor_stable_id = 10;
    baseline.channel_deadline_tick = 250;

    CompetitivePresentationSnapshotMessage stable_world = baseline;
    stable_world.source_server_tick = 101;
    stable_world.phase_deadline_tick = 199;
    stable_world.objective_deadline_tick = 299;
    stable_world.channel_deadline_tick = 249;
    stable_world.roster[0].ping_ms = 44;
    stable_world.radar[0].position = glm::vec3(7, 8, 9);
    if (!competitivePresentationHasSameDurableContent(baseline, stable_world))
        return fail(name, "stable world observations changed the durable fingerprint");

    CompetitivePresentationSnapshotMessage phase_change = baseline;
    phase_change.phase = static_cast<uint8_t>(Competitive::RoundPhase::Live);
    if (competitivePresentationHasSameDurableContent(baseline, phase_change))
        return fail(name, "phase transition did not change durable fingerprint");

    CompetitivePresentationSnapshotMessage team_change = baseline;
    team_change.roster[0].team = Competitive::teamId(Competitive::FpsTeam::Attackers);
    if (competitivePresentationHasSameDurableContent(baseline, team_change))
        return fail(name, "team change did not change durable fingerprint");

    CompetitivePresentationSnapshotMessage buy_change = baseline;
    buy_change.roster[0].money = 200;
    if (competitivePresentationHasSameDurableContent(baseline, buy_change))
        return fail(name, "buy money change did not change durable fingerprint");

    CompetitivePresentationSnapshotMessage death_change = baseline;
    death_change.roster[0].state = static_cast<uint8_t>(CompetitivePresentationRosterState::Dead);
    death_change.roster[0].deaths = 2;
    if (competitivePresentationHasSameDurableContent(baseline, death_change))
        return fail(name, "death state did not change durable fingerprint");
    return pass(name);
}

bool testCompetitivePresentationRecipientRevisionPolicy()
{
    const std::string name = "CompetitivePresentation_RecipientRevisionPolicy";
    CompetitivePresentationSnapshotMessage attacker_before;
    attacker_before.match_instance_id = 9;
    attacker_before.presentation_revision = 3;
    attacker_before.recipient_team = Competitive::teamId(Competitive::FpsTeam::Attackers);
    attacker_before.recipient_stable_player_id = 10;
    attacker_before.roster_count = 1;
    attacker_before.roster[0] = {10, "alpha", Competitive::teamId(Competitive::FpsTeam::Attackers),
        static_cast<uint8_t>(CompetitivePresentationRosterState::Alive), 0, 0, 0, 1, 800, 0};

    CompetitivePresentationSnapshotMessage defender_before = attacker_before;
    defender_before.presentation_revision = 17;
    defender_before.recipient_team = Competitive::teamId(Competitive::FpsTeam::Defenders);
    defender_before.recipient_stable_player_id = 20;
    defender_before.roster[0].money_visible = 0;
    defender_before.roster[0].money = 0;

    CompetitivePresentationSnapshotMessage attacker_after = attacker_before;
    attacker_after.roster[0].money = 801;
    CompetitivePresentationSnapshotMessage defender_after = defender_before;

    if (competitivePresentationHasSameDurableContent(attacker_before, attacker_after))
        return fail(name, "visible wallet update was not durable for the allowed recipient");
    if (!competitivePresentationHasSameDurableContent(defender_before, defender_after))
        return fail(name, "hidden wallet update changed the opposing recipient view");
    if (competitivePresentationNextRecipientRevision(attacker_before.presentation_revision, true) != 4 ||
        competitivePresentationNextRecipientRevision(defender_before.presentation_revision, false) !=
            defender_before.presentation_revision)
        return fail(name, "recipient revisions did not advance independently");
    if (competitivePresentationNextRecipientRevision(std::numeric_limits<uint32_t>::max(), true) != 1)
        return fail(name, "recipient revision did not skip zero on wrap");
    return pass(name);
}
