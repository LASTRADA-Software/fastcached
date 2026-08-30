// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

/// A port this node LISTENS on.
///
/// The vocabulary is surfaces rather than flags, because two of the six are not one
/// flag: the compile port is `--bind` and `--port` together, and discovery is an
/// address plus an optional private reply port. A `ListenFlag` enum would have had
/// to leave both out, which is how the port map came to live in five places.
///
/// `--advertise` is deliberately **not** here. It is what this node tells other
/// machines to dial, not a socket it opens -- it defaults to `{--bind}:{--port}` and
/// binds nothing -- so a firewall rule derived from it would open a port twice and
/// still miss whatever the supervisor actually chose.
enum class NodeSurface : std::uint8_t
{
    Compile = 0, ///< Where clients send translation units to be compiled.
    Cache,       ///< Where `fastcache-cc` on this machine reads and writes objects.
    Scheduler,   ///< Where the fleet asks this node for capacity, while it leads.
    Admin,       ///< `/metrics`, `/healthz` and the fleet dashboard.
    Raft,        ///< Where this node's peers send consensus traffic.
    Discovery,   ///< The LAN beacon, and the port it is answered on.
    Last,        ///< Count; never a surface.
};

/// Whether a surface speaks TCP or UDP.
///
/// A column rather than an assumption, and it is load-bearing for the one row that
/// is not TCP: an operator handed a six-row worksheet writes six rules, and if one of
/// them silently needs UDP the beacon reaches nobody. That presents as a fleet that
/// never forms -- a diagnosis costing hours, pointed at consensus rather than at a
/// firewall -- because every other surface works perfectly.
enum class SurfaceProtocol : std::uint8_t
{
    Tcp = 0,
    Udp,
};

/// Where a surface's host comes from when the operator wrote only a port.
///
/// Three mechanisms supply it across the six surfaces, and flattening them would be
/// its own drift: a fallback a bare port takes and a value an operator sets are
/// different facts that merely happen to both be strings.
enum class HostOrigin : std::uint8_t
{
    /// A named constant this surface FALLS BACK to -- `CacheListenDefaultHost` and
    /// its three siblings. The operator overrides it by writing a host.
    DefaultConstant = 0,
    /// A flag of its own. Only the compile port, whose host is `--bind`: there is no
    /// bare-port case to fall back from, because the two halves are separate values
    /// of separate types.
    OperatorFlag,
    /// Always this host, whatever the operator wrote. Only discovery, and the
    /// distinction from `DefaultConstant` is not pedantry: `--discovery`'s host half
    /// is the address this node ANNOUNCES to, while the socket binds the wildcard
    /// unconditionally (`DiscoveryTier.cpp`). An operator who writes
    /// `--discovery=255.255.255.255:6681` has changed where beacons go and not what
    /// is bound, so reading that host as the bind address would put the wrong
    /// address on a firewall worksheet.
    Fixed,
};

/// One endpoint a surface would actually bind.
///
/// Owning, deliberately. `role` points into the static table and outlives any call;
/// `host` is built by resolving a spec against a default and must not borrow from the
/// configuration it was derived from, which is the rule a `*View` type would be
/// announcing and this one is not.
struct SurfaceEndpoint
{
    std::string host;         ///< The address that would be bound.
    std::uint16_t port {};    ///< The port that would be bound.
    std::string_view role {}; ///< Empty for a surface's only endpoint; names the second when there is one.
};

/// What a surface would bind under one configuration: none, one, or two.
///
/// "One or two endpoints" rather than "one or two flags", because the two awkward
/// surfaces strain the row in different directions and only this shape absorbs both.
/// The compile port is two flags of two types; discovery is one spec plus an optional
/// second port, opened as a shared listen socket and a private reply socket. A row
/// built for two flags takes the first and not the second.
using SurfaceEndpoints = std::vector<SurfaceEndpoint>;

/// One listening surface, described once.
///
/// This is the port map. Every consumer -- the code that OPENS the surface, the
/// install-time grammar refusal, `--print-surfaces`, and the operator documentation
/// -- reads these rows, so that adding a surface is adding a row rather than a row
/// plus edits in four other places that can each be forgotten separately.
struct SurfaceRow
{
    /// Which surface this row describes; its own index in the table.
    NodeSurface surface {};

    /// The operator-facing label, and the tag the startup lines already use.
    std::string_view name;

    /// The flags that configure it: one, or two for the compile port and discovery.
    ///
    /// One field rather than a `flag` plus a `secondFlag` that is empty for four rows
    /// out of six. The second entry is empty when the surface takes one flag, and
    /// `FlagsOf` hands back only what is populated.
    std::array<std::string_view, 2> flags {};

    /// TCP or UDP.
    SurfaceProtocol protocol {};

    // There is deliberately **no** `presence` column. It was written, and it was
    // both redundant and wrong.
    //
    // Redundant, because `resolve` already answers it: a surface is served exactly
    // when it resolves to an endpoint, so "on by default" is `resolve(NodeConfig{})`
    // and a column restating that is a second author of one fact -- which is the
    // thing this table exists to stop.
    //
    // Wrong, because its own contract could not survive the raft row. "Not served
    // until an operator names an address" is false there: `--listen-raft` names one
    // and binds nothing until `--node-id` turns consensus on, so the column would
    // have rendered "not requested" for a surface the operator did request. A cell
    // that misdescribes why a port is closed is the same class of defect as one that
    // misdescribes where it is open.
    //
    // What a reader needs beyond "not served" is *why*, and that is not uniform
    // enough to be an enumerator: raft's reason is knowable from the configuration
    // and the compile port's is not knowable at all. Each row says its own in `note`.

    /// Where its host comes from for a bare port.
    HostOrigin hostOrigin {};

    /// The constant a bare port falls back to; empty when `hostOrigin` is not
    /// `DefaultConstant`.
    std::string_view defaultHost {};

    /// Where the raw `[<host>:]<port>` text lives, or null when there is none.
    ///
    /// Null for the compile port alone: its halves are a `std::string` and a
    /// `std::uint16_t` validated by their own value parsers, never a spec string, so
    /// there is nothing here for a grammar to check.
    std::string NodeConfig::* spec = nullptr;

    /// The grammar `spec` must satisfy when given; null when `spec` is.
    bool (*parses)(std::string_view) = nullptr;

    /// What `spec` should have looked like, for a refusal an operator can act on.
    std::string_view shape {};

    /// What this configuration would actually bind.
    ///
    /// The one column both the openers and `--print-surfaces` read, which is what
    /// makes them incapable of disagreeing. Empty means the surface is not served.
    SurfaceEndpoints (*resolve)(NodeConfig const&) = nullptr;

    /// What an operator has to know about this row that the columns cannot say.
    ///
    /// Empty for most. It carries the two facts that are neither a port nor a host
    /// and would otherwise be lost: that a `.socket` unit overrides the compile
    /// port's flags entirely, and that the admin surface's loopback default is what
    /// its credential rule turns on.
    ///
    /// The compile port's note is a **third** instance of one rule rather than a new
    /// observation -- "a flag that describes nothing under socket activation cannot
    /// answer whether a port faces the network"
    /// ([`distributed-compilation.md`](../../../.agent/rules/distributed-compilation.md)).
    /// `--bind` answered the wrong question for the lease validator; here it answers
    /// the wrong question for a firewall worksheet. Same flag, same mechanism, a
    /// different consumer each time, which is why the note points at the rule instead
    /// of restating the reasoning: a reader who meets it here should land on the
    /// general statement rather than on one instance of it.
    ///
    /// **The row states what this CONFIGURATION would bind; it never claims to know
    /// whether a listener was inherited.** That fact belongs to the running node,
    /// which has it (`main.cpp` binds only when nothing was handed over), and not to
    /// this table -- because the table's other consumer, `--print-surfaces`, is run
    /// by hand and is therefore never under the supervisor that would set it. An
    /// enumerator that cannot be computed truthfully where it is read would look
    /// authoritative and be wrong for exactly the deployed service it described.
    std::string_view note {};
};

/// Every listening surface, one row each, indexed by `NodeSurface`.
///
/// @return The table; stable for the life of the process.
[[nodiscard]] std::array<SurfaceRow, EnumeratorCount<NodeSurface>> const& NodeSurfaceTable() noexcept;

/// The flags a row names, without the empty second entry.
/// @param row The surface to read.
/// @return One or two flag spellings.
[[nodiscard]] std::vector<std::string_view> FlagsOf(SurfaceRow const& row);

/// The row describing @p surface.
/// @param surface Which surface.
/// @return Its row.
[[nodiscard]] SurfaceRow const& RowFor(NodeSurface surface) noexcept;

/// Every port this configuration would open, as a firewall worksheet.
///
/// **The RESOLVED configuration, never the defaults.** A worksheet naming a port
/// nothing listens on is a wasted rule; one claiming `127.0.0.1` for a node started
/// with `--listen-cache 0.0.0.0:6674` tells an operator a surface is loopback-only
/// when it is open to the network. Those are not two versions of one mistake: the
/// first is untidy and the second is a security misstatement, which is why this
/// takes a configuration rather than rendering the table alone.
///
/// The protocol column is not decoration. Discovery is UDP and the other five are
/// TCP, so a worksheet without it yields five correct rules and one wrong one -- and
/// a beacon that reaches nobody presents as a fleet that never forms rather than as
/// a firewall mistake.
///
/// What it cannot know, it says rather than guesses: under socket activation the
/// unit owns the compile port and this process is never told which port it got, so
/// that row carries a note instead of a claim. `--print-surfaces` is run by hand and
/// is therefore never under the supervisor that would tell it, which is why the fact
/// is a note on the row and not a value in a column.
/// @param cfg What the operator asked for.
/// @return The worksheet, one line per endpoint plus each row's notes.
[[nodiscard]] std::string RenderSurfaces(NodeConfig const& cfg);

} // namespace FastCache::Node
