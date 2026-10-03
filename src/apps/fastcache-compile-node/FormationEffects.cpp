// SPDX-License-Identifier: Apache-2.0
#include "EnrollmentWindow.hpp"
#include "FormationEffects.hpp"
#include "NodeDefaults.hpp"
#include "RaftStoreArchiver.hpp"

#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/PeerText.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace FastCache::Node
{

namespace
{
    /// Save @p record, or say what could not be recorded.
    /// @param store Where it is kept.
    /// @param record The record.
    /// @param what What the record says, for the refusal.
    /// @return Nothing, or the sentence.
    [[nodiscard]] std::expected<void, std::string> SaveOrSay(Cluster::IFormationStore& store,
                                                             Cluster::FormationRecord const& record,
                                                             std::string_view what)
    {
        if (auto saved = store.Save(record); !saved.has_value())
            return std::unexpected { std::format("cannot record {}: {}", what, saved.error().context) };
        return {};
    }

    /// Where a client dials @p member of the fleet @p target names.
    ///
    /// **A HINT, never an identity.** The fleet's leader said where it answers the `0xFC` port, and
    /// that is used as it said it; any other voter's is its consensus host at the default node port --
    /// a guess until members record the endpoint they answer at. It fails closed: whoever answers
    /// there must still prove the key the roster holds, so a wrong guess costs a dial, never trust.
    /// @param member A voter of the roster.
    /// @param target What the fleet said about itself when this node asked it.
    /// @return The endpoint, or empty when the voter names no host.
    [[nodiscard]] std::string NodeEndpointOf(Cluster::RosterMember const& member,
                                             CompileCacheWire::FleetSummary const& target)
    {
        if (member.id == target.leaderId && !target.leaderNodeEndpoint.empty())
            return target.leaderNodeEndpoint;
        auto const split = SplitHostPort(member.raftEndpoint);
        if (!split.has_value() || split->first.empty())
            return {};
        return FormatHostPort(split->first, DefaultNodePort);
    }

    /// The fleet's voters, as a seed walk remembers them.
    /// @param roster The roster, decoded.
    /// @param target What the fleet said about itself.
    /// @return Its voters' endpoints, in roster order.
    [[nodiscard]] Cluster::FleetEndpoints VotersOf(Cluster::Roster const& roster,
                                                   CompileCacheWire::FleetSummary const& target)
    {
        auto remembered = Cluster::FleetEndpoints { .clusterId = target.clusterId, .voters = {} };
        for (auto const& member: roster.members)
        {
            if (member.seat != Cluster::MemberSeat::Voter)
                continue;
            auto nodeEndpoint = NodeEndpointOf(member, target);
            if (nodeEndpoint.empty())
                continue;
            remembered.voters.push_back(Cluster::FleetEndpoint {
                .id = member.id, .raftEndpoint = member.raftEndpoint, .nodeEndpoint = std::move(nodeEndpoint) });
        }
        return remembered;
    }

    /// Archive the store @p record says is still in the root, then record that it is not.
    /// @param record The record whose `archivePending` names the store; cleared on success.
    /// @param store Where the record is kept.
    /// @param archiver What moves the store.
    /// @return Nothing, or why the archive or the record failed.
    [[nodiscard]] std::expected<void, std::string> FinishArchive(Cluster::FormationRecord& record,
                                                                 Cluster::IFormationStore& store,
                                                                 IStoreArchiver& archiver)
    {
        if (!record.archivePending.has_value())
            return {};
        auto const clusterId = *record.archivePending;
        if (auto archived = archiver.Archive(clusterId); !archived.has_value())
            return std::unexpected { std::format(
                "cannot archive the consensus store of cluster {}: {}", clusterId, archived.error()) };
        auto done = record;
        done.archivePending.reset();
        if (auto saved = SaveOrSay(store, done, std::format("that the store of cluster {} was archived", clusterId));
            !saved.has_value())
            return saved;
        record = std::move(done);
        return {};
    }
} // namespace

std::expected<void, std::string> CheckAdmission(JoinerIdentity const& self,
                                                Cluster::JoinTarget const& target,
                                                std::span<std::byte const> roster)
{
    auto const decoded = Cluster::DecodeRoster(roster);
    if (!decoded.has_value())
        return std::unexpected { std::format(
            "the fleet admitted {} with a roster this build cannot read: {}", self.nodeId, decoded.error().context) };
    if (!RosterRecordsJoiner(*decoded, self.nodeId, self.publicKey, self.role))
        return std::unexpected { std::format("the fleet said {} was admitted, and the roster it sent does not record "
                                             "{} as a {} under this machine's key: an operator approved another key "
                                             "for this id",
                                             self.nodeId,
                                             self.nodeId,
                                             EnrollRoleRowFor(self.role).name) };
    // The roster's tie to the fleet this node PROVED. The certified roster a reading may carry is not
    // what ties it: certified rosters are leaving this build, and the key that signed the summary the
    // join was decided on is the one fact about that fleet this node has verified itself.
    auto const speaker = target.summary.nodeId;
    auto const vouched = std::ranges::any_of(decoded->members, [&](Cluster::RosterMember const& member) {
        return member.publicKey == std::optional { target.provenKey } && (speaker.empty() || member.id == speaker);
    });
    if (!vouched)
        return std::unexpected { std::format(
            "the roster that admits {} records no member under the key that proved the fleet {} this node asked{}: "
            "it is not the fleet this node decided to join",
            self.nodeId,
            target.summary.clusterId,
            speaker.empty() ? std::string {} : std::format(" (its member {})", speaker)) };
    return {};
}

std::expected<Cluster::FormationRecord, std::string> DissolveInto(Cluster::FormationRecord const& pending,
                                                                  std::span<std::byte const> roster,
                                                                  JoinerIdentity const& self,
                                                                  Cluster::IFormationStore& store,
                                                                  Cluster::FleetEndpointsFile& endpoints,
                                                                  ILogger& logger)
{
    if (!pending.joining.has_value())
        return std::unexpected { std::string { "no join is recorded to take on" } };
    // The fleet's id is the one id that reached this node from ANOTHER machine and will one day name
    // a directory here -- the archive of this fleet, if it forgets this node. So a node never takes on
    // a fleet whose id could not name one: refused now, rather than a forgotten node that cannot start.
    if (!IsArchivableClusterId(pending.joining->summary.clusterId))
        return std::unexpected { std::format(
            "will not join the fleet {}: its id could not name the archive this "
            "node would keep its store in",
            BoundedPeerText(pending.joining->summary.clusterId, CompileCacheWire::MaxIdBytes)) };
    if (auto admitted = CheckAdmission(self, *pending.joining, roster); !admitted.has_value())
        return std::unexpected { std::move(admitted).error() };
    auto const decoded = Cluster::DecodeRoster(roster); // decodes: `CheckAdmission` said so
    auto const target = pending.joining->summary;

    auto next = pending;
    next.mode = Cluster::NodeMode::Learner;
    next.fleet = Cluster::FleetMembership { .clusterId = target.clusterId,
                                            .roster = { roster.begin(), roster.end() },
                                            .createdAtUnixSeconds = target.createdAtUnixSeconds };
    next.archivePending = Cluster::CurrentClusterId(pending);
    // The memo of this ask becomes one no later ask displaces: the fleet admitted this node under it.
    Cluster::RememberAdmitted(next, target.clusterId, pending.joining->provenKey);
    next.joining.reset();

    // The fleet's voters are remembered BEFORE the record is saved, because that save is where the move
    // is judged (`IShapeJudge`), and the judge reads this file: the learner's schedulers are derived from
    // it (`ApplyFormation`), as the reform that adopts the record and every restart derive them. Written
    // after, the judge would weigh a learner with no fleet schedulers -- a shape no restart sees -- and
    // pass a record the reform then refuses. ONE source of truth, the file, read at one moment by all
    // three. Safe when the save is refused or fails: the file names its cluster, and a learner's
    // schedulers are read from it only for a record naming that cluster, so a node still pending on the
    // fleet holds a hint about the very fleet it asked, nothing more.
    //
    // A hint, so a failure is said and never stops the move: no state of that file may keep a node
    // from joining, any more than from starting.
    if (decoded.has_value())
        if (auto remembered = endpoints.Save(VotersOf(*decoded, target)); !remembered.has_value())
            logger.Logf(LogLevel::Warn,
                        "formation: cannot remember the voters of {} as seeds: {}; this node finds its fleet through "
                        "its configuration instead",
                        target.clusterId,
                        remembered.error().context);

    if (auto saved = SaveOrSay(store, next, std::format("this node as a learner of {}", target.clusterId));
        !saved.has_value())
        return std::unexpected { std::move(saved).error() };
    return next;
}

std::expected<Cluster::FormationRecord, std::string> ArchiveAndMint(Cluster::FormationRecord const& forgotten,
                                                                    Cluster::IFormationStore& store,
                                                                    ISecureRandom& random,
                                                                    core::platform::IWallClock const& wall)
{
    auto minted = Cluster::MintSolitary(random, wall);
    if (!minted.has_value())
        return std::unexpected { std::format("cannot mint a new cluster: {}", minted.error().detail) };
    auto const left = Cluster::CurrentClusterId(forgotten);

    auto next = forgotten;
    next.mode = Cluster::NodeMode::Solitary;
    next.own = minted->own;
    next.fleet.reset();
    next.joining.reset();
    next.archivePending = left;
    // A forget outranks an observation: the memo of the fleet that forgot this node is not evidence a
    // split of it could heal on. Every other memo is this machine's history and stays.
    std::erase_if(next.askedJoins, [&left](Cluster::AskedJoin const& memo) { return memo.clusterId == left; });
    if (auto saved = SaveOrSay(store, next, std::format("this node alone in a new cluster {}", next.own.clusterId));
        !saved.has_value())
        return std::unexpected { std::move(saved).error() };
    return next;
}

std::expected<Cluster::FormationRecord, std::string> LeaveForSurvivor(Cluster::FormationRecord const& member,
                                                                      Cluster::DissolveOrder const& order,
                                                                      Cluster::IFormationStore& store,
                                                                      ISecureRandom& random,
                                                                      core::platform::IWallClock const& wall)
{
    auto const left = Cluster::CurrentClusterId(member);
    // For `DissolveInto`'s reason: the survivor's id may one day name the archive of its store here.
    if (!IsArchivableClusterId(order.clusterId))
        return std::unexpected { std::format("will not leave for the fleet {}: its id could not name the archive this "
                                             "node would keep its store in",
                                             BoundedPeerText(order.clusterId, CompileCacheWire::MaxIdBytes)) };
    if (order.clusterId == left)
        return std::unexpected { std::format("the order names {}, the fleet this node is in; a fleet does not "
                                             "dissolve into itself",
                                             left) };

    auto minted = Cluster::MintSolitary(random, wall);
    if (!minted.has_value())
        return std::unexpected { std::format("cannot mint a new cluster: {}", minted.error().detail) };
    auto const asked = minted->own.createdAtUnixSeconds; // read off the same clock, a moment ago

    auto next = member;
    next.mode = Cluster::NodeMode::Pending;
    next.own = minted->own;
    next.fleet.reset();
    next.archivePending = left;
    next.joining =
        Cluster::JoinTarget { .summary = CompileCacheWire::FleetSummary { .clusterId = order.clusterId,
                                                                          .state = CompileCacheWire::FleetState::Established,
                                                                          .createdAtUnixSeconds = order.createdAtUnixSeconds,
                                                                          .leaderId = {},
                                                                          .leaderNodeEndpoint = order.leaderNodeEndpoint,
                                                                          .nodeId = {},
                                                                          .raftEndpoint = {},
                                                                          .members = {},
                                                                          .memberTotal = 0,
                                                                          .nodeEndpoint = {},
                                                                          .leaderKey = order.leaderKey,
                                                                          .pointsAt = {} },
                              .provenKey = order.provenKey,
                              .askedAtUnixSeconds = asked };
    Cluster::RememberAsked(
        next,
        Cluster::AskedJoin { .clusterId = order.clusterId, .provenKey = order.provenKey, .askedAtUnixSeconds = asked });
    if (auto saved = SaveOrSay(
            store,
            next,
            std::format("this node leaving {} for {}, in a new cluster {}", left, order.clusterId, next.own.clusterId));
        !saved.has_value())
        return std::unexpected { std::move(saved).error() };
    return next;
}

std::expected<Cluster::FormationRecord, std::string> ResumeFormation(Cluster::FormationRecord record,
                                                                     Cluster::IFormationStore& store,
                                                                     IStoreArchiver& archiver)
{
    if (!record.archivePending.has_value())
        return record;
    auto const clusterId = *record.archivePending;
    if (auto finished = FinishArchive(record, store, archiver); !finished.has_value())
        return std::unexpected { std::format(
            "this node was leaving cluster {} when it last stopped, and did not finish putting that cluster's "
            "consensus store away: {}. It will not start on a store it was leaving; fix what that names and start "
            "it again",
            clusterId,
            finished.error()) };
    return record;
}

} // namespace FastCache::Node
