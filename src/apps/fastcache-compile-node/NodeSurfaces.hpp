// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <array>
#include <cstdint>
#include <expected>
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

/// A listen-value grammar and the shape a refusal shows for it.
///
/// Together, never apart: the predicate decides and the string is what an operator is
/// told to type, so a row pairing one surface's predicate with another's shape would
/// refuse a value while advertising the spelling that produces it.
struct Grammar
{
    bool (*parses)(std::string_view) = nullptr; ///< Whether the text is acceptable; null when a surface has no spec.
    std::string_view shape {};                  ///< What it should have looked like.
};

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

    /// The host a bare port falls back to, and the only host discovery ever binds.
    ///
    /// There is deliberately **no** `HostOrigin` column beside it saying which of
    /// those two this is. One was written -- `DefaultConstant` / `OperatorFlag` /
    /// `Fixed` -- and nothing in production ever read it: it documented rather than
    /// drove, its only test asserted it agreed with the column it was derived from,
    /// and its own doc had already gone stale against its own table, claiming this
    /// field is empty unless the origin is `DefaultConstant` while discovery was
    /// `Fixed` with a non-empty one. That is the third column here to fail the same
    /// test, after `presence` and the `explicitBit` that was never added.
    ///
    /// The distinction it tried to record is real and lives in `note`, where prose
    /// belongs: `--discovery`'s host half is where beacons are SENT, and its sockets
    /// bind this wildcard whatever the operator wrote. What ENFORCES that is
    /// discovery's resolver, not an enumerator -- so the protection is in the code
    /// that produces the address rather than in a label describing it.
    std::string_view defaultHost {};

    /// Where the raw `[<host>:]<port>` text lives, or null when there is none.
    ///
    /// Null for the compile port alone: its halves are a `std::string` and a
    /// `std::uint16_t` validated by their own value parsers, never a spec string, so
    /// there is nothing here for a grammar to check.
    std::string NodeConfig::* spec = nullptr;

    /// The grammar `spec` must satisfy, and the shape a refusal shows for it.
    ///
    /// **One column, so a mismatch is unrepresentable.** They were two -- a predicate
    /// and a `shape` string -- and nothing could check that the right shape sat beside
    /// the right predicate: a row pairing the bare-port-accepting grammar with
    /// `<address>:<port>` compiled, and told an operator to write something the parser
    /// would then refuse. The `static_assert` could only ask whether both were present.
    /// Paired in one constant, the wrong combination cannot be written down.
    Grammar grammar {};

    /// How this surface resolves.
    ///
    /// Every row has one; there is no "null means the default" case to get wrong. The
    /// three whose resolution is entirely their own columns share `ResolveFromSpec`,
    /// which reads `spec` and `defaultHost` off the row rather than naming them a
    /// second time -- they used to carry a lambda restating the very member pointer and
    /// constant the row already held, which is two copies of one fact inside the table
    /// built to delete exactly that.
    ///
    /// It takes its own row, which is what makes that sharing possible, and is why raft
    /// can gate on `--node-id` and then delegate rather than re-spelling the fallback.
    /// Call it through `Resolve`.
    SurfaceEndpoints (*resolve)(SurfaceRow const&, NodeConfig const&) = nullptr;

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

    /// What @p cfg would bind for this surface: none, one, or two endpoints.
    /// @param cfg What the operator asked for.
    /// @return The endpoints it would bind; empty when it is not served.
    [[nodiscard]] SurfaceEndpoints Resolve(NodeConfig const& cfg) const
    {
        return resolve(*this, cfg);
    }
};

/// Every listening surface, one row each, indexed by `NodeSurface`.
///
/// @return The table; stable for the life of the process.
[[nodiscard]] std::array<SurfaceRow, EnumeratorCount<NodeSurface>> const& NodeSurfaceTable() noexcept;

/// The flags a row names, without the empty second entry.
/// @param row The surface to read.
/// @return One or two flag spellings.
[[nodiscard]] std::vector<std::string_view> FlagsOf(SurfaceRow const& row);

/// The flag a refusal about this surface should name.
///
/// `flags[0]`, which the table `static_assert`s non-empty -- so this is the invariant
/// the build already proves rather than a vector materialised to read one entry.
/// @param row The surface to read.
/// @return Its primary flag spelling.
[[nodiscard]] std::string_view PrimaryFlag(SurfaceRow const& row) noexcept;

/// The row describing @p surface.
/// @param surface Which surface.
/// @return Its row.
[[nodiscard]] SurfaceRow const& RowFor(NodeSurface surface) noexcept;

/// The single endpoint @p surface would bind, or why it would bind none.
///
/// For the five surfaces served by exactly one socket, which is every one but
/// discovery. Written once because it was written four times: each opener resolved
/// the row, refused an empty result and took `front()`, and the three refusals said
/// three different things about one condition -- so an operator met a different
/// sentence per surface for an identical fault, and a fifth TCP surface would have
/// re-derived it a fifth time. That is the shape this table exists to delete,
/// reappearing at the table's own consumers.
/// @param surface Which surface to open.
/// @param cfg What the operator asked for.
/// @return Its endpoint, or a message naming the flag that failed to supply one.
[[nodiscard]] std::expected<SurfaceEndpoint, std::string> SoleEndpointOf(NodeSurface surface, NodeConfig const& cfg);

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
