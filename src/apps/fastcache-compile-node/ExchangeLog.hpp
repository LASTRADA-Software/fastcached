// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Node
{

/// What a node says about each 0xFC exchange it answers, and at which level.
///
/// **A node that says nothing about its clients cannot be investigated.** Before this
/// the whole request path was silent: `WorkerProtocol.cpp` held no logger reference at
/// all, and a live node's journal was startup and the toolchain survey and then
/// nothing, however many compiles it served. An operator watching
/// `journalctl -u fastcache-compile-node -f` through an entire build saw not one line,
/// so "is anything reaching this node" was answerable only from `/metrics` -- which
/// tallies but does not say WHO, WHEN, or WHICH KEY, and is therefore the wrong
/// instrument for the question an operator actually asks first.
///
/// The counters stay the load-bearing signal; this is the narrative beside them, and
/// the two answer different questions. A counter says a refusal happened; a line says
/// which peer was refused which verb at what time.
///
/// ## Why a level per VERB rather than one level for the surface
///
/// The rates differ by orders of magnitude and so does what a line is worth. A build
/// issues thousands of `fetch`/`store` exchanges, one per translation unit or more --
/// at `Info` those bury every other line in the journal, which is how a log stops
/// being read at all. A `lease` or a `compile` is one per translation unit
/// DISTRIBUTED, takes seconds, and is exactly what an operator is looking for. So the
/// level is a column, `Debug` for the high-rate verbs and `Info` for the ones whose
/// arrival is itself the event.
///
/// The consequence is deliberate: at the default level the journal shows scheduling
/// and distribution, and `--log-level=debug` adds the cache traffic underneath it.
struct VerbLogRow
{
    CompileCacheWire::Op code; ///< The verb this row governs.
    LogLevel level;            ///< The level its one-line record is emitted at.
    /// Why this verb sits at this level.
    ///
    /// A required field, not decoration. "High-rate, so Debug" and "nobody thought
    /// about it" are the same silence otherwise, which is the argument
    /// `RefuseWithoutCounter` makes one layer down in
    /// `.agent/rules/metrics-and-observability.md`: a considered decision and an
    /// omission must not be spelled the same way.
    std::string_view rationale;
};

/// The level for every verb in `OpTable`, in its order.
inline constexpr std::array ExchangeLogTable {
    VerbLogRow { .code = CompileCacheWire::Op::Store,
                 .level = LogLevel::Debug,
                 .rationale = "one or more per translation unit; at Info a single build buries the journal" },
    VerbLogRow { .code = CompileCacheWire::Op::Fetch,
                 .level = LogLevel::Debug,
                 .rationale = "the highest-rate verb on this surface -- every TU asks before it compiles" },
    VerbLogRow { .code = CompileCacheWire::Op::Auth,
                 .level = LogLevel::Debug,
                 .rationale = "once per connection, but a connection is per TU; the outcome that MATTERS is "
                              "the refusal, which is counted and logged by the refusal path regardless" },
    VerbLogRow { .code = CompileCacheWire::Op::Register,
                 .level = LogLevel::Info,
                 .rationale = "a worker joining is rare and is what an operator checks first when the fleet "
                              "distributes nothing" },
    VerbLogRow { .code = CompileCacheWire::Op::Heartbeat,
                 .level = LogLevel::Trace,
                 .rationale = "the only verb that arrives whether or not anybody is building, so at Debug it is the "
                              "one line that makes an IDLE node's journal grow; below the cache verbs "
                              "deliberately, since a reader who turns on Debug wants the traffic, not the "
                              "pulse. What an operator wants from a heartbeat is ABSENCE, which no line can "
                              "carry and the registry's liveness view can" },
    VerbLogRow { .code = CompileCacheWire::Op::Lease,
                 .level = LogLevel::Info,
                 .rationale = "the scheduling decision itself -- the verb whose silence prompted this table" },
    VerbLogRow { .code = CompileCacheWire::Op::Release,
                 .level = LogLevel::Info,
                 .rationale = "a lease's third transition; logged with the grant so a lease's life reads as a "
                              "pair rather than a start with no end" },
    VerbLogRow { .code = CompileCacheWire::Op::ClusterStatus,
                 .level = LogLevel::Info,
                 .rationale = "an operator action, by hand, at human rate" },
    VerbLogRow { .code = CompileCacheWire::Op::ClusterSet,
                 .level = LogLevel::Info,
                 .rationale = "changes what every node in the fleet believes; an unlogged one is unauditable" },
    VerbLogRow { .code = CompileCacheWire::Op::ClusterForget,
                 .level = LogLevel::Info,
                 .rationale = "removes a member; same audit argument as cluster-set" },
    VerbLogRow { .code = CompileCacheWire::Op::ClusterAdmit,
                 .level = LogLevel::Info,
                 .rationale = "adds or moves a member; same audit argument as cluster-set" },
    VerbLogRow { .code = CompileCacheWire::Op::Compile,
                 .level = LogLevel::Info,
                 .rationale = "seconds of somebody's CPU, one per distributed TU; the heaviest thing this node "
                              "does and the cheapest to log" },
};

/// Whether every verb this build serves states a log level.
///
/// The same shape as `EveryVerbHasAFamily` in `CompileCacheWire.hpp`, and for the
/// same reason: a verb added to `OpTable` with no row here would be served and
/// narrated nowhere, and the omission is invisible -- a silent verb looks exactly
/// like a verb nobody used. `Op` is sparse and carries no trailing `Last`, so this
/// cannot be an `EnumTable`; the coverage assert is what replaces it.
/// @return True when every `OpTable` row has exactly one `ExchangeLogTable` row.
[[nodiscard]] constexpr bool EveryVerbHasALogLevel() noexcept
{
    return std::ranges::all_of(CompileCacheWire::OpTable, [](CompileCacheWire::OpDescriptor const& row) {
        return std::ranges::count_if(ExchangeLogTable, [&](VerbLogRow const& r) { return r.code == row.code; }) == 1;
    });
}

static_assert(EveryVerbHasALogLevel(), "a verb this node serves must state the level its exchanges are logged at");

/// Whether every level choice carries its reason.
/// @return True when no row leaves `rationale` empty.
[[nodiscard]] constexpr bool EveryLogLevelIsExplained() noexcept
{
    return std::ranges::all_of(ExchangeLogTable, [](VerbLogRow const& row) { return !row.rationale.empty(); });
}

static_assert(EveryLogLevelIsExplained(), "a level chosen without a reason cannot be told from one nobody chose");

/// The level one exchange is recorded at.
///
/// An opcode with no row -- one this build does not implement -- is `Debug`. It is
/// still worth a line, because "a client keeps asking for a verb I do not serve" is a
/// real diagnosis, but it is attacker-reachable on any open port and so must not be
/// able to fill a disk at `Info`.
/// @param opRaw The opcode byte as it arrived, not yet validated.
/// @return The level for that verb.
[[nodiscard]] constexpr LogLevel LogLevelForOp(std::uint8_t opRaw) noexcept
{
    // A loop rather than `std::ranges::find_if`. `std::array`'s iterator is a raw
    // POINTER on libstdc++ and a class type on MSVC's STL, so the spelling that
    // satisfies `readability-qualified-auto` on the one fails to COMPILE on the other
    // -- `error C3535: cannot deduce type for 'const auto *const '`. There is no
    // portable `auto` form here, and naming the iterator type is worse than not
    // holding one.
    for (auto const& row: ExchangeLogTable)
        if (static_cast<std::uint8_t>(row.code) == opRaw)
            return row.level;
    return LogLevel::Debug;
}

/// The verb's stable name, or a hex spelling when this build has no row for it.
///
/// Falls back to the BYTE rather than to a word like "unknown", so two different
/// unimplemented opcodes do not render identically -- which is the difference
/// between "a client is probing" and "a client is one version ahead".
/// @param opRaw The opcode byte as it arrived.
/// @return A name suitable for a log line.
[[nodiscard]] inline std::string ExchangeVerbName(std::uint8_t opRaw)
{
    for (auto const& row: CompileCacheWire::OpTable)
        if (static_cast<std::uint8_t>(row.code) == opRaw)
            return std::string { row.name };
    return std::format("opcode-0x{:02x}", opRaw);
}

/// One row of the reply-status table: the wire byte and its name.
///
/// `CompileCacheWire.hpp` names every OP and every ERROR CODE and does not name the
/// four statuses, so this is the first place that needs to. Kept here rather than
/// pushed into the wire header because a name for a log line is a server concern and
/// that header is compiled into `fastcache-cc` without linking `FastCache`.
struct StatusNameRow
{
    CompileCacheWire::Status code; ///< The wire byte.
    std::string_view name;         ///< Its stable lower-case name.
};

/// Every status this wire defines.
inline constexpr std::array StatusNameTable {
    StatusNameRow { .code = CompileCacheWire::Status::Miss, .name = "miss" },
    StatusNameRow { .code = CompileCacheWire::Status::Ok, .name = "ok" },
    StatusNameRow { .code = CompileCacheWire::Status::Error, .name = "error" },
    StatusNameRow { .code = CompileCacheWire::Status::Progress, .name = "progress" },
};

/// The name of a reply's status byte.
///
/// An unrecognised byte renders as its hex spelling rather than as a word, for
/// `ExchangeVerbName`'s reason: two different malformed replies must not read the
/// same.
/// @param statusRaw The reply's first byte.
/// @return A name suitable for a log line.
[[nodiscard]] inline std::string ExchangeStatusName(std::uint8_t statusRaw)
{
    for (auto const& row: StatusNameTable)
        if (static_cast<std::uint8_t>(row.code) == statusRaw)
            return std::string { row.name };
    return std::format("status-0x{:02x}", statusRaw);
}

/// One exchange, rendered.
///
/// A pure function over what the endpoint already holds, so the whole decision is
/// testable without a socket, a reactor or a clock -- the reason it lives here and
/// not inline at the call site.
///
/// The reply's first byte is its status (`Status::Ok`/`Miss`/`Error`); a reply this
/// node declined to write at all is an empty span, which is a fourth outcome and is
/// named rather than folded into `error`. Those are four states and a `bool` would
/// collapse them.
///
/// @param opRaw        The opcode byte as it arrived.
/// @param peer         The peer's host, as the kernel reports it.
/// @param requestBytes Length of the request frame.
/// @param reply        The reply frame, empty when none was produced.
/// @param elapsed      How long the responder took.
/// @return The line to log.
[[nodiscard]] inline std::string FormatExchange(std::uint8_t opRaw,
                                                std::string_view peer,
                                                std::size_t requestBytes,
                                                std::span<std::byte const> reply,
                                                std::chrono::milliseconds elapsed)
{
    auto const status =
        reply.empty() ? std::string { "no-reply" } : ExchangeStatusName(static_cast<std::uint8_t>(reply.front()));
    return std::format("{} {} from {} -> {} ({} B in, {} B out, {} ms)",
                       "0xFC",
                       ExchangeVerbName(opRaw),
                       peer.empty() ? std::string_view { "<unknown peer>" } : peer,
                       status,
                       requestBytes,
                       reply.size(),
                       elapsed.count());
}

/// Record one exchange, if this logger is listening at that verb's level.
///
/// **A function rather than an `if` at the call site**, and that is not a style
/// preference: `ServeConnection` sits at 58 of clang-tidy's cognitive-complexity
/// threshold of 60 on master (#675), so one more branch there fails the build. The
/// level test belongs with the table that decides the level anyway.
///
/// Guarded on `MinLevel` rather than left to `Logf`, which filters but takes its
/// arguments by value: the rendering would otherwise run for every `fetch` on a node
/// at the default level and be thrown away.
///
/// @param logger       Where the line goes.
/// @param opRaw        The opcode byte as it arrived.
/// @param peer         The peer's host, as the kernel reports it.
/// @param requestBytes Length of the request frame.
/// @param reply        The reply frame, empty when none was produced.
/// @param elapsed      How long the responder took.
inline void LogExchange(ILogger& logger,
                        std::uint8_t opRaw,
                        std::string_view peer,
                        std::size_t requestBytes,
                        std::span<std::byte const> reply,
                        std::chrono::milliseconds elapsed)
{
    auto const level = LogLevelForOp(opRaw);
    if (level < logger.MinLevel())
        return;
    logger.Log(level, FormatExchange(opRaw, peer, requestBytes, reply, elapsed));
}

} // namespace FastCache::Node
