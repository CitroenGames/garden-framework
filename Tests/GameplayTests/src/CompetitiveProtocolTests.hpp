#pragma once

#include "shared/CombatProtocol.hpp"
#include "ui/UiLogic.hpp"

#include <string>

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
                             received_round.wins_defenders, received_round.bomb_planted != 0);
    if (!client.has_round_state || client.phase != Competitive::RoundPhase::Live ||
        client.round_number != 14 || client.score_attackers != 8 ||
        client.score_defenders != 5 || !client.bomb_planted) {
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

    Net::BitReader wrong_reader(economy_writer.getData(), economy_writer.getByteSize());
    RoundStateMessage wrong_type;
    if (CompetitiveSerializer::deserialize(wrong_reader, wrong_type))
        return fail(name, "round handler accepted an owner-only economy message");

    return pass(name);
}
