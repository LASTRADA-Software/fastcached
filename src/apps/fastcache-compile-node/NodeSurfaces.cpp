// SPDX-License-Identifier: Apache-2.0
#include "NodeIdentity.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Node
{

namespace
{
    /// What discovery's sockets bind, whatever `--discovery` names.
    ///
    /// The wildcard, unconditionally: a beacon is a broadcast, so every node has to
    /// be where the others shout. `--discovery`'s host half is where this node
    /// ANNOUNCES, never where it listens, and conflating the two would put an
    /// operator's own broadcast address on a firewall worksheet as though it were a
    /// bind address.
    ///
    /// Named here rather than spelled at the socket, which is the whole point of this
    /// file: it was three copies of a literal in `DiscoveryTier.cpp` and no constant
    /// at all, making it the least defended entry in the port map -- and the one an
    /// operator is least likely to get right unaided, because it is also the only UDP
    /// surface.
    constexpr std::string_view DiscoveryBindHost = "0.0.0.0";

    constexpr auto PortKindRows = EnumTable<SurfacePortKind, SurfacePortKindRow> { {
        { .kind = SurfacePortKind::Fixed,
          .portText = [](std::uint16_t port) { return std::format("{}", port); },
          .trailer = {} },
        // `*` rather than the 0 the endpoint carries: a worksheet line reading `:0` is a port an
        // operator would copy into a rule, and a rule on port 0 admits nothing.
        { .kind = SurfacePortKind::KernelChosen,
          .portText = [](std::uint16_t /*port*/) { return std::string { "*" }; },
          .trailer = ", port chosen by the kernel at bind" },
    } };

    static_assert(RowsInEnumeratorOrder(PortKindRows, [](SurfacePortKindRow const& row) { return row.kind; }),
                  "every SurfacePortKind needs a row, at its own index");
    static_assert(std::ranges::all_of(PortKindRows, [](SurfacePortKindRow const& row) { return row.portText != nullptr; }),
                  "every SurfacePortKind row spells its port");

    /// Whether text names an address a beacon can be sent to.
    ///
    /// Exactly `ParseDialEndpoint`'s question, so it is asked through it: a host
    /// somebody wrote, plus a port. It used to be spelled out here, which made a
    /// fourth author of a predicate whose own header says the three refusals it
    /// encodes have each already cost a bug.
    /// @param text What the operator typed.
    /// @return True when it is `<address>:<port>`.
    [[nodiscard]] bool ParsesAsBeaconAddress(std::string_view text)
    {
        return ParseDialEndpoint(text).has_value();
    }

    /// Whether text names an address a listen surface can bind.
    ///
    /// The grammar above, plus the one thing a bound surface takes and a beacon
    /// cannot: a bare port, which binds that surface's own default host. Written in
    /// terms of the other rather than beside it, because "an address, or just a port"
    /// is the whole of the difference and a second copy of the address half is how
    /// the two would come to disagree.
    ///
    /// NOT foldable into `ParseEndpoint`, which supplies a default host and therefore
    /// accepts an empty one -- and `--listen-node=:6674` binding every interface is
    /// exactly what this refuses.
    /// @param text What the operator typed.
    /// @return True when it is `[<address>:]<port>`.
    [[nodiscard]] bool ParsesAsListenEndpoint(std::string_view text)
    {
        if (!SplitHostPort(text).has_value())
            return ParseTcpPort(text).has_value();
        return ParsesAsBeaconAddress(text);
    }

    /// `[<address>:]<port>`: an address, or a bare port taking the surface's own
    /// default host.
    constexpr Grammar ListenEndpointGrammar { .parses = ParsesAsListenEndpoint, .shape = "[<address>:]<port>" };

    /// `<address>:<port>`: a beacon is SENT to an address, so a bare port names nobody
    /// and no default host may stand in for one.
    constexpr Grammar BeaconAddressGrammar { .parses = ParsesAsBeaconAddress, .shape = "<address>:<port>" };

    /// Resolve a row's own `spec` against its own `defaultHost`.
    ///
    /// The whole of what admin does, so it is written once here rather than as a
    /// lambda naming the member pointer and constant the row already holds. Raft
    /// delegates here too, after its `--node-id` gate, and so does the node's own 0xFC
    /// row once it has picked which default host applies.
    ///
    /// Through `ParseEndpoint` rather than `SplitHostPort`, and the difference is the
    /// default host -- right for a BIND address an operator typed, wrong for text
    /// naming somewhere else to ask. `Core/HostPort.hpp` states that distinction where
    /// `ParseDialEndpoint` is defined; this is the bind side of it.
    ///
    /// An empty spec resolves to nothing, which is how the off-by-default surfaces
    /// spell "not served". That is the same answer an unusable spec gives,
    /// deliberately: the grammar refusal happens once at startup where it can name the
    /// flag, and a port map has nothing to draw for either.
    /// @param row The surface being resolved.
    /// @param cfg What the operator asked for.
    /// @return The one endpoint, or none.
    [[nodiscard]] SurfaceEndpoints ResolveFromSpec(SurfaceRow const& row, NodeConfig const& cfg)
    {
        auto const endpoint = ParseEndpoint(cfg.*row.spec, row.defaultHost);
        if (!endpoint.has_value())
            return {};
        return SurfaceEndpoints { SurfaceEndpoint { .host = endpoint->first, .port = endpoint->second } };
    }

    /// The table itself.
    ///
    /// `EnumTable` so the extent comes from `NodeSurface` rather than from however
    /// many rows somebody last wrote, and `RowsInEnumeratorOrder` below checks both
    /// halves: the length against the enum's own count, and each row's position.
    /// Append an enumerator without a row and the build fails on a value-initialized
    /// row that claims to describe surface 0.
    constexpr auto Surfaces = EnumTable<NodeSurface, SurfaceRow> {
        SurfaceRow {
            .surface = NodeSurface::Node,
            .name = "node",
            .flags = { "--listen-node", {} },
            .protocol = SurfaceProtocol::Tcp,
            .bindFailure = BindFailurePolicy::Refuse,
            .bindFailureReason =
                "a node opens exactly one 0xFC port, so without it a dispatched compile has nowhere to "
                "arrive -- and the node would still register, because the endpoint it advertises comes from "
                "the configuration rather than from the listener. It would announce an address nothing "
                "answers, be leased out, and every client would meet a failed connection and fall back to "
                "compiling locally, which is silent by design. Green everywhere, working nowhere",
            // The wildcard on every node. It was the one row whose default host depended on
            // the configuration -- loopback on a worker, the wildcard on a scheduler -- until
            // the zero-config defaults made every node a fleet participant (#290 merged the two
            // surfaces that pulled it apart).
            .defaultHost = NodeSurfaceDefaultHost,
            .spec = &NodeConfig::nodeListen,
            .grammar = ListenEndpointGrammar,
            // A port with nothing behind it is not a served surface -- but there is no
            // configuration in which this one has nothing behind it. Every node answers
            // `--node-status` and live stats here, whatever else it runs, and a dispatched
            // compile arrives here since #290 stage 3 retired the worker's own port. So the
            // row resolves from its spec alone; `StartNodeSurfaceOrExplain` counts every family
            // it answers (`AnswersAnyFamily`), so the listener binds what this row names.
            .resolve = ResolveFromSpec,
            .closedBecause = nullptr,
            .note = "a systemd .socket unit is served on this surface: the unit owns the address, so this "
                    "flag configures nothing there -- the node reads the address and port off the socket it is "
                    "handed and advertises those, unless --advertise pins a value. one 0xFC port for the cache "
                    "verbs, this node's own compile verbs, and -- where "
                    "its mode serves them -- the scheduler verbs. A bare "
                    "port binds the wildcard on every node, because every node is a fleet participant -- and "
                    "the cache verbs answer this machine alone whatever the bind, while every other verb refuses a "
                    "caller that is not a member. Bound by every node that starts, whatever "
                    "components it runs: node status (`fastcache-cli node`) and live stats are answered here. "
                    "Scheduling is answered "
                    "only while this node LEADS; a follower redirects and an election in progress refuses, so "
                    "the port is open on every member whether or not it is answering today",
        },
        SurfaceRow {
            .surface = NodeSurface::Admin,
            .name = "admin",
            .flags = { "--admin-listen", {} },
            .protocol = SurfaceProtocol::Tcp,
            .bindFailure = BindFailurePolicy::Refuse,
            .bindFailureReason =
                "an operator who asks a WORKER for an admin endpoint is almost always wiring a probe to "
                "it, so a worker that started without one looks healthy to the only thing that would have "
                "noticed. fastcached continues past the same failure, and is right to for what it is: its "
                "admin surface is shared infrastructure an operator watches deliberately, where a node's is "
                "a probe target somebody attached. Same surface, two binaries, two right answers",
            .defaultHost = AdminListenDefaultHost,
            .spec = &NodeConfig::adminListen,
            .grammar = ListenEndpointGrammar,
            .resolve = ResolveFromSpec,
            .closedBecause = nullptr,
            // The one row whose default host is not a firewall detail.
            .note = "the loopback default is what the dashboard's credential rule turns on: reaching loopback "
                    "already means being on the machine, so a bare port needs no token while an address you "
                    "widened does",
        },
        SurfaceRow {
            .surface = NodeSurface::Raft,
            .name = "raft",
            .flags = { "--listen-raft", {} },
            .protocol = SurfaceProtocol::Tcp,
            .bindFailure = BindFailurePolicy::Refuse,
            .bindFailureReason =
                "consensus is the one thing a node cannot do partially: a peer that cannot be dialled is "
                "counted in the quorum it is absent from, so a cluster sized to include it stalls elections "
                "rather than degrading. This node would still serve compiles and still cache -- which is "
                "what makes carrying on tempting and wrong, because the fleet's decisions are the part that "
                "stops working and nothing here would report it",
            .defaultHost = RaftListenDefaultHost,
            .spec = &NodeConfig::raftListen,
            .grammar = ListenEndpointGrammar,
            // The MODE opens this port (zero-config formation): the formation record decides, a
            // learner dials the leader and listens for nobody, and a configuration no record
            // shaped opens nothing. `RunsConsensus` reads the port off this row for every mode
            // that listens, so there is no second author of "is the raft surface served".
            .resolve = [](SurfaceRow const& row, NodeConfig const& cfg) -> SurfaceEndpoints {
                if (!cfg.formation.has_value() || !ModeOpensRaftPort(cfg.formation->mode))
                    return {};
                // Confined to LOOPBACK, when the port was never asked for and this machine's name
                // reaches only itself (`ConsensusConfinedToThisMachine`): a consensus member must
                // name the address its peers dial, and that name would send every one of them to
                // itself -- so it binds where only this machine dials, and the node runs as a fleet
                // of its own, its own scheduler and worker included. Asked HERE so `RunsConsensus`,
                // `--print-surfaces` and the tier cannot disagree. A TYPED `--listen-raft`, or a
                // mode other machines dial, is refused by name instead.
                auto endpoints = ResolveFromSpec(row, cfg);
                if (ConsensusConfinedToThisMachine(cfg))
                    for (auto& endpoint: endpoints)
                        endpoint.host = std::string { ThisMachineLoopbackHost };
                return endpoints;
            },
            .closedBecause = RaftClosedByFormation,
            .note = "the formation record's mode opens the port, and every mode but a learner's does; it is on by "
                    "default, an empty --listen-raft= closes it (a node running no consensus), and a node whose name "
                    "reaches only itself (localhost) binds it to loopback, a fleet of its own, unless --raft-self names "
                    "it. The wildcard for a bare "
                    "port: peers are on other machines by definition, so a loopback default would be one that silently "
                    "cannot work",
        },
        SurfaceRow {
            .surface = NodeSurface::Discovery,
            .name = "discovery",
            .flags = { "--discovery", "--discovery-reply-port" },
            .protocol = SurfaceProtocol::Udp,
            .bindFailure = BindFailurePolicy::Refuse,
            .bindFailureReason =
                "a node that started without the discovery it was told to run looks healthy to a fleet "
                "that will never hear from it. Peers are seen and never admitted, which is the same silence "
                "--discovery-reply-port exists to prevent and is diagnosed at the firewall rather than here",
            .defaultHost = DiscoveryBindHost,
            .spec = &NodeConfig::discoveryAddress,
            .grammar = BeaconAddressGrammar,
            .resolve = [](SurfaceRow const& row, NodeConfig const& cfg) -> SurfaceEndpoints {
                // Beside consensus only, which is `StartDiscoveryOrExplain`'s own rule: a
                // node that runs none -- an empty `--listen-raft=` -- opens no discovery
                // socket whatever this address says, and since discovery is on by default
                // that is the ordinary worker. Asked here so `--print-surfaces`,
                // `--node-status` and the tier cannot disagree about whether it is served.
                //
                // And, defaulted, not beside a consensus address that reaches only this machine --
                // a loopback bind, or a consensus confined to it -- which the tier stands down on
                // too: a beacon would send every peer to itself. A typed one is refused instead.
                if (!RunsConsensus(cfg) || (!cfg.discoveryAddressExplicit && ConsensusAddressReachesOnlyThisMachine(cfg)))
                    return {};

                // NOT `ResolveFromSpec`: the host half of `--discovery` is where
                // beacons are SENT, and both sockets bind the wildcard whatever it
                // says. This is where that protection lives -- reading the announce
                // address as a bind address would put a broadcast address on a
                // firewall worksheet.
                auto const beacon = ParseDialEndpoint(cfg.*row.spec);
                if (!beacon.has_value())
                    return {};

                // Two sockets, and the second is why this row could not be one
                // endpoint: a node LISTENS on the beacon port, shared, and ANSWERS on
                // a port only it holds. An operator who opened the first and not the
                // second gets a fleet that hears every beacon and completes no
                // handshake.
                //
                // The reply socket is ALWAYS here, pinned or not. It used to appear only
                // when `--discovery-reply-port` named it, which left the default -- a port
                // the kernel chooses -- out of the worksheet and out of the firewall
                // rules, and every challenge and proof then arrived at a port no rule
                // covered. Unpinned, it is an endpoint whose port nobody can name, and
                // saying so is the port kind's job rather than a missing line's.
                auto const pinned = cfg.discoveryReplyPort != 0;
                return SurfaceEndpoints {
                    SurfaceEndpoint { .host = std::string { row.defaultHost }, .port = beacon->second, .role = "beacon" },
                    SurfaceEndpoint { .host = std::string { row.defaultHost },
                                      .port = cfg.discoveryReplyPort,
                                      .portKind = pinned ? SurfacePortKind::Fixed : SurfacePortKind::KernelChosen,
                                      .role = "reply" },
                };
            },
            .closedBecause = nullptr,
            .note = "UDP, and the only surface that is. The address you write is where beacons are SENT; the "
                    "sockets always bind the wildcard. Unless you write one, beacons go to every up interface's "
                    "directed broadcast on this port, re-read on an interval, never to the limited broadcast. "
                    "Without --discovery-reply-port the reply socket takes a kernel-chosen port, new at every start, "
                    "and peers send their challenges and proofs TO it: a restrictive firewall has to let it in, "
                    "INBOUND, by program, since no port rule can name it. --discovery-reply-port pins one where a "
                    "site must name it. On by default, and beside consensus only: a node with an empty "
                    "--listen-raft opens neither socket, and one whose consensus address reaches only this machine "
                    "opens neither unless you typed --discovery, which is refused",
        },
    };

    static_assert(RowsInEnumeratorOrder(Surfaces, [](SurfaceRow const& row) { return row.surface; }),
                  "every NodeSurface needs a row, at its own index");

    // A surface with no name could not be printed, one with no first flag could not be
    // configured, and one with no resolver could not be opened. Checked at compile time
    // because all three are what a half-written row looks like.
    static_assert(std::ranges::all_of(Surfaces,
                                      [](SurfaceRow const& row) {
                                          return !row.name.empty() && !row.flags[0].empty() && row.resolve != nullptr;
                                      }),
                  "every row needs a name, a first flag and a resolver");

    // Every row states what happens when it cannot be bound, and why. `Unstated` is
    // the zero value, so this fires on a row that simply omits the column -- which is
    // what a fifth surface added by copying a neighbour looks like.
    //
    // The reason is checked as well as the verdict, and that is the half #352 turns
    // on. All four surfaces already refused; what was missing was the sentence. Raft's
    // did not exist anywhere in the tree -- its bind site records only why the listener
    // is a reactor one and why `IsBound()` beats a null check -- so a guard that
    // demanded only a policy would have been satisfied by the value Raft already had,
    // and the row would still not say why.
    static_assert(std::ranges::all_of(Surfaces,
                                      [](SurfaceRow const& row) {
                                          return row.bindFailure != BindFailurePolicy::Unstated
                                                 && !row.bindFailureReason.empty();
                                      }),
                  "every row states its bind-failure policy and the reason for it");

    // **And no row is `Tolerate`, because no opener implements carrying one.**
    //
    // Not a second spelling of the assert above: that one forbids the ABSENCE of a
    // policy, this one forbids the one policy whose caller-side half is unwritten.
    // `JudgeBindFailure` understands `Tolerate` and is tested on it; what does not
    // exist is any opener able to hand its caller a surface that is not there. Two of
    // them would pass back a null inside a satisfied `expected`, and their callers
    // dereference it unconditionally.
    //
    // So this is the notice, delivered at the moment somebody flips a column, and it
    // is deliberately a build failure rather than a runtime check -- the point is that
    // it cannot be started and observed.
    //
    // **The message names the WORK, not a prohibition.** Somebody flipping a row to
    // `Tolerate` is not making a mistake; they are asking for a feature that is not
    // built, and a message that only forbids invites them to delete the assert --
    // which is the one outcome worse than having written nothing. So it says which
    // openers are unsound and why, because both were measured once and the next person
    // should not have to measure them again.
    static_assert(std::ranges::all_of(Surfaces,
                                      [](SurfaceRow const& row) { return row.bindFailure != BindFailurePolicy::Tolerate; }),
                  "you are asking for a tolerated bind failure, and the opener half is not built yet -- this assert is "
                  "where that work starts, not a rule against it. JudgeBindFailure already handles Tolerate and is tested "
                  "on it; what is missing is an opener that can hand its caller a surface which is not there. Two are "
                  "unsound today and were measured: AdminEndpoint returns a null unique_ptr inside a SUCCESSFUL expected "
                  "and its caller dereferences it unconditionally to log the bound endpoint, and ConsensusTier::Launch "
                  "would return success before _transport, _driver, _sink and _peerServer exist, so Start logs consensus "
                  "as running and the first Propose dereferences a null _driver. Write that surface's opener and its "
                  "caller's absent case, then remove this row from the assert");

    // A spec and its grammar travel together: text nothing validates is text an
    // operator can typo into a registration that replays forever, and a grammar with
    // no text to judge is a row that lies about having one. The grammar is one column,
    // so this can no longer be satisfied by a predicate paired with the wrong shape.
    static_assert(std::ranges::all_of(Surfaces,
                                      [](SurfaceRow const& row) {
                                          return (row.spec == nullptr) == (row.grammar.parses == nullptr)
                                                 && (row.grammar.parses == nullptr) == row.grammar.shape.empty();
                                      }),
                  "a row carries a spec and a grammar, or neither");

    // Every row that resolves from its own columns needs both of them. The two that do
    // not use `ResolveFromSpec` are exempt by construction, so this is asked of the
    // table rather than of the shared function, which cannot see who called it.
    static_assert(std::ranges::all_of(Surfaces,
                                      [](SurfaceRow const& row) {
                                          return row.resolve != ResolveFromSpec
                                                 || (row.spec != nullptr && !row.defaultHost.empty());
                                      }),
                  "a row resolving from its spec needs a spec and a default host");

    /// Why a surface resolved to nothing, in words an operator can act on.
    ///
    /// Three answers rather than one, because "set the flags this row names" is only
    /// the first of them and it is *wrong* for the other two. A row's flags are what
    /// would turn the surface on, so printing them unconditionally told an operator
    /// who wrote `--listen-raft=6680` -- and got no port, because `--node-id` was what
    /// switched consensus on -- to set the flag they had just set. The same line met
    /// somebody whose address was malformed, which is a state `--print-surfaces` is
    /// deliberately reachable in: it runs BEFORE `StartupPolicyRejection`, precisely
    /// so the map is available while a port is still wrong, and "set --listen-node"
    /// then describes the wrong problem in the one situation the flag exists for.
    ///
    /// **That first case is gone, and the third answer is kept anyway.** #1022 made
    /// `--listen-raft` itself the switch, so raft -- the one surface that could be
    /// configured, well-formed and still unserved -- no longer can be, and no row can
    /// today. It stays because what reaches it is a row whose `grammar` accepts a value
    /// its own `resolve` refuses, which is two columns that are written apart and are
    /// only equal by inspection: `--discovery` parses its spec twice, in two functions.
    /// The alternative is a bare "not served" for a state whose whole difficulty is
    /// that nothing says why.
    ///
    /// Only the PRIMARY flag is named for a surface that is off, never every flag the
    /// row carries: `--discovery-reply-port` is optional, and listing it beside
    /// `--discovery` reads as two flags that are both required.
    /// @param row The surface that resolved to no endpoint.
    /// @param cfg What the operator asked for.
    /// @return What to print in that line's trailing column.
    [[nodiscard]] std::string WhyNotServed(SurfaceRow const& row, NodeConfig const& cfg)
    {
        // The row's own reason first: "set the flag" is wrong for a port the mode keeps closed.
        if (row.closedBecause != nullptr)
            if (auto because = row.closedBecause(cfg); because.has_value())
                return *std::move(because);

        if (row.spec == nullptr || (cfg.*row.spec).empty())
            return std::format("not served; set {}", PrimaryFlag(row));

        if (!row.grammar.parses(cfg.*row.spec))
            // Echoed, and in the shape the row itself advertises -- the same sentence
            // `StartupPolicyRejection` will produce a moment later for a node that is
            // actually starting, rather than a second author of it.
            return std::format("not served; {}={} is not {}", PrimaryFlag(row), cfg.*row.spec, row.grammar.shape);

        // Configured, well-formed, and still nothing bound: discovery on a node running no
        // consensus, which it runs beside. Why is not uniform enough to be a column, so the
        // row's own note carries it and this points at it rather than guessing.
        return row.note.empty() ? std::string { "not served" } : std::format("not served; see the {} note below", row.name);
    }
} // namespace

std::array<SurfaceRow, EnumeratorCount<NodeSurface>> const& NodeSurfaceTable() noexcept
{
    return Surfaces;
}

std::expected<void, std::string> JudgeBindFailure(SurfaceRow const& row, std::string message, ILogger& logger)
{
    switch (row.bindFailure)
    {
        case BindFailurePolicy::Refuse:
            // The opener's own sentence, unchanged. It names the remedy rather than
            // the diagnosis (#229), and the row's reason is a maintainer's answer to
            // "why is this fatal" rather than an operator's to "what do I do now" --
            // appending it here would bury the second in the first.
            return std::unexpected { std::move(message) };

        case BindFailurePolicy::Tolerate:
            // Warn, never info: the operator asked for a surface and is not getting
            // it. The reason travels HERE because there is no refusal to carry it,
            // and because a tolerated failure is the case where somebody later asks
            // why the node thought this was survivable.
            logger.Logf(LogLevel::Warn, "{}: {}; continuing without it -- {}", row.name, message, row.bindFailureReason);
            return {};

        case BindFailurePolicy::Unstated:
            break;
    }

    // Unreachable through the table, which `static_assert`s no row is `Unstated`.
    // Reached only by a caller that built a row by hand and left the column out --
    // which is a programmer error, and is the one case where refusing is not a
    // judgement about the surface but about the caller.
    return std::unexpected { std::format("{}: {} (no bind-failure policy stated for this surface)", row.name, message) };
}

std::string BindToleranceUnsupported(SurfaceRow const& row, std::string_view why)
{
    return std::format("{}: this node cannot continue without the {} surface, even though its row says the "
                       "failure is tolerable -- {}. the row and this opener disagree, which is a defect in "
                       "this binary rather than in the configuration",
                       PrimaryFlag(row),
                       row.name,
                       why);
}

std::vector<std::string_view> FlagsOf(SurfaceRow const& row)
{
    std::vector<std::string_view> out;
    for (auto const& flag: row.flags)
        if (!flag.empty())
            out.push_back(flag);
    return out;
}

std::string_view PrimaryFlag(SurfaceRow const& row) noexcept
{
    return row.flags[0];
}

SurfacePortKindRow const& SurfacePortKindRowOf(SurfacePortKind kind) noexcept
{
    return PortKindRows[static_cast<std::size_t>(kind)];
}

SurfaceRow const& RowFor(NodeSurface surface) noexcept
{
    return Surfaces[static_cast<std::size_t>(surface)];
}

std::expected<SurfaceEndpoint, std::string> SoleEndpointOf(NodeSurface surface, NodeConfig const& cfg)
{
    auto const& row = RowFor(surface);
    auto endpoints = row.Resolve(cfg);
    if (endpoints.empty())
        // Naming the flag, because an operator reading it has to know which surface
        // went unserved. One sentence for one condition: this was three, so the same
        // fault read differently depending on which port it happened to.
        return std::unexpected { std::format("{} names no address to bind", PrimaryFlag(row)) };
    return std::move(endpoints.front());
}

namespace
{
    /// What the worksheet prints beside a consensus dial address the node HAS.
    constexpr std::string_view StatedDialTrailer = "-- what peers DIAL; the raft row above is what this node BINDS";

    /// How the worksheet prints a consensus dial address the node does not have.
    struct DialGapCell
    {
        ConsensusDialGap gap;     ///< Which absence this row describes.
        std::string_view address; ///< What stands in the address column instead of one.
        std::string_view trailer; ///< What the absence means, and what would end it.
    };

    /// One row per absence, so a third one is a row rather than a branch.
    constexpr EnumTable<ConsensusDialGap, DialGapCell> DialGapCells { {
        { .gap = ConsensusDialGap::NoConsensus,
          .address = "-",
          .trailer = "(absent: this node runs no consensus, --listen-raft does not resolve)" },
        { .gap = ConsensusDialGap::Unstated,
          .address = "NOT STATED",
          .trailer = "-- this node runs consensus and names no address peers dial it at; give --raft-self" },
        { .gap = ConsensusDialGap::AwaitingHostName,
          .address = "AT STARTUP",
          .trailer = "-- the address this machine routes from on the raft port, or its fully qualified name until a "
                     "route is known; derived when the node starts; give --raft-self to state it now" },
        { .gap = ConsensusDialGap::DialsIn,
          .address = "-",
          .trailer = "(absent: this node dials its fleet's voters, and nobody dials it)" },
    } };
    static_assert(RowsInEnumeratorOrder(DialGapCells, &DialGapCell::gap));
    static_assert(DialGapCells[static_cast<std::size_t>(ConsensusDialGap::Unstated)].trailer.contains(ConsensusDialRemedy),
                  "the worksheet names every way to state the dial address, in the startup refusal's words");

    /// The address column and the trailer of the worksheet's dial line.
    /// @param dial What `ConsensusDialAddressOf` answered; must outlive the result.
    /// @return The address or what stands in for it, and what it means.
    [[nodiscard]] std::pair<std::string_view, std::string_view> DialColumns(
        std::expected<std::string, ConsensusDialGap> const& dial)
    {
        if (dial.has_value())
            return { *dial, StatedDialTrailer };
        auto const& row = DialGapCells[static_cast<std::size_t>(dial.error())];
        return { row.address, row.trailer };
    }
} // namespace

std::string RenderSurfaces(NodeConfig const& cfg)
{
    // Resolved ONCE per row and kept, rather than resolved again to print. Not for the
    // five allocations -- this prints and exits -- but because a resolver reached twice
    // could answer differently between the measuring pass and the printing pass, and
    // would then rag the very columns the first pass exists to align.
    struct Line
    {
        std::string label;   ///< The surface, plus its endpoint's role when it has one.
        std::string address; ///< `host:port`, or `-` when the surface is not served.
        std::string trailer; ///< The protocol, or why the surface is not served.
    };
    std::vector<Line> lines;

    for (auto const& row: NodeSurfaceTable())
    {
        auto const endpoints = row.Resolve(cfg);
        if (endpoints.empty())
        {
            // Named rather than omitted. A surface missing from the list reads as one
            // this build does not have, and an operator cannot tell that from one they
            // did not turn on -- so the line says why, which is `WhyNotServed`'s to
            // answer because "set the flag" is only right for one of the three ways a
            // surface goes unserved.
            lines.push_back(Line { .label = std::string { row.name }, .address = "-", .trailer = WhyNotServed(row, cfg) });
            continue;
        }

        for (auto const& endpoint: endpoints)
        {
            auto const& portKind = SurfacePortKindRowOf(endpoint.portKind);
            lines.push_back(Line {
                .label = endpoint.role.empty() ? std::string { row.name } : std::format("{} {}", row.name, endpoint.role),
                // `FormatHostPort`, not a hand-rolled join: it brackets a v6
                // host, so `--listen-node [2001:db8::1]:6674` comes back as an
                // address that reads back rather than as `2001:db8::1:6674`.
                // This is the surface whose whole purpose is being transcribed.
                .address = FormatHostPort(endpoint.host, portKind.portText(endpoint.port)),
                .trailer = std::format("{}{}", row.protocol == SurfaceProtocol::Udp ? "UDP" : "TCP", portKind.trailer) });
        }
    }

    // Widths over what is actually PRINTED -- every line, not only the served ones.
    // Measured over served endpoints alone, the longest names were `scheduler` and
    // `discovery`, which are exactly the rows that are off by default and so were
    // excluded from their own measurement.
    std::size_t labelWidth = 0;
    std::size_t addressWidth = 0;
    for (auto const& line: lines)
    {
        labelWidth = std::max(labelWidth, line.label.size());
        addressWidth = std::max(addressWidth, line.address.size());
    }

    // The MODE first, on a line of its own: it is what decided which of the rows below are
    // served, and it is not a port -- `mode:` has no column-one `label  address` shape, so a
    // transcript reader cannot take it for a surface.
    auto out = std::format("{}\n\n", DescribeFormationMode(cfg));
    for (auto const& line: lines)
        out += std::format("{:<{}}  {:<{}}  {}\n", line.label, labelWidth, line.address, addressWidth, line.trailer);

    // **A block of its own, never a row of the table above** (#1328). Every row there is
    // an address this node BINDS -- what goes on a firewall worksheet -- and this is the
    // address peers DIAL, which is routinely not one of them: a bare `--listen-raft` binds
    // the wildcard. A column or a row would read as a second bind, and comparing the bound
    // address against `--cluster-admit`'s receipt is exactly the mistake this block exists
    // to end. Labelled `consensus endpoint`, the receipt's own label, so the two strings an
    // operator compares carry the same name.
    //
    // Indented under a heading, the shape the notes take, because COLUMN ONE belongs to the
    // table: a column-one `label  address` line reads as a port to open to anybody copying a
    // pasted transcript into firewall rules -- and `check-node-surface-docs.cmake` reads a
    // transcript by that same shape.
    auto const dial = ConsensusDialAddressOf(cfg);
    auto const [address, trailer] = DialColumns(dial);
    out += std::format("\n{}:\n  {}  {}  {}\n",
                       CompileCacheWire::ConsensusEndpointHeading,
                       CompileCacheWire::ConsensusEndpointLabel,
                       address,
                       trailer);

    // Where this node keeps its identity, and why there -- indented under a heading like the
    // block above, because it is not a port. A machine can hold two identities (the service's
    // and a hand-started node's), and the reason is what tells an operator which one this is.
    out += std::format("\nstate directory:\n  {}\n", DescribeNodeStateDirectory(cfg));

    // Which cluster this node is in and which one `--fleet-id` lets it be in, indented for the same
    // reason. The id is what another machine's pin names; `none` under the pin is the answer an
    // operator asks for -- discovery is trust-on-first-use -- and is said, never left blank.
    out +=
        std::format("\nfleet:\n  cluster    {}\n  pinned to  {}\n",
                    cfg.formation.has_value() && !cfg.formation->clusterId.empty() ? cfg.formation->clusterId
                                                                                   : std::string { "none minted yet" },
                    cfg.fleetPin.has_value() ? Cluster::FormatPinnedFleet(*cfg.fleetPin)
                                             : std::string { "none (--fleet-id unset: discovery is trust-on-first-use)" });

    // The notes last and separately, because they are prose while the table above is
    // something an operator transcribes into firewall rules. Mixing them would rag the
    // columns for the rows that carry one -- and the compile port's note says this list
    // can be WRONG for that row, which is not something to bury in a column.
    out += "\nnotes:\n";
    for (auto const& row: NodeSurfaceTable())
        if (!row.note.empty())
            out += std::format("  {}: {}\n", row.name, row.note);
    return out;
}

SurfaceReport ReportSurfaces(NodeConfig const& cfg)
{
    // Rendered FIRST and unconditionally, which is the whole shape of the fix: the
    // worksheet is what the operator asked for and they get it whatever the verdict
    // (#582). `RenderSurfaces` is untouched -- a case in `NodeSurfaces_test.cpp`
    // renders the sheet for a default `NodeConfig`, which is an invalid configuration,
    // and it must go on passing.
    //
    // The verdict comes from `StartupPolicyRejection` and is passed through
    // unaltered. Not reworded, not prefixed, not summarised: it is the same sentence
    // the node prints when it refuses to start, so an operator who sees it here and
    // then sees it at boot is reading one message rather than matching two.
    //
    // **Except a state directory no reading could shape the line from**, which is answered first and
    // ends by its OWN arm (`UnreadStateStage`): the same sentence the start refuses with, but a
    // transient read ends `Failed` (1) rather than `Declined` (2), and no flag row is asked of a
    // configuration that has no mode.
    if (auto const stage = UnreadStateStage(cfg); stage.has_value())
        return SurfaceReport { .text = RenderSurfaces(cfg),
                               .refusal = StateDirectoryUnreadRefusal(cfg),
                               .ending = EndingOf(*stage) };
    auto refusal = StartupPolicyRejection(cfg);
    auto const ending = refusal.has_value() ? CommandEnding::Declined : CommandEnding::Completed;
    return SurfaceReport { .text = RenderSurfaces(cfg), .refusal = std::move(refusal), .ending = ending };
}

ServedSurfaces NodeServedSurfacesFor(NodeConfig const& cfg)
{
    // A TABLE rather than ten `push_back`s behind ten `if`s, so adding a surface is adding a
    // row and the predicate sits beside the thing it decides. The three `false` rows are the
    // point of the table rather than noise in it: they are the surfaces a compile node can
    // never serve, and stating them is what makes this a complete answer instead of a list of
    // the ones somebody remembered.
    //
    // Each predicate is the SAME one that decides whether the component is constructed, never
    // a second reading of the flags -- a surface set that disagrees with what `main` built is
    // exactly the silent absence this is meant to prevent.
    struct Row
    {
        MetricsSurface surface;
        bool served;
    };

    // `ServesScheduler`, the one predicate `main` builds the tier under. Enrollment asks
    // `ServesEnrollment` itself, stating the scheduler by that same predicate: a started tier does not
    // exist when a scrape asks this question, and a copy of its clauses here missed the pin's.
    auto const servesScheduler = ServesScheduler(cfg);
    std::array const rows {
        // Never: this binary constructs no `Server`/`ReactorServerLoop`, so
        // `fastcached_connections_*` read absent rather than as a node nobody has connected to.
        Row { .surface = MetricsSurface::CacheAcceptPath, .served = false },
        // Never: no `CacheEngine`, so nothing here expires or reclaims.
        Row { .surface = MetricsSurface::CacheStorage, .served = false },
        // Never: the DAEMON's `0xFC` executor. The node's own door is `NodeFrameEndpoint`.
        Row { .surface = MetricsSurface::CacheCompileSurface, .served = false },
        // Always: `main` constructs `LiveStatsResponder` unconditionally.
        Row { .surface = MetricsSurface::LiveStats, .served = true },
        // Always: the shared frame listener is what a compile node IS.
        Row { .surface = MetricsSurface::NodeFrameEndpoint, .served = true },
        Row { .surface = MetricsSurface::ConsensusPeerWire, .served = RunsConsensus(cfg) },
        Row { .surface = MetricsSurface::CompileScheduler, .served = servesScheduler },
        Row { .surface = MetricsSurface::CompileWorker, .served = RunsWorker(cfg) },
        Row { .surface = MetricsSurface::NodeCacheTier, .served = ConfiguresCacheTier(cfg) },
        Row { .surface = MetricsSurface::NodeEnrollment, .served = ServesEnrollment(cfg, servesScheduler) },
        // The discovery row, which asks `StartDiscoveryOrExplain`'s conditions: an announce address,
        // a consensus tier to desire peers onto, and a consensus address that leaves this machine.
        Row { .surface = MetricsSurface::NodeDiscovery, .served = !RowFor(NodeSurface::Discovery).Resolve(cfg).empty() },
        // Never, yet: no `FormationController` is constructed until `main` wires one, so its
        // counters read absent rather than as a node that never yielded. The wiring makes this the
        // condition it constructs the controller under.
        Row { .surface = MetricsSurface::NodeFormation, .served = false },
        // Always: the fleet cache verbs sit on the same merged listener as `NodeFrameEndpoint`
        // above, so every node that starts builds the component that answers them -- serving,
        // when the `shared-cache` setting names this machine, or refusing `not-shared-cache`
        // otherwise. A dormant node still moves the refusal counters, so it can move these too.
        Row { .surface = MetricsSurface::NodeSharedCache, .served = true },
    };
    static_assert(rows.size() == EnumeratorCount<MetricsSurface>,
                  "every MetricsSurface needs a row here: a surface omitted is answered 'not "
                  "served', which renders its counters absent on a node that does serve it");

    // `.count = 0` EXPLICITLY. A default-constructed `ServedSurfaces` means *not narrowed* and
    // therefore holds every surface, so a default-constructed accumulator starts at count 10
    // and the first append runs off the end. It did: `array::at` threw on a node-status case,
    // which is the one place a bounds-checked `at` earns its keep over `[]` -- the unchecked
    // form would have written past the array and reported a wrong surface set instead.
    ServedSurfaces out { .storage = {}, .count = 0 };
    for (auto const& row: rows)
        if (row.served)
            out.storage.at(out.count++) = row.surface;
    return out;
}

} // namespace FastCache::Node
