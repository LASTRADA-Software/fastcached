// SPDX-License-Identifier: Apache-2.0
#include "ClusterAdminCli.hpp"

#include "CacheProtocol.hpp"
#include "ITcpClient.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <format>
#include <utility>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// How long to wait for the scheduler to answer.
    ///
    /// Generous, because an operator typed this and is watching: the alternative to
    /// waiting is telling them it failed when it had not, and a proposal that is in
    /// flight when the client gives up still commits.
    constexpr std::chrono::milliseconds DialTimeout { 10'000 };

    /// Column width for a member id in the rendered report.
    constexpr std::size_t IdColumn = 12;

    /// A dash where a value is absent, so a column is never blank.
    ///
    /// An empty cell reads as "nothing was rendered here" while a dash reads as
    /// "this member has not said" -- which for a scheduler endpoint is the ordinary
    /// state of every node that has never led, and not a fault.
    constexpr std::string_view Absent = "-";
} // namespace

std::vector<std::byte> EncodeClusterRequest(ClusterRequest const& request)
{
    switch (request.action)
    {
        case ClusterAction::None:
            return {};
        case ClusterAction::Status:
            return Wire::EncodeClusterStatus();
        case ClusterAction::Set:
            return Wire::EncodeClusterSet(Wire::ClusterSetRequest { .name = request.key, .value = request.value });
        case ClusterAction::Forget:
            return Wire::EncodeClusterForget(request.key);
    }

    return {};
}

std::string RenderClusterState(Cluster::ClusterState const& state)
{
    auto out = std::string {};

    out += std::format("members ({}):\n", state.members.size());
    if (state.members.empty())
        // Said out loud rather than left as a blank section. An empty member set is
        // a real and alarming state -- a cluster that admits nobody -- and a report
        // that merely showed no rows would read as a rendering problem.
        out += "  (none)\n";
    for (auto const& member: state.members)
        out += std::format("  {:<{}} raft={} scheduler={}\n",
                           member.id,
                           IdColumn,
                           member.raftEndpoint,
                           member.schedulerEndpoint.empty() ? std::string { Absent } : member.schedulerEndpoint);

    out += std::format("settings ({}):\n", state.settings.size());
    if (state.settings.empty())
        out += "  (none)\n";
    for (auto const& setting: state.settings)
        out += std::format("  {:<{}} {}\n", setting.name, IdColumn, setting.value);

    // Every key this build knows, whether or not it is set, because the operator's
    // real question is usually "what CAN I set" -- and a report that listed only
    // what somebody had already set would answer it wrongly by omission.
    out += "known settings:\n";
    for (auto const& row: Cluster::SettingTable)
        out += std::format("  {:<{}} {}\n", row.name, IdColumn, row.summary);

    return out;
}

std::expected<std::string, std::string> InterpretClusterReply(ClusterAction action, std::span<std::byte const> reply)
{
    switch (action)
    {
        case ClusterAction::None:
            return std::unexpected { std::string { "no cluster request was made" } };

        case ClusterAction::Status: {
            auto const state = Cluster::DecodeState(reply);
            if (!state.has_value())
                // A leader running a build whose state format this one does not know.
                // Refused rather than rendered as an empty cluster, which is what a
                // partial read would look like and would be read as a fact.
                return std::unexpected { std::string { "the leader's reply is in a format this build cannot read" } };
            return RenderClusterState(*state);
        }

        case ClusterAction::Set:
        case ClusterAction::Forget:
            // Appended, not committed, and the wording says so: the leader cannot know
            // the difference until a majority answers, and claiming otherwise would be
            // the one thing a report like this must not do.
            return std::string { "accepted; the change is replicating\n" };
    }

    return std::unexpected { std::string { "unknown cluster request" } };
}

std::expected<std::string, std::string> RunClusterAdmin(NodeConfig const& cfg, ClusterRequest const& request)
{
    if (cfg.scheduler.empty())
        return std::unexpected { std::string { "--scheduler names where to ask; a cluster command needs one" } };

    auto client = Cc::ConnectTcp(cfg.scheduler, DialTimeout);
    if (client == nullptr)
        return std::unexpected { std::format("cannot reach the scheduler at {}", cfg.scheduler) };

    // Through the launcher's own exchange rather than a second copy of it. That
    // function exists precisely so the distributed verbs do not grow one: the
    // credential pipelining and the "a daemon that does not know AUTH still served
    // the command" fall-through are each subtle enough that two implementations
    // would differ, and the one that differed would be the untested one.
    auto const outcome = Cc::ExchangeFramed(*client,
                                            EncodeClusterRequest(request),
                                            Cc::Credential { .username = {}, .secret = cfg.token });

    if (outcome.kind == Cc::CacheOutcomeKind::Transport)
        return std::unexpected { std::format("the scheduler at {} did not answer", cfg.scheduler) };

    if (outcome.kind == Cc::CacheOutcomeKind::Rejected)
    {
        // `NotLeader` carries the leader's endpoint as its message, so the refusal is
        // turned into the instruction it actually is.
        //
        // Tested by SPLITTING it rather than by asking whether it is empty, and that
        // is not fastidiousness: an empty message is replaced with the error table's
        // default sentence before it reaches the wire, so "no leader is known" and
        // "the leader is at h:p" arrive as the same shape. Only "does this parse as
        // an address" tells them apart -- and a client told to ask a sentence would
        // report a scheduler endpoint no operator ever typed.
        if (outcome.code == Wire::ErrorCode::NotLeader && SplitHostPort(outcome.message).has_value())
            return std::unexpected { std::format("this node does not lead the cluster; ask --scheduler={} instead",
                                                 outcome.message) };
        if (outcome.code == Wire::ErrorCode::NotLeader)
            // An election in progress, which is a different fact from "somebody else
            // leads" and has no address to offer.
            return std::unexpected { std::string { "the cluster has no leader right now; try again shortly" } };
        return std::unexpected { Cc::DescribeOutcome(outcome) };
    }

    return InterpretClusterReply(request.action, outcome.value);
}

} // namespace FastCache::Node
