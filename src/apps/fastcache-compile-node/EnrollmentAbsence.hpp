// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

/// @file EnrollmentAbsence.hpp
/// Why a node serves no enrollment surface, and what a joiner pointed at it is told.
///
/// **The node knows the reason; the joiner does not.** A node that records nobody refuses the whole
/// family `NoCluster` at the door, and the joiner reports the seed's own words (`ReadEnrollReply`)
/// rather than guessing one: a joiner told *runs no consensus* by a node that runs it -- one pinned
/// elsewhere, say -- is sent looking for a fix in the wrong place, a confident wrong signal. So the
/// sentence is chosen where the reason is known, from this table, and every remedy names a flag or a
/// command that exists. A header of its own so the router can read it without the configuration it
/// is derived from (`EnrollmentAbsenceOf`).
namespace FastCache::Node
{

/// Why this node records no joiner.
///
/// **Private**: never transmitted or persisted, so its order is free. The wire carries `NoCluster`
/// and the row's sentence.
enum class EnrollmentAbsence : std::uint8_t
{
    NoConsensus,            ///< No formation record, or consensus closed (`--listen-raft=` empty).
    ConfinedToThisMachine,  ///< Consensus runs alone on loopback: this machine's name reaches only itself.
    NoScheduler,            ///< Consensus runs and no scheduler does: a learner of its fleet.
    PinnedToAnotherCluster, ///< `--fleet-id` names another cluster: this node is on its way there.
    NotAPinnedVoter,        ///< `--fleet-id` names its cluster, but not under this node's own key.
    NoIdentityKey,          ///< No identity key to sign an admission with.
    Last,                   ///< Not a reason: the length of a table keyed by one.
};

/// One reason, and what a joiner pointed at this node is told.
struct EnrollmentAbsenceRow
{
    EnrollmentAbsence absence; ///< Which reason.
    std::string_view detail;   ///< The refusal's words: what is true here, and where to ask instead.
};

/// One row per `EnrollmentAbsence`, in enumerator order. Every remedy points the joiner at a voter of
/// the fleet it means, through the flag that names one (`--fleet-seed`); a remedy that is THIS node's
/// operator's to apply says so.
inline constexpr EnumTable<EnrollmentAbsence, EnrollmentAbsenceRow> EnrollmentAbsenceTable { {
    { .absence = EnrollmentAbsence::NoConsensus,
      .detail = "this node runs no consensus, so it belongs to no cluster and there is nothing here to join; point "
                "--fleet-seed at a voter of the fleet you mean -- `fastcache-cli node` names the components a node "
                "serves" },
    { .absence = EnrollmentAbsence::ConfinedToThisMachine,
      .detail = "this node's name reaches only this machine, so it runs as a fleet of its own that no other machine "
                "can join; point --fleet-seed at a voter of the fleet you mean -- or, on this node, name the address "
                "its peers dial (--raft-self) and restart it" },
    { .absence = EnrollmentAbsence::NoScheduler,
      .detail = "this node runs consensus and schedules nothing -- a learner of its fleet -- and only a voter that "
                "schedules records a joiner; point --fleet-seed at a voter of its fleet" },
    { .absence = EnrollmentAbsence::PinnedToAnotherCluster,
      .detail = "this node's --fleet-id pins it to another fleet, which it is on its way to join, so it records "
                "nobody into the cluster it is leaving; point --fleet-seed at a voter of the fleet you mean" },
    { .absence = EnrollmentAbsence::NotAPinnedVoter,
      .detail = "this node's --fleet-id names its own cluster but not its own identity key, so every admission it "
                "signed would be refused by a joiner pinned to this fleet, and it records nobody; point --fleet-seed "
                "at a voter its --fleet-id names -- or, on this node, add its own key to its --fleet-id, as "
                "`fastcache-cli node` prints the fleet-id" },
    { .absence = EnrollmentAbsence::NoIdentityKey,
      .detail = "this node holds no identity key to sign an admission with, so it records nobody; point --fleet-seed "
                "at another voter of its fleet" },
} };

static_assert(RowsInEnumeratorOrder(EnrollmentAbsenceTable, &EnrollmentAbsenceRow::absence),
              "EnrollmentAbsenceTable must hold one row per EnrollmentAbsence, in enumerator order");

/// What a joiner pointed at this node is told for @p absence.
/// @param absence Why this node records nobody.
/// @return Its row's words.
[[nodiscard]] constexpr std::string_view EnrollmentAbsenceDetail(EnrollmentAbsence absence) noexcept
{
    return EnrollmentAbsenceTable[static_cast<std::size_t>(absence)].detail;
}

} // namespace FastCache::Node
