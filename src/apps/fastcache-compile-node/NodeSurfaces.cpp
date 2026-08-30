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
    /// A host and a port, both required. Moved here with the rows that use it: it
    /// existed to serve `EndpointFlags`, and that table is now these rows.
    /// @param text What the operator typed.
    /// @return True when it is `<address>:<port>`.
    [[nodiscard]] bool ParsesAsBeaconAddress(std::string_view text)
    {
        auto const address = SplitHostPort(text);
        return address.has_value() && !address->first.empty() && ParseTcpPort(address->second).has_value();
    }

    /// Whether text names an address a listen surface can bind.
    ///
    /// The grammar above, plus the one thing a bound surface takes and a beacon
    /// cannot: a bare port, which binds that surface's own default host. Written in
    /// terms of the other rather than beside it, because "an address, or just a port"
    /// is the whole of the difference and a second copy of the address half is how
    /// the two would come to disagree.
    /// @param text What the operator typed.
    /// @return True when it is `[<address>:]<port>`.
    [[nodiscard]] bool ParsesAsListenEndpoint(std::string_view text)
    {
        if (!SplitHostPort(text).has_value())
            return ParseTcpPort(text).has_value();
        return ParsesAsBeaconAddress(text);
    }

    /// Resolve a `[<host>:]<port>` spec against the host a bare port falls back to.
    ///
    /// Through `ParseEndpoint` rather than `SplitHostPort`, and the difference is the
    /// default host -- which is right for a BIND address an operator typed and wrong
    /// for text naming somewhere else to ask. `Core/HostPort.hpp` states that
    /// distinction where `ParseDialEndpoint` is defined; these are the bind side of
    /// it.
    ///
    /// An empty spec resolves to nothing, which is how four of the six surfaces spell
    /// "not served". That is the same answer an unusable spec gives, deliberately:
    /// the grammar refusal happens once at startup where it can name the flag, and a
    /// port map has nothing to draw for either.
    /// @param spec What the operator typed.
    /// @param defaultHost The host a bare port takes.
    /// @return The one endpoint, or none.
    [[nodiscard]] SurfaceEndpoints ResolveSpec(std::string const& spec, std::string_view defaultHost)
    {
        auto const endpoint = ParseEndpoint(spec, defaultHost);
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
            .hostOrigin = HostOrigin::OperatorFlag,
            .defaultHost = {},
            // No spec: the halves are a `std::string` and a `std::uint16_t` with
            // their own value parsers, so there is no text for a grammar to judge.
            .spec = nullptr,
            .parses = nullptr,
            .shape = {},
            .resolve = [](NodeConfig const& cfg) -> SurfaceEndpoints {
                return SurfaceEndpoints { SurfaceEndpoint { .host = cfg.bindAddress, .port = cfg.port } };
            },
            .note = "a systemd .socket unit overrides --bind and --port entirely; the unit then owns the "
                    "address and this process is never told which port it got, so --advertise is what names "
                    "where clients actually go",
        },
        SurfaceRow {
            .surface = NodeSurface::Cache,
            .name = "cache",
            .flags = { "--listen-cache", {} },
            .protocol = SurfaceProtocol::Tcp,
            .hostOrigin = HostOrigin::DefaultConstant,
            .defaultHost = CacheListenDefaultHost,
            .spec = &NodeConfig::cacheListen,
            .parses = ParsesAsListenEndpoint,
            .shape = "[<address>:]<port>",
            .resolve = [](NodeConfig const& cfg) -> SurfaceEndpoints {
                return ResolveSpec(cfg.cacheListen, CacheListenDefaultHost);
            },
            .note = "loopback for a bare port, the opposite of the scheduler and raft: this machine's whole "
                    "build output is served here, so widening it is a decision rather than a typo",
        },
        SurfaceRow {
            .surface = NodeSurface::Scheduler,
            .name = "scheduler",
            .flags = { "--listen-scheduler", {} },
            .protocol = SurfaceProtocol::Tcp,
            .hostOrigin = HostOrigin::DefaultConstant,
            .defaultHost = SchedulerListenDefaultHost,
            .spec = &NodeConfig::schedulerListen,
            .parses = ParsesAsListenEndpoint,
            .shape = "[<address>:]<port>",
            .resolve = [](NodeConfig const& cfg) -> SurfaceEndpoints {
                return ResolveSpec(cfg.schedulerListen, SchedulerListenDefaultHost);
            },
            .note = "answered only while this node LEADS; a follower redirects and an election in progress "
                    "refuses, so the port is open on every member whether or not it is answering today",
        },
        SurfaceRow {
            .surface = NodeSurface::Admin,
            .name = "admin",
            .flags = { "--admin-listen", {} },
            .protocol = SurfaceProtocol::Tcp,
            .hostOrigin = HostOrigin::DefaultConstant,
            .defaultHost = AdminListenDefaultHost,
            .spec = &NodeConfig::adminListen,
            .parses = ParsesAsListenEndpoint,
            .shape = "[<address>:]<port>",
            .resolve = [](NodeConfig const& cfg) -> SurfaceEndpoints {
                return ResolveSpec(cfg.adminListen, AdminListenDefaultHost);
            },
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
            .hostOrigin = HostOrigin::DefaultConstant,
            .defaultHost = RaftListenDefaultHost,
            .spec = &NodeConfig::raftListen,
            .parses = ParsesAsListenEndpoint,
            .shape = "[<address>:]<port>",
            .resolve = [](NodeConfig const& cfg) -> SurfaceEndpoints {
                // Consensus is what `--node-id` turns on, so a `--listen-raft` with
                // no id binds nothing -- the surface is configured and not served.
                if (cfg.nodeId.empty())
                    return {};
                return ResolveSpec(cfg.raftListen, RaftListenDefaultHost);
            },
            .note = "the wildcard for a bare port: peers are on other machines by definition, so a loopback "
                    "default would be one that silently cannot work. Served only when --node-id is given",
        },
        SurfaceRow {
            .surface = NodeSurface::Discovery,
            .name = "discovery",
            .flags = { "--discovery", "--discovery-reply-port" },
            .protocol = SurfaceProtocol::Udp,
            .hostOrigin = HostOrigin::Fixed,
            .defaultHost = DiscoveryBindHost,
            .spec = &NodeConfig::discoveryAddress,
            .parses = ParsesAsBeaconAddress,
            .shape = "<address>:<port>",
            .resolve = [](NodeConfig const& cfg) -> SurfaceEndpoints {
                auto const beacon = SplitHostPort(cfg.discoveryAddress);
                if (!beacon.has_value())
                    return {};
                auto const port = ParseTcpPort(beacon->second);
                if (!port.has_value())
                    return {};

                // Two sockets, and the second is why this row could not be one
                // endpoint: a node LISTENS on the beacon port, shared, and ANSWERS
                // on a port only it holds. An operator who opened the first and not
                // the second gets a fleet that hears every beacon and completes no
                // handshake.
                SurfaceEndpoints out;
                out.push_back(
                    SurfaceEndpoint { .host = std::string { DiscoveryBindHost }, .port = *port, .role = "beacon" });
                if (cfg.discoveryReplyPort != 0)
                    out.push_back(SurfaceEndpoint {
                        .host = std::string { DiscoveryBindHost }, .port = cfg.discoveryReplyPort, .role = "reply" });
                return out;
            },
            .note = "UDP, and the only surface that is. The address you write is where beacons are SENT; the "
                    "sockets always bind the wildcard. Without --discovery-reply-port the reply socket takes "
                    "an ephemeral port, which a restrictive firewall has to allow as outbound",
        },
    };

    static_assert(RowsInEnumeratorOrder(Surfaces, [](SurfaceRow const& row) { return row.surface; }),
                  "every NodeSurface needs a row, at its own index");

    // A surface with no name could not be printed, and one with no first flag could
    // not be configured. Checked at compile time because both are what a
    // half-written row looks like.
    static_assert(std::ranges::all_of(Surfaces,
                                      [](SurfaceRow const& row) {
                                          return !row.name.empty() && !row.flags[0].empty() && row.resolve != nullptr;
                                      }),
                  "every row needs a name, a first flag and a resolver");

    // A spec and its grammar travel together: text nothing validates is text an
    // operator can typo into a registration that replays forever, and a grammar with
    // no text to judge is a row that lies about having one.
    static_assert(std::ranges::all_of(Surfaces,
                                      [](SurfaceRow const& row) {
                                          return (row.spec == nullptr) == (row.parses == nullptr)
                                                 && (row.spec == nullptr) == row.shape.empty();
                                      }),
                  "a row carries a spec, its grammar and its shape, or none of the three");
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

SurfaceRow const& RowFor(NodeSurface surface) noexcept
{
    return Surfaces[static_cast<std::size_t>(surface)];
}

std::string RenderSurfaces(NodeConfig const& cfg)
{
    // Columns wide enough for the longest value, computed rather than guessed: a
    // worksheet an operator reads down is one whose addresses line up, and a
    // hard-coded width is one a longer surface name silently ragged.
    // Over EVERY row, not only the served ones. Measuring what is printed is the
    // whole point of computing a width, and the rows that print no address still
    // print a name -- so widths taken over served endpoints alone left the longest
    // names (`scheduler`, `discovery`, which are exactly the ones off by default)
    // hanging past a column sized without them.
    std::size_t nameWidth = 0;
    std::size_t addressWidth = 1; // the "-" an unserved row shows.
    for (auto const& row: Surfaces)
    {
        nameWidth = std::max(nameWidth, row.name.size());
        for (auto const& endpoint: row.resolve(cfg))
        {
            nameWidth = std::max(nameWidth, row.name.size() + endpoint.role.size() + (endpoint.role.empty() ? 0 : 1));
            addressWidth = std::max(addressWidth, std::format("{}:{}", endpoint.host, endpoint.port).size());
        }
    }

    std::string out;
    for (auto const& row: Surfaces)
    {
        auto const endpoints = row.resolve(cfg);
        if (endpoints.empty())
        {
            // Named rather than omitted. A surface missing from the list reads as one
            // this build does not have, and an operator cannot tell that from one
            // they did not turn on -- so the flags that would turn it on are given.
            auto flags = std::string {};
            for (auto const& flag: FlagsOf(row))
                flags += (flags.empty() ? "" : " ") + std::string { flag };
            out += std::format("{:<{}}  {:<{}}  {}\n", row.name, nameWidth, "-", addressWidth, "not served; set " + flags);
            continue;
        }

        for (auto const& endpoint: endpoints)
        {
            auto const label =
                endpoint.role.empty() ? std::string { row.name } : std::format("{} {}", row.name, endpoint.role);
            out += std::format("{:<{}}  {:<{}}  {}\n",
                               label,
                               nameWidth,
                               std::format("{}:{}", endpoint.host, endpoint.port),
                               addressWidth,
                               row.protocol == SurfaceProtocol::Udp ? "UDP" : "TCP");
        }
    }

    // The notes last and separately, because they are prose and the table above is
    // something an operator transcribes into firewall rules. Mixing them would make
    // the columns ragged for the rows that carry one -- and the compile port's note
    // is the one that says this list can be WRONG for that row, which is not
    // something to bury in a column.
    out += "\nnotes:\n";
    for (auto const& row: Surfaces)
        if (!row.note.empty())
            out += std::format("  {}: {}\n", row.name, row.note);
    return out;
}

} // namespace FastCache::Node
