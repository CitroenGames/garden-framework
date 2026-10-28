#pragma once

// Template-local durable receipt cache for the recipient-filtered competitive
// presentation snapshot. It owns no gameplay state and deliberately does not
// project data into HUD/RML bindings.
#include "CombatProtocol.hpp"

enum class CompetitivePresentationSnapshotAcceptResult : uint8_t
{
    AcceptedInitial,
    AcceptedNewerRevision,
    AcceptedNewerInstance,
    AcceptedHeartbeat,
    RejectedInvalid,
    RejectedStaleRevision,
    RejectedStaleInstance
};

// A presentation revision identifies the recipient-visible facts that persist
// between heartbeats. Countdown tick anchors, ping samples, and radar poses
// are current observations rather than new durable match state. Keeping them
// out of the fingerprint lets a bounded heartbeat refresh those observations
// without manufacturing a new revision every simulation tick.
inline bool competitivePresentationDurableFingerprint(
    const CompetitivePresentationSnapshotMessage& snapshot,
    std::vector<uint8_t>& out)
{
    CompetitivePresentationSnapshotMessage durable = snapshot;
    durable.presentation_revision = 1;
    durable.source_server_tick = 0;
    durable.phase_deadline_tick = 0;
    durable.objective_deadline_tick = 0;
    // The wire validator requires an active channel to retain a nonzero
    // deadline. Use a valid sentinel while excluding its moving tick value
    // from the durable revision fingerprint.
    if (durable.channel_kind != 0)
        durable.channel_deadline_tick = 1;
    for (uint8_t index = 0; index < durable.roster_count; ++index)
        durable.roster[index].ping_ms = 0;
    durable.radar_count = 0;

    Net::BitWriter writer;
    if (!CompetitiveSerializer::serialize(writer, durable)) {
        out.clear();
        return false;
    }
    out.assign(writer.getData(), writer.getData() + writer.getByteSize());
    return true;
}

inline bool competitivePresentationHasSameDurableContent(
    const CompetitivePresentationSnapshotMessage& left,
    const CompetitivePresentationSnapshotMessage& right)
{
    std::vector<uint8_t> left_content;
    std::vector<uint8_t> right_content;
    return competitivePresentationDurableFingerprint(left, left_content) &&
        competitivePresentationDurableFingerprint(right, right_content) &&
        left_content == right_content;
}

// Revisions belong to one recipient's filtered view. A change that is hidden
// from another recipient must not manufacture a revision on that recipient's
// stream: the next packet there remains a same-revision heartbeat.
constexpr uint32_t competitivePresentationNextRecipientRevision(
    uint32_t current_revision, bool durable_changed)
{
    if (!durable_changed)
        return current_revision;
    ++current_revision;
    return current_revision == 0 ? 1 : current_revision;
}

class CompetitivePresentationSnapshotCache
{
public:
    void clear()
    {
        has_snapshot_ = false;
        snapshot_ = CompetitivePresentationSnapshotMessage{};
    }

    bool hasSnapshot() const { return has_snapshot_; }

    const CompetitivePresentationSnapshotMessage* snapshot() const
    {
        return has_snapshot_ ? &snapshot_ : nullptr;
    }

    CompetitivePresentationSnapshotAcceptResult accept(
        const CompetitivePresentationSnapshotMessage& candidate)
    {
        // Deserialization already validates this, but retain the boundary
        // here because this cache is the only mutation point for client state.
        if (!isValidCompetitivePresentationSnapshot(candidate))
            return CompetitivePresentationSnapshotAcceptResult::RejectedInvalid;

        if (!has_snapshot_)
        {
            snapshot_ = candidate;
            has_snapshot_ = true;
            return CompetitivePresentationSnapshotAcceptResult::AcceptedInitial;
        }

        if (candidate.match_instance_id != snapshot_.match_instance_id)
        {
            // Match instance IDs are server serial numbers. A newer instance
            // replaces all old durable data; a delayed prior instance cannot
            // resurrect roster or radar records.
            if (!isCompetitivePresentationRevisionNewer(
                    candidate.match_instance_id, snapshot_.match_instance_id))
                return CompetitivePresentationSnapshotAcceptResult::RejectedStaleInstance;

            snapshot_ = candidate;
            return CompetitivePresentationSnapshotAcceptResult::AcceptedNewerInstance;
        }

        if (candidate.presentation_revision == snapshot_.presentation_revision) {
            // A same-revision packet is the bounded server heartbeat. It may
            // refresh countdown anchors, ping, and radar poses, but can never
            // overwrite a durable roster/round/objective fact.
            if (!isCompetitivePresentationRevisionNewer(
                    candidate.source_server_tick, snapshot_.source_server_tick) ||
                !competitivePresentationHasSameDurableContent(candidate, snapshot_))
                return CompetitivePresentationSnapshotAcceptResult::RejectedStaleRevision;
            snapshot_ = candidate;
            return CompetitivePresentationSnapshotAcceptResult::AcceptedHeartbeat;
        }

        if (!isCompetitivePresentationRevisionNewer(
                candidate.presentation_revision, snapshot_.presentation_revision))
            return CompetitivePresentationSnapshotAcceptResult::RejectedStaleRevision;

        snapshot_ = candidate;
        return CompetitivePresentationSnapshotAcceptResult::AcceptedNewerRevision;
    }

private:
    bool has_snapshot_ = false;
    CompetitivePresentationSnapshotMessage snapshot_{};
};
