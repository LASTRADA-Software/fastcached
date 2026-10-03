// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file SrvResolver.hpp
/// A DNS SRV lookup, behind a seam: how a node that is not on the office LAN finds the fleet
/// across a VPN, through `_fastcache._tcp.<domain>` (`Cluster/SeedSources.hpp`).
///
/// A lookup that fails, times out or finds nothing answers NO targets and a `SrvLookupFault` the
/// caller can log; it never throws. And it is only ever asked a name: a machine with no DNS
/// domain makes no SRV query at all (`SrvQueryName` answers empty), which is a different fact
/// from a query that was made and answered nothing, and must not be reported as one.
///
/// Not cached, deliberately. A stale target is a dial to a machine that may no longer be the
/// fleet's -- a wrong answer that looks right, not a miss -- so staleness is the caller's to
/// bound, by asking again.
namespace FastCache
{

/// A DNS SRV record, as a resolver hands one back.
struct SrvTarget
{
    std::string host;             ///< The target host, without a trailing root dot.
    std::uint16_t port { 0 };     ///< The target port.
    std::uint16_t priority { 0 }; ///< Lower is tried first.
    std::uint16_t weight { 0 };   ///< Higher is tried first among equal priorities.

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(SrvTarget const&, SrvTarget const&) = default;
};

/// Why a lookup answered nothing. Private: never transmitted or persisted.
enum class SrvLookupFault : std::uint8_t
{
    NoSuchName,     ///< The name does not exist in DNS (NXDOMAIN).
    NoAnswer,       ///< The name exists and holds no usable SRV record.
    ResolverFailed, ///< The resolver could not answer: unreachable, timed out, or unreadable.
    Last,
};

/// What a fault is called in a log line.
struct SrvLookupFaultRow
{
    SrvLookupFault fault;  ///< The fault this row describes.
    std::string_view name; ///< Its name in a log line.
};

/// Every fault's name.
inline constexpr EnumTable<SrvLookupFault, SrvLookupFaultRow> SrvLookupFaultTable { {
    { .fault = SrvLookupFault::NoSuchName, .name = "no-such-name" },
    { .fault = SrvLookupFault::NoAnswer, .name = "no-answer" },
    { .fault = SrvLookupFault::ResolverFailed, .name = "resolver-failed" },
} };
static_assert(RowsInEnumeratorOrder(SrvLookupFaultTable, &SrvLookupFaultRow::fault),
              "SrvLookupFaultTable must hold one row per SrvLookupFault, in enumerator order");

/// @param fault A fault.
/// @return What a log line calls it.
[[nodiscard]] constexpr std::string_view SrvLookupFaultName(SrvLookupFault fault) noexcept
{
    return SrvLookupFaultTable[static_cast<std::size_t>(fault)].name;
}

/// Why a lookup answered no target, and what the records the answer did hold said.
///
/// **The counts are the second half of the answer, not decoration.** `NoAnswer` covers three
/// different DNS states, and each sends an operator somewhere else:
///
///   nothing counted   the answer held no record: nobody published one
///   `notOffered`      RFC 2782's root target: the domain says, correctly, that the service is
///                     decidedly not available there -- an ANSWER, not a broken record
///   `undialable`      a record naming a host nobody could dial: a record published wrong
///
/// Folding the second into the third would tell the operator who deliberately published "not
/// here" to go and fix a record that is right, which is a confident wrong signal.
struct SrvLookupFailure
{
    SrvLookupFault fault {};      ///< Why there are no targets.
    std::size_t undialable { 0 }; ///< Records naming a host `IsDialableSrvTarget` refused.
    std::size_t notOffered { 0 }; ///< Root targets (`IsRootSrvTarget`): "decidedly not available here".

    /// Field-wise equality.
    [[nodiscard]] friend bool operator==(SrvLookupFailure const&, SrvLookupFailure const&) = default;
};

/// A failure with nothing dropped: every fault decided before any record was read.
/// @param fault The fault.
/// @return The failure.
[[nodiscard]] constexpr SrvLookupFailure SrvFailureOf(SrvLookupFault fault) noexcept
{
    return SrvLookupFailure { .fault = fault, .undialable = 0, .notOffered = 0 };
}

/// What a log line says about a lookup that answered no target.
/// @param failure The failure.
/// @return `<fault>`, then after a colon one clause per non-zero count: the root targets saying
///         the service is not offered, and the targets dropped as undialable -- each by name, so
///         a log line never leaves the reader to infer which of the two it was.
[[nodiscard]] std::string DescribeSrvLookupFailure(SrvLookupFailure const& failure);

/// The longest one system lookup may keep its caller waiting.
///
/// Enforced on Windows, where `DnsQueryEx` runs asynchronously, with the name treated as fully
/// qualified so an NXDOMAIN does not walk the suffix search list, and is cancelled at this deadline.
/// **The deadline bounds the wait for an ANSWER, not the whole call**: the frame then waits for the
/// DNS client's completion routine, which writes into it. A cancellation that takes completes at
/// once (measured: a zero deadline returned in 32-50 ms); one `DnsCancelQuery` refuses -- the query
/// was already completing -- is bounded by the client's own query timeout, a registry retry
/// schedule this process does not set, and its answer, when it has one, is returned.
/// On POSIX `res_nquery` cannot be cancelled, so the resolver is CONFIGURED to finish inside it
/// instead -- one pass over at most `MAXNS` servers at a fixed per-server wait, and never over
/// TCP (`use-vc` cleared, a truncated answer not retried), whose blocking connect and read
/// nothing here could bound -- and `SrvResolver.cpp` `static_assert`s that the configuration fits.
inline constexpr std::chrono::seconds SrvLookupDeadline { 6 };

/// A DNS SRV lookup.
class ISrvResolver
{
  public:
    ISrvResolver() = default;
    ISrvResolver(ISrvResolver const&) = delete;
    ISrvResolver(ISrvResolver&&) = delete;
    ISrvResolver& operator=(ISrvResolver const&) = delete;
    ISrvResolver& operator=(ISrvResolver&&) = delete;
    virtual ~ISrvResolver() = default;

    /// Look up the SRV records for @p name.
    ///
    /// **Precondition: @p name is not empty.** No name is no query -- the caller says so itself
    /// rather than asking and reading the fault as the answer.
    /// @param name The SRV owner name, e.g. `_fastcache._tcp.corp.example`.
    /// @return The usable targets in the order the resolver gave them (never empty), or why
    ///         there are none.
    [[nodiscard]] virtual std::expected<std::vector<SrvTarget>, SrvLookupFailure> Lookup(std::string_view name) const = 0;
};

/// The platform's resolver: `DnsQueryEx` on Windows, `res_nquery` over a private `res_state`
/// elsewhere. Thread-safe; bounded by `SrvLookupDeadline`.
/// @return The system resolver. Never null.
[[nodiscard]] std::unique_ptr<ISrvResolver> MakeSystemSrvResolver();

/// What a platform's resolver reported, before it is classified.
///
/// Private: filled by each platform's implementation from its resolver's own return code, and
/// never transmitted or persisted.
enum class SrvStatus : std::uint8_t
{
    NameError,   ///< The name does not exist (NXDOMAIN).
    EmptyAnswer, ///< The name exists and the answer held no records of the type asked.
    Other,       ///< Anything else: a timeout, an unreachable server, a refusal, a parse failure.
    Last,
};

/// Which fault a resolver status is.
/// @param status What the platform's resolver reported.
/// @return The fault a caller logs.
[[nodiscard]] SrvLookupFault SrvFaultOf(SrvStatus status) noexcept;

/// Whether a resolver's answer arrived whole.
///
/// Private: never transmitted or persisted.
enum class SrvAnswerExtent : std::uint8_t
{
    Whole,     ///< Every record the answer declared was read.
    Truncated, ///< The answer was cut short -- flagged TC, or a record past the first would not parse.
    Last,
};

/// Whether an SRV target is a host name this node would dial.
///
/// ONE predicate, applied by `SrvAnswerOf` to every platform's answer, so a record the Windows DNS
/// client hands back and the same record parsed from bytes on POSIX are judged alike. A host name
/// here is dot-separated labels of ASCII letters, digits, `-` and `_` (the underscore for the
/// service labels SRV itself uses), none empty, none starting or ending with `-`, none over 63
/// characters, and at most 253 characters in all -- the presentation form of a name that fits
/// DNS's 255 wire bytes. Everything else is refused: `:` `[` `]` `%` `@` `/` and whitespace, a
/// backslash escape, a trailing root dot, and the empty name and `.`, which are RFC 2782's "the
/// service is decidedly not available at this domain" (`IsRootSrvTarget`).
/// @param host A target's host, in presentation form.
/// @return Whether it names a host this node would dial.
[[nodiscard]] bool IsDialableSrvTarget(std::string_view host) noexcept;

/// Whether an SRV target is the root: RFC 2782's "the service is decidedly not available at this
/// domain". `.` in presentation form, and the empty name a parser reads the root label as.
/// @param host A target's host, in presentation form.
/// @return Whether it is the root.
[[nodiscard]] constexpr bool IsRootSrvTarget(std::string_view host) noexcept
{
    return host.empty() || host == ".";
}

/// The targets an answer offers, or why it offers none.
///
/// A root target (`IsRootSrvTarget`) and a target failing `IsDialableSrvTarget` are dropped, and
/// COUNTED APART: the first is the domain's deliberate "not here", the second a record published
/// wrong. What remains is the answer, in the order given. Nothing remaining is `NoAnswer` when the
/// answer was whole, but `ResolverFailed` when it was truncated: a record may exist that did not
/// fit, so "nothing here" is not what was learned. Either way the failure carries both counts.
/// @param records Every SRV record the answer's answer section held.
/// @param extent  Whether the answer was read whole.
/// @return The usable targets (never empty), or the fault and what was dropped, by kind.
[[nodiscard]] std::expected<std::vector<SrvTarget>, SrvLookupFailure> SrvAnswerOf(std::vector<SrvTarget> records,
                                                                                  SrvAnswerExtent extent);

/// The SRV targets a DNS response message holds, or why it holds none.
///
/// The one reader of raw DNS bytes in this tree, and pure so every platform's tests drive it: the
/// POSIX resolver hands it what `res_nquery` returned, and Windows, whose DNS client parses for
/// itself, never reaches it in production. The bytes came off the network, so every length is
/// checked against the message before it is used -- a count, an RDATA length, a label, a
/// compression pointer -- and nothing is read that the message does not hold.
///
/// - The header's RCODE decides first: NXDOMAIN is `NoSuchName`, any other non-zero code
///   `ResolverFailed`. A message shorter than a header, or whose question section will not parse,
///   is `ResolverFailed`.
/// - Only the ANSWER section is read, and in it only class-IN SRV records: a CNAME the name led
///   through, a record of another type, and everything in the authority and additional sections
///   are skipped.
/// - A compression pointer must point strictly BACKWARD and past the header, and a name is at most
///   255 bytes on the wire, which together bound the walk. A label byte a host name cannot hold --
///   not printable ASCII, or a `.` or `\` -- is kept as a `\DDD` escape, so the text says what the
///   wire said and `IsDialableSrvTarget` drops the target rather than dialling another name.
/// - The TC flag, a record that runs past the message, and an SRV record whose RDATA does not hold
///   a target -- too short, or a target name that does not end exactly where the RDATA does -- each
///   make the answer `Truncated` for `SrvAnswerOf`; records before a cut are kept.
/// @param message A DNS response, as a resolver returned it.
/// @return The usable targets (never empty), or the fault.
[[nodiscard]] std::expected<std::vector<SrvTarget>, SrvLookupFailure> SrvTargetsInMessage(
    std::span<std::uint8_t const> message);

} // namespace FastCache
