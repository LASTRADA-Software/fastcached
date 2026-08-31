// SPDX-License-Identifier: Apache-2.0
#include "NodeSurfaces.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <algorithm>
#include <cstddef>
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
            .surface = NodeSurface::Compile,
            .name = "compile",
            .flags = { "--bind", "--port" },
            .protocol = SurfaceProtocol::Tcp,
            .defaultHost = {},
            // No spec: the halves are a `std::string` and a `std::uint16_t` with their
            // own value parsers, so there is no text for a grammar to judge.
            .spec = nullptr,
            .grammar = {},
            .resolve = [](SurfaceRow const& /*row*/, NodeConfig const& cfg) -> SurfaceEndpoints {
                return SurfaceEndpoints { SurfaceEndpoint { .host = cfg.bindAddress, .port = cfg.port } };
            },
            .note = "a systemd .socket unit overrides --bind and --port entirely; the unit then owns the "
                    "address and this process is never told which port it got, so --advertise is what names "
                    "where clients actually go",
        },
        SurfaceRow {
            .surface = NodeSurface::Node,
            .name = "node",
            .flags = { "--listen-node", {} },
            .protocol = SurfaceProtocol::Tcp,
            // Empty, and the one row where that is not "this surface has no default".
            // Its default host depends on the CONFIGURATION -- loopback on a worker,
            // the wildcard on a node that schedules -- so it cannot be one constant,
            // and `NodeListenDefaultHost` is where it is decided. A value here would be
            // a second author of the rule, which is exactly what this table exists to
            // prevent (#290).
            .defaultHost = {},
            .spec = &NodeConfig::nodeListen,
            .grammar = ListenEndpointGrammar,
            .resolve = [](SurfaceRow const& row, NodeConfig const& cfg) -> SurfaceEndpoints {
                // A port with nothing behind it is not a served surface -- but "nothing
                // behind it" now has two halves, because two components answer here.
                // A node with no cache tier still serves the scheduler, and one that
                // neither caches nor schedules binds nothing at all. The same shape as
                // raft's `--node-id` gate: a surface can be configured and still not
                // served.
                //
                // `--cache-memory 0` with no `--cache-dir` leaves the tier nothing to
                // keep objects in, so `StartCacheTierOrExplain` returns without one --
                // and a worksheet that listed the port anyway would have an operator
                // open a port for a socket that is never created.
                auto const holdsCache = cfg.cacheMemoryBytes != 0 || !cfg.cacheDir.empty();
                if (!holdsCache && !cfg.serveScheduler)
                    return {};

                // Its own `defaultHost` is empty by design, so the row is resolved
                // against the one this configuration picks.
                auto resolved = row;
                resolved.defaultHost = NodeListenDefaultHost(cfg);
                return ResolveFromSpec(resolved, cfg);
            },
            .note = "one 0xFC port for the cache verbs and, with --serve-scheduler, the scheduler verbs. A bare "
                    "port binds loopback on a worker and the wildcard on a scheduler, because peers are "
                    "elsewhere by definition -- and the cache verbs answer this machine alone whichever it is, "
                    "so widening it admits nobody new to them. Not bound at all by a node that neither holds a "
                    "cache tier nor schedules. Scheduling is answered only while this node LEADS; a follower "
                    "redirects and an election in progress refuses, so the port is open on every member whether "
                    "or not it is answering today",
        },
        SurfaceRow {
            .surface = NodeSurface::Admin,
            .name = "admin",
            .flags = { "--admin-listen", {} },
            .protocol = SurfaceProtocol::Tcp,
            .defaultHost = AdminListenDefaultHost,
            .spec = &NodeConfig::adminListen,
            .grammar = ListenEndpointGrammar,
            .resolve = ResolveFromSpec,
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
            .defaultHost = RaftListenDefaultHost,
            .spec = &NodeConfig::raftListen,
            .grammar = ListenEndpointGrammar,
            .resolve = [](SurfaceRow const& row, NodeConfig const& cfg) -> SurfaceEndpoints {
                // Consensus is what `--node-id` turns on, so a `--listen-raft` with no
                // id binds nothing -- the surface is configured and not served. Then
                // delegating rather than re-spelling the fallback, which is what
                // taking the row buys.
                if (cfg.nodeId.empty())
                    return {};
                return ResolveFromSpec(row, cfg);
            },
            .note = "the wildcard for a bare port: peers are on other machines by definition, so a loopback "
                    "default would be one that silently cannot work. Served only when --node-id is given",
        },
        SurfaceRow {
            .surface = NodeSurface::Discovery,
            .name = "discovery",
            .flags = { "--discovery", "--discovery-reply-port" },
            .protocol = SurfaceProtocol::Udp,
            .defaultHost = DiscoveryBindHost,
            .spec = &NodeConfig::discoveryAddress,
            .grammar = BeaconAddressGrammar,
            .resolve = [](SurfaceRow const& row, NodeConfig const& cfg) -> SurfaceEndpoints {
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
                SurfaceEndpoints out;
                out.push_back(
                    SurfaceEndpoint { .host = std::string { row.defaultHost }, .port = beacon->second, .role = "beacon" });
                if (cfg.discoveryReplyPort != 0)
                    out.push_back(SurfaceEndpoint {
                        .host = std::string { row.defaultHost }, .port = cfg.discoveryReplyPort, .role = "reply" });
                return out;
            },
            .note = "UDP, and the only surface that is. The address you write is where beacons are SENT; the "
                    "sockets always bind the wildcard. Without --discovery-reply-port the reply socket takes "
                    "an ephemeral port, which a restrictive firewall has to allow as outbound",
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
    /// who wrote `--listen-raft=6680` -- and got no port, because `--node-id` is what
    /// switches consensus on -- to set the flag they had just set. The same line met
    /// somebody whose address was malformed, which is a state `--print-surfaces` is
    /// deliberately reachable in: it runs BEFORE `StartupPolicyRejection`, precisely
    /// so the map is available while a port is still wrong, and "set --listen-node"
    /// then describes the wrong problem in the one situation the flag exists for.
    ///
    /// Only the PRIMARY flag is named for a surface that is off, never every flag the
    /// row carries: `--discovery-reply-port` is optional, and listing it beside
    /// `--discovery` reads as two flags that are both required.
    /// @param row The surface that resolved to no endpoint.
    /// @param cfg What the operator asked for.
    /// @return What to print in that line's trailing column.
    [[nodiscard]] std::string WhyNotServed(SurfaceRow const& row, NodeConfig const& cfg)
    {
        if (row.spec == nullptr || (cfg.*row.spec).empty())
            return std::format("not served; set {}", PrimaryFlag(row));

        if (!row.grammar.parses(cfg.*row.spec))
            // Echoed, and in the shape the row itself advertises -- the same sentence
            // `StartupPolicyRejection` will produce a moment later for a node that is
            // actually starting, rather than a second author of it.
            return std::format("not served; {}={} is not {}", PrimaryFlag(row), cfg.*row.spec, row.grammar.shape);

        // Configured, well-formed, and still nothing bound -- today that is raft
        // waiting on `--node-id`. Why is not uniform enough to be a column, so the
        // row's own note carries it and this points at it rather than guessing.
        return row.note.empty() ? std::string { "not served" } : std::format("not served; see the {} note below", row.name);
    }
} // namespace

std::array<SurfaceRow, EnumeratorCount<NodeSurface>> const& NodeSurfaceTable() noexcept
{
    return Surfaces;
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
            lines.push_back(Line { .label = endpoint.role.empty() ? std::string { row.name }
                                                                  : std::format("{} {}", row.name, endpoint.role),
                                   // `FormatHostPort`, not a hand-rolled join: it brackets a v6
                                   // host, so `--listen-node [2001:db8::1]:6674` comes back as an
                                   // address that reads back rather than as `2001:db8::1:6674`.
                                   // This is the surface whose whole purpose is being transcribed.
                                   .address = FormatHostPort(endpoint.host, endpoint.port),
                                   .trailer = row.protocol == SurfaceProtocol::Udp ? "UDP" : "TCP" });
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

    std::string out;
    for (auto const& line: lines)
        out += std::format("{:<{}}  {:<{}}  {}\n", line.label, labelWidth, line.address, addressWidth, line.trailer);

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

} // namespace FastCache::Node
