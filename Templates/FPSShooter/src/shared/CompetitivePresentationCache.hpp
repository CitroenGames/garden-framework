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
    RejectedInvalid,
    RejectedStaleRevision,
    RejectedStaleInstance
};

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
