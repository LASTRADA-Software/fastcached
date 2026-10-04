// SPDX-License-Identifier: Apache-2.0
#include "CacheTier.hpp"
#include "EnrollAutoApprove.hpp"
#include "NodeConfig.hpp"
#include "NodeIdentity.hpp"
#include "NodeKey.hpp"
#include "NodeMembership.hpp"
#include "NodeSurfaces.hpp"
#include "NodeToolchains.hpp"

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Cli/Duration.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/FleetPin.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/SeedSources.hpp>
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/CompressionValues.hpp>
#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Config/FileOptions.hpp>
#include <FastCache/Config/SecretProvenance.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Platform/Firewall.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>

namespace FastCache::Node
{

namespace
{
    /// Why an id is refused for its length: the sentence every door that takes one says.
    /// @param bytes How long the id was.
    /// @return The reason, naming the bound.
    [[nodiscard]] std::string IdTooLong(std::size_t bytes)
    {
        return std::format("is {} bytes, longer than the {} an id may be: every peer refuses a fleet summary naming "
                           "a longer one",
                           bytes,
                           CompileCacheWire::MaxIdBytes);
    }

    /// An id typed on this command line -- this node's own, or the machine an enrollment verb names --
    /// held to the ONE bound every reader of an id holds it to (`CompileCacheWire::MaxIdBytes`), and
    /// to being text.
    ///
    /// Refused here, by name, because past it the id is no less valid to this node: it is every
    /// PEER that refuses the fleet summary carrying it, silently, as malformed. Empty passes: an
    /// empty `--node-id` is how a node asks to mint one.
    /// @param sv The value text.
    /// @return The id, or why it is too long or not text.
    [[nodiscard]] std::expected<std::string, ConfigError> ParseId(std::string_view sv)
    {
        if (sv.size() > CompileCacheWire::MaxIdBytes)
            return std::unexpected(ArgvError(ConfigErrorCode::ParseError, std::format("value {}", IdTooLong(sv.size()))));
        return ParseUtf8Text(sv);
    }

    /// CLI spelling of a LogLevel, matching what ParseNodeLogLevel accepts.
    ///
    /// The inverse of that parser, and the pair is what makes a log level survive
    /// a round trip into a supervisor's argument list.
    /// @param level Level to name.
    /// @return Its CLI spelling.
    [[nodiscard]] constexpr std::string_view LogLevelName(LogLevel level) noexcept
    {
        switch (level)
        {
            case LogLevel::Trace:
                return "trace";
            case LogLevel::Debug:
                return "debug";
            case LogLevel::Info:
                return "info";
            case LogLevel::Warn:
                return "warn";
            case LogLevel::Error:
                return "error";
            case LogLevel::Fatal:
                return "fatal";
        }
        return "info";
    }

    /// A port, refusing 0 rather than letting a bind fail with a confusing message.
    /// @param sv Text to parse.
    /// @return The port, or why it is not one.
    [[nodiscard]] std::expected<std::uint16_t, ConfigError> ParseNodePort(std::string_view sv)
    {
        auto value = 0U;
        auto const* const begin = sv.data();
        auto const* const end = std::next(begin, static_cast<std::ptrdiff_t>(sv.size()));
        auto const [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc {} || ptr != end || value == 0 || value > 65535)
            return std::unexpected(ArgvError(ConfigErrorCode::OutOfRange, std::format("not a port: {}", sv)));
        return static_cast<std::uint16_t>(value);
    }

    /// A slot count, which may be zero.
    ///
    /// Zero is accepted since #206 and means this node runs no worker (`RunsWorker`);
    /// it used to be refused because it was the field's own spelling of "derive". That
    /// answer is now the field's ABSENCE, so the two cannot be confused.
    /// @param sv Text to parse.
    /// @return The count, or why it is not one.
    [[nodiscard]] std::expected<std::optional<std::uint32_t>, ConfigError> ParseSlots(std::string_view sv)
    {
        auto value = 0U;
        auto const* const begin = sv.data();
        auto const* const end = std::next(begin, static_cast<std::ptrdiff_t>(sv.size()));
        auto const [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc {} || ptr != end)
            return std::unexpected(ArgvError(ConfigErrorCode::OutOfRange, std::format("not a slot count: {}", sv)));
        return std::optional<std::uint32_t> { value };
    }

    /// A node class, by the name `NodeClassTable` spells it.
    ///
    /// Off the shared table rather than a two-arm `if`, so a class added there is
    /// accepted here without an edit -- and so the spelling an operator types is
    /// necessarily the spelling the table documents.
    /// @param sv Text to parse.
    /// @return The class, or why it is not one.
    [[nodiscard]] std::expected<Distributed::NodeClass, ConfigError> ParseNodeClass(std::string_view sv)
    {
        if (auto const parsed = Distributed::NodeClassByName(sv); parsed.has_value())
            return *parsed;

        // Names the accepted spellings, because a rejection that cannot say what
        // would have worked cannot be acted on -- the same reason the wire's
        // `UnsupportedVersion` names its supported range.
        std::string accepted;
        for (auto const& row: Distributed::NodeClassTable)
        {
            if (!accepted.empty())
                accepted += ", ";
            accepted += row.name;
        }
        return std::unexpected(
            ArgvError(ConfigErrorCode::OutOfRange, std::format("not a node class: {} (expected {})", sv, accepted)));
    }

    /// A core reserve, which may legitimately be zero.
    ///
    /// Unlike `ParseSlots`, zero is accepted: "reserve nothing" is a real answer an
    /// operator gives for a machine they are not sitting at, and it is a *different*
    /// answer from not passing the flag at all -- which is why the field holding it
    /// is an optional.
    /// @param sv Text to parse.
    /// @return The count, or why it is not one.
    [[nodiscard]] std::expected<std::optional<std::uint32_t>, ConfigError> ParseReservedCores(std::string_view sv)
    {
        auto value = 0U;
        auto const* const begin = sv.data();
        auto const* const end = std::next(begin, static_cast<std::ptrdiff_t>(sv.size()));
        auto const [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc {} || ptr != end)
            return std::unexpected(ArgvError(ConfigErrorCode::OutOfRange, std::format("not a core count: {}", sv)));
        return std::optional<std::uint32_t> { value };
    }

    /// Parse `--drain-timeout`, a duration kept in whole seconds.
    ///
    /// Zero is accepted and means "wait forever", which is a different answer from
    /// not passing the flag -- and is what this program did before the flag existed,
    /// so it has to stay sayable. A value finer than a second (`1500ms`) is refused
    /// by `ParseDurationValue` rather than rounded to a bound nobody typed.
    /// @param sv Text to parse.
    /// @return The duration, or why it is not one this setting keeps.
    [[nodiscard]] std::expected<std::chrono::seconds, ConfigError> ParseDrainTimeout(std::string_view sv)
    {
        return ParseDurationValue<std::chrono::seconds>(sv);
    }

    /// An applier for a flag that names an enrollment action, and its operand when it
    /// takes one.
    ///
    /// The same shape as `SelectClusterAction` below and for the same reason: one flag
    /// sets both the action and its operand, and a row using `SelectOutcome` alone would
    /// leave the operand to a second row nobody would remember to add. Which verbs take
    /// a subject is a COMPILE-TIME branch, so a row whose arity and whose applier
    /// disagreed would not build.
    ///
    /// The subject goes through `ParseUtf8Text`'s rule rather than `ParseText`'s,
    /// deliberately and unlike `--cluster-forget`: this id is what the leader COMMITS
    /// through `ClusterAdmit`, so it is copied into every peer's `ClusterState` and
    /// rendered into `/fleet.json`, where one byte that is not text makes that document
    /// unparseable for the whole fleet. `--cluster-forget` is out of the gate because
    /// its operand is the OFFENDING id and refusing it would make a bad member
    /// unremovable; here the operand names a machine that is not a member yet, so
    /// refusing it removes nothing and stops the fleet from acquiring the problem.
    /// @return The applier, usable as an OptionSpec::apply in a `constexpr` table.
    template <EnrollAction Action>
    [[nodiscard]] constexpr auto SelectEnrollAction() noexcept
    {
        return [](auto& result, [[maybe_unused]] std::string_view value) -> std::expected<void, ConfigError> {
            auto& request = TargetOf<&NodeConfig::enroll>(result);
            request.action = Action;
            // No verb this applier selects carries a duration, so one an earlier
            // `--enroll-auto-approve` left goes with the verb it belonged to.
            request.duration = std::chrono::seconds::zero();
            request.key.reset();

            if constexpr (Action == EnrollAction::Approve)
            {
                // `<id>@<key>`, split at the LAST `@`: a key is base64url and holds none. The key
                // is REQUIRED: an id alone names a row, and a row whose machine stopped asking is
                // replaced by the next machine to ask under that id -- which an approval by id
                // would admit, unseen.
                auto const at = value.rfind('@');
                if (at == std::string_view::npos)
                    return std::unexpected(ArgvError(ConfigErrorCode::ParseError,
                                                     "is <id>@<key>: paste the approve line --enroll-list prints for "
                                                     "the row, which names the key you compared"));
                auto const id = value.substr(0, at);
                if (id.empty())
                    return std::unexpected(ArgvError(ConfigErrorCode::ParseError, "names no machine"));
                // The one id bound and the text rule every door that takes an id holds (`ParseId`):
                // this id is what the leader commits, so one past the bound is a member every peer's
                // fleet summary decoder would refuse.
                auto bounded = ParseId(id);
                if (!bounded.has_value())
                    return std::unexpected(std::move(bounded).error());
                auto key = ParseEd25519PublicKey(value.substr(at + 1));
                if (!key.has_value())
                    return std::unexpected(
                        ArgvError(ConfigErrorCode::ParseError, std::string { DescribePublicKeyFault(key.error()) }));
                request.subject = *std::move(bounded);
                request.key = *key;
            }
            else if constexpr (Action == EnrollAction::Reject)
            {
                if (value.empty())
                    return std::unexpected(ArgvError(ConfigErrorCode::ParseError, "names no machine"));
                auto bounded = ParseId(value);
                if (!bounded.has_value())
                    return std::unexpected(std::move(bounded).error());
                request.subject = *std::move(bounded);
            }
            else
            {
                // **Cleared, because last-flag-wins has to move BOTH halves.** These
                // flags are one-shot verbs that overwrite each other, so
                // `--enroll-approve=n4 --enroll-list` must mean `List`. Leaving the
                // subject behind sent `EnrollControl(List, "n4")`, which the decoder
                // refuses on its arity rule -- `EnrollControlSubjectOf(List)` is `None`
                // while the subject is non-empty -- and the operator was answered
                // *a control frame this build cannot read*: a VERSION-MISMATCH sentence
                // for a flag-combination mistake, which sends somebody comparing builds
                // across the fleet. The action and its operand are one decision and are
                // reset together.
                request.subject.clear();
            }

            return {};
        };
    }

    /// The applier for `--enroll-auto-approve=<duration>|off`.
    ///
    /// `off` ends the deadline; anything else is a duration, judged by the ONE table the leader
    /// judges it by (`AutoApproveRefusals`), so the operator hears the same sentence here, before
    /// anything is sent, as a peer that sent it anyway would be answered with. The subject is
    /// cleared for `SelectEnrollAction`'s reason: the action and its operand are one decision.
    /// @return The applier, usable as an OptionSpec::apply in a `constexpr` table.
    [[nodiscard]] constexpr auto SelectEnrollAutoApprove() noexcept
    {
        return [](auto& result, std::string_view value) -> std::expected<void, ConfigError> {
            auto& request = TargetOf<&NodeConfig::enroll>(result);
            request.subject.clear();
            request.duration = std::chrono::seconds::zero();
            if (value == "off")
            {
                request.action = EnrollAction::AutoApproveOff;
                return {};
            }
            auto const duration = ParseDurationValue<std::chrono::seconds>(value);
            if (!duration.has_value())
                return std::unexpected(duration.error());
            if (auto const refusal = JudgeAutoApprove(*duration); refusal.has_value())
                return std::unexpected(ArgvError(ConfigErrorCode::OutOfRange, AutoApproveSentence(*refusal)));
            request.action = EnrollAction::AutoApprove;
            request.duration = *duration;
            return {};
        };
    }

    /// An applier for a flag that names a cluster action AND carries its operand.
    ///
    /// Neither `SelectOutcome` nor `AssignFrom` covers this on its own: one sets
    /// the action and the other the operand, and a row using either would leave
    /// the other half to a second row nobody would remember to add. `Status` takes
    /// no operand at all, which is why the operand handling is a compile-time
    /// branch rather than a runtime one -- a `--cluster-status` row that quietly
    /// stored an empty key would be a row whose arity and whose applier disagreed.
    /// @return The applier, usable as an OptionSpec::apply in a `constexpr` table.
    template <ClusterAction Action>
    [[nodiscard]] constexpr auto SelectClusterAction() noexcept
    {
        return [](auto& result, [[maybe_unused]] std::string_view value) -> std::expected<void, ConfigError> {
            auto& request = TargetOf<&NodeConfig::cluster>(result);
            request.action = Action;

            if constexpr (Action == ClusterAction::Admit || Action == ClusterAction::AdmitLearner
                          || Action == ClusterAction::Set)
            {
                // These two COMMIT their operand through consensus: an admitted
                // member's id and endpoint land in every peer's `ClusterState` and
                // are rendered into `/fleet.json`, and a setting is agreed by the
                // whole cluster and survives every restart. A consensus entry is
                // applied after it is committed, with nobody left to refuse it, so
                // this is the last place a person can be told.
                //
                // `Forget` is deliberately NOT here, and that omission is the trap
                // issue #159 records: its operand IS the offending id, so a check
                // covering it would make a bad member -- one admitted by an older
                // peer -- impossible to remove, and it would count towards quorum
                // forever.
                if (auto const text = ParseUtf8Text(value); !text.has_value())
                    return std::unexpected(text.error());
            }

            if constexpr (Action == ClusterAction::Set)
            {
                auto assignment = ParseSettingAssignment(value);
                if (!assignment.has_value())
                    return std::unexpected(
                        ArgvError(ConfigErrorCode::ParseError, std::format("not <name>=<value>: {}", value)));
                request.key = std::move(assignment->first);
                request.value = std::move(assignment->second);
            }
            else if constexpr (Action == ClusterAction::Forget)
            {
                if (value.empty())
                    return std::unexpected(ArgvError(ConfigErrorCode::ParseError, "names no member"));
                request.key = std::string { value };
            }
            else if constexpr (Action == ClusterAction::Admit || Action == ClusterAction::AdmitLearner)
            {
                // The same grammar `--print-identity`'s line spells, through the same function:
                // an operator adding a member types the token they would have put in
                // that flag, the documentation tells them so, and a second
                // implementation would be two flags accepting different token sets
                // for one concept -- with only one of them being what the transport
                // actually dials. The learner flag takes it too (#1449): which set a
                // member is admitted into is the flag, never a second grammar.
                auto member = Cluster::ParseMemberSpec(value);
                if (!member.has_value())
                    return std::unexpected(ArgvError(ConfigErrorCode::ParseError, member.error()));

                // `@<key>` rides the request as its third field (#178), already PARSED --
                // so a key that is not one is refused here, in `ParseMemberSpec`'s own
                // sentence, in front of whoever typed it. The leader parses the text it is
                // sent once more, because the wire door is not the flag door.
                request.key = std::move(member->id);
                request.value = std::move(member->raftEndpoint);
                request.publicKey = member->publicKey;
            }

            return {};
        };
    }

    /// An `--advertise` value: text, and no longer than a scheduler records an endpoint.
    ///
    /// Both are asked by the parse for `ParseToolchain`'s reason: a scheduler refuses a
    /// registration whose endpoint is either, on every heartbeat, and a node started or
    /// reloaded with one would register nowhere while it looked healthy. A host that long is
    /// one no resolver answers for, so nothing that worked is refused.
    /// @param sv The flag's value.
    /// @return `sv` verbatim, or why a scheduler would refuse it.
    [[nodiscard]] std::expected<std::string, ConfigError> ParseAdvertise(std::string_view sv)
    {
        auto text = ParseUtf8Text(sv);
        if (text.has_value() && sv.size() > CompileCacheWire::MaxEndpointBytes)
            return std::unexpected(ConfigError { .code = ConfigErrorCode::ParseError,
                                                 .source = {},
                                                 .line = 0,
                                                 .field = {},
                                                 .context = std::format("an endpoint of {} bytes is longer than the "
                                                                        "{} a scheduler records; a registration "
                                                                        "carrying it is refused on every round",
                                                                        sv.size(),
                                                                        CompileCacheWire::MaxEndpointBytes) });
        return text;
    }

    /// A `--toolchain` value, with the half that TRAVELS checked for being text.
    ///
    /// Split on the first `=`, as `SplitToolchain` does and for the same reason: a
    /// fingerprint is hex and contains none, so a compiler path holding one is only
    /// reachable through the override form. Restated here rather than shared,
    /// because sharing it would mean this file depending on the toolchain module it
    /// configures -- and what is restated is one `find`, while the rule that
    /// matters (what counts as text, and what the operator is told) stays in
    /// `ParseUtf8Text`.
    ///
    /// Only the fingerprint. The compiler beside it is a path on this machine, and
    /// on a host that transcodes nothing a legacy filename is a perfectly good
    /// filename -- refusing it would break a working node over a rule about a field
    /// it is not.
    ///
    /// Asked HERE rather than where the two halves are used, so it is decided by the
    /// parse: `--install-service` returns before a toolchain is ever resolved, and a
    /// registration that bakes in a fingerprint no scheduler will accept is one that
    /// fails at every boot with nobody watching.
    ///
    /// **The GRAMMAR is checked here, not only the text.** It used to be enforced by
    /// `SplitToolchain` at survey time, which was harmless while this flag could only
    /// arrive at startup -- a malformed value exited with a message and nothing had
    /// been applied. Since #403 made the row reloadable it is not: such a value now
    /// passes the applier, passes `ValidateNodeReloadable`, is published, and is
    /// discovered to be malformed on the heartbeat thread, where the only thing left
    /// to do is serve NOTHING. The worker cannot recover either, because an empty
    /// served set has no witnesses to notice a later correction with.
    ///
    /// So it is asked where every other value grammar is asked -- in the row -- and a
    /// bad one declines the whole reload with the previous configuration still in
    /// force. `SplitToolchain` is the authority and is called rather than restated.
    /// @param sv The flag's value.
    /// @return `sv` verbatim, or why it is not a usable toolchain.
    [[nodiscard]] std::expected<std::string, ConfigError> ParseToolchain(std::string_view sv)
    {
        if (auto const eq = sv.find('='); eq != std::string_view::npos)
        {
            if (auto const pinned = ParseUtf8Text(sv.substr(0, eq)); !pinned.has_value())
                return std::unexpected(pinned.error());
            // And no longer than a scheduler records one. A pin that long could never match a
            // client's digest anyway, and a scheduler refuses its registration on every round --
            // the fails-at-every-boot shape above, so asked here for the same reason.
            if (eq > CompileCacheWire::MaxToolchainFingerprintBytes)
                return std::unexpected(ConfigError {
                    .code = ConfigErrorCode::ParseError,
                    .source = {},
                    .line = 0,
                    .field = {},
                    .context = std::format("a pinned fingerprint of {} bytes is longer than the {} a scheduler "
                                           "records; a registration carrying it is refused on every round",
                                           eq,
                                           CompileCacheWire::MaxToolchainFingerprintBytes) });
        }
        if (!SplitToolchain(sv).has_value())
            return std::unexpected(
                ConfigError { .code = ConfigErrorCode::ParseError,
                              .source = {},
                              .line = 0,
                              .field = {},
                              .context = std::format("'{}' is not <compiler> or <fingerprint>=<compiler>; both halves "
                                                     "of a pinned toolchain have to be non-empty",
                                                     sv) });
        return std::string { sv };
    }

    /// Render a compile-argument allowlist for a log line.
    ///
    /// Its own function rather than a join written twice, because the EMPTY case is a
    /// real one at the reload site: an operator who removed every entry is told so,
    /// and a bare join renders that as a sentence ending in a colon and nothing, which
    /// reads as a truncated message rather than as a set with no members.
    /// @param args The spellings in force, possibly none.
    /// @return `a, b, c`, or `(none)` when there are none.
    [[nodiscard]] std::string JoinAllowedArgs(std::span<std::string const> args)
    {
        if (args.empty())
            return "(none)";

        std::string out;
        for (auto const& arg: args)
        {
            if (!out.empty())
                out += ", ";
            out += arg;
        }
        return out;
    }

    /// One extra compile-argument spelling an operator has allowed.
    ///
    /// Validated for the SHAPE a spelling must have, never for membership of any
    /// list -- the point of the flag is to name something no list here knows.
    ///
    /// It must introduce an option. A bare word on a compiler command line is an
    /// INPUT FILE, so admitting one would let a job name a path, which is the
    /// property the whole filter exists to deny.
    ///
    /// A wildcard is refused rather than ignored. Matching is whole and exact, so
    /// `-f*` would simply never match anything -- and an operator who wrote it would
    /// have a flag that silently does nothing, which is worse than a refusal that
    /// says so. It is also the shape that, if it ever were honoured, would re-admit
    /// the program-invoking options the built-in table is enumerated to exclude.
    ///
    /// A PATH SEPARATOR is refused for the same reason a bare word is: what an entry
    /// may name is a flag, and a flag carrying a path names a file on the worker.
    /// `IsAcceptableJobArgument` refuses one again when the argument arrives, so this
    /// is the first of two doors rather than the only one -- but it is the door that
    /// can say WHY, at the moment the operator is looking at their own file, instead
    /// of leaving them an entry that is quietly never matched.
    /// @param sv The operand as typed.
    /// @return The spelling, or why it cannot be one.
    [[nodiscard]] std::expected<std::string, ConfigError> ParseAllowedCompileArg(std::string_view sv)
    {
        // The reason travels in `context`, already formatted by the caller, so this
        // captures nothing: every refusal below names the operand itself, and a
        // capture that only LOOKED used would be one more thing to keep in step.
        auto const refuse = [](std::string context) {
            return std::unexpected(ConfigError {
                .code = ConfigErrorCode::ParseError, .source = {}, .line = 0, .field = {}, .context = std::move(context) });
        };
        if (sv.empty())
            return refuse("an allowed compile argument cannot be empty");
        if (sv.front() != '-' && sv.front() != '/')
            return refuse(std::format("'{}' does not introduce an option: it needs a leading '-' or '/'. A bare word "
                                      "on a compiler command line is an input file, which this filter exists to "
                                      "refuse",
                                      sv));
        if (sv.find_first_of("*?") != std::string_view::npos)
            return refuse(std::format("'{}' looks like a pattern, and matching here is whole and exact. Name the "
                                      "spelling itself; a prefix would re-admit the program-invoking options the "
                                      "built-in table enumerates in order to exclude",
                                      sv));
        if (sv.find_first_of(" \t\r\n") != std::string_view::npos)
            return refuse(std::format("'{}' contains whitespace, so it is more than one argument. Allow each one "
                                      "separately",
                                      sv));
        // The leading introducer is not a separator: `/O2` is an MSVC option and `/etc`
        // is a path, and what tells them apart is everything AFTER the first character.
        if (sv.substr(1).find_first_of("/\\") != std::string_view::npos)
            return refuse(std::format("'{}' names a path, and a path names a file on the WORKER rather than anything "
                                      "in the client's build -- a dispatched compile arrives preprocessed, so its "
                                      "headers are already inlined. Allow the flag, never a value carrying a path",
                                      sv));
        return ParseUtf8Text(sv);
    }

    /// A byte count for the local cache tier, accepting the k/m/g suffixes the
    /// daemon's own size flags do.
    ///
    /// Reuses `ParseByteSize` rather than a second parser: an operator who has
    /// written `--storage-max-value=64m` for the daemon must not discover that this
    /// flag spells sizes differently, and two grammars for one concept is the
    /// table-shaped defect this codebase keeps a list about.
    /// @param sv Text to parse.
    /// @return The size in bytes, or why it is not one.
    [[nodiscard]] std::expected<std::uint64_t, ConfigError> ParseCacheBytes(std::string_view sv)
    {
        // Host total passed, so `N%` parses -- which is the vocabulary this flag's
        // own default is stated in. A default an operator cannot spell is one they
        // cannot adjust by a little: without this, "a quarter of RAM, but half"
        // means working out the bytes for every machine by hand.
        auto const parsed = ParseByteSize(sv, QueryHostTotalMemoryBytes());
        if (!parsed.has_value())
            return std::unexpected(parsed.error());
        return static_cast<std::uint64_t>(*parsed);
    }

    /// The on-disk tier's byte budget.
    ///
    /// `k`/`m`/`g` as `--cache-memory` takes them, but **not** its `N%`: that share
    /// is of host RAM, and a disk budget expressed as a fraction of memory would be
    /// a number with no meaning on any machine whose disk is not its RAM.
    /// @param sv Text to parse.
    /// @return The size in bytes, or why it is not one.
    [[nodiscard]] std::expected<std::uint64_t, ConfigError> ParseCacheDiskBytes(std::string_view sv)
    {
        auto const parsed = ParseByteSize(sv);
        if (!parsed.has_value())
            return std::unexpected(parsed.error());
        return static_cast<std::uint64_t>(*parsed);
    }

    /// The fleet `--fleet-id` pins this node to: text, then `Cluster::ParsePinnedFleet`'s grammar.
    ///
    /// Refused by name here rather than accepted and matched by nothing: a truncated copy of what
    /// `fastcache-cli node` printed would pin the node to a fleet that cannot exist and withhold every
    /// real one -- and an id with no key is refused rather than read as a pin by name, which every
    /// beacon would defeat while the node reported itself pinned.
    /// @param sv Text to parse.
    /// @return The pinned fleet, or why the text is not one.
    [[nodiscard]] std::expected<std::optional<Cluster::PinnedFleet>, ConfigError> ParseFleetPin(std::string_view sv)
    {
        if (auto const text = ParseUtf8Text(sv); !text.has_value())
            return std::unexpected(text.error());
        auto fleet = Cluster::ParsePinnedFleet(sv);
        if (!fleet.has_value())
            return std::unexpected(ArgvError(ConfigErrorCode::ParseError, std::move(fleet).error()));
        return std::optional { *std::move(fleet) };
    }

    /// One fleet seed, from the `<host>` or `<host>:<port>` an operator typed.
    ///
    /// Through `Cluster::NormalizeSeed`, the one grammar a seed has whichever source it came
    /// from, so a typed seed and a remembered one normalize to the same token and are tried
    /// once. Text first: the seed is written into the remembered-fleet file and every log line
    /// about the walk, both of which other tools read.
    /// @param sv Text to parse.
    /// @return The seed as `host:port`, or why the text is not one.
    [[nodiscard]] std::expected<std::string, ConfigError> ParseFleetSeed(std::string_view sv)
    {
        if (auto const text = ParseUtf8Text(sv); !text.has_value())
            return std::unexpected(text.error());
        auto seed = Cluster::NormalizeSeed(sv);
        if (!seed.has_value())
            return std::unexpected(ArgvError(ConfigErrorCode::ParseError, "is not <host> or <host>:<port>"));
        return *std::move(seed);
    }

    /// A filesystem path, taken as written.
    /// @param sv Text to parse.
    /// @return The path.
    [[nodiscard]] std::expected<std::filesystem::path, ConfigError> ParsePathValue(std::string_view sv)
    {
        return std::filesystem::path { sv };
    }

    /// A log level by name.
    /// @param sv Text to parse.
    /// @return The level, or why it is not one.
    [[nodiscard]] std::expected<LogLevel, ConfigError> ParseNodeLogLevel(std::string_view sv)
    {
        // Spelled here rather than shared with the daemon's parser, which lives in an
        // anonymous namespace in CliParser.cpp. The names must match what the daemon
        // accepts -- an operator setting the same level on both should not have to learn
        // two vocabularies.
        static constexpr std::array<std::pair<std::string_view, LogLevel>, 6> levels { {
            { "trace", LogLevel::Trace },
            { "debug", LogLevel::Debug },
            { "info", LogLevel::Info },
            { "warn", LogLevel::Warn },
            { "error", LogLevel::Error },
            { "fatal", LogLevel::Fatal },
        } };
        // Iterated rather than searched with an iterator, and that is a portability
        // fix rather than a style choice. `readability-qualified-auto` asks for
        // `auto const* const` here, which is right on libc++ -- where a std::array
        // iterator IS a raw pointer -- and does not compile on MSVC's STL, where it is
        // a class type. Taking the value directly sidesteps the difference entirely.
        for (auto const& [name, level]: levels)
            if (name == sv)
                return level;
        return std::unexpected(ArgvError(ConfigErrorCode::OutOfRange, std::format("unknown level: {}", sv)));
    }

    /// The supervisor domain to register into, by name.
    /// @param sv Text to parse.
    /// @return The scope, or why it is not one.
    [[nodiscard]] std::expected<ServiceScope, ConfigError> ParseNodeServiceScope(std::string_view sv)
    {
        // The library's parser, not a second spelling of it: an operator who learned
        // `--service-scope=user` from the daemon must not find the worker accepting a
        // different vocabulary.
        return ParseServiceScope(sv);
    }

    /// How the supervisor starts the registration, by name.
    /// @param sv Text to parse.
    /// @return The start mode, or why it is not one.
    [[nodiscard]] std::expected<ServiceStart, ConfigError> ParseNodeServiceStart(std::string_view sv)
    {
        // The library's parser, for `ParseNodeServiceScope`'s reason: one vocabulary for both binaries.
        return ParseServiceStart(sv);
    }

    /// One `--firewall-allow` scope.
    /// @param sv Text to parse.
    /// @return The scope as typed, or why it is not one.
    [[nodiscard]] std::expected<std::string, ConfigError> ParseNodeFirewallScope(std::string_view sv)
    {
        // The library's parser, for `ParseNodeServiceScope`'s reason: one grammar for both binaries.
        return ParseFirewallScope(sv);
    }

    /// Memory this node's cache tiers hold, and so cannot lend to a compile.
    ///
    /// A fold over `StorageTierTable`'s `budgetIsResidentMemory` column rather than a
    /// look at the memory tier by name: the taxonomy is open and enumerators are
    /// appended, so a resident tier added later reaches this arithmetic by being a
    /// row. Naming `StorageTier::Memory` here would ignore it, and under-counting is
    /// the direction that over-commits the machine.
    ///
    /// Only budgets that are *denominated in* RAM are summed, which is what that
    /// column says. A disk tier's own in-memory key index is real and is not covered
    /// here (#175) -- but adding its DISK budget to a memory total would be wrong by
    /// the ratio between the two, not right by accident.
    ///
    /// Total over the vocabulary rather than correct by distant assumption, which is
    /// the difference between the two spellings of "nothing to add":
    ///
    ///   - **Absent** is a tier this node does not run, and contributes nothing.
    ///   - **Present and zero** is a tier with no ceiling -- the same UNBOUNDED that
    ///     `--cache-memory 0` once meant to `InMemoryLruStorage` -- so a resident one
    ///     may take the whole machine and is reserved as such. `BuildStorage` does
    ///     not build the memory half that way today, but reading it as "reserve
    ///     nothing" would be the exact inverse of what it says, and that number's two
    ///     meanings have bitten this codebase before.
    /// @param cache What this node's tiers actually hold.
    /// @param totalMemoryBytes The machine's RAM, which an unbounded tier may take all of.
    /// @return Bytes held back from compiles.
    [[nodiscard]] std::uint64_t ResidentCacheBytes(Distributed::NodeCacheCapacity const& cache,
                                                   std::uint64_t totalMemoryBytes) noexcept
    {
        std::uint64_t reserved = 0;
        for (auto const& row: StorageTierTable)
        {
            if (!row.budgetIsResidentMemory)
                continue;

            auto const& budget = cache.tierBytesLimit[static_cast<std::size_t>(row.tier)];
            if (!budget.has_value())
                continue;
            if (*budget == 0)
                return totalMemoryBytes;

            // Saturating rather than wrapping, for the reason `OfferableSlots`'s core
            // reserve is: budgets that summed past 64 bits would otherwise come back
            // as a SMALL reservation, which is the over-commit direction again.
            if (*budget > std::numeric_limits<std::uint64_t>::max() - reserved)
                return std::numeric_limits<std::uint64_t>::max();
            reserved += *budget;
        }
        return reserved;
    }

    /// One rule a single configuration may break, for the startup and install tables.
    struct ConfigRule
    {
        /// The component this rule is about, or null for a rule every configuration
        /// answers. A scoped rule is asked only of a configuration that runs its
        /// component -- a column rather than a conjunct in each predicate, because the
        /// pasted conjuncts are the shape that dropped one (#206).
        OptionComponent<NodeConfig> const* scope { nullptr };
        bool (*refuses)(NodeConfig const&); ///< Whether this rule objects.
        std::string_view message;           ///< What the operator is told, with the remedy.
    };

    /// Whether a rule scoped to @p scope is asked of @p cfg at all.
    /// @param scope The rule's component, or null.
    /// @param cfg The configuration.
    /// @return True when the rule applies.
    [[nodiscard]] bool InScope(OptionComponent<NodeConfig> const* scope, NodeConfig const& cfg) noexcept
    {
        return scope == nullptr || scope->runs(cfg);
    }

    /// The first rule of @p rules that @p cfg breaks, in table order.
    /// @param rules The table; first match wins.
    /// @param cfg The configuration.
    /// @return That rule's message, or nothing.
    [[nodiscard]] std::optional<std::string> FirstRefusal(std::span<ConfigRule const> rules, NodeConfig const& cfg)
    {
        if (auto const* const rule = core::findIfOrNull(
                rules, [&cfg](ConfigRule const& row) { return InScope(row.scope, cfg) && row.refuses(cfg); }))
            return std::string { rule->message };
        return std::nullopt;
    }
} // namespace

std::optional<std::pair<std::string, std::string>> ParseSettingAssignment(std::string_view text)
{
    auto const split = text.find('=');
    if (split == std::string_view::npos)
        return std::nullopt;

    auto name = std::string { text.substr(0, split) };
    if (name.empty())
        return std::nullopt;

    return std::pair { std::move(name), std::string { text.substr(split + 1) } };
}

std::expected<void, ConfigError> ApplyNodeConfiguration(std::vector<YamlSetting> const& settings,
                                                        std::filesystem::path const& path,
                                                        std::span<char const* const> args,
                                                        NodeConfig& result)
{
    if (auto applied = ApplyFileSettings(NodeOptions(), settings, path, result); !applied.has_value())
        return std::unexpected(ExplainRetiredNodeOption(std::move(applied).error()));

    // A repeatable row's applier APPENDS, so a `--toolchain` on the command line
    // would otherwise EXTEND the file's list rather than replace it. Replacement is
    // the rule -- mixing partial file values with partial command-line values makes
    // precedence depend on declaration order, which is not something an operator can
    // reason about -- and it is driven off the table's own `clear` column, so a
    // fourth repeatable flag needs no edit here.
    ClearListsNamedOn(NodeOptions(), args, result);

    // The command line over the file-seeded result, through the same appliers. Its
    // outcome is discarded rather than ignored: the caller has already parsed this
    // exact argv once to find the config path, so anything that could be refused
    // here was refused there, with the message an operator wants and before a file
    // was read at all.
    (void) ParseOptionsInto(NodeOptions(), args, result);
    return {};
}

std::span<RetiredNodeFlag const> RetiredNodeFlags() noexcept
{
    // Each step names only what this build has. Appended to, never reordered.
    static constexpr auto rows = std::to_array<RetiredNodeFlag>({
        { .flag = "--raft-peer",
          .fileKey = "raft_peer",
          .step = "remove it: the members consensus starts with are the formation record's, kept in the state "
                  "directory -- this node alone where it founded its cluster, the fleet's roster where it joined "
                  "one" },
        { .flag = "--raft-join",
          .fileKey = "raft_join",
          .step = "remove it: whether this node founded its cluster or joined one is the formation record's, kept "
                  "in the state directory, and a first start founds a cluster of one" },
        { .flag = "--cluster-id",
          .fileKey = "cluster_id",
          .step = "remove it: the cluster id is the formation record's, minted at this node's first start, or the "
                  "fleet's once it has joined one" },
        { .flag = "--serve-scheduler",
          .fileKey = "serve_scheduler",
          .step = "remove it: a node whose mode serves the scheduler serves it while it runs consensus -- a "
                  "solitary node and a voter do, a learner does not" },
        // Admission follows the machine: an address admits nobody, so the flags that admitted, revoked or
        // guarded by one are gone with it.
        { .flag = "--fleet-member",
          .fileKey = "fleet_member",
          .step = "remove it: an address admits nobody -- a machine is admitted by its identity key, a node by the "
                  "key it proves and a client by the machine ticket its own node mints; a machine asks to "
                  "enroll, and --enroll-approve admits it" },
        { .flag = "--scheduler-token-file",
          .fileKey = "scheduler_token_file",
          .step = "remove it: a node checks no password -- another machine is admitted by its identity key, and "
                  "a client presents the machine ticket its own node mints" },
        { .flag = "--cluster-admit-client",
          .fileKey = "cluster_admit_client",
          .step = "remove it: a client machine is admitted by its identity key, not its address -- run a node "
                  "there: it asks to enroll, and --enroll-approve admits it" },
        { .flag = "--cluster-forget-client",
          .fileKey = "cluster_forget_client",
          .step = "remove it: a machine is forgotten by its key with --cluster-forget, which revokes that key from "
                  "every address" },
        // Principal mode is retired: every machine joins ONE way, as a learner member holding its key, so the
        // flags that named a seed to join through, anchored a roster nobody applied, or admitted a principal
        // are gone with it.
        { .flag = "--enroll-from",
          .fileKey = "enroll_from",
          .step = "remove it: a machine finds its fleet by discovery, or at the node --fleet-seed names, and asks "
                  "to enroll there on its own" },
        { .flag = "--voter-key",
          .fileKey = "voter_key",
          .step = "remove it: every machine joins as a learner and checks grants against the roster its own "
                  "consensus applies, so no key anchors a roster" },
        { .flag = "--cluster-admit-worker",
          .fileKey = "cluster_admit_worker",
          .step = "remove it: every machine joins as a learner holding its key -- it asks to enroll and "
                  "--enroll-approve admits it, or --cluster-admit-learner admits it by that key" },
    });
    return rows;
}

namespace
{
    /// Where a retired spelling was found, and what an operator does about that place.
    struct RetiredSpelling
    {
        std::string_view RetiredNodeFlag::* spelling; ///< Which column the refused name matched.
        std::string_view where;                       ///< What keeps refusing it until it is removed.
    };

    /// One row per place a retired name can arrive from: the command line, and a config file.
    ///
    /// The refusal names the spelling the operator actually WROTE, in the place they wrote it --
    /// a config file's `raft_join` answered as `--raft-join` and "a service registration" sends
    /// them looking for a command line that does not carry it.
    constexpr auto RetiredSpellings = std::to_array<RetiredSpelling>({
        { .spelling = &RetiredNodeFlag::flag,
          .where = "A service registration that replays it fails at every boot, so remove it there as well" },
        { .spelling = &RetiredNodeFlag::fileKey,
          .where = "The config file that carries it is refused at every start and every reload, so remove it "
                   "there" },
    });
} // namespace

ConfigError ExplainRetiredNodeOption(ConfigError error)
{
    if (error.code != ConfigErrorCode::UnknownKey)
        return error;

    // The spelling before any `=`, so `--raft-peer=n1=host:6680` is the `--raft-peer` row.
    auto const named = std::string_view { error.field }.substr(0, error.field.find('='));
    for (auto const& place: RetiredSpellings)
    {
        auto const* const row = core::findIfOrNull(RetiredNodeFlags(), [named, &place](RetiredNodeFlag const& retired) {
            return named == retired.*place.spelling;
        });
        if (row == nullptr)
            continue;
        error.context = std::format(
            "{} was retired; {}. {}. See {}", row->*place.spelling, row->step, place.where, RetiredNodeFlagsGuide);
        return error;
    }
    return error;
}

std::expected<ParseFlow, ConfigError> ParseNodeCommandLine(std::span<char const* const> args, NodeConfig& result)
{
    return ParseOptionsInto(NodeOptions(), args, result).transform_error(&ExplainRetiredNodeOption);
}

std::span<OptionSpec<NodeConfig> const> NodeOptions() noexcept
{
    static constexpr auto options = std::to_array<OptionSpec<NodeConfig>>({
        { .primary = "--config",
          .arity = Arity::Value,
          .operand = "=<path>",
          .apply = AssignFrom<&NodeConfig::configPath, ParseText>(),
          .description = "read settings from this YAML file. Every SETTING flag here\n"
                         "is a key in it, spelled with underscores; the one-shot\n"
                         "verbs (--install-service, --cluster-*, --help) are not, and\n"
                         "neither is this flag. The command line wins where both name\n"
                         "one. A named path is strict -- absent, unreadable or\n"
                         "malformed refuses to start -- while the machine-wide file\n"
                         "this looks for when unset is skipped when it is not there.\n"
                         "Its MODE is not checked: anyone who can write it decides\n"
                         "what this worker runs (#384)." },
        {
            .primary = "--scheduler",
            .arity = Arity::Value,
            .operand = "=<host:port>",
            // Repeatable, and no provenance bit: a list's default is EMPTY, so every value
            // the command line holds is one the operator typed and the registration
            // emits each of them (#1310).
            .apply = AppendFrom<&NodeConfig::schedulers, ParseText>(),
            .description = "where a one-shot verb (--cluster-*, --enroll-*) is\n"
                           "sent: a node's --listen-node endpoint. Unset, that is\n"
                           "this machine's own node at 127.0.0.1:6674, which\n"
                           "follows the fleet's answer to its leader. Repeatable:\n"
                           "each is tried in order until one answers. Refused on\n"
                           "a node that serves: it finds its scheduler from the\n"
                           "fleet it joined, and --fleet-seed is how it joins\n"
                           "one no beacon reaches.",
            .yamlKey = "scheduler",
            .same = FieldEq<&NodeConfig::schedulers>(),
            .clear = ClearList<&NodeConfig::schedulers>(),
            .component = &OneShotVerbComponent,
            .present = PresentIn<&NodeConfig::schedulers>(),
        },
        {
            .primary = "--advertise",
            .arity = Arity::Value,
            .operand = "=<host:port>",
            .apply = AssignFrom<&NodeConfig::advertise, ParseAdvertise>(),
            .explicitBit = &NodeConfig::advertiseExplicit,
            .description = "host:port CLIENTS should use to reach this worker.\n"
                           "Defaults to this machine's fully qualified name on the\n"
                           "--listen-node port while that binds the wildcard, and\n"
                           "to --listen-node itself otherwise: the scheduler hands\n"
                           "this string to clients verbatim, so a worker that\n"
                           "advertises an address only it can reach is leased and\n"
                           "then never answers. Reloadable: a node that learns its\n"
                           "address late re-registers under the new one and\n"
                           "retires the old entry.",
            .yamlKey = "advertise",
            // Reloadable since #1279, and it is the flag the third classification list
            // exists for. `AddressReloadableFlags`: the registration has to move, the
            // toolchain survey must not.
            //
            // **The row alone changes nothing an operator can observe**, which is the
            // half this ticket warns about in its own body. Two sites read this value --
            // the REGISTER/heartbeat registrars and the lease validator, whose
            // expectation a grant's MAC is taken over -- and both used to capture it at
            // construction, so a reload published a snapshot neither read. They now read
            // `Cc::IAdvertisedEndpointSource`, the heartbeat republishes it, and
            // `AdoptRegistrars` withdraws the entry under the old address rather than
            // leaving the scheduler to expire it. A reload that reached one of the two
            // would be worse than one that reached neither: the worker would refuse
            // grants the scheduler authentically signed.
            //
            // Safe to reload because the candidate is judged by the STARTUP rules too
            // (`ValidateNodeReloadable` asks `StartupPolicyRejection` first), so a save
            // naming the wildcard is refused by name rather than advertised to a fleet.
            .reloadable = Reloadable::Yes,
            .same = FieldEq<&NodeConfig::advertise>(),
        },
        {
            .primary = "--toolchain",
            .arity = Arity::Value,
            .operand = "=<compiler>|<fingerprint>=<compiler>",
            .apply = AppendFrom<&NodeConfig::toolchains, ParseToolchain>(),
            .description = "a toolchain this worker serves; repeatable. An OVERRIDE:\n"
                           "naming any pins this worker to exactly that set, and\n"
                           "naming none means serve whatever this machine has.\n"
                           "There is still no default COMPILER -- a default is how a\n"
                           "job ends up running against something nobody chose.",
            .yamlKey = "toolchain",
            // Reloadable since #403, and the band's cheap half: nothing new had to be
            // built for it. The heartbeat thread ALREADY replaces this node's served
            // set mid-life when a compiler is patched underneath it (#238), and
            // `RefreshToolchains` already reads the set from the configuration -- so a
            // reload publishes a snapshot and that path does the rest, in the order it
            // already establishes: the compile port first, the registration second, so
            // a job naming a dropped fingerprint is refused `UnknownFingerprint` rather
            // than served with a compiler nobody keyed against.
            //
            // What a reload must additionally do is FORCE that re-derivation. The
            // existing trigger is a witness stamp moving on disk, and editing a file
            // moves none -- so a change here would otherwise sit unapplied until the
            // next unconditional sweep, which is a reload an operator watched do
            // nothing.
            .reloadable = Reloadable::Yes,
            .same = FieldEq<&NodeConfig::toolchains>(),
            .clear = ClearList<&NodeConfig::toolchains>(),
            .component = &WorkerComponent,
            .present = PresentIn<&NodeConfig::toolchains>(),
        },
        {
            .primary = "--allow-compile-arg",
            .arity = Arity::Value,
            .operand = "=<flag>",
            .apply = AppendFrom<&NodeConfig::extraAllowedArgs, ParseAllowedCompileArg>(),
            .description = "accept this compile argument in addition to the built-in\n"
                           "per-driver-family table; repeatable. EXTENDS the table and\n"
                           "cannot shrink it -- a refusal the table makes is never\n"
                           "removable by configuration. Matched WHOLE and exactly, so\n"
                           "name the spelling rather than a prefix.",
            .yamlKey = "allow_compile_arg",
            // Reloadable, and that is the whole ticket rather than a convenience. A
            // built-in table cannot be complete forever, and a site meeting a
            // legitimate flag it does not name gets the silent failure: the build
            // stays green, the worker refuses the argument, and the fleet quietly
            // stops distributing that translation unit. Waiting for a release to
            // spell one flag is what #293 exists to remove, so the flag has to be
            // answerable without a restart or it has only moved the wait.
            .reloadable = Reloadable::Yes,
            .same = FieldEq<&NodeConfig::extraAllowedArgs>(),
            .clear = ClearList<&NodeConfig::extraAllowedArgs>(),
            .component = &WorkerComponent,
            .present = PresentIn<&NodeConfig::extraAllowedArgs>(),
        },
        {
            .primary = "--no-toolchain-discovery",
            .arity = Arity::None,
            .apply = SetFalse<&NodeConfig::toolchainDiscovery>(),
            // A bit rather than presence: the field is a `bool` whose default is a
            // value, and a scoped row must say it was NAMED (`OptionSpec::present`).
            .explicitBit = &NodeConfig::toolchainDiscoveryExplicit,
            .description = "do not survey this machine for compilers. Without\n"
                           "--toolchain this leaves the worker with nothing to\n"
                           "serve, so it refuses to start -- and refuses to be\n"
                           "INSTALLED as a service, which is the registration that\n"
                           "would otherwise fail at every boot with nobody watching.",
            .yamlKey = "no_toolchain_discovery",
            // Reloadable for the reason `--toolchain` is, and through the same path.
            // It only ever decides anything when NO `--toolchain` is named: an
            // operator-named set is `ToolchainSource::OperatorNamed` and discovery is
            // not consulted at all. So the reachable change is "stop searching this
            // machine and serve nothing" or its reverse, and both are states the
            // re-survey already knows how to reach.
            .reloadable = Reloadable::Yes,
            .same = FieldEq<&NodeConfig::toolchainDiscovery>(),
            .component = &WorkerComponent,
        },
        {
            .primary = "--slots",
            .arity = Arity::Value,
            .operand = "=<n>",
            .apply = AssignFrom<&NodeConfig::slots, ParseSlots>(),
            .description = "concurrent compiles. Default: derived from this\n"
                           "machine's cores and memory, less what --node-class\n"
                           "reserves. A number given here is the answer and is\n"
                           "not clamped or reduced further. Advertised to the\n"
                           "scheduler AND enforced here: a worker that accepted\n"
                           "more would be fuller and slower than the scheduler\n"
                           "believes, at the same moment. 0 runs NO worker: the\n"
                           "node surveys nothing, registers nothing and is never\n"
                           "sent a compile -- a machine that only schedules or\n"
                           "caches. Until #1440 such a node's /metrics and history\n"
                           "still read 0 slots.",
            .yamlKey = "slots",
            .same = FieldEq<&NodeConfig::slots>(),
        },
        {
            .primary = "--node-class",
            .arity = Arity::Value,
            .operand = "=workstation|dedicated",
            .apply = AssignFrom<&NodeConfig::nodeClass, ParseNodeClass>(),
            .explicitBit = &NodeConfig::nodeClassExplicit,
            .description = "how hard this machine may be driven (default:\n"
                           "workstation). A workstation keeps cores free for the\n"
                           "person using it; a dedicated node may be driven to its\n"
                           "slot limit. The default is the safe answer rather than\n"
                           "the common one.",
            .yamlKey = "node_class",
            .same = FieldEq<&NodeConfig::nodeClass>(),
            .component = &WorkerComponent,
        },
        {
            .primary = "--drain-timeout",
            .arity = Arity::Value,
            .operand = "=<duration>",
            .apply = AssignFrom<&NodeConfig::drainTimeout, ParseDrainTimeout>(),
            .explicitBit = &NodeConfig::drainTimeoutExplicit,
            .description = "how long a stop waits for compiles still running\n"
                           "before giving up and saying what it abandoned\n"
                           "(default 30s); 0s waits forever. Whole seconds, in\n"
                           "one of {duration-units}. Unbounded, the supervisor\n"
                           "decides instead and answers with SIGKILL and no\n"
                           "diagnostic.",
            .yamlKey = "drain_timeout",
            .same = FieldEq<&NodeConfig::drainTimeout>(),
            .component = &WorkerComponent,
        },
        {
            .primary = "--reserve-cores",
            .arity = Arity::Value,
            .operand = "=<n>",
            .apply = AssignFrom<&NodeConfig::reservedCores, ParseReservedCores>(),
            .description = "cores never offered to the fleet, overriding what the\n"
                           "node class reserves. 0 is a real answer and is not the\n"
                           "same as omitting the flag. Ignored when --slots names\n"
                           "a number, which is the operator's answer already.",
            .yamlKey = "reserve_cores",
            .same = FieldEq<&NodeConfig::reservedCores>(),
            .component = &WorkerComponent,
            .present = PresentIn<&NodeConfig::reservedCores>(),
        },
        {
            .primary = "--node-id",
            .arity = Arity::Value,
            .operand = "=<id>",
            .apply = AssignFrom<&NodeConfig::nodeId, ParseId>(),
            // **No `explicitBit`, alone among the value flags, and its removal is the
            // point rather than an omission.** A provenance bit exists so a
            // registration does not bake in a DEFAULT that a later build might change
            // -- and since #1024 this flag has no default to bake: it holds a value
            // RESOLVED on this machine and recorded beside its Raft log, which the
            // registration must carry whether or not anybody typed it. The bit had
            // exactly one reader, `MakeNodeServiceSpec`, and that reader now asks the
            // value; a bit nothing reads is a claim nothing can check.
            .description = "this node's identity in the cluster, and what every\n"
                           "vote is counted against. --listen-raft is what turns\n"
                           "consensus on; this names the node that runs it.",
            .yamlKey = "node_id",
            .same = FieldEq<&NodeConfig::nodeId>(),
        },
        {
            .primary = "--listen-raft",
            .arity = Arity::Value,
            .operand = "=[<address>:]<port>",
            .apply = AssignFrom<&NodeConfig::raftListen, ParseText>(),
            .explicitBit = &NodeConfig::raftListenExplicit,
            .description = "where peers reach this node's consensus port; 6680\n"
                           "unless given. The port is what turns consensus ON, so\n"
                           "a node with no flags is a cluster of one, and an\n"
                           "empty --listen-raft= runs none. A bare port binds the\n"
                           "WILDCARD: peers are on other machines by definition,\n"
                           "so loopback would silently not work.",
            .yamlKey = "listen_raft",
            .same = FieldEq<&NodeConfig::raftListen>(),
        },
        {
            .primary = "--raft-self",
            .arity = Arity::Value,
            .operand = "=<host>",
            .apply = AssignFrom<&NodeConfig::raftSelf, ParseText>(),
            .explicitBit = &NodeConfig::raftSelfExplicit,
            .description = "the host this node's peers dial it at; the port comes\n"
                           "from --listen-raft. This machine's fully qualified\n"
                           "name unless given. How a node names ITSELF when its\n"
                           "identity was derived rather than typed. A bare\n"
                           "--listen-raft binds the wildcard, so what this\n"
                           "node binds is usually not what a peer can dial.",
            .yamlKey = "raft_self",
            .same = FieldEq<&NodeConfig::raftSelf>(),
        },
        {
            .primary = "--cluster-dir",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::clusterDir, ParsePathValue>(),
            .description = "this node's state directory: its identity key,\n"
                           "its id, and where consensus keeps its durable state.\n"
                           "A node that answered a vote and forgot it votes\n"
                           "twice in one term after a restart, which is two\n"
                           "leaders; a node that lost it is a new identity.\n"
                           "Unless given: the machine's (%ProgramData%, /var/lib or\n"
                           "/Library/Application Support, /fastcache-node) for a\n"
                           "privileged process or a service, the account's own\n"
                           "(%LOCALAPPDATA% or ~/.local/state) otherwise.",
            .yamlKey = "cluster_dir",
            .same = FieldEq<&NodeConfig::clusterDir>(),
        },
        { .primary = "--cordon",
          .arity = Arity::None,
          .apply = SelectOutcome<&NodeConfig::cordon, CordonCommand::Cordon>(),
          .description = "cordon the worker running on THIS machine, then exit:\n"
                         "it refuses new compiles and finishes the running ones,\n"
                         "and logs once it has drained. Asks this node's own\n"
                         "--listen-node. Held in memory: a restart lifts it." },
        { .primary = "--uncordon",
          .arity = Arity::None,
          .apply = SelectOutcome<&NodeConfig::cordon, CordonCommand::Lift>(),
          .description = "lift the cordon on the worker running on this machine,\n"
                         "then exit." },
        { .primary = "--cluster-status",
          .arity = Arity::None,
          .apply = SelectClusterAction<ClusterAction::Status>(),
          .description = "ask the cluster at --scheduler what it has agreed,\n"
                         "print it and exit. Answered by the LEADER; a\n"
                         "follower says who to ask instead." },
        { .primary = "--cluster-set",
          .arity = Arity::Value,
          .operand = "=<name>=<value>",
          .apply = SelectClusterAction<ClusterAction::Set>(),
          .description = "change one replicated cluster setting and exit.\n"
                         "Every member then agrees on it, and it survives\n"
                         "their restarts. --cluster-status lists the keys." },
        { .primary = "--cluster-admit",
          .arity = Arity::Value,
          .operand = "=<id>=<host>:<port>[@<key>]",
          .apply = SelectClusterAction<ClusterAction::Admit>(),
          .description = "add a member to the cluster and exit, or record that\n"
                         "one has moved. Both halves in one token, because an\n"
                         "id with no address is counted towards quorum and\n"
                         "never reached. The member itself must be waiting to\n"
                         "be admitted, never the founder of a cluster of its\n"
                         "own. A VOTER: counted by every quorum. On a\n"
                         "learner this promotes it. @<key> records the\n"
                         "member's identity key, as its --print-identity prints\n"
                         "it; without one, a key already recorded stays." },
        { .primary = "--cluster-admit-learner",
          .arity = Arity::Value,
          .operand = "=<id>=<host>:<port>[@<key>]",
          .apply = SelectClusterAction<ClusterAction::AdmitLearner>(),
          .description = "add a member as a LEARNER and exit, or move a voter\n"
                         "into that set. A learner is replicated to and\n"
                         "counted by nothing: it never votes and never stands\n"
                         "for election, so a machine that comes and goes --\n"
                         "a laptop on a VPN -- costs the cluster no quorum\n"
                         "while it is away. It follows the leader and\n"
                         "redirects writes to it as any follower does.\n"
                         "--cluster-admit on it promotes it; the same token\n"
                         "as --cluster-admit, and the member must likewise\n"
                         "be waiting to be admitted." },
        { .primary = "--cluster-forget",
          .arity = Arity::Value,
          .operand = "=<node-id>",
          .apply = SelectClusterAction<ClusterAction::Forget>(),
          .description = "forget a machine and exit: take the id out of the\n"
                         "cluster -- a voter or a learner -- and\n"
                         "revoke the key it was admitted under, which is\n"
                         "never admitted again: it can come back only under\n"
                         "a new identity (a fresh --cluster-dir). The one\n"
                         "membership change nothing automatic makes:\n"
                         "discovery only ever adds, because a peer goes\n"
                         "quiet far more often than it leaves." },
        {
            .primary = "--discovery",
            .arity = Arity::Value,
            .operand = "=<address>:<port>",
            .apply = AssignFrom<&NodeConfig::discoveryAddress, ParseText>(),
            .explicitBit = &NodeConfig::discoveryAddressExplicit,
            .description = "announce this node on the segment and listen for peers\n"
                           "here. Unless given, beacons go to every up interface's\n"
                           "directed broadcast on port 6681 -- never the limited\n"
                           "broadcast, which leaves by one guessed interface; a\n"
                           "named address is used exactly, and empty turns it\n"
                           "off. Runs beside consensus only. A peer\n"
                           "counts only when it proves the key the cluster holds\n"
                           "for it, and joins as a learner, counted by no quorum;\n"
                           "--cluster-admit promotes it. Any other key is\n"
                           "reported, never admitted.",
            .yamlKey = "discovery",
            .same = FieldEq<&NodeConfig::discoveryAddress>(),
        },
        {
            .primary = "--discovery-reply-port",
            .arity = Arity::Value,
            .operand = "=<n>",
            .apply = AssignFrom<&NodeConfig::discoveryReplyPort, ParseNodePort>(),
            .explicitBit = &NodeConfig::discoveryReplyPortExplicit,
            .description = "port peers unicast their discovery challenges and\n"
                           "proofs to; kernel-chosen unless given. NOT the\n"
                           "--discovery port: that one is shared by every node on\n"
                           "the segment, and only one socket sharing a port is\n"
                           "handed a unicast. Pin it where a host firewall opens\n"
                           "named ports only -- one per node on the machine.",
            .yamlKey = "discovery_reply_port",
            .same = FieldEq<&NodeConfig::discoveryReplyPort>(),
        },
        {
            .primary = "--fleet-seed",
            .arity = Arity::Value,
            .operand = "=<host[:port]>",
            // Repeatable, and no provenance bit, for `--scheduler`'s reason: a list's default
            // is empty, so every value present is one the operator typed.
            .apply = AppendFrom<&NodeConfig::fleetSeeds, ParseFleetSeed>(),
            .description = "a machine in the fleet to ask, across a VPN where no\n"
                           "beacon reaches; repeatable. A name or name:port (the\n"
                           "node port, 6674, when none). Tried after the fleet\n"
                           "this node remembers and before DNS SRV\n"
                           "_fastcache._tcp.<domain>. Asked only while this node\n"
                           "is alone; a fleet it finds is joined by approval.",
            .yamlKey = "fleet_seed",
            .same = FieldEq<&NodeConfig::fleetSeeds>(),
            .clear = ClearList<&NodeConfig::fleetSeeds>(),
        },
        {
            .primary = "--fleet-id",
            .arity = Arity::Value,
            .operand = "=<cluster-id>@<key>[,<key>...]",
            .apply = AssignFrom<&NodeConfig::fleetPin, ParseFleetPin>(),
            .description = "the ONE fleet this node may belong to, and the voters\n"
                           "whose key it takes that fleet's word from: paste the\n"
                           "fleet-id `fastcache-cli node` prints on a machine of\n"
                           "that fleet. Without it discovery is trust-on-first-use:\n"
                           "anybody on the segment can prove an older fleet and be\n"
                           "joined. Pinned, the node joins no other fleet, and on its\n"
                           "way in takes no summary, answer or leader its fleet's\n"
                           "pinned voters did not sign; once joined, the fleet's\n"
                           "agreed state decides. An id with no key is refused:\n"
                           "every beacon carries the id.",
            .yamlKey = "fleet_id",
            // A new pin changes which fleet the running node may be in, and the formation it holds
            // was decided under the old one: a restart judges the record against it by name.
            .reloadable = Reloadable::No,
            .same = FieldEq<&NodeConfig::fleetPin>(),
            .present = PresentIn<&NodeConfig::fleetPin>(),
        },
        {
            .primary = "--admin-listen",
            .arity = Arity::Value,
            .operand = "=[<address>:]<port>",
            .apply = AssignFrom<&NodeConfig::adminListen, ParseText>(),
            .explicitBit = &NodeConfig::adminListenExplicit,
            .description = "serve /metrics and /healthz here; off unless given.\n"
                           "A bare port binds loopback: a scrape endpoint on a\n"
                           "public interface is an operator's decision, not a\n"
                           "default. /healthz is also the liveness probe this\n"
                           "worker otherwise has none of.",
            .yamlKey = "admin_listen",
            .same = FieldEq<&NodeConfig::adminListen>(),
        },
        {
            .primary = "--dashboard",
            .apply = SetTrue<&NodeConfig::dashboard>(),
            .description = "also serve the fleet dashboard on --admin-listen, at\n"
                           "/fleet and /fleet.json. Off unless given: the page is a\n"
                           "map of every member's hostname, endpoint and capacity.\n"
                           "Answered in full only while this node LEADS; anyone else\n"
                           "names the leader rather than showing half a fleet.",
            .yamlKey = "dashboard",
            .same = FieldEq<&NodeConfig::dashboard>(),
        },
        {
            .primary = "--dashboard-token-file",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::dashboardTokenFile, ParsePathValue>(),
            .description = "credential the fleet requires: the dashboard, as Basic or\n"
                           "Bearer, and a live-stats fleet SUBSCRIBE, which carries\n"
                           "it in the request. Without one the fleet streams to this\n"
                           "machine only. A FILE and not a flag: a command line is\n"
                           "readable through ps. Its own secret rather than\n"
                           "--requirepass, which every member of the fleet already\n"
                           "holds. Required when --admin-listen is not on loopback.",
            .yamlKey = "dashboard_token_file",
            .same = FieldEq<&NodeConfig::dashboardTokenFile>(),
        },
        {
            .primary = "--tls-self-signed",
            .apply = SetTrue<&NodeConfig::tlsSelfSigned>(),
            .description = "generate a self-signed certificate at startup and serve\n"
                           "the admin surface over HTTPS with it, so an internal\n"
                           "deployment needs no certificate to obtain. Encrypts the\n"
                           "traffic; it does NOT prove which node answered, so the\n"
                           "fingerprint is logged for you to compare. Regenerated\n"
                           "every restart -- name --tls-cert for a stable identity.",
            .yamlKey = "tls_self_signed",
            .same = FieldEq<&NodeConfig::tlsSelfSigned>(),
        },
        {
            .primary = "--tls-cert",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::tlsCertFile, ParsePathValue>(),
            .description = "serve the admin surface over HTTPS with this\n"
                           "certificate. TLS is on by naming a certificate and a\n"
                           "key rather than by a flag, so there is no way to ask\n"
                           "for it without the material to do it.",
            .yamlKey = "tls_cert",
            .same = FieldEq<&NodeConfig::tlsCertFile>(),
        },
        {
            .primary = "--tls-key",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::tlsKeyFile, ParsePathValue>(),
            .description = "private key for --tls-cert. Both or neither.",
            .yamlKey = "tls_key",
            .same = FieldEq<&NodeConfig::tlsKeyFile>(),
        },
        {
            .primary = "--fleet-open",
            .arity = Arity::None,
            .apply = SetTrue<&NodeConfig::fleetOpen>(),
            .description = "admit every caller to this node, keyed or not. For\n"
                           "one machine, or a network that is already the\n"
                           "boundary. A revoked key is refused even here.",
            .yamlKey = "fleet_open",
            // Reloadable since #405, and it has two directions with an asymmetry:
            // turning it ON widens, turning it OFF narrows, and narrowing is the half
            // nothing would report -- a caller the operator has just shut out went on
            // being served until the next restart. `NodeMembership::Adopt` is what
            // makes the row true, and `ValidateNodeReloadable` is what keeps it safe: a
            // candidate that WIDENS admission on a node with no roster to check grants
            // against is refused, because such a node built an unchecked lease
            // validator at startup. LOCAL rather than advertised: this decides who this
            // node SERVES, and a registration says which toolchains it serves rather
            // than to whom. `fleet_open: false` in a
            // file is how it is turned off, and it works because a reload builds the
            // candidate FRESH -- the key spells the flag, so a key set to `false`
            // passes nothing and the fresh configuration is simply not opened.
            .reloadable = Reloadable::Yes,
            .same = FieldEq<&NodeConfig::fleetOpen>(),
        },
        {
            .primary = "--cache-memory",
            .arity = Arity::Value,
            .operand = "=<size>",
            .apply = AssignFrom<&NodeConfig::cacheMemoryBytes, ParseCacheBytes>(),
            .explicitBit = &NodeConfig::cacheMemoryExplicit,
            .description = "size of this node's own in-memory cache tier;\n"
                           "k/m/g = KiB/MiB/GiB or N% of host RAM. Defaults\n"
                           "to 25% of RAM within [512m, 8g]; 0 turns it off.\n"
                           "It exists so a local rebuild on a slow or bad\n"
                           "network never reaches the wire at all.",
            .yamlKey = "cache_memory",
            .same = FieldEq<&NodeConfig::cacheMemoryBytes>(),
        },
        {
            .primary = "--cache-disk",
            .arity = Arity::Value,
            .operand = "=<bytes>",
            .apply = AssignFrom<&NodeConfig::cacheDiskBytes, ParseCacheDiskBytes>(),
            .explicitBit = &NodeConfig::cacheDiskBytesExplicit,
            .description = "cap this node's on-disk cache tier at this size\n"
                           "(default 0, meaning grow as needed): what the store\n"
                           "occupies on disk, compressed values at their\n"
                           "compressed size, so compression makes it hold more.\n"
                           "Only means anything with --cache-dir: without a\n"
                           "path there is no disk tier for a budget to bound.",
            .yamlKey = "cache_disk",
            .same = FieldEq<&NodeConfig::cacheDiskBytes>(),
        },
        {
            .primary = "--shared-cache-disk",
            .arity = Arity::Value,
            .operand = "=<bytes>",
            .apply = AssignFrom<&NodeConfig::sharedCacheDiskBytes, ParseCacheDiskBytes>(),
            .explicitBit = &NodeConfig::sharedCacheDiskBytesExplicit,
            .description = "cap the shared tier this node serves when the fleet's\n"
                           "shared-cache setting names it (default 64g; 0 grows\n"
                           "as needed). Kept under <state-dir>/shared-cache.",
            .yamlKey = "shared_cache_disk",
            .same = FieldEq<&NodeConfig::sharedCacheDiskBytes>(),
        },
        {
            .primary = "--cache-dir",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::cacheDir, ParsePathValue>(),
            .description = "back the local cache tier with disk at this path.\n"
                           "Memory-only otherwise: a disk tier is a resource an\n"
                           "operator should have to name. ONE node per path,\n"
                           "enforced: the store is claimed exclusively, so a\n"
                           "second node sharing it refuses to start.",
            .yamlKey = "cache_dir",
            .same = FieldEq<&NodeConfig::cacheDir>(),
        },
        // The six compression settings, in the daemon's spelling and with the daemon's
        // defaults. One concept, one name across both binaries: an operator moving
        // between a `fastcached` and a worker types the same flag, and the shipped
        // reference blocks read the same in both files.
        //
        // All six are `Reloadable::No`, and DELIBERATELY rather than by inheritance
        // (#756 asked for the distinction to be stated where the next reader finds it).
        // The reason is reach, not safety: a value is decodable whatever the setting
        // later becomes, because every entry is stamped with the codec it was written
        // under -- but `SetCompression` is an unsynchronised member write, the L1
        // instances are built inside `ShardedStorage`/`LayeredStorage` with no handle
        // that outlives `BuildStorage`, and the disk codec is fixed at
        // `CowTreeStorage::Open`. Publishing a new value that reached no tier would be
        // a configuration claiming something about a live object that is not true.
        {
            .primary = "--compression",
            .arity = Arity::Value,
            .operand = "=<codec>",
            .apply = AssignFrom<&NodeConfig::compression, ParseCompressionCodec>(),
            .explicitBit = &NodeConfig::compressionExplicit,
            .description = "on-disk value codec for --cache-dir:\n"
                           "none|lz4|zstd (default zstd). Reads always return\n"
                           "plaintext and each record decodes by its own tag,\n"
                           "so changing this needs no migration.",
            .yamlKey = "compression",
            .reloadable = Reloadable::No,
            .same = FieldEq<&NodeConfig::compression>(),
        },
        {
            .primary = "--compression-level",
            .arity = Arity::Value,
            .operand = "=<N>",
            .apply = AssignFrom<&NodeConfig::compressionLevel, ParseCompressionLevel>(),
            .explicitBit = &NodeConfig::compressionLevelExplicit,
            .description = "codec effort level for --compression\n"
                           "(1..22; default 3, zstd).",
            .yamlKey = "compression_level",
            .reloadable = Reloadable::No,
            .same = FieldEq<&NodeConfig::compressionLevel>(),
        },
        {
            .primary = "--compression-min-bytes",
            .arity = Arity::Value,
            .operand = "=<size>",
            .apply = AssignFrom<&NodeConfig::compressionMinBytes, ParseCompressionMinBytes>(),
            .explicitBit = &NodeConfig::compressionMinBytesExplicit,
            .description = "skip on-disk compression for values smaller than\n"
                           "this; k/m/g accepted (default 256).",
            .yamlKey = "compression_min_bytes",
            .reloadable = Reloadable::No,
            .same = FieldEq<&NodeConfig::compressionMinBytes>(),
        },
        {
            .primary = "--memory-compression",
            .arity = Arity::Value,
            .operand = "=<codec>",
            .apply = AssignFrom<&NodeConfig::memoryCompression, ParseCompressionCodec>(),
            .explicitBit = &NodeConfig::memoryCompressionExplicit,
            .description = "in-memory value codec for --cache-memory:\n"
                           "none|lz4|zstd (default none). Independent of\n"
                           "--compression, which is the on-disk one. The budget\n"
                           "counts COMPRESSED bytes, so a codec makes the tier\n"
                           "hold more, at a decompress on every read.",
            .yamlKey = "memory_compression",
            .reloadable = Reloadable::No,
            .same = FieldEq<&NodeConfig::memoryCompression>(),
        },
        {
            .primary = "--memory-compression-level",
            .arity = Arity::Value,
            .operand = "=<N>",
            .apply = AssignFrom<&NodeConfig::memoryCompressionLevel, ParseCompressionLevel>(),
            .explicitBit = &NodeConfig::memoryCompressionLevelExplicit,
            .description = "codec effort level for --memory-compression\n"
                           "(1..22; default 3, zstd).",
            .yamlKey = "memory_compression_level",
            .reloadable = Reloadable::No,
            .same = FieldEq<&NodeConfig::memoryCompressionLevel>(),
        },
        {
            .primary = "--memory-compression-min-bytes",
            .arity = Arity::Value,
            .operand = "=<size>",
            .apply = AssignFrom<&NodeConfig::memoryCompressionMinBytes, ParseCompressionMinBytes>(),
            .explicitBit = &NodeConfig::memoryCompressionMinBytesExplicit,
            .description = "keep in-memory values smaller than this\n"
                           "uncompressed; k/m/g accepted (default 4096).",
            .yamlKey = "memory_compression_min_bytes",
            .reloadable = Reloadable::No,
            .same = FieldEq<&NodeConfig::memoryCompressionMinBytes>(),
        },
        {
            .primary = "--listen-node",
            .arity = Arity::Value,
            .operand = "=[<address>:]<port>",
            .apply = AssignFrom<&NodeConfig::nodeListen, ParseText>(),
            .explicitBit = &NodeConfig::nodeListenExplicit,
            .description = "this node's 0xFC port: cache verbs, and the\n"
                           "scheduler verbs where its mode serves them (default\n"
                           "port 6674, where fastcache-cc already looks; empty\n"
                           "closes it). Binds the wildcard unless given, because\n"
                           "every node is a fleet participant. That admits nobody\n"
                           "new to the cache: those verbs answer this machine\n"
                           "alone whatever this is bound to, and every other verb\n"
                           "refuses a caller that is not a member.",
            .yamlKey = "listen_node",
            .same = FieldEq<&NodeConfig::nodeListen>(),
        },
        {
            .primary = "--upstream",
            .arity = Arity::Value,
            .operand = "=<host:port>",
            .apply = AssignFrom<&NodeConfig::upstream, ParseText>(),
            .explicitBit = &NodeConfig::upstreamExplicit,
            .description = "the shared fastcached this node reads through to.\n"
                           "Empty is honest rather than broken: one developer's\n"
                           "machine has no shared cache.",
            .yamlKey = "upstream",
            .same = FieldEq<&NodeConfig::upstream>(),
        },
        {
            .primary = "--requirepass",
            .arity = Arity::Value,
            .operand = "=<secret>",
            .apply = AssignFrom<&NodeConfig::requirePass, ParseText>(),
            .description = "the password presented to the --upstream fastcached,\n"
                           "and to nothing else: a scheduler admits this machine\n"
                           "by its node proof and checks no password.",
            .yamlKey = "requirepass",
            // Reloadable since #404, and the whole of what made it possible is that
            // this secret is presented and never required. An INBOUND credential
            // cannot be rotated by one machine at a time -- every client would have
            // to move with it -- while an outbound one is exactly what a fleet-wide
            // rotation needs: each worker adopts the new secret when its operator
            // says so, and a worker still holding the old one fails visibly at the
            // one peer that has already moved.
            //
            // It is `LocalReloadableFlags` rather than advertised. A registration
            // says which toolchains this node serves; the credential it presents
            // while saying so is not part of the claim, so re-deriving the toolchain
            // set on a rotation would spend an include-tree walk telling the fleet
            // nothing.
            //
            // The row alone does NOT make a rotation reach anybody. Three sites took
            // a copy of this field at construction, and the reload publishing a
            // snapshot none of them read is the "green while doing nothing" failure
            // in its most expensive form. `Node::ICredentialSource` is what they read
            // through now, and what stops a fourth site taking a copy instead is the
            // `[node][credential][seam]` case, which walks this directory's sources.
            // Named by its TAG: this said `node-credential-seam`, a phrase that matches
            // nothing runnable, which reads as a guard that was never written.
            .reloadable = Reloadable::Yes,
            .same = FieldEq<&NodeConfig::requirePass>(),
        },
        {
            .primary = "--log-level",
            .arity = Arity::Value,
            .operand = "=<level>",
            .apply = AssignFrom<&NodeConfig::logLevel, ParseNodeLogLevel>(),
            .explicitBit = &NodeConfig::logLevelExplicit,
            .description = "trace, debug, info, warn, error, fatal (default info)",
            .yamlKey = "log_level",
            // The FIRST reloadable row, and it earns it: `ILogger::SetMinLevel`
            // exists, so raising the level to diagnose something takes effect on a
            // running worker without restarting it mid-build. `logTimestamps` next
            // door is deliberately NOT marked -- `ConsoleLogger` takes its timestamp
            // setting at construction and offers no setter, so marking it would
            // publish a snapshot the logger does not honour, which is the exact
            // disagreement the default guards against.
            .reloadable = Reloadable::Yes,
            .same = FieldEq<&NodeConfig::logLevel>(),
        },
        {
            // Beside `--log-level` because they are one concern, and two flags
            // because they are not one question: a level is a FILTER and this is a
            // FORMAT. Folding them into one grammar would invent a spelling neither
            // binary has.
            //
            // Spelled exactly as `fastcached`'s. An operator who learned it there
            // must not find the worker wanting a different word for the same thing --
            // the rule `--service-scope` already follows here.
            //
            // **No `explicitBit`, and that is the node's shape rather than an
            // omission.** The daemon carries `logTimestampsExplicit` because its
            // merge is `MergeField` copying field by field, so a file value and a
            // typed value are told apart per field or the command line stops winning.
            // This table applies a FILE and then argv through the same appliers, in
            // that order, so "the command line wins" is which loop runs second. The
            // node has no explicit-bit layer for booleans at all -- `--daemon`,
            // `--dashboard` and `--no-toolchain-discovery` have none either -- and a
            // default of false that only ever sets true has nothing to arrive at
            // without being asked for, which is the whole question a provenance bit
            // answers.
            .primary = "--log-timestamps",
            .arity = Arity::None,
            .apply = SetTrue<&NodeConfig::logTimestamps>(),
            .description = "prefix every log line with an ISO 8601 UTC timestamp\n"
                           "(default: on under macOS, where nothing else stamps a\n"
                           "service's output; off elsewhere)",
            .yamlKey = "log_timestamps",
            .same = FieldEq<&NodeConfig::logTimestamps>(),
        },
        {
            // The negative spelling, for the reason the daemon's carries: the DEFAULT
            // is platform-dependent now (#496, #507), so under macOS this is the only
            // way to say "off" -- and the only way a REGISTRATION can, since
            // `--install-service` replays its command line forever and a flag that can
            // only say "on" would turn an operator's explicit "off" back on at every
            // boot.
            //
            // **And a key, `no_log_timestamps`, for the same reason.** A file reaches
            // this setting through the rows' own appliers, and a presence key applies on
            // `true` alone -- so `log_timestamps: false` means "do not pass
            // `--log-timestamps`", which is the platform default and is ON under macOS.
            // This row was once kept out of files on the claim that `log_timestamps` "wins
            // in both directions"; through `ApplyFileSettings` it never did, and a worker's
            // `log_timestamps: false` left macOS stamping (#1437).
            .primary = "--no-log-timestamps",
            .arity = Arity::None,
            .apply = SetFalse<&NodeConfig::logTimestamps>(),
            .description = "do not prefix log lines with a timestamp, overriding the\n"
                           "platform default. The one way to ask for unstamped output\n"
                           "under macOS",
            .yamlKey = "no_log_timestamps",
            .same = FieldEq<&NodeConfig::logTimestamps>(),
        },
        { .primary = "--daemon",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::daemon>(),
          .description = "run in the background (POSIX), or as the service body\n"
                         "the Windows SCM starts. Supervisors that manage a\n"
                         "foreground process -- systemd and launchd -- must NOT\n"
                         "pass it: they reap a job that forks as 'exited'." },
        {
            .primary = "--pidfile",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::pidfile, ParseText>(),
            .description = "write the pid here when daemonizing (POSIX)",
            .yamlKey = "pidfile",
            .same = FieldEq<&NodeConfig::pidfile>(),
        },
        { .primary = "--install-service",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::installService>(),
          .description = "register this worker with the platform's supervisor\n"
                         "(Windows SCM, macOS launchd) and exit. Every other flag\n"
                         "on this command line is baked into the registration." },
        { .primary = "--uninstall-service",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::uninstallService>(),
          .description = "remove that registration and exit" },
        { .primary = "--migrate-cache",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::migrateCache>(),
          .description = "convert the --cache-dir store to this build's on-disk\n"
                         "record layout and exit, instead of serving. Run it\n"
                         "with the worker STOPPED. Safe to re-run: a store\n"
                         "already in this layout is left untouched, and a run\n"
                         "that is interrupted resumes where it stopped" },
        { .primary = "--service-name",
          .arity = Arity::Value,
          .operand = "=<name>",
          .apply = AssignFrom<&NodeConfig::serviceName, ParseText>(),
          .description = "name the supervisor keys the registration on\n"
                         "(default FastCacheCompileNode). Distinct from the\n"
                         "daemon's by default: a machine may run both, and one\n"
                         "name would make installing either displace the other." },
        { .primary = "--service-scope",
          .arity = Arity::Value,
          .operand = "=<user|system>",
          .apply = AssignFrom<&NodeConfig::serviceScope, ParseNodeServiceScope>(),
          .description = "which supervisor domain to register in (default system).\n"
                         "Ignored on Windows, which has only one." },
        { .primary = "--service-start",
          .arity = Arity::Value,
          .operand = "=<auto|manual>",
          .apply = AssignFrom<&NodeConfig::serviceStart, ParseNodeServiceStart>(),
          .description = "how --install-service registers this worker (default\n"
                         "auto): auto starts with the machine, manual waits to be\n"
                         "started. Install-time only. On macOS a manual system\n"
                         "job is restarted after a crash but not after a clean\n"
                         "non-zero exit: launchd's keep-alive would also start it\n"
                         "at boot" },
        { .primary = "--firewall-allow",
          .arity = Arity::Value,
          .operand = "=<address[/prefix]>",
          .apply = AppendFrom<&NodeConfig::firewallAllow, ParseNodeFirewallScope>(),
          .description = "limit the firewall rules --install-service creates to\n"
                         "these remote addresses (IPv4 or IPv6, with an optional\n"
                         "/prefix; repeatable). Omit it to allow any address;\n"
                         "/0 is refused. Install-time only" },
        { .primary = "--help",
          .alias = "-h",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::help>(),
          .flow = ParseFlow::Stop,
          .description = "show this help and exit" },
        // `ParseFlow::Continue`, unlike `--help` and `--version`, and the difference
        // is the whole point of the flag. Those two ignore the rest of the command
        // line; this one REPORTS on it. With `Stop`, `--print-surfaces
        // --listen-node 6675` parsed nothing after the first flag and printed
        // the defaults -- a worksheet that silently describes a different node from
        // the one the operator asked about, which is the misleading-document failure
        // this flag exists to prevent.
        {
            .primary = "--seed-config",
            .arity = Arity::Value,
            .operand = "=<path>",
            .apply = AssignFrom<&NodeConfig::seedConfigTemplate, ParseText>(),
            .description = "copy <path> to the machine-wide config location, but\n"
                           "only when no config is there yet, then exit (used by\n"
                           "the installer; needs the same privileges as writing\n"
                           "that location). This worker seeds its OWN file: the\n"
                           "destination comes from the table this binary's startup\n"
                           "lookup walks, so the two cannot drift apart.",
        },
        { .primary = "--print-identity",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::printIdentity>(),
          .description = "print this node's id, its identity key and the\n"
                         "--cluster-admit line that admits it, and exit. Mints\n"
                         "the id and the key into the state directory when it\n"
                         "holds none yet, which is how an operator learns a\n"
                         "member's key before it is admitted: run it as the\n"
                         "account the node runs as, with the flags it runs\n"
                         "with." },
        { .primary = "--print-surfaces",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::printSurfaces>(),
          .description = "list every port this configuration would open,\n"
                         "with its protocol, and exit. Generated from the\n"
                         "node's own port map rather than from prose, so a\n"
                         "firewall list comes from the binary that binds\n"
                         "them. Pass the flags you would run with: it\n"
                         "prints what THIS configuration serves, not the\n"
                         "defaults." },
        { .primary = "--enroll-list",
          .arity = Arity::None,
          .apply = SelectEnrollAction<EnrollAction::List>(),
          .description = "print what is waiting at the enrollment window and\n"
                         "exit. Shows each machine's identity key WHOLE -- the\n"
                         "string to compare with what that machine printed --\n"
                         "the fingerprint of the roster it was handed, and its\n"
                         "claimed address beside the host it actually came\n"
                         "from. A disagreement there is marked, not refused:\n"
                         "DNS, NAT and multi-homing all produce it." },
        { .primary = "--enroll-approve",
          .arity = Arity::Value,
          .operand = "=<node-id>@<key>",
          .apply = SelectEnrollAction<EnrollAction::Approve>(),
          .description = "admit the named machine to the cluster under the key\n"
                         "named with it, then exit. Compare that key with the\n"
                         "one the machine printed first: the comparison is the\n"
                         "whole of what makes this safe. --enroll-list prints\n"
                         "this line for every waiting row; a row whose key is\n"
                         "not the one named is refused, and nothing admitted." },
        { .primary = "--enroll-reject",
          .arity = Arity::Value,
          .operand = "=<node-id>",
          .apply = SelectEnrollAction<EnrollAction::Reject>(),
          .description = "refuse the named machine, then exit. It is told so\n"
                         "and stops asking. A machine still WAITING was never\n"
                         "committed to the cluster and needs no\n"
                         "--cluster-forget. One already approved is a\n"
                         "different matter: the approval committed it, so this\n"
                         "stops it being handed the roster but does NOT remove\n"
                         "it, and --cluster-forget is what does." },
        { .primary = "--enroll-clear",
          .arity = Arity::None,
          .apply = SelectEnrollAction<EnrollAction::Clear>(),
          .description = "drop every request on the leader's enrollment list\n"
                         "that nobody has decided about, then exit. Approved\n"
                         "and rejected rows stay, and a machine still asking\n"
                         "is recorded again at its next poll: this makes room\n"
                         "on a list somebody filled, it bans nobody." },
        { .primary = "--enroll-auto-approve",
          .arity = Arity::Value,
          .operand = "=<duration>|off",
          .apply = SelectEnrollAutoApprove(),
          .description = "arm the leader's auto-approve window for this long\n"
                         "from now, then exit: until it ends, any machine that\n"
                         "asks is admitted under the key it asks with, with\n"
                         "nobody comparing it, and --enroll-list marks it. At\n"
                         "most 24h; running it again re-arms from now, and\n"
                         "=off ends it. A restart or a change of leader ends\n"
                         "it too: it is held in the leader's memory alone. It\n"
                         "admits rows already waiting as well, at their next\n"
                         "poll: a row that must not be admitted is rejected\n"
                         "first." },
        { .primary = "--version",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::version>(),
          .flow = ParseFlow::Stop,
          .description = "print the version and exit" },
        // `ParseFlow::Continue`, unlike `--version`: the arguments AFTER it are what it checks.
        { .primary = "--check-arguments",
          .arity = Arity::None,
          .apply = SetTrue<&NodeConfig::checkArguments>(),
          .description = "parse every other argument and exit: success when each\n"
                         "one parses, the refusal naming the flag when one does\n"
                         "not. Reads no file, opens nothing and judges no rule\n"
                         "across flags. The Windows installer runs it over the\n"
                         "arguments it is about to remember and register, so a\n"
                         "value this node would refuse fails the install first." },
    });
    static_assert(TableIsWellFormed<NodeConfig>(options));

    // Every row a file may NOT carry, and why for each.
    //
    // The guard below reads this array rather than restating it, so the two cannot
    // disagree: a new flag either names a key or is listed here with a reason, and
    // there is no third state in which it is quietly unreachable from the file an
    // operator is told configures this worker.
    //
    // Two kinds live here and they are not the same objection. A one-shot verb
    // (`--install-service`, `--cluster-forget`, `--migrate-cache`, `--help`) is a
    // decision taken once; a file is read at EVERY start, so a key for one would
    // replay that decision forever -- a worker that re-registers itself, or asks
    // the cluster a question, instead of serving. The rest are settings that
    // describe how this process was STARTED rather than what it does, and reading
    // them from the very file the start already found is circular.
    //
    // Which of those two objections a row is. `scripts/check-node-fixture-starts.sh` reads the
    // `FileExclusion::Verb` rows' spellings as the node's one-shot verbs, so this column is the
    // one list of them: a verb added, retired or renamed here is one the scan follows.
    //
    // Private: read by this function and by that script's text scan, by NAME, and never
    // transmitted or persisted.
    enum class FileExclusion : std::uint8_t
    {
        Verb,    ///< Answers and exits; a key would replay it at every start.
        Context, ///< Describes how this process was started; a key would be circular.
    };
    struct NotFromFile
    {
        FileExclusion kind;      ///< Which objection.
        std::string_view flag;   ///< The row's primary spelling.
        std::string_view reason; ///< Why a file may not carry it.
    };
    static constexpr auto notFromFile = std::to_array<NotFromFile>({
        { .kind = FileExclusion::Context,
          .flag = "--config",
          .reason = "names the file being read; a key for it would name a file to read while reading one" },
        { .kind = FileExclusion::Context,
          .flag = "--daemon",
          .reason = "how this process was started, decided by whoever started it -- a service is already "
                    "supervised, and a file that forked an operator's foreground run would take away the "
                    "console they were watching" },
        { .kind = FileExclusion::Context,
          .flag = "--service-name",
          .reason = "the identity a registration is made under, read back from the file that "
                    "registration points at -- so the name would come from the file the name found" },
        { .kind = FileExclusion::Context,
          .flag = "--service-scope",
          .reason = "the same circle as --service-name, for which supervisor the registration goes to" },
        { .kind = FileExclusion::Context,
          .flag = "--service-start",
          .reason = "the same circle as --service-scope: how the supervisor starts the registration" },
        { .kind = FileExclusion::Context,
          .flag = "--firewall-allow",
          .reason = "install-time only: it scopes the rules the registration creates" },
        { .kind = FileExclusion::Verb,
          .flag = "--install-service",
          .reason = "registers and exits; a key would re-register at every start" },
        { .kind = FileExclusion::Verb,
          .flag = "--uninstall-service",
          .reason = "removes the registration and exits; a key would remove it at every start" },
        { .kind = FileExclusion::Verb,
          .flag = "--migrate-cache",
          .reason = "converts the store and exits; a key would convert at every start, on a store "
                    "that after the first run has nothing left to convert" },
        { .kind = FileExclusion::Verb,
          .flag = "--seed-config",
          .reason = "installs the file a key would be read from, then exits; a key for it would re-seed at every start" },
        { .kind = FileExclusion::Verb,
          .flag = "--print-surfaces",
          .reason = "prints the ports and exits; a key would print them instead of serving them" },
        { .kind = FileExclusion::Verb,
          .flag = "--print-identity",
          .reason = "prints this node's identity and exits; a key would print it instead of serving" },
        { .kind = FileExclusion::Verb,
          .flag = "--cordon",
          .reason = "cordons the running worker and exits; a key would cordon it at every start, which is a machine that "
                    "never comes back to the fleet" },
        { .kind = FileExclusion::Verb, .flag = "--uncordon", .reason = "lifts the running worker's cordon and exits" },
        { .kind = FileExclusion::Verb, .flag = "--cluster-status", .reason = "asks a running cluster a question and exits" },
        { .kind = FileExclusion::Verb, .flag = "--cluster-set", .reason = "changes a running cluster's settings and exits" },
        { .kind = FileExclusion::Verb, .flag = "--cluster-admit", .reason = "admits a member and exits" },
        { .kind = FileExclusion::Verb,
          .flag = "--cluster-admit-learner",
          .reason = "admits a member as a learner and exits; a key would re-admit it at every start, demoting a "
                    "member somebody had since promoted" },
        { .kind = FileExclusion::Verb, .flag = "--cluster-forget", .reason = "removes a member and exits" },
        { .kind = FileExclusion::Verb, .flag = "--enroll-list", .reason = "prints what is waiting and exits" },
        { .kind = FileExclusion::Verb,
          .flag = "--enroll-approve",
          .reason = "admits one machine and exits; a key would re-admit it at every start" },
        { .kind = FileExclusion::Verb, .flag = "--enroll-reject", .reason = "refuses one machine and exits" },
        { .kind = FileExclusion::Verb,
          .flag = "--enroll-auto-approve",
          .reason = "arms or ends the auto-approve window and exits" },
        { .kind = FileExclusion::Verb, .flag = "--enroll-clear", .reason = "drops the undecided requests and exits" },
        { .kind = FileExclusion::Verb, .flag = "--help", .reason = "prints usage and exits" },
        { .kind = FileExclusion::Verb, .flag = "--version", .reason = "prints the version and exits" },
        { .kind = FileExclusion::Verb,
          .flag = "--check-arguments",
          .reason = "parses the command line and exits; a key would check instead of serving" },
    });

    // A row is reachable from the file or it is named above. Checked at compile
    // time, because a flag that is silently absent from the file is not a condition
    // to report -- it is a setting an operator writes, restarts, and never sees take
    // effect, with nothing anywhere saying why.
    static_assert(std::ranges::all_of(options,
                                      [](OptionSpec<NodeConfig> const& spec) {
                                          return !spec.yamlKey.empty()
                                                 || std::ranges::any_of(notFromFile, [&spec](auto const& excluded) {
                                                        return excluded.flag == spec.primary;
                                                    });
                                      }),
                  "every --flag must carry a yamlKey or be listed in notFromFile with a reason");

    // And the converse: a row named above must not also carry a key, which is how
    // an exclusion becomes a comment describing something that stopped being true.
    static_assert(std::ranges::all_of(notFromFile,
                                      [](auto const& excluded) {
                                          return std::ranges::any_of(options, [&excluded](auto const& spec) {
                                              return spec.primary == excluded.flag && spec.yamlKey.empty();
                                          });
                                      }),
                  "every notFromFile entry must name a real row that carries no yamlKey");

    // A row a FILE can set must be comparable, and a row it cannot must not be.
    //
    // **Both directions, at compile time, because the correspondence was verified by
    // hand once and nothing kept it in step.** That is exactly
    // [#406](https://github.com/LASTRADA-Software/fastcached/issues/406)'s shape --
    // a hand-checked list beside a table -- and reproducing it inside the change that
    // gives its sibling a column would be the joke telling itself. Row 39 arrives
    // without a comparator, and without this nothing says so: a reload would then
    // read that field as unchanged forever, which is the silent-success failure this
    // whole mechanism exists to prevent.
    //
    // The forward direction is the load-bearing one. The converse matters too: a
    // comparator on a row no file can reach is dead code that reads as coverage, and
    // the next person to audit this would count it.
    static_assert(
        std::ranges::all_of(
            options, [](OptionSpec<NodeConfig> const& spec) { return spec.yamlKey.empty() == (spec.same == nullptr); }),
        "a row with a yamlKey needs a FieldEq comparator, and a row without one must not have it");

    // **Marking a row reloadable is a decision about the FLEET, not only about this
    // process, so every such row must say which.** A flag that feeds REGISTER has to
    // re-derive and re-register when it changes (`AdvertisedReloadableFlags`, which
    // `AdvertisedClaimsDiffer` walks); one that is local wiring must not, because that
    // re-derivation is minutes of include-tree walking. Nothing here can guess which a
    // new row is, so an unclassified one fails the build rather than defaulting into
    // the silent half -- a registration-bearing flag that goes live and never reaches
    // the scheduler.
    //
    // One assertion, not two. A count of `Reloadable::Yes` rows stood beside this and
    // is subsumed by it -- any row this does not name already fails here -- while
    // adding a hand-kept literal next to the very list it counted, which is the
    // "hand-checked list beside a table" shape the note above calls the joke telling
    // itself.
    //
    // **Walked over `ReloadableFlagLists`, for the reason its two converse guards below
    // already carry.** This spelled its lists by hand -- the exact shape #1027 records
    // one screen down -- so a THIRD kind of reloadable row silently satisfied nothing
    // here until somebody remembered to add a clause. It did not stay hypothetical:
    // `AddressReloadableFlags` is that third kind (#1279), and deriving the set is why
    // adding it cost one edit rather than two.
    static_assert(std::ranges::all_of(options,
                                      [](OptionSpec<NodeConfig> const& spec) {
                                          return spec.reloadable != Reloadable::Yes
                                                 || std::ranges::any_of(ReloadableFlagLists,
                                                                        [&spec](std::span<std::string_view const> list) {
                                                                            return std::ranges::contains(list, spec.primary);
                                                                        });
                                      }),
                  "a new Reloadable::Yes row must be classified by one of the ReloadableFlagLists");

    // And the converse, which is what makes `AdvertisedClaimsDiffer` safe to write as
    // a table walk: every name on either list is a real row AND carries a comparator.
    // Without this a listed flag that no row answers to would compare nothing and
    // report "unchanged" forever, and one whose row had no `same` would be a null call
    // on the reload path -- both silent, both at run time.
    //
    // **Walked over `ReloadableFlagLists` rather than over one list by name.** This
    // guard was written for `AdvertisedReloadableFlags` alone, with that reasoning
    // beside it, while `LocalReloadableFlags` -- declared three lines below it -- got
    // nothing; every word of the reasoning applied to both. Naming the lists here
    // instead of deriving them is what let that happen, and would let it happen again
    // for the third list (#1027).
    //
    // A `LocalReloadableFlags` entry naming no row is inert TODAY, because that list is
    // only ever asked `contains` -- which is a property of the current consumers rather
    // than of the list, and is exactly the silence that reads identically to coverage.
    // The next consumer that WALKS it inherits a dead entry with no signal.
    static_assert(std::ranges::all_of(ReloadableFlagLists,
                                      [](std::span<std::string_view const> list) {
                                          return std::ranges::all_of(list, [](std::string_view flag) {
                                              return std::ranges::any_of(
                                                  options, [flag](OptionSpec<NodeConfig> const& spec) {
                                                      return spec.primary == flag && spec.same != nullptr;
                                                  });
                                          });
                                      }),
                  "every reloadable-flag list entry must name a row that carries a FieldEq comparator");

    // And the direction neither list had at all: a listed flag whose row is
    // `Reloadable::No`. The row and the list then disagree about the one fact both
    // exist to state -- and `AdvertisedClaimsDiffer` walks that list, so a
    // `Reloadable::No` row named there is compared and re-advertised on every reload,
    // which is the setting being treated as reloadable by the code while the table says
    // it is not. Neither list violates this today; both facts were compile-time
    // checkable and left uncheckable, in a table that has taken four new rows recently.
    static_assert(std::ranges::all_of(ReloadableFlagLists,
                                      [](std::span<std::string_view const> list) {
                                          return std::ranges::all_of(list, [](std::string_view flag) {
                                              return std::ranges::any_of(
                                                  options, [flag](OptionSpec<NodeConfig> const& spec) {
                                                      return spec.primary == flag && spec.reloadable == Reloadable::Yes;
                                                  });
                                          });
                                      }),
                  "every reloadable-flag list entry must name a row that is Reloadable::Yes");

    return options;
}

bool AdvertisedClaimsDiffer(NodeConfig const& previous, NodeConfig const& candidate)
{
    return std::ranges::any_of(NodeOptions(), [&](OptionSpec<NodeConfig> const& spec) {
        return std::ranges::contains(AdvertisedReloadableFlags, spec.primary) && !spec.same(previous, candidate);
    });
}

std::optional<std::string> AdmissionAnnouncement(NodeConfig const& previous, NodeConfig const& current)
{
    if (previous.fleetOpen == current.fleetOpen)
        return std::nullopt;

    // Both directions, and the narrowing one is the one nothing else would say: turning
    // `--fleet-open` ON is announced by the first stranger it serves, while turning it OFF
    // refuses every caller the roster does not admit -- a set no list can enumerate, so the
    // sentence says what it is.
    auto const tail = previous.fleetOpen
                          ? std::string_view { "; --fleet-open is off, so every caller the roster does not admit is now "
                                               "refused" }
                          : std::string_view { "; --fleet-open is on, so every caller is now admitted, keyed or not" };

    return std::format(
        "admission policy reloaded: {} (was {}){}", AdmissionSummary(current), AdmissionSummary(previous), tail);
}

std::optional<std::string> AllowlistAnnouncement(AllowlistMoment moment,
                                                 std::span<std::string const> previous,
                                                 std::span<std::string const> current)
{
    if (moment == AllowlistMoment::Startup)
    {
        if (current.empty())
            return std::nullopt;
        return std::format("compile-argument allowlist extended by configuration with {} entry/entries: {}",
                           current.size(),
                           JoinAllowedArgs(current));
    }

    if (std::ranges::equal(previous, current))
        return std::nullopt;

    return std::format(
        "compile-argument allowlist reloaded; {} entry/entries now in force: {}", current.size(), JoinAllowedArgs(current));
}

std::optional<std::string> ObservabilityAnnouncement(NodeConfig const& cfg)
{
    // The single-machine install says nothing, and this is the clause that keeps it
    // quiet. The question is the one that decides the lease check -- could a machine that
    // is not this one reach this node at all -- asked of what the node is NOW. A node's
    // roster is the state its OWN consensus applies (T25), so a node running consensus is
    // asked as `Formed`: its formation record says whether other machines are members, and a
    // solitary one -- which serves its own machine only, on a wildcard port like every
    // node -- is not told it works for others.
    //
    // A node running no consensus holds no roster, so no key route admits anybody there:
    // `Unknown` counts one only where consensus runs (`AdmitsRemotePeers`), which leaves
    // `--fleet-open` and the formation record as the routes this asks about.
    auto const roster = RunsConsensus(cfg) ? RosterPresence::Formed : RosterPresence::Unknown;
    if (!CompileVerbsReachOtherMachines(cfg, roster))
        return std::nullopt;

    // The address `AdminEndpoint::Start` will actually take, asked of the surface's own
    // row. Empty is that row's spelling of "not served", so this and `--print-surfaces`
    // cannot disagree about whether the admin port is open.
    if (!RowFor(NodeSurface::Admin).Resolve(cfg).empty())
        return std::nullopt;

    // Says what is unavailable and which flag provides it, and says the node is fine --
    // an operator who reads this as a fault has been sent to look at a healthy worker.
    return std::string {
        "this node works for machines other than this one and opens no admin surface: /healthz, /metrics and the "
        "fleet dashboard are all served on --admin-listen, which is off unless asked for. The node is configured "
        "rather than broken -- it does exactly what it was told -- but with no admin surface there is nothing to "
        "probe it through: no /healthz for a supervisor on this machine, and nothing to scrape from "
        "any other. --admin-listen=<port> opens it, and a bare port "
        "binds loopback; the dashboard needs --dashboard beside it."
    };
}

std::vector<std::string_view> UnreloadableChanges(NodeConfig const& previous, NodeConfig const& candidate)
{
    std::vector<std::string_view> changed;
    for (auto const& spec: NodeOptions())
    {
        // A row with no comparator is not configuration state -- a one-shot verb, or
        // an install-time flag. No file can set it, so no reload can change it. The
        // static_assert beside the table is what keeps that true rather than assumed.
        if (spec.same == nullptr || spec.reloadable == Reloadable::Yes)
            continue;
        if (!spec.same(previous, candidate))
            changed.push_back(spec.primary);
    }
    return changed;
}

std::expected<void, ConfigError> ValidateNodeReloadable(NodeConfig const& previous, NodeConfig const& candidate)
{
    // **A candidate is judged by the STARTUP rules as well**, which is the same
    // composition `NodeInstallRejection` makes and for the same reason: whatever this
    // accepts becomes the configuration in force, so a reload must not be able to
    // reach a state the process would have refused to start in. Newly reachable
    // because #403 made the toolchain pair reloadable -- `no_toolchain_discovery: true`
    // with no `toolchain:` key is a worker with nothing to serve, refused at startup by
    // name and, until this, accepted by a reload that then quietly emptied the served
    // set on the heartbeat thread.
    //
    // Asked FIRST, because it describes the candidate on its own terms; the
    // immutability check below is about the pair.
    if (auto const rejection = StartupPolicyRejection(candidate))
        return std::unexpected(ConfigError { .code = ConfigErrorCode::ParseError,
                                             .source = {},
                                             .line = 0,
                                             .field = {},
                                             .context = std::format("not applied: {}", *rejection) });

    // The immutability rule, asked SECOND because it is about the pair.
    auto const changed = UnreloadableChanges(previous, candidate);
    if (!changed.empty())
    {
        std::string names;
        for (auto const& flag: changed)
        {
            if (!names.empty())
                names += ", ";
            names += flag;
        }

        return std::unexpected(ConfigError {
            .code = ConfigErrorCode::ImmutableChanged,
            .source = {},
            .line = 0,
            // The FIRST name, because the field is one string and something has to go
            // in it; the whole list is in the context, which is what an operator reads.
            .field = std::string { changed.front() },
            .context = std::format("not reloadable, so nothing was applied: {}", names),
        });
    }

    // No rule about the PAIR of configurations lives here. The one that did refused a reload widening
    // admission on a worker running no consensus, read off flag shapes, and review I-2 found the path
    // the shapes missed. Its replacement asks what the running worker BUILT, which no configuration
    // carries, so it is composed beside this by `ReloadCheckWith` (`WorkerLease.hpp`), last.

    return {};
}

Distributed::NodeCapacity NodeCapacityOf(NodeConfig const& cfg,
                                         IHostFactsSource const& host,
                                         Distributed::NodeCacheCapacity const& cache,
                                         std::uint64_t indexReserveBytes)
{
    // The tiers' own budgets PLUS what their key indexes cost, which is #175: a
    // disk-only node reserved zero and offered the fleet the whole machine while
    // holding an index that can run to hundreds of megabytes. Under-reserving is the
    // direction that over-commits -- the jobs come back as refusals the client retries
    // locally, so the build gets slower while distribution looks like it is working.
    //
    // Added here rather than inside `ResidentCacheBytes`, which folds tier BUDGETS and
    // is right to refuse a figure that is not one: a disk budget is denominated in
    // filesystem bytes and this is RAM. Clamped, because both terms are bounded by the
    // machine and their sum need not be.
    auto const tiers = ResidentCacheBytes(cache, host.TotalMemoryBytes());
    auto const reserved = std::min<std::uint64_t>(tiers + indexReserveBytes, host.TotalMemoryBytes());

    return Distributed::NodeCapacity { .logicalCores = host.LogicalCores(),
                                       .totalMemoryBytes = host.TotalMemoryBytes(),
                                       .reservedMemoryBytes = reserved,
                                       .nodeClass = cfg.nodeClass,
                                       // Absent is not zero, and this is the one line
                                       // where the two are told apart: a reserve the
                                       // operator typed as 0 means "drive this machine
                                       // to its last core", while one they never
                                       // mentioned means "use whatever the class
                                       // reserves". Collapsing them makes somebody's
                                       // desktop unusable in one direction and wastes
                                       // a build server in the other.
                                       .reservedCores = cfg.reservedCores.value_or(0),
                                       .reserveIsExplicit = cfg.reservedCores.has_value(),
                                       // The same record the reservation was derived
                                       // from, carried on to the leader -- so what
                                       // this node holds back and what the fleet is
                                       // shown cannot drift apart at this end.
                                       .cache = cache };
}

std::optional<std::filesystem::path> RegisteredStateDirectory(NodeConfig const& registration, IConfigPathProbe const& probe)
{
    if (!registration.clusterDir.empty())
        return registration.clusterDir;
    return MachineWideNodeClusterDirectory(probe);
}

ServiceSpec MakeNodeServiceSpec(std::filesystem::path const& exePath, NodeConfig const& cfg, IConfigPathProbe const& probe)
{
    std::vector<std::string> argv;

    /// Emit `--flag=value` when the operator TYPED it, whatever its value.
    ///
    /// **The only value-bearing emitter, and there is deliberately no sibling that
    /// compares against a default** (#713). There was one -- `emitIfSet` -- and
    /// fourteen rows used it. Deleting it rather than leaving it beside this is the
    /// point: reaching for it IS the mistake, so the fix is that there is nothing
    /// left to reach for.
    ///
    /// What it got wrong: a value comparison and a provenance question part company
    /// on the one input that matters, an operator typing the default. That is what
    /// somebody does after reading a value off the startup line to pin it -- and the
    /// flag was then dropped from the registration, so the service re-derived it at
    /// every start. A registration replays its command line forever, so the pin was
    /// lost permanently and silently.
    ///
    /// Emission by value is sound only while every default is a compile-time
    /// constant the next start re-derives identically. The node's were, when those
    /// fourteen rows were written. That is a property of the constants and not of
    /// the mechanism, and this binary has already watched it move: the
    /// `--log-timestamps` pair below carries the comment explaining what happened
    /// when `DefaultLogTimestamps` became platform-dependent in #496. The daemon's
    /// audit under #349 found FIVE unsafe rows once somebody looked, not the one
    /// that had been reported.
    ///
    /// So the bits are not on the rows whose defaults look risky today. That
    /// judgement would have to be re-made correctly by whoever next changes a
    /// constant, in a change that does not look like it touches registration at all.
    /// `NodeConfig_test` walks `NodeOptions()` and requires every row carrying an
    /// `explicitBit` to arrive here.
    auto const emitIfExplicit = [&argv](std::string_view flag, auto const& value, bool wasTyped) {
        if (wasTyped)
            argv.emplace_back(std::format("--{}={}", flag, value));
    };

    /// Emit a flag whose `optional` IS its provenance: absent and zero are different
    /// instructions, so a present value is emitted whatever it is.
    auto const emitIfPresent = [&argv](std::string_view flag, auto const& value) {
        if (value.has_value())
            argv.emplace_back(std::format("--{}={}", flag, *value));
    };

    /// Emit a path flag, made absolute.
    ///
    /// A service does not inherit the installing shell's working directory, so a
    /// relative path captured at install time resolves somewhere else at start --
    /// which for a pidfile means a supervisor that cannot find its own process.
    /// Resolve a path the same way `emitPathIfSet` does, for a spec FIELD.
    ///
    /// Shared with it rather than restated: a `configPath` that disagreed with the
    /// `--config=` in the very same registration is two answers to one question,
    /// and the refusal that reads the field would then name a path the service was
    /// never given.
    auto const absoluteOrAsWritten = [](std::string const& value) {
        if (value.empty())
            return std::string {};
        std::error_code ec;
        auto const absolute = std::filesystem::absolute(value, ec);
        return ec ? value : absolute.string();
    };

    auto const emitPathIfSet = [&argv, &absoluteOrAsWritten](std::string_view flag, std::string const& value) {
        if (value.empty())
            return;
        argv.emplace_back(std::format("--{}={}", flag, absoluteOrAsWritten(value)));
    };

    // Unconditional: it is what the running service identifies itself by, and on
    // launchd it is what the job label derives from.
    argv.push_back(std::format("--service-name={}", cfg.serviceName));

    // First, and made absolute like every other path: it is what supplies every
    // setting the operator did NOT type here, and a service does not inherit the
    // installing shell's working directory. Emitted rather than resolved -- an
    // empty value means the operator named no file, and the service repeats the
    // machine-wide lookup at each start, which is what lets a package replace that
    // file without touching the registration.
    emitPathIfSet("config", cfg.configPath);

    emitIfExplicit("advertise", cfg.advertise, cfg.advertiseExplicit);
    // On presence, for `--reserve-cores`' reason below: since #206 a zero is the
    // instruction that runs no worker.
    emitIfPresent("slots", cfg.slots);
    emitIfExplicit("node-class", std::string { Distributed::TraitsFor(cfg.nodeClass).name }, cfg.nodeClassExplicit);
    // Emitted on presence, because the difference this flag carries IS presence: a
    // reserve of zero the operator typed and a reserve nobody mentioned are
    // different instructions. The `optional` is this row's provenance bit, and it
    // predates the ones the other rows now carry.
    emitIfPresent("reserve-cores", cfg.reservedCores);
    emitIfExplicit("admin-listen", cfg.adminListen, cfg.adminListenExplicit);
    if (cfg.dashboard)
        argv.emplace_back("--dashboard");
    // The PATH, never the secret it holds -- the same rule `--requirepass` is
    // refused outright by, one step less strict because a path is not a credential.
    emitPathIfSet("dashboard-token-file", cfg.dashboardTokenFile.string());
    if (cfg.tlsSelfSigned)
        argv.emplace_back("--tls-self-signed");
    emitPathIfSet("tls-cert", cfg.tlsCertFile.string());
    emitPathIfSet("tls-key", cfg.tlsKeyFile.string());
    emitIfExplicit("cache-memory", cfg.cacheMemoryBytes, cfg.cacheMemoryExplicit);
    emitIfExplicit("cache-disk", cfg.cacheDiskBytes, cfg.cacheDiskBytesExplicit);
    emitIfExplicit("shared-cache-disk", cfg.sharedCacheDiskBytes, cfg.sharedCacheDiskBytesExplicit);
    // Codecs by NAME: `emitIfExplicit` formats its value, and a `CompressionCodec`
    // is a byte-wide enum, so the registration would otherwise bake in `2` -- which
    // the next start's own parser refuses.
    emitIfExplicit("compression", Compression::NameOf(cfg.compression), cfg.compressionExplicit);
    emitIfExplicit("compression-level", cfg.compressionLevel, cfg.compressionLevelExplicit);
    emitIfExplicit("compression-min-bytes", cfg.compressionMinBytes, cfg.compressionMinBytesExplicit);
    emitIfExplicit("memory-compression", Compression::NameOf(cfg.memoryCompression), cfg.memoryCompressionExplicit);
    emitIfExplicit("memory-compression-level", cfg.memoryCompressionLevel, cfg.memoryCompressionLevelExplicit);
    emitIfExplicit("memory-compression-min-bytes", cfg.memoryCompressionMinBytes, cfg.memoryCompressionMinBytesExplicit);
    emitIfExplicit("listen-node", cfg.nodeListen, cfg.nodeListenExplicit);
    // **The one flag emitted on VALUE rather than on provenance, and NOT through a
    // general-purpose emitter.** The provenance rule is not being broken here so much
    // as answered: `emitIfExplicit` exists so a registration does not bake a DEFAULT an
    // operator can then never change by editing anything. `emitIfSet` was the general
    // way to say what this row needs, fourteen rows used it, and #713 deleted it
    // precisely so that reaching for it is not possible -- an operator who types a
    // default is indistinguishable from one who did not, and the flag was then dropped
    // from a registration that replays forever.
    //
    // This row is the case that argument does not cover, so it is written out at the
    // call site rather than behind a name somebody else could reuse. Since #1024
    // `--node-id` has no default for a value comparison to be wrong about: it holds a
    // RESOLVED identity, minted on this machine and recorded beside its Raft log.
    // Omitting it would replay a command line whose meaning depends on a file an
    // operator may delete or a re-image may not carry -- the node would then answer to
    // an identity the cluster never admitted, the cluster would count a member that no
    // longer exists beside a stranger nobody admitted, and nothing anywhere would say
    // so. That is the failure with no symptom.
    if (!cfg.nodeId.empty())
        argv.push_back(std::format("--node-id={}", cfg.nodeId));
    emitIfExplicit("raft-self", cfg.raftSelf, cfg.raftSelfExplicit);
    emitIfExplicit("listen-raft", cfg.raftListen, cfg.raftListenExplicit);
    // The TYPED directory only, never the resolved default: the service resolves its own again
    // at every start, as the account it runs as (`stateDirectory` is never carried).
    emitPathIfSet("cluster-dir", cfg.clusterDir.string());
    emitIfExplicit("discovery", cfg.discoveryAddress, cfg.discoveryAddressExplicit);
    emitIfExplicit("discovery-reply-port", cfg.discoveryReplyPort, cfg.discoveryReplyPortExplicit);
    emitIfExplicit("upstream", cfg.upstream, cfg.upstreamExplicit);
    emitPathIfSet("cache-dir", cfg.cacheDir.string());
    if (cfg.fleetOpen)
        argv.emplace_back("--fleet-open");
    // One token per seed: stored normalized, so what is replayed is the same token the parser
    // produces from it.
    for (auto const& seed: cfg.fleetSeeds)
        argv.push_back(std::format("--fleet-seed={}", seed));
    if (cfg.fleetPin.has_value())
        argv.push_back(std::format("--fleet-id={}", Cluster::FormatPinnedFleet(*cfg.fleetPin)));
    emitIfExplicit("drain-timeout", FormatDuration(cfg.drainTimeout), cfg.drainTimeoutExplicit);
    emitIfExplicit("log-level", LogLevelName(cfg.logLevel), cfg.logLevelExplicit);

    // **Both spellings, because the DEFAULT is platform-dependent** (#496, #507).
    // Emitting only the positive one is sound while the default is false everywhere:
    // "it is on" is then the only thing a registration ever has to say. Under macOS
    // the default is true, so an operator who asked for `--no-log-timestamps` would
    // have that dropped from the registration and get timestamps back at every boot,
    // silently and forever -- a registration replays its command line.
    //
    // Compared against the DEFAULT rather than tested for truth, so the registration
    // carries a flag exactly when it has something to say. `--no-toolchain-discovery`
    // below keeps the one-sided shape: its default is true on every platform.
    if (cfg.logTimestamps != NodeConfig {}.logTimestamps)
        argv.emplace_back(cfg.logTimestamps ? "--log-timestamps" : "--no-log-timestamps");
    emitPathIfSet("pidfile", cfg.pidfile);

    // Repeatable, so one token per toolchain rather than one joined value: a
    // worker that came back serving fewer compilers than it was installed with
    // would present as a fleet that stopped matching, not as a packaging bug.
    //
    // A bare compiler path is left as the operator wrote it. It is NOT resolved
    // to a fingerprint here, deliberately: the worker derives that at startup
    // through the identical code its clients use, and baking a digest computed
    // at install time would pin the registration to a toolchain that an update
    // then changes underneath it.
    for (auto const& toolchain: cfg.toolchains)
        argv.push_back(std::format("--toolchain={}", toolchain));

    // Carried into the registration, because it changes what the service DOES at
    // every boot. A node installed with discovery off and no toolchain is refused
    // below; one installed with it off and a toolchain named must come back with it
    // still off, or the service quietly starts serving compilers the operator
    // deliberately excluded. On the bit, like every row that carries one.
    if (cfg.toolchainDiscoveryExplicit)
        argv.emplace_back("--no-toolchain-discovery");

    // Repeatable for the toolchains' reason, and carried for a sharper one: a
    // registration replays its command line forever, so a dropped entry is a worker
    // that comes back refusing an argument the operator installed it to accept -- and
    // that refusal is a SILENT local fallback, the failure #293 exists to remove. The
    // registration would have removed it right back.
    for (auto const& allowed: cfg.extraAllowedArgs)
        argv.push_back(std::format("--allow-compile-arg={}", allowed));

    // Directories root will create for an account that is not root. Without the
    // handover the worker's first write fails with EACCES, which launchd surfaces
    // only as a job that exits over and over -- the same reason the daemon hands
    // over its --storage.
    //
    // Only what the operator actually named, never a parent: `--cache-dir=/var/db/fc`
    // must not reassign /var/db, shared with other system services, to an
    // unprivileged compile account.
    //
    // And whether each keeps what it inherits. The cache holds objects the fleet already
    // shares, so it is Shared. The state directory -- named or defaulted -- is not a row here
    // but the block below, since which directory it is has one derivation the install reads by.
    struct OwnedDirectory
    {
        std::filesystem::path NodeConfig::* member;
        PathPrivacy privacy;
        std::vector<std::filesystem::path> credentialFiles;
    };
    auto OwnedDirectories = std::to_array<OwnedDirectory>({
        OwnedDirectory { .member = &NodeConfig::cacheDir, .privacy = PathPrivacy::Shared, .credentialFiles = {} },
    });
    std::vector<OwnedPath> owned;
    for (auto& row: OwnedDirectories)
        if (!(cfg.*row.member).empty())
            owned.push_back(OwnedPath {
                .path = cfg.*row.member, .privacy = row.privacy, .credentialFiles = std::move(row.credentialFiles) });

    // **A service keeps the MACHINE's identity, in a directory the install OWNS -- on every
    // platform.** A registration naming no `--cluster-dir` owns the machine-wide directory, so the
    // install creates it and secludes it for the service's account: `Private`, exactly as a named
    // `--cluster-dir` is, because it is the same directory holding the same key -- the identity
    // key, named as a credential file whose exposure is a re-mint rather than a reset. That
    // REPLACES `%ProgramData%`'s inherited read for every local user with a protected list of its
    // own (`SecureDirectoryForService`), and a list that does not take refuses the install. The
    // KEY FILE keeps its own guarantee beside it -- created owner-only whatever the directory
    // grants, and never read from a directory other accounts may write in (`ResolveNodeKey`) --
    // and the two are defence in depth rather than alternatives.
    //
    // And on POSIX the account is not privileged, so left to itself it would take the per-user
    // default in its home. There the registration also hands the job the directory the way
    // systemd's `StateDirectory=` hands the packaged unit one (`STATE_DIRECTORY`): an
    // environment variable rather than a baked `--cluster-dir`, because a command-line value
    // outranks the configuration file for the life of the registration, and an operator who
    // later sets `cluster_dir:` in the file would be silently overridden. Windows has no such
    // row: its service token is privileged and takes the machine-wide default by itself.
    //
    // Which directory that is -- the named one, else the machine-wide default -- is
    // `RegisteredStateDirectory`'s answer, the one the install also reads the formation record
    // from, so what is secured and what is read cannot be two derivations.
    std::vector<std::pair<std::string, std::string>> accountEnvironment;
    if (auto const state = RegisteredStateDirectory(cfg, probe); state.has_value())
    {
        owned.push_back(OwnedPath { .path = *state,
                                    .privacy = PathPrivacy::Private,
                                    .credentialFiles = { std::filesystem::path { NodeKeyFileName } } });
        if (cfg.clusterDir.empty() && ServiceManagerHandsOverStateDirectory())
            accountEnvironment.emplace_back(std::string { ServiceStateDirectoryVariable }, state->string());
    }

    return ServiceSpec { .serviceName = cfg.serviceName,
                         .exePath = exePath,
                         .arguments = std::move(argv),
                         .daemonFlag = "--daemon",
                         .displayName = "fastcache-compile-node",
                         .description = "fastcache-compile-node \u2014 a compile worker for fastcached",
                         // Named rather than left empty, and the difference is not
                         // cosmetic: a system-scope launchd job with no UserName runs
                         // as ROOT, and this process compiles input that arrived over
                         // the network. Naming the account the Linux unit already uses
                         // (packaging/linux/fastcache-compile-node.sysusers) puts the
                         // existing "that account does not exist" guard in the way, so
                         // a macOS system-scope install REFUSES until the package
                         // creates it -- rather than silently succeeding as root.
                         // `--service-scope=user` works today and is the per-developer
                         // case anyway: a user agent runs as the invoking account.
                         .serviceAccount = "fastcache-node",
                         .ownedPaths = std::move(owned),
                         .serviceAccountEnvironment = std::move(accountEnvironment),
                         .inlineCredential = cfg.requirePass.empty() ? InlineCredential::Absent : InlineCredential::Present,
                         // What the operator named, so InlineCredentialRejection can
                         // say where the secret belongs instead of merely that it
                         // may not go here. Absolute for the same reason the flag
                         // above is: an install run from a shell resolves a relative
                         // path somewhere the service never will.
                         // `ConfigPath` and NOT `StoragePath`, which is the whole of
                         // #396: this worker reads a configuration file and has no
                         // `--storage` flag at all, so it is the first service for
                         // which "keeps files" and "takes both flags" are different
                         // answers. While `applicationName` decided both, the only
                         // safe thing to say was nothing -- naming an application
                         // baked a `--storage=` into every user-scope registration
                         // and the job answered its own command line with
                         // "unrecognised argument" at every start, reported installed
                         // and dead at every boot.
                         .acceptedScopeDefaults = ScopeDefaults({ ScopeDefault::ConfigPath }),
                         .startMode = cfg.serviceStart,
                         .configPath = absoluteOrAsWritten(cfg.configPath),
                         // Named since #396, and what that buys is the two things the
                         // empty name silently cost: the system-scope `--config=`
                         // default (so the registration STATES which file the service
                         // reads, rather than relying on the worker re-running the
                         // machine-wide lookup and finding the same one), and the
                         // install-time `ServiceAccountReadDenial` on that path --
                         // an install-time error that was demoted to a silent
                         // fall-through to built-in defaults.
                         //
                         // It is also what makes the packaged config findable at all:
                         // `InstallService` looks a spec's machine-wide candidate up
                         // under THIS name, so an empty one looked nothing up.
                         .applicationName = std::string { NodeApplicationName },
                         // The Windows half of the same decision `serviceAccount`
                         // makes for launchd. Told nothing, the SCM logs a service
                         // on as LocalSystem -- the whole machine -- and this one
                         // compiles input that arrived over the network. A virtual
                         // account gives it a per-service SID, no group membership
                         // and no machine credentials on the network, and the SCM
                         // creates it from the service name with no account for the
                         // installer to make and no password to keep.
                         .windowsLogon = WindowsLogonAccount::VirtualAccount };
}

bool RunsWorker(NodeConfig const& cfg) noexcept
{
    return cfg.slots != 0U;
}

bool IsOneShotVerb(NodeConfig const& cfg, OneShotVerb verb) noexcept
{
    switch (verb)
    {
        case OneShotVerb::Cluster:
            return cfg.cluster.action != ClusterAction::None;
        case OneShotVerb::Enroll:
            return cfg.enroll.action != EnrollAction::None;
        case OneShotVerb::Last:
            break;
    }
    return false;
}

bool AimsAtScheduler(NodeConfig const& cfg) noexcept
{
    return std::ranges::any_of(Enumerators<OneShotVerb>(), [&cfg](OneShotVerb verb) { return IsOneShotVerb(cfg, verb); });
}

bool WorkerConsensusClosed(NodeConfig const& cfg) noexcept
{
    if (!RunsWorker(cfg))
        return false;
    // Before a record shaped the configuration -- an install, a parse -- it is judged as the first
    // start it will become, a solitary node, whose port only the flag can close.
    if (!cfg.formation.has_value())
        return cfg.raftListen.empty();
    // Once a record is applied, and asked again after the names resolve: the REAL predicate, never a
    // flag shape. Whatever leaves this node running no consensus -- the flag today, any stand-down a
    // later rule adds -- leaves a worker that holds no roster to check a grant against and has no
    // scheduler of its own to register with, and the reload rule that once guarded such a worker's
    // admission was the only thing between it and an unchecked compile port (I-2).
    return !RunsConsensus(cfg);
}

std::vector<std::string> AdminTargetsOf(NodeConfig const& cfg)
{
    if (!cfg.schedulers.empty())
        return cfg.schedulers;
    return { std::string { DefaultAdminTarget } };
}

std::string UnrunComponentRefusal(OptionSpec<NodeConfig> const& spec)
{
    if (!spec.component->refusal.empty())
        return std::string { spec.component->refusal };
    return std::format("{} configures the {}, and {}: the setting would be accepted and reach nothing. Drop {}, or {}.",
                       spec.primary,
                       spec.component->name,
                       spec.component->absentBecause,
                       spec.primary,
                       spec.component->remedy);
}

bool ConfiguresCacheTier(NodeConfig const& cfg) noexcept
{
    // The same two halves `StartCacheTierOrExplain` declines on, and it asks THIS rather
    // than spelling them, so the table and the tier cannot disagree about whether a tier
    // was asked for.
    return !RowFor(NodeSurface::Node).Resolve(cfg).empty() && (cfg.cacheMemoryBytes != 0 || !cfg.cacheDir.empty());
}

std::filesystem::path SharedCacheStorePath(NodeConfig const& cfg)
{
    return NodeStateDirectory(cfg) / "shared-cache" / DiskStoreFileName;
}

std::uint64_t DefaultSharedCacheDiskBytes() noexcept
{
    // The `: 0` arm is unreachable -- the static_assert beside this function's
    // declaration in NodeConfig.hpp rules the Disk row out ever lacking a default --
    // and is written anyway because that is what keeps this read CHECKED rather than a
    // bare dereference clang-tidy's bugprone-unchecked-optional-access cannot see past.
    auto const& bytes = TraitsFor(StorageTier::Disk).sharedTierDefaultBytes;
    return bytes ? *bytes : 0;
}

std::optional<std::uint32_t> WorkerSlotsOf(NodeConfig const& cfg, Distributed::NodeCapacity const& capacity) noexcept
{
    if (!RunsWorker(cfg))
        return std::nullopt;
    return Distributed::OfferableSlots(capacity, cfg.slots);
}

bool RunsConsensus(NodeConfig const& cfg) noexcept
{
    // It must stay the same expression `StartConsensusOrExplain` uses to decide whether to start
    // a driver at all: the scheduler asks this to know whether a role is COMING, and if the two
    // ever disagreed one of them would be wrong about the other (#613).
    //
    // The MODE first, because the record is what decides (#1022's lesson, one level up): no
    // record, no consensus. A mode that dials in needs no port; one that listens runs consensus
    // when the surface row opens its port, which is where the empty flag and the localhost
    // stand-down close it.
    if (!cfg.formation.has_value())
        return false;
    if (!ModeOpensRaftPort(cfg.formation->mode))
        return true;
    return !RowFor(NodeSurface::Raft).Resolve(cfg).empty();
}

std::string RaftSelfEndpoint(NodeConfig const& cfg)
{
    // The PORT off the surface row, never off `cfg.raftListen`: a bare `--listen-raft`
    // takes the row's own default host and the row is where that is decided.
    auto const bound = RowFor(NodeSurface::Raft).Resolve(cfg);
    if (bound.empty())
        return {};
    auto const& bindHost = bound.front().host;
    auto const port = bound.front().port;

    // The flag's host whenever it was given.
    if (!cfg.raftSelf.empty())
        return FormatHostPort(cfg.raftSelf, port);

    // Otherwise exactly `AdvertisedEndpoint`'s rule, for the same reason: **this machine's name
    // stands in only for a WILDCARD bind**, which is wrong by construction on every other machine
    // and which the name is what it means to a peer. Any other bind is advertised as bound -- a
    // loopback one is a one-machine cluster, an address is what the operator bound -- because the
    // name would resolve to an interface that bind never answers, and every peer's dial would go
    // unanswered with nothing at either end saying why. Before the names are resolved there is no
    // name to give.
    if (!IsWildcardHost(bindHost))
        return FormatHostPort(bindHost, port);
    if (cfg.hostNames.has_value() && !cfg.hostNames->fqdn.empty())
        return FormatHostPort(cfg.hostNames->fqdn, port);
    return {};
}

std::expected<std::string, ConsensusDialGap> ConsensusDialAddressOf(NodeConfig const& cfg)
{
    if (!RunsConsensus(cfg))
        return std::unexpected { ConsensusDialGap::NoConsensus };

    // Asked of the mode's row before any address: a mode that binds no Raft port is dialled by
    // nobody, so an address it named would be one no peer ever uses. `RunsConsensus` held, so
    // the node is formed; the engagement is asked again only where the dereference can see it.
    if (cfg.formation.has_value() && !ModeOpensRaftPort(cfg.formation->mode))
        return std::unexpected { ConsensusDialGap::DialsIn };

    if (auto endpoint = RaftSelfEndpoint(cfg); !endpoint.empty())
        return endpoint;

    // Not stated yet rather than unstated: the host name it falls back to is resolved at
    // startup, and a parse or an install has not got there. Only a RESOLVED empty name is a
    // node that names itself neither way.
    if (cfg.raftSelf.empty() && !cfg.hostNames.has_value())
        return std::unexpected { ConsensusDialGap::AwaitingHostName };
    return std::unexpected { ConsensusDialGap::Unstated };
}

std::string AdvertisedEndpoint(NodeConfig const& cfg)
{
    // The flag wins whenever it was given. Written ONCE -- `main` hands the result to
    // `MakeWorkerLeaseValidator` and to the heartbeat's REGISTER, and the refusals
    // below judge it -- so the three consumers of this endpoint cannot disagree about
    // what it is. This expression once stood character-for-character in `main.cpp` too.
    if (!cfg.advertise.empty())
        return cfg.advertise;

    // The fallback is the `Node` surface, which is where a dispatched compile now
    // ARRIVES. It was `{--bind}:{--port}`, a dedicated compile port that no longer
    // exists -- telling clients to dial one would be silent at both ends, because the
    // registration still succeeds.
    //
    // Resolved through the surface row rather than read off `nodeListen`, because a
    // bare port takes the row's default host and the row is where that is decided.
    // Reading the flag directly would be the second author this function was written to
    // delete.
    auto const node = RowFor(NodeSurface::Node).Resolve(cfg);
    if (node.empty())
        return {};

    // **A wildcard bind is advertised under this machine's name**, because the wildcard is
    // wrong by construction on every other machine -- a client dialling it reaches itself --
    // and the name is what a wildcard bind MEANS to a peer. Only the wildcard: a loopback bind
    // is a single-machine fleet and an address is what the operator bound, and both are
    // advertised as bound, as they always were. Before the names are resolved the wildcard is
    // answered as bound, and `AdvertisedNameAwaited` says that is not the answer yet.
    //
    // A name that reaches only this machine is withheld (`AdvertisedNameWithheld`), and then
    // NOTHING is advertised: the wildcard would send a client to itself, and so would the name.
    if (IsWildcardHost(node.front().host) && cfg.hostNames.has_value() && !cfg.hostNames->fqdn.empty())
        return FormatHostPort(cfg.hostNames->fqdn, node.front().port);

    // A withheld name is offered to no peer. A scheduler on THIS machine reaches the wildcard
    // bind at loopback, so that is what it is told; one elsewhere is told nothing, and the start
    // refuses it by name (`WorkerNameReachesOnlyThisMachineRefusal`).
    if (AdvertisedNameWithheld(cfg))
        return SchedulerIsRemote(cfg) ? std::string {} : FormatHostPort(ThisMachineLoopbackHost, node.front().port);
    return FormatHostPort(node.front().host, node.front().port);
}

bool AdvertisedNameWithheld(NodeConfig const& cfg)
{
    if (!cfg.advertise.empty() || !cfg.hostNames.has_value() || cfg.hostNames->withheld.empty())
        return false;
    auto const node = RowFor(NodeSurface::Node).Resolve(cfg);
    return !node.empty() && IsWildcardHost(node.front().host);
}

std::string DescribeListeningEndpoint(NodeConfig const& cfg, bool socketHandedOver)
{
    if (socketHandedOver)
        return "the socket its supervisor handed it";
    auto described = std::string {};
    for (auto const& endpoint: RowFor(NodeSurface::Node).Resolve(cfg))
        described += std::format("{}{}", described.empty() ? "" : " and ", FormatHostPort(endpoint.host, endpoint.port));
    return described.empty() ? std::string { "nothing" } : described;
}

std::string DescribeAdvertisedEndpoint(NodeConfig const& cfg)
{
    if (auto endpoint = AdvertisedEndpoint(cfg); !endpoint.empty())
        return endpoint;
    if (AdvertisedNameWithheld(cfg) && cfg.hostNames.has_value())
        return std::format("withheld ({})", cfg.hostNames->withheld);
    return "nothing";
}

bool ConsensusNameWithheld(NodeConfig const& cfg)
{
    if (!cfg.raftSelf.empty() || !cfg.hostNames.has_value() || cfg.hostNames->withheld.empty())
        return false;

    // Only a WILDCARD bind would have taken the name (`RaftSelfEndpoint`). Parsed off the row's
    // own spec and default host rather than through its resolver, which asks this predicate.
    auto const& row = RowFor(NodeSurface::Raft);
    auto const bind = ParseEndpoint(cfg.*row.spec, row.defaultHost);
    return bind.has_value() && IsWildcardHost(bind->first);
}

bool ConsensusConfinedToThisMachine(NodeConfig const& cfg)
{
    // Not a learner's: it dials out and needs no name. A typed port is not confined either -- the
    // operator asked for consensus other machines dial, and that is refused by name.
    return cfg.formation.has_value() && ModeOpensRaftPort(cfg.formation->mode) && !cfg.raftListenExplicit
           && ConsensusNameWithheld(cfg);
}

std::span<ConsensusPeerAsk const> ConsensusPeerAsks() noexcept
{
    // `--discovery` when TYPED: the default is on, and stands down with consensus.
    static constexpr auto rows = std::to_array<ConsensusPeerAsk>({
        { .setting = "--listen-raft", .asks = [](NodeConfig const& c) { return c.raftListenExplicit; } },
        { .setting = "--discovery",
          .asks = [](NodeConfig const& c) { return c.discoveryAddressExplicit && !c.discoveryAddress.empty(); } },
        { .setting = "recorded mode",
          .asks =
              [](NodeConfig const& c) { return c.formation.has_value() && ModeServesConsensusToPeers(c.formation->mode); } },
    });
    return rows;
}

bool AsksPeersToReachConsensus(NodeConfig const& cfg)
{
    return std::ranges::any_of(ConsensusPeerAsks(), [&cfg](ConsensusPeerAsk const& row) { return row.asks(cfg); });
}

bool ConsensusPastALoopbackBind(NodeConfig const& cfg)
{
    auto const bound = RowFor(NodeSurface::Raft).Resolve(cfg);
    if (bound.empty() || !IsLoopbackHost(bound.front().host))
        return false;
    auto const dial = ConsensusDialAddressOf(cfg);
    return dial.has_value() && !NamesOnlyThisMachine(HostOfEndpoint(*dial));
}

bool ConsensusAddressReachesOnlyThisMachine(NodeConfig const& cfg)
{
    auto const dial = ConsensusDialAddressOf(cfg);
    // Asked through the one predicate the discovery service holds at its door, over the summary
    // field it would carry, so the refusal and the guarantee cannot disagree about what "reaches
    // only this machine" means.
    return dial.has_value() && Cluster::AnnouncesOnlyThisMachine(CompileCacheWire::FleetSummary { .raftEndpoint = *dial });
}

bool DiscoveryAnnouncesOnlyThisMachine(NodeConfig const& cfg)
{
    return cfg.discoveryAddressExplicit && !cfg.discoveryAddress.empty() && ConsensusAddressReachesOnlyThisMachine(cfg);
}

bool AdvertisedNameAwaited(NodeConfig const& cfg)
{
    if (!cfg.advertise.empty() || cfg.hostNames.has_value())
        return false;
    auto const node = RowFor(NodeSurface::Node).Resolve(cfg);
    return !node.empty() && IsWildcardHost(node.front().host);
}

/// Whether the endpoint this node would advertise names no host a client can dial.
///
/// A worker that names the wildcard registers `0.0.0.0:<port>` -- which the
/// scheduler hands to clients verbatim, and a client on another machine dialling the
/// wildcard reaches **itself**. `NodeServiceRejection` has always said so for an
/// install; this is the same fact for a hand-started worker.
///
/// Judged on the endpoint the node would actually advertise rather than on whether
/// the flag was typed, so an operator who spells the default out is answered too.
/// Only a wildcard is refused, never an address that merely might not resolve: a
/// host that is down today can be the right one at the next boot (#208), while the
/// wildcard is wrong by construction on every machine and forever.
/// @param cfg The parsed configuration.
/// @return Whether remote clients would be told to dial a wildcard.
[[nodiscard]] bool AdvertisesWildcard(NodeConfig const& cfg)
{
    // Through the one derivation, never spelled again here. This function is a startup
    // REFUSAL, so a copy of the rule would let it pass a configuration whose advertised
    // endpoint differs from the one it judged -- and that endpoint is what every lease's
    // MAC is taken over. See `AdvertisedEndpoint`.
    //
    // A wildcard the start will replace with this machine's name is not one: before the names
    // are resolved -- a parse, an install -- it stands in the endpoint only because nothing
    // else can yet, and refusing it would refuse every node that names no `--advertise`.
    if (AdvertisedNameAwaited(cfg))
        return false;
    auto const advertised = AdvertisedEndpoint(cfg);

    // `HostOfEndpoint` rather than `SplitHostPort` plus a fallback: an endpoint that
    // will not split is a bare host rather than a parse failure, and that rule is
    // spelled once in `Core/HostPort` for every caller that needs it.
    return IsWildcardHost(HostOfEndpoint(advertised));
}

/// Whether the endpoint this node would advertise names only this machine.
///
/// The third author of `IsLoopbackHost(HostOfEndpoint(AdvertisedEndpoint(cfg)))`
/// deleted. Three of the four reachability rows ask it -- two positively, one
/// negated -- and it is the composition rather than the primitives that was copied,
/// which is the defect `AdvertisedEndpoint` itself was extracted for: its own comment
/// records that the expression once stood character-for-character in `main.cpp`.
///
/// Returns a `bool` and never the host: `AdvertisedEndpoint` hands back a value, so a
/// helper yielding the `string_view` `HostOfEndpoint` carves out of it would dangle
/// into a destroyed temporary at every call site.
/// @param cfg The parsed configuration.
/// @return Whether peers would be told to dial a loopback address.
[[nodiscard]] bool AdvertisedHostIsLoopback(NodeConfig const& cfg)
{
    // `NamesOnlyThisMachine`, not `IsLoopbackHost`: this is what peers are TOLD, and `localhost`
    // sends every one of them to itself as surely as `127.0.0.1` does.
    return NamesOnlyThisMachine(HostOfEndpoint(AdvertisedEndpoint(cfg)));
}

/// Why this node must not advertise the PORT it would advertise, or nullopt.
///
/// **The startup reachability rules judged the advertised HOST and never its port**, so
/// a node could tell every client to dial a port nothing on it listens on and start
/// cleanly ([#594](https://github.com/LASTRADA-Software/fastcached/issues/594)). In the
/// documented layout that is worse than a wrong number: `--advertise=host:6674` beside
/// `--listen-node=6675` sends every dispatched compile to 6674, which is the shared
/// `fastcached` cache daemon. The registration succeeds, the scheduler hands the
/// endpoint out verbatim, and every counter reads normally.
///
/// `AdvertisedEndpoint`'s own comment already described this failure, about the flag
/// pair it replaced -- *"telling clients to dial one would be silent at both ends,
/// because the registration still succeeds"* -- and the guard was never extended to the
/// flag that inherited it.
///
/// **NOT "the advertised port must equal `--listen-node`".** That would refuse
/// legitimate deployments -- NAT, port forwarding, a proxy, a container publishing a
/// different external port -- and `--advertise` exists precisely so a node can name an
/// address only clients can reach. `AdvertisesPastALoopbackBind`'s earlier draft refused
/// a loopback advertise outright and was wrong for exactly this reason.
///
/// **The provable case is an advertised host that names THIS machine.** Then the port is
/// this machine's port, and it can be compared: it must be the one the `0xFC` surface
/// serves. A mismatch there is wrong by construction, on every machine and forever --
/// the same standard `AdvertisesWildcard` uses to justify refusing only the wildcard.
///
/// **Scoped to LOOPBACK, deliberately, and narrower than the ticket's wording.** It also
/// offers "or one of this machine's own addresses", which cannot be asked here: this
/// table is a pure function of the parsed configuration, and enumerating the host's
/// addresses is I/O behind `Platform/ILocalityOracle`. A rule that cannot state its
/// premise without reaching for the machine has no business in this table -- the same
/// argument a retired `--cluster-key-file` rule once recorded, whose premise moved every time a new
/// reader of the file arrived. Loopback is
/// decidable from the text, so that is what is judged.
///
/// The wider form the ticket calls unambiguous -- refuse whenever the advertised port
/// matches a DIFFERENT surface this node serves, whatever the host -- is deliberately
/// not taken. A NAT external port may legitimately coincide with this node's admin port,
/// and a wrong refusal of a working deployment is worse than the silent failure it
/// replaces. What that observation is good for is the MESSAGE, which names the surface
/// when it recognises the port.
///
/// Judged through `AdvertisedEndpoint`, never by re-reading `cfg.advertise`: a copy of
/// the derivation would judge a different endpoint from the one the lease MAC is taken
/// over, which is the defect that function was extracted to delete.
/// @param cfg The parsed configuration.
/// @return The refusal, naming the port this node actually serves.
[[nodiscard]] std::optional<std::string> AdvertisedPortRejection(NodeConfig const& cfg)
{
    auto const advertised = AdvertisedEndpoint(cfg);
    if (advertised.empty())
        return std::nullopt; // Nothing is advertised, so there is nothing to be wrong.

    // **Only when the operator NAMED the port this is compared against**
    // ([#770](https://github.com/LASTRADA-Software/fastcached/issues/770)). This rule
    // says "you advertise a port on this machine that is not the one this node serves",
    // and with `--listen-node` unset the second half is a DEFAULT nobody typed --
    // so refusing on it is refusing on an assumption.
    //
    // Under socket activation that assumption is not merely unstated, it is FALSE: the
    // unit owns the address, so `--listen-node` "still holds a value that describes
    // nothing" -- the rulebook's own words, recorded against `--bind` and inherited by
    // this flag. The packaged worker is configured exactly that way, with an
    // `advertise:` naming the socket unit's port and no `listen_node:` at all, and this
    // rule refused it at startup: `fastcache-compile-node.socket` accepted the
    // activating connection, the service exited 2, and the job failed on "a connection
    // did not bring the worker up". It took the packaging jobs red on master, and it
    // was invisible to every local suite because none of them is socket-activated.
    //
    // **`StartupPolicyRejection` cannot ask whether this node is activated**, which is
    // why the fix is here rather than a condition on the environment: this table is a
    // pure function of the parsed configuration and runs in `main()`, while activation
    // is discovered inside `WorkerBody` long afterwards -- and at INSTALL time nobody
    // knows either, since whether the unit is socket-activated is the unit's business.
    // So the rule states its own premise instead: it judges a port an operator wrote
    // against a port an operator wrote.
    //
    // Asked of `nodeListenExplicit` and NOT of the value, because `nodeListen` carries
    // a non-empty default (`DefaultNodeListen`, `0.0.0.0:6674`) and
    // `--listen-node=0.0.0.0:6674` is a promise whose value equals it. Provenance is recorded by the parse and cannot be
    // recovered by comparing against the default -- that bit exists for exactly this
    // question (#286), and reaching for `.empty()` here would have been the same defect
    // one flag along. Measured: `.empty()` left the packaged worker still refused,
    // because the default it was testing is never empty.
    //
    // What that gives up is stated rather than hidden: a node that names only
    // `--advertise`, is not activated, and serves the default port is no longer
    // refused for advertising some other port. That case is now unreachable from the
    // configuration alone, and it is the narrower miss -- the alternative refuses the
    // packaged worker, which is a deployment that works.
    if (!cfg.nodeListenExplicit)
        return std::nullopt;

    // Only an endpoint naming this machine is judged; see the note above.
    auto const host = HostOfEndpoint(advertised);
    if (!IsLoopbackHost(host))
        return std::nullopt;

    // **ONE parser answers both halves, because two disagree about a bare IPv6
    // literal** ([#822](https://github.com/LASTRADA-Software/fastcached/issues/822)).
    //
    // The gate above asks `HostOfEndpoint`, which deliberately declines to split an
    // unbracketed IPv6 literal -- its own comment says why: `::1` is a host, and
    // splitting at the last colon yields "a plausible-looking wrong answer rather than
    // a failure". `SplitHostPort` has no such guard and splits on the LAST colon
    // regardless, so `--advertise=::1` reached it as host `:` and port `1`, and this
    // rule refused a correct configuration while **naming a port nobody typed**.
    //
    // `HostOfEndpoint` returns the WHOLE endpoint when there is no port to split off,
    // so "is the host the entire string" is exactly "did the operator name a port" --
    // and it is the same answer the gate was already given. A bare host advertises no
    // port, so there is nothing for this rule to disagree with. `[::1]` with no port
    // does not reach here as a bare host (the brackets are stripped, so the sizes
    // differ), and is refused a line below by `SplitHostPort` returning nothing.
    if (host.size() == advertised.size())
        return std::nullopt;

    auto const split = SplitHostPort(advertised);
    if (!split.has_value())
        return std::nullopt;
    auto const advertisedPort = ParseTcpPort(split->second);
    if (!advertisedPort.has_value())
        // Not an endpoint at all. `--advertise` carries no grammar row, so this is a
        // different defect from this one and is left to whoever gives it one -- saying
        // "wrong port" about text that names no port would describe the wrong problem.
        return std::nullopt;

    auto const node = RowFor(NodeSurface::Node).Resolve(cfg);
    if (node.empty())
        return std::nullopt; // No compile surface, so no port to disagree with.
    if (node.front().port == *advertisedPort)
        return std::nullopt;

    // Which of this node's OWN surfaces serves the port the operator typed, when one
    // does. Walked off `NodeSurfaceTable()` rather than listed here, so a surface added
    // later is recognised without a second author -- and the `Node` row is skipped
    // because reaching this line means it is not the match.
    for (auto const& surface: NodeSurfaceTable())
    {
        if (surface.surface == NodeSurface::Node)
            continue;
        auto const endpoints = surface.Resolve(cfg);
        if (std::ranges::none_of(endpoints, [&](auto const& e) { return e.port == *advertisedPort; }))
            continue;

        return std::format("--advertise={} names this machine at port {}, which is this node's own {} surface "
                           "({}) and not the port it serves compiles on. A client told to dial it reaches the "
                           "wrong service on the right host, the registration succeeds, and every counter reads "
                           "normally. This node serves compiles on {}.",
                           advertised,
                           *advertisedPort,
                           surface.name,
                           PrimaryFlag(surface),
                           FormatHostPort(node.front().host, node.front().port));
    }

    return std::format("--advertise={} names this machine at port {}, and this node serves compiles on {}. A client "
                       "told to dial a port nothing here listens on gets no compile, while the registration "
                       "succeeds and every counter reads normally -- so nothing at either end reports a fault. "
                       "Name the port {} serves, or an address only clients can reach if this is a forwarded port.",
                       advertised,
                       *advertisedPort,
                       FormatHostPort(node.front().host, node.front().port),
                       PrimaryFlag(RowFor(NodeSurface::Node)));
}

/// Whether a machine that is not this one could reach this node's COMPILE verbs.
///
/// **ONE port answers them now**, and this function is what is left of a rule that
/// once had to ask two. `--bind` was the whole answer until the compile verbs gained a
/// second door on the merged `0xFC` listener, and for one release the question was the
/// disjunction: asking either half alone let an open surface pass this table --
/// a scheduler with `--bind 127.0.0.1 --fleet-open` and nothing to check a grant against
/// looked local, passed, and served unauthenticated compiles on a wildcard-bound port
/// with every `worker_jobs_refused_lease_*` counter reading zero. #290 stage 3 retires
/// the dedicated port, so the surface row is the whole answer again -- and it is the
/// row rather than `--listen-node`, because a bare port's host is the row's default
/// (`NodeSurfaceDefaultHost`).
///
/// The node surface is asked of its own ROW rather than re-deriving the host here,
/// which is the rule the dashboard credential rule below already follows: that row's
/// default host depends on the configuration, it may resolve to nothing at all, and a
/// second author of the resolution is one that judges an address the surface no longer
/// binds. The row also answers "not served" for a node with neither a cache tier nor a
/// scheduler -- correctly, because such a node opens no `0xFC` port and its compiles
/// arrive only on `--bind`.
///
/// Whether this node accepts from the network and then tells peers to dial loopback.
///
/// The wildcard's sibling, and the shape #290 stage 3 newly made the default. The
/// advertised endpoint used to fall back to `{--bind}:{--port}`, whose bind defaults to
/// the wildcard; it now falls back to the `Node` surface, which a node binds to LOOPBACK
/// only when told to (the default is the wildcard, advertised under this machine's name).
/// A loopback advertise is unreachable from another machine as a wildcard one is, and
/// both fail the same silent way -- the worker registers, heartbeats, is leased out,
/// and every remote client dials something that is not this worker.
///
/// **The bind must DISAGREE with the advertise, and that clause is the whole rule.**
/// An earlier draft refused a loopback advertise outright, on the reasoning that
/// loopback is unreachable from another machine. True, and not a defect when the
/// SURFACE is on loopback too: nobody else is meant to reach that node, which is the
/// single-machine fleet this project supports and tests. The three reachability rows
/// are one idea -- refuse when what a peer is TOLD and what this node ACCEPTS ON do
/// not agree -- and this one is its second cell, `AdvertisesPastALoopbackBind` the
/// third.
///
/// Kept a separate predicate behind a separate row rather than folded into
/// `AdvertisesWildcard`, because the two are different operator mistakes with
/// different remedies and one message could only serve them by describing neither. A
/// row is the refusal here, not the predicate.
///
/// **A DEFAULTED bind disagrees only on a node that admits other machines NOW.** The zero-config
/// defaults put every node's surface on the wildcard, so a bind nobody typed states nothing about
/// where the operator meant this node to be reached -- and the packaged socket-activated worker
/// (#770), whose unit owns the address and which forms a solitary cluster, would otherwise be
/// refused for a bind it never typed. A node that already admits other machines -- `--fleet-open`,
/// or a formation record whose mode reaches beyond this machine -- is judged whatever the bind's
/// provenance: its peers are real, and each would be leased `127.0.0.1`. A TYPED reachable bind is
/// judged always, the provenance the port row asks too.
///
/// **Residual, stated rather than hidden:** a solitary node that FORMS a fleet later -- by
/// admitting a joiner at runtime -- was not judged here at its start, because at its start it
/// admitted nobody; startup cannot see a membership that has not happened. Its remedy is the one
/// this row names, at the next start.
/// @param cfg The parsed configuration.
/// @return Whether remote clients would be told to dial their own machine.
[[nodiscard]] bool AdvertisesLoopbackFromAReachableBind(NodeConfig const& cfg)
{
    if (!AdvertisedHostIsLoopback(cfg))
        return false;
    if (!cfg.nodeListenExplicit
        && !AdmitsRemotePeers(cfg, RunsConsensus(cfg) ? RosterPresence::Formed : RosterPresence::Absent))
        return false;

    // **And the bind has to disagree with it.** A node whose surface is ALSO on
    // loopback is a coherent single-machine fleet, not a mistake: no remote client is
    // supposed to reach it, so "no remote client can reach the advertised endpoint" is
    // the configuration working rather than failing. This repository runs whole fleets
    // that way -- `dist-compile-e2e.sh` starts a `--fleet-open` scheduler on
    // `127.0.0.1` and advertises `127.0.0.1` -- and an earlier draft of this rule
    // refused it, which would have broken the fixture that exercises the fleet.
    auto const bound = RowFor(NodeSurface::Node).Resolve(cfg);
    return std::ranges::any_of(bound, [](SurfaceEndpoint const& endpoint) { return !IsLoopbackHost(endpoint.host); });
}

/// Whether this node advertises an address peers can dial and then binds where it
/// cannot accept them.
///
/// The rule's THIRD spelling, and the one reached by obeying the second's advice. A
/// worker told to "name --advertise with an address peers can dial" sets
/// `worker-01.internal:6674` and stops -- and `--listen-node` still defaults to
/// loopback, so the advertised host is neither loopback nor the wildcard, both rows
/// above pass, and the surface is bound to `127.0.0.1`. The worker registers, is
/// leased out, and every dispatch fails to connect. Same silent shape, same
/// consequence, arrived at from the other side.
///
/// **Routable is decided SYNTACTICALLY and the host is never resolved.**
/// `StartupPolicyRejection` is a table of pure functions of the parsed configuration,
/// and a lookup here would make this daemon's ability to start depend on the network:
/// a resolver outage would become a refusal to boot, and an unbounded call would sit
/// in a table whose whole contract is that it needs nothing but argv. "Neither
/// loopback nor a wildcard" is enough -- a host that does not resolve today can be the
/// right one at the next boot (#208), exactly as `AdvertisesWildcard` documents.
///
/// **Its own conditions, never "not the other two".** An earlier draft defined this as
/// the negation of its siblings, on the reasoning that it made the three cases
/// mutually exclusive by construction. It did -- and then row 2 was narrowed, the
/// loopback/loopback cell stopped matching it, and fell straight through into THIS
/// row: a coherent single-machine fleet refused with a message about an address peers
/// cannot dial. A predicate defined by what its neighbours are not changes meaning
/// silently whenever a neighbour does, which is the same defect as two authors of one
/// fact, wearing the costume of a tidy invariant. Each row states what it is about.
/// @param cfg The parsed configuration.
/// @return Whether peers are told an address this node will not accept on.
[[nodiscard]] bool AdvertisesPastALoopbackBind(NodeConfig const& cfg)
{
    // A ROUTABLE advertise, decided syntactically: neither the wildcard nor loopback.
    // Both of those are other rows' business, and a loopback advertise over a loopback
    // bind is not any row's business at all -- it is the single-machine fleet.
    if (AdvertisesWildcard(cfg))
        return false;
    if (AdvertisedHostIsLoopback(cfg))
        return false;

    // The row is asked rather than `nodeListen` read, for the reason
    // `AdvertisedEndpoint` gives: a bare port's host is the row's default, and the row is
    // where that lives.
    auto const bound = RowFor(NodeSurface::Node).Resolve(cfg);
    return std::ranges::any_of(bound, [](SurfaceEndpoint const& endpoint) { return IsLoopbackHost(endpoint.host); });
}

/// Whether this node admits other machines by any route: `--fleet-open`, a key route, or the fleet
/// its formation record puts it in.
///
/// The gate all four reachability rows share. Admitting another machine is only ever so
/// that it can dial this worker, so a row that tells it to dial somewhere it cannot is
/// worth firing exactly when some route admits it. A key route counts: a roster -- the state a
/// consensus node applies -- admits ticket holders and proven keys whatever address they dial
/// from, so a worker that holds one tells them where to dial as surely as an open one does.
///
/// Positive and self-contained, never "what the other rows are not": the hazard the
/// sibling predicates document is a row defined by NEGATING its neighbours, which
/// changes meaning silently when a neighbour moves. A row that later needs a different
/// gate writes a different expression, exactly as the fourth already does -- it drops
/// the `--scheduler` half, because `SchedulerIsRemote` asks something stronger.
///
/// It is `AdmitsRemotePeers` asked with `RosterPresence::Unknown`, and deliberately that
/// reading rather than a spelling of its own: these rows are asked of a configuration
/// before any roster is read, so a key route counts whenever this node runs consensus, and a
/// node its formation record puts in a fleet, or asking into one, counts too. Every node that runs consensus answers yes,
/// which is the point: it admits ticket holders and proven keys, and each of them is told where to dial.
/// @param cfg The parsed configuration.
/// @return Whether `--fleet-open` was given, a key could admit another machine, or the
///         formation record puts this node in a fleet.
[[nodiscard]] bool NamesAnAdmissionRoute(NodeConfig const& cfg)
{
    return AdmitsRemotePeers(cfg, RosterPresence::Unknown);
}

/// Whether a compression setting was NAMED for a tier half this node will not build.
///
/// Two of them, one per half, because the remedies differ: a memory codec is turned
/// back on with `--cache-memory` and a disk one with `--cache-dir`, so a single
/// predicate would give half its readers the wrong flag.
///
/// **Asked of the EXPLICIT bit, never of the value.** All six settings carry a
/// default and two of them default to something non-zero (`3`, `4096`), so "differs
/// from the default" cannot be the test and "is not `none`" would not see the level
/// flag at all -- which is `platform-service-and-config.md`'s provenance rule: only
/// the parse knows whether an operator typed a setting, and a default nobody typed
/// has to go on starting a node that asked for nothing.
///
/// **Named functions rather than expressions in the rows**, and that is measured
/// rather than stylistic: spelled inline, the two `||` chains took
/// `StartupPolicyRejection` to a cognitive complexity of 62 against clang-tidy's
/// threshold of 60, which with `WarningsAsErrors` is a failed build rather than a
/// review note.
/// @param cfg The parsed configuration.
/// @return Whether the in-memory trio was named with no in-memory tier to configure.
[[nodiscard]] bool NamesMemoryCompressionWithoutATier(NodeConfig const& cfg)
{
    auto const named =
        cfg.memoryCompressionExplicit || cfg.memoryCompressionLevelExplicit || cfg.memoryCompressionMinBytesExplicit;
    return named && cfg.cacheMemoryBytes == 0;
}

/// Whether the on-disk trio was named with no on-disk tier to configure.
///
/// The disk half's mirror; see `NamesMemoryCompressionWithoutATier` for why the bit
/// rather than the value, and why these are two predicates.
/// @param cfg The parsed configuration.
/// @return Whether the on-disk trio was named with no `--cache-dir`.
[[nodiscard]] bool NamesDiskCompressionWithoutATier(NodeConfig const& cfg)
{
    auto const named = cfg.compressionExplicit || cfg.compressionLevelExplicit || cfg.compressionMinBytesExplicit;
    return named && cfg.cacheDir.empty();
}

/// Whether this node registers with a scheduler that cannot be on this machine.
///
/// Asked of where the worker REGISTERS -- its formation record's answer, `SchedulersOf` --
/// never of `--scheduler`, which aims one-shot verbs and is refused on a node that serves.
///
/// Syntactic, never resolved, for the reason `AdvertisesPastALoopbackBind` gives: this
/// table is a set of pure functions of argv, and a lookup here would make the node's
/// ability to start depend on a resolver being up.
///
/// **`localhost` counts as this machine, and `IsLoopbackHost` deliberately does not
/// say so.** That helper is asked security questions -- who may read this node's cache
/// tier -- where a name a resolver decides must not be trusted. This is a USABILITY
/// refusal about a name the operator typed, and RFC 6761 reserves `localhost` to
/// resolve to loopback on every host there is, so nothing has to be looked up to know
/// it. Left out, a fleet remembered as `localhost:6674` -- a working one-machine
/// install -- would be refused and told to bind the wildcard.
///
/// **A value that is not `host:port` is nobody's business here.** A bare port arrives
/// at `HostOfEndpoint` as a bare HOST, so it would read as "not loopback" and be
/// refused with a message about where the scheduler is when the fault is the value's
/// shape; this row stays quiet for it.
///
/// **ANY value, never the first** (#1310). The heartbeat falls back through every one of
/// them, so a loopback first entry says nothing about where this node registers the day
/// that entry stops answering -- and the address it registered is then the one only this
/// machine can reach.
/// @param cfg The parsed configuration.
/// @return Whether some endpoint this worker registers with names a host that is not this machine.
[[nodiscard]] bool SchedulerIsRemote(NodeConfig const& cfg)
{
    // This machine's own names for itself, as this node knows them without asking the network: the
    // address its node surface binds when it binds ONE (`SchedulersOf` dials its own scheduler there),
    // the host a typed `--advertise` names, and the name a wildcard bind is advertised under. A
    // scheduler at any of them is this process's own. NOT `AdvertisedEndpoint`, which asks this.
    auto own = std::vector<std::string> {};
    for (auto const& endpoint: RowFor(NodeSurface::Node).Resolve(cfg))
        if (!IsWildcardHost(endpoint.host))
            own.push_back(endpoint.host);
    if (!cfg.advertise.empty())
        own.emplace_back(HostOfEndpoint(cfg.advertise));
    if (cfg.hostNames.has_value() && !cfg.hostNames->fqdn.empty())
        own.push_back(cfg.hostNames->fqdn);

    return std::ranges::any_of(SchedulersOf(cfg, AsConfigured), [&own](std::string const& scheduler) {
        auto const endpoint = SplitHostPort(scheduler);
        if (!endpoint.has_value() || NamesOnlyThisMachine(endpoint->first))
            return false;
        // Through `SameHost`, the one comparison admission makes, so a LAN-bound node's own
        // scheduler at its own address is this machine here exactly as it is at the door.
        return std::ranges::none_of(own, [&endpoint](std::string const& mine) { return SameHost(endpoint->first, mine); });
    });
}

[[nodiscard]] bool AdvertisesLoopbackToARemoteScheduler(NodeConfig const& cfg)
{
    if (!SchedulerIsRemote(cfg))
        return false;
    return AdvertisedHostIsLoopback(cfg);
}

/// An EMPTY bind address is the wildcard rather than a missing answer: it reaches
/// `getaddrinfo` as nullptr under AI_PASSIVE, the same third case `AdvertisesWildcard`
/// exists for. So it is reachable, and `IsLoopbackHost("")` answering false is the
/// behaviour this relies on rather than an accident. Why it is asked at all is on the
/// rule that asks it.
/// @param cfg The parsed configuration.
/// @return Whether either door onto the compile verbs answers anywhere but this
///         machine.
[[nodiscard]] bool CompilePortFacesTheNetwork(NodeConfig const& cfg)
{
    auto const nodePort = RowFor(NodeSurface::Node).Resolve(cfg);
    return std::ranges::any_of(nodePort, [](SurfaceEndpoint const& endpoint) { return !IsLoopbackHost(endpoint.host); });
}

bool CompileVerbsReachOtherMachines(NodeConfig const& cfg, RosterPresence roster)
{
    return CompilePortFacesTheNetwork(cfg) && AdmitsRemotePeers(cfg, roster);
}

std::string WorkerReadinessPhrase(NodeConfig const& cfg, std::optional<std::uint32_t> workerSlots, std::size_t toolchains)
{
    if (!workerSlots.has_value())
        return "running no worker";
    return std::format("{} slot(s) as a {} node, identifying {} toolchain(s)",
                       *workerSlots,
                       Distributed::TraitsFor(cfg.nodeClass).name,
                       toolchains);
}

std::string AdmissionSummary(NodeConfig const& cfg)
{
    // One sentence with a conditional tail rather than two whole ones: written twice
    // they drift, and a phrase an operator reads is exactly the thing nobody notices
    // has drifted.
    return std::format("admits this machine, and machines the roster admits by key or ticket{}",
                       cfg.fleetOpen ? ", and every other caller (--fleet-open)" : "");
}

std::optional<std::string> NodeServiceRejection(NodeConfig const& cfg)
{
    // A table, so a new rule is a new row rather than another `if` in main().
    constexpr auto Rules = std::to_array<ConfigRule>({
        // The worker rows ask of a WORKER (#206): a node running none surveys nothing and
        // advertises no compile port, and the startup table says what is wrong with naming a
        // worker's settings on it.
        // Conditional, where it used to be absolute. Registering a service before
        // anybody knows what the machine holds is the entire point of #139 -- the
        // node answers that at boot. What still cannot work is discovery turned OFF
        // with nothing named, and that is refused here, where an operator is
        // watching, rather than at every boot where nobody is.
        { .scope = &WorkerComponent,
          .refuses = [](NodeConfig const& c) { return c.toolchains.empty() && !c.toolchainDiscovery; },
          .message = "--toolchain is required alongside --no-toolchain-discovery: with both, a worker has nothing to "
                     "serve, so it would register and then refuse every job the scheduler sends it. Drop "
                     "--no-toolchain-discovery to let the machine answer at boot instead." },
        // Two rows went with the zero-config defaults. `--advertise` is no longer required: a
        // worker that names none advertises this machine's fully qualified name, resolved by
        // the service at every start rather than baked in here. And `--listen-raft` no longer
        // needs `--cluster-dir`: the directory that rule feared -- relative to a service's
        // working directory -- is gone, and the default is the platform's machine-wide one,
        // resolved by the service as the process it runs as (`DefaultNodeClusterDirectory`).
    });

    return FirstRefusal(Rules, cfg);
}

std::span<NodeSecretFile const> NodeSecretFileTable() noexcept
{
    static constexpr auto table = std::to_array<NodeSecretFile>({
        // The admin credential. The dashboard's own rules are REFUSALS about a
        // MISSING one; this is a warning about an EXPOSED one, and the two
        // deliberately differ in kind about one file for #384's stated reason:
        // refusing a missing credential fails closed and breaks nothing that worked,
        // while refusing an exposed one breaks a deployment that is running today.
        { .flag = "--dashboard-token-file",
          .path = [](NodeConfig const& cfg) { return cfg.dashboardTokenFile; },
          .hint = &SecretExposureHint },
        // A TLS private key, which #752's own list does not name. As severe as the identity
        // key below: anything holding it can terminate this node's admin surface.
        { .flag = "--tls-key", .path = [](NodeConfig const& cfg) { return cfg.tlsKeyFile; }, .hint = &SecretExposureHint },
        // The node's IDENTITY key (#178), which the flag does not name: it names the
        // state directory, and the key is the file inside it this node minted. A row of
        // this table rather than a public one, because the directory stopped being "Raft
        // state, no credential" the moment it held the key a machine proves itself with --
        // and a row whose path is DERIVED, because the file is found where the start put
        // it (`NodeKeyPath`), never re-spelled here. Empty on a node that holds no key,
        // which the loop below skips like any unnamed file. Told the OWNER-ONLY remedy: the
        // node minted it and nothing else reads it, and the services remedy grants no owner --
        // a node run by a user that followed it could no longer read its own key.
        { .flag = "--cluster-dir", .path = &NodeKeyPath, .hint = &OwnerOnlySecretExposureHint },
    });
    return table;
}

std::span<NodePublicPathFlag const> NodePublicPathFlags() noexcept
{
    static constexpr auto table = std::to_array<NodePublicPathFlag>({
        { .flag = "--config",
          .why = "the file itself holds no secret by construction; whether the secret IN it is exposed is the "
                 "provenance-gated question NodeSecretFiles asks separately" },
        { .flag = "--cache-dir", .why = "compiled objects, which the fleet already shares" },
        { .flag = "--pidfile", .why = "a process id, which every process list already publishes" },
        // The shipped TEMPLATE, which is payload: it is the annotated reference every
        // package installs, identical on every machine, and it holds no value at all --
        // every setting in it is a comment. What is written FROM it can hold a secret,
        // and that file is `--config`, one row above, where the question is asked.
        { .flag = "--seed-config", .why = "a shipped reference template in which every setting is a comment" },
        // Explicitly classified rather than left off, because it is the one an
        // author would reach for by symmetry with `--tls-key`. A certificate is
        // handed to every client during the handshake, so it is public BY
        // CONSTRUCTION -- and warning about the mode of a file that is meant to be
        // readable is the alarm that teaches operators to ignore the other four.
        { .flag = "--tls-cert", .why = "a certificate is presented to every client during the handshake" },
    });
    return table;
}

std::vector<std::filesystem::path> NodeSecretFiles(NodeConfig const& cfg,
                                                   std::filesystem::path const& configFile,
                                                   bool secretNamedOnCommandLine)
{
    std::vector<std::filesystem::path> files;
    for (auto& subject: NodeSecretFileSubjects(cfg, configFile, secretNamedOnCommandLine))
        files.push_back(std::move(subject.path));
    return files;
}

std::vector<SecretFileSubject> NodeSecretFileSubjects(NodeConfig const& cfg,
                                                      std::filesystem::path const& configFile,
                                                      bool secretNamedOnCommandLine)
{
    std::vector<SecretFileSubject> files;

    // First, because it is the one an operator most often has open. Gated on the
    // shared provenance rule rather than a second copy of it: `--requirepass` typed
    // in argv is a `ps` exposure, which is a different problem with a different owner.
    if (SecretCameFromConfigFile(SecretProvenanceFacts {
            .secretInForce = !cfg.requirePass.empty(),
            .namedOnCommandLine = secretNamedOnCommandLine,
            .fileWasRead = !configFile.empty(),
        }))
        files.push_back(SecretFileSubject { .path = configFile, .hint = &SecretExposureHint });

    for (auto const& row: NodeSecretFileTable())
        if (auto path = row.path(cfg); !path.empty())
            files.push_back(SecretFileSubject { .path = std::move(path), .hint = row.hint });

    return files;
}

namespace
{
    /// The value of a scalar dialled-address flag when it is not an address to dial.
    ///
    /// **One helper rather than three lambdas differing only in which member they
    /// read.** `--upstream`, a retired joiner's seed flag and `--scheduler`, before it
    /// became the list `ElementNotAnAddressToDial` reads, all asked the identical
    /// question, and they asked it in three copy-pasted bodies inside
    /// `StartupPolicyRejection` -- branches diverging by a name, which this codebase
    /// treats as a defect on its own. It also cost that function real
    /// cognitive-complexity budget: three lambdas, each with a branch and a
    /// short-circuit, counted against a threshold the enrollment window's rows pushed
    /// it over.
    /// @tparam Field The `NodeConfig` member holding the typed value.
    /// @param cfg The parsed configuration.
    /// @return The offending value, or nothing when it is empty or well formed.
    template <auto Field>
    [[nodiscard]] std::optional<std::string> NotAnAddressToDial(NodeConfig const& cfg)
    {
        auto const& value = cfg.*Field;
        if (value.empty() || ParseDialEndpoint(value).has_value())
            return std::nullopt;
        return value;
    }

    /// The first element of a repeatable dialled-address flag that is not an address
    /// to dial.
    ///
    /// Not `NotAnAddressToDial` over each element, because the two disagree about
    /// EMPTY: for a scalar it means the flag was never given, while an empty element is
    /// a value somebody typed (`--scheduler=`) and dials nothing. A list whose every
    /// entry is good but one is still refused -- a fallback that can never answer is
    /// discovered on the day it is needed, which is the day the entries before it are
    /// gone.
    /// @tparam Field The `NodeConfig` member holding the list.
    /// @param cfg The parsed configuration.
    /// @return The offending element, or nothing when every element is well formed.
    template <auto Field>
    [[nodiscard]] std::optional<std::string> ElementNotAnAddressToDial(NodeConfig const& cfg)
    {
        auto const& values = cfg.*Field;
        auto const bad = std::ranges::find_if(
            values, [](std::string const& value) { return value.empty() || !ParseDialEndpoint(value).has_value(); });
        if (bad == values.end())
            return std::nullopt;
        return *bad;
    }
} // namespace

std::optional<std::string> StartupPolicyRejection(NodeConfig const& cfg)
{
    // The value each listen flag takes, judged here rather than inside the tier that
    // binds it. Every one of these grammars used to be checked in its tier -- which
    // `--install-service` returns long before reaching -- so a typo registered
    // cleanly and then exited at every boot into a log nobody reads (#186).
    //
    // Walked off `NodeSurfaceTable()`, which is the port map. This used to be an
    // `EndpointFlags` table of its own, four rows carrying a flag spelling, a member
    // pointer, a grammar and a shape -- the same four columns for the same flags the
    // surface rows carry, which made it the fifth place the port map lived and the
    // one closest in shape to the table replacing it. Two tables with two member
    // pointers for one concept do not merely drift, they drift *silently*: the half
    // an operator is refused by and the half `--print-surfaces` prints them from
    // would have been different sets, so a worksheet could name a surface whose
    // spelling was never validated (#288).
    //
    // Each row asks its OWN surface's question, which is why `--discovery` carries a
    // different grammar: a beacon is sent TO an address, so no bare port may default
    // to a host, and a shared "is this an endpoint" test would accept `6681` here and
    // leave the tier to refuse it at every boot.
    //
    // **`--listen-raft` is now in this loop, and its absence used to be correct.**
    // The old comment said its own rules already refuse it absent and unusable, and
    // that a second row would answer in their place -- true while this table held
    // four hand-picked flags, and falsified by one that holds every surface. Leaving
    // it out would now need a column meaning "somebody else checks this one", which
    // is a column encoding an exception, in the table written to delete exceptions.
    //
    // What makes the coexistence safe is that both narrower rules still fire for the
    // inputs they were written about: `--node-id` with an EMPTY `--listen-raft` still
    // reaches the first, and a WELL-FORMED `--listen-raft` with no `--node-id` still
    // reaches the second. Only a malformed address moves here -- and it is better
    // answered here, because this message echoes what the operator typed and the
    // consensus prose can name a flag but not a value.
    //
    // "Parses when GIVEN", never "must parse". Empty is how five of the six say the
    // surface is off, and `--listen-node` carries a non-empty default every ordinary
    // node runs with -- so a rule spelled the other way would refuse the default
    // deployment outright.
    //
    // Asked before the rules below, because a value that is not an address is a typo
    // and answering it with a policy rule about the flag it was typed on describes
    // the wrong problem. Unlike those rules, whose messages are static prose, this
    // one echoes what the operator wrote -- which is the half a table row cannot do
    // and the half that matters when five ports were typed and one is wrong.
    for (auto const& surface: NodeSurfaceTable())
    {
        // The compile port has no spec text: its halves are `--bind` and `--port`,
        // each validated by its own value parser, so there is nothing here to judge.
        if (surface.spec == nullptr)
            continue;

        auto const& text = cfg.*surface.spec;
        if (text.empty() || surface.grammar.parses(text))
            continue;

        // "cannot use" rather than "cannot bind": most of these are bound and
        // discovery is sent to, and a message that named the wrong verb would send an
        // operator looking for a listening socket discovery never opens.
        return std::format("{}={} is not {}. The surface it configures cannot use an address that was never one, so "
                           "this node refuses to start.",
                           PrimaryFlag(surface),
                           text,
                           surface.grammar.shape);
    }

    // And the addresses this node DIALS, which the loop above cannot reach: it walks
    // `NodeSurfaceTable()`, and a surface is a port this process BINDS. #208 answered
    // `--advertise` -- the one that costs something silently, since a worker whose
    // advertised address nobody can dial registers, heartbeats, is leased out, and
    // fails at every client -- and left the rest, which is
    // [#968](https://github.com/LASTRADA-Software/fastcached/issues/968).
    //
    // **Each row carries its own predicate, because each flag has its own grammar**,
    // which is #208's own point and the reason one widened predicate will not do:
    //
    //   * `--scheduler` and `--upstream` are DIALLED, so a bare port names no machine
    //     and `ParseDialEndpoint` refuses it -- the same call `--discovery` already
    //     makes, and the one `--advertise` was given.
    //
    //     Both rows are "parses when GIVEN", never "must parse", exactly as the
    //     surface loop above spells it -- and for `--scheduler` that is not because
    //     an empty LIST is legal (a rule below requires one) but because these two
    //     questions belong to different rules. A shape row that also demanded presence
    //     would answer "is not an address to dial" for a flag nobody typed, which
    //     describes the wrong problem; `--upstream` really is legitimately empty, since
    //     a machine with no shared cache gets `NoUpstream`. An empty ELEMENT of the
    //     `--scheduler` list is different again -- somebody typed it -- and is refused
    //     here as a shape (`ElementNotAnAddressToDial`).
    // `--bind` is deliberately absent. Its value is a HOST with `--port` beside it,
    // not an endpoint, and whether a host is usable is answerable only by binding it
    // -- a rule here would either refuse legitimate spellings or pass everything.
    struct DialledAddress
    {
        /// The flag, spelled as the operator types it.
        std::string_view flag;
        /// The first value that fails this row's grammar, or nothing when all pass.
        ///
        /// A function rather than a member pointer, because the rows are not one
        /// shape: `--scheduler` is a repeatable list and the others are `std::string`,
        /// and a table that could only hold scalars would have left the list to a
        /// hand-written check beside it -- which is the fifth-place-the-map-lives failure #288
        /// records, one flag earlier.
        std::optional<std::string> (*offender)(NodeConfig const&);
        /// What a refusal tells the operator this value should have been.
        std::string_view shape;
    };

    constexpr auto DialledAddresses = std::to_array<DialledAddress>({
        { .flag = "--scheduler",
          .offender = &ElementNotAnAddressToDial<&NodeConfig::schedulers>,
          .shape = "an address to dial, as <host>:<port>" },
        { .flag = "--upstream",
          .offender = &NotAnAddressToDial<&NodeConfig::upstream>,
          .shape = "an address to dial, as <host>:<port>" },
    });

    for (auto const& row: DialledAddresses)
        if (auto const bad = row.offender(cfg); bad.has_value())
            // The same sentence shape the surface loop uses, and it ECHOES what the
            // operator wrote for the same reason: five addresses were typed and this
            // says which one is wrong. The verb differs because the fact does -- these
            // are asked of somewhere else, never bound here -- so a message about a
            // listener would send an operator looking for a socket this node never
            // opens.
            return std::format("{}={} is not {}. This node refuses to start rather than "
                               "run with an address it can never use.",
                               row.flag,
                               *bad,
                               row.shape);

    // Separate from NodeServiceRejection because it is a *startup* rule rather than
    // an install-time one: this misconfiguration is fatal every time the process
    // runs, not only when a registration is written, and gating it on
    // --install-service would let a hand-started scheduler make the same mistake.
    //
    // **And one generated row ahead of the table, for every setting only a component
    // reads** (#206). A row of `NodeOptions()` naming its component is refused on a
    // configuration that does not run it, with one sentence built from the row: the
    // value would be accepted and reach nothing. Ahead of the table because the table's
    // rows judge combinations of settings a node USES, and a setting it cannot use is
    // the more specific diagnosis. Scoped rules below cannot collide with it: they are
    // asked only where their component runs, and it only where its component does not.
    if (auto const* const unrun = core::findIfOrNull(NodeOptions(), [&cfg](OptionSpec<NodeConfig> const& spec) {
            return spec.component != nullptr && !spec.component->runs(cfg) && NamesSetting(spec, cfg);
        }))
        return UnrunComponentRefusal(*unrun);

    constexpr auto Rules = std::to_array<ConfigRule>({
        // **A name that reaches only this machine is never offered to a peer** -- and a feature the
        // operator ASKED for that would have to offer it is refused by name, first, so the answer
        // is its cause rather than a downstream symptom (`--discovery needs --listen-raft`,
        // with consensus standing down, would send the operator to add a flag already in force).
        // A DEFAULTED feature is confined to this machine instead -- consensus on loopback, a fleet of
        // its own, and discovery standing down -- and the node serves this machine alone
        // (`host-name-reaches-only-this-machine`). Both read the RESOLVED names, so a parse or an
        // install, which has not got there, is never refused for a name it has not seen.
        //
        // What counts as ASKED is what needs other machines to reach this one: a worker leased out
        // by a scheduler elsewhere (one on this machine reaches it at loopback), and consensus
        // other members dial -- every row of `ConsensusPeerAsks()`, a recorded mode that serves
        // consensus to peers (`ModeServesConsensusToPeers`) among them. A solitary or pending
        // node's port serves no peer yet and a learner dials out, so those are confined instead.
        { .scope = &WorkerComponent,
          .refuses = [](NodeConfig const& c) { return AdvertisedNameWithheld(c) && SchedulerIsRemote(c); },
          .message = WorkerNameReachesOnlyThisMachineRefusal },
        { .refuses = [](NodeConfig const& c) { return ConsensusNameWithheld(c) && AsksPeersToReachConsensus(c); },
          .message = ConsensusNameReachesOnlyThisMachineRefusal },
        // The consensus twin of `AdvertisesPastALoopbackBind`: peers told to dial a host that
        // resolves off this machine, at a port bound to loopback, are never answered -- and a
        // peer that cannot be dialled is counted in a quorum it is absent from. It was refused
        // by accident once, when a loopback bind with no `--raft-self` had no address at all;
        // now that such a bind names itself as bound, the case left is a stated address that
        // contradicts the bind, and it is refused on purpose.
        { .refuses = [](NodeConfig const& c) { return ConsensusPastALoopbackBind(c); },
          .message = ConsensusPastALoopbackBindRefusal },
        // A consensus address that reaches only this machine is never announced: a typed
        // discovery asking to beacon one is refused here, and a defaulted one stands down.
        { .refuses = [](NodeConfig const& c) { return DiscoveryAnnouncesOnlyThisMachine(c); },
          .message = DiscoveryAnnouncesOnlyThisMachineRefusal },
        // Only when the machine will not be asked. With discovery on, "no toolchain"
        // is not yet a fact -- it is a question this node answers once its survey
        // lands -- and refusing here would refuse every worker installed by a package.
        //
        // A row rather than a check in `main`, where it used to live: it reads nothing
        // but the parsed configuration, and a rule in the tier is one a RELOAD cannot
        // consult. Since #403 made both its flags reloadable, that was reachable --
        // the reload was accepted and the heartbeat thread then quietly emptied the
        // served set, leaving a live worker registering nothing.
        //
        // A worker's rule: on a node running none the generated row above answers either
        // flag first (#206).
        { .scope = &WorkerComponent,
          .refuses = [](NodeConfig const& c) { return c.toolchains.empty() && !c.toolchainDiscovery; },
          .message = "--no-toolchain-discovery was given and no --toolchain: a worker with none would register "
                     "and then refuse every job the scheduler sent it." },
        // A cache the operator NAMED, on a node that serves no surface to reach it
        // through. The tier already says so at Info and names the flags, which is
        // better than the silence #229 was filed about -- but an operator who
        // configured 64 GiB of cache and got none of it deserves a refusal, not a
        // line at boot that scrolls away. `--cache-memory`'s default is not the
        // subject: only an EXPLICIT one, plus a `--cache-dir` which has no default
        // at all, so a node that asked for nothing still starts.
        //
        // A ROW here rather than a check in the tier, which `--install-service`
        // returns long before reaching: the same registration would otherwise bake
        // the mistake in and replay it at every boot, into a log nobody reads.
        { .refuses =
              [](NodeConfig const& c) { return (c.cacheMemoryExplicit || !c.cacheDir.empty()) && c.nodeListen.empty(); },
          .message = "--cache-memory or --cache-dir was given with an empty --listen-node: the cache tier is "
                     "served on the node surface, so with no port there is nothing to reach it through and the "
                     "cache would be configured and unused. Give --listen-node a port, or drop the cache flags." },
        // A codec trio that reaches no tier. Same shape as the row above and as
        // `--dashboard-token-file` without `--dashboard`: a setting that configures a
        // half this node does not build is inert, and inert is invisible -- the
        // startup line drops the codec along with the half, so the ONE surface that
        // reports a codec says nothing about the one that was typed.
        //
        // Two rows rather than one, and named predicates rather than expressions; both
        // reasons are on `NamesMemoryCompressionWithoutATier`.
        { .refuses = [](NodeConfig const& c) { return NamesMemoryCompressionWithoutATier(c); },
          .message = "--memory-compression, --memory-compression-level or --memory-compression-min-bytes was given "
                     "with --cache-memory=0: there is no in-memory tier for it to configure, so the setting would "
                     "be accepted and reach nothing. Give --cache-memory a budget, or drop the memory-compression "
                     "flags." },
        { .refuses = [](NodeConfig const& c) { return NamesDiskCompressionWithoutATier(c); },
          .message = "--compression, --compression-level or --compression-min-bytes was given with no --cache-dir: "
                     "those configure the ON-DISK half, which is off without a path, so the setting would be "
                     "accepted and reach nothing. Name a --cache-dir, or use the --memory-compression flags, which "
                     "are the in-memory half's." },
        // Admitting other machines is only ever so they can dial this worker, and a worker
        // that registers a wildcard has told them to dial themselves -- so the two halves
        // have to be typed together or neither is worth anything. Scoped to a node that
        // registers with a scheduler, because that is what makes the advertised endpoint
        // travel: a node admitting peers to its CACHE tier is reached at `--listen-node` and
        // needs no advertise at all. A KEY route counts as admitting them
        // (`NamesAnAdmissionRoute`): a roster admits ticket holders and proven keys whatever
        // address they dial from.
        //
        // **The route gate is not what keeps the one-machine deployment working.** Every
        // node with a formation record runs consensus in every mode but a stood-down one
        // (`RunsConsensus`), and consensus is a key route -- so the gate is open on nearly
        // every worker that can start, a solitary one included, since it can found a fleet
        // at runtime with no restart for these rows to judge again. What spares that
        // deployment is the rest of each row: these three fire only on a DISAGREEMENT
        // between the bind and the advertise, and a node that binds loopback and advertises
        // loopback agrees; the fourth fires only when `SchedulerIsRemote`. The gate stays as
        // defence in depth: a node that runs no consensus and keeps no state directory admits
        // nobody but this machine, and is one these rows have no reason to judge.
        //
        // The four advertise rows describe what a WORKER registers, so they are scoped to
        // it: a node running none registers nothing (#206). Where it registers is its
        // formation record's answer (`SchedulersOf`), never `--scheduler`, which a node that
        // serves is refused.
        { .scope = &WorkerComponent,
          .refuses =
              [](NodeConfig const& c) {
                  return !SchedulersOf(c, AsConfigured).empty() && NamesAnAdmissionRoute(c) && AdvertisesWildcard(c);
              },
          .message = "this node admits other machines -- by key, or by --fleet-open -- so that they can dial this "
                     "worker, and --advertise names no address they can dial: the wildcard resolves to the CALLER's "
                     "own machine. This worker would register, heartbeat, be leased out and never be reached, with "
                     "no error at either end. Name --advertise=<this host>:6674, an address other machines can "
                     "dial." },

        // `--advertise` is text CLIENTS DIAL, and nothing parsed it (#208).
        //
        // `--install-service --advertise=nope` installed cleanly: the worker registered,
        // heartbeated, was leased out by the scheduler, and every client failed to reach
        // it. Silent at BOTH ends -- the same failure the emptiness rule beside this one
        // was written to prevent, reached by typing something instead of nothing.
        //
        // `ParseDialEndpoint` and not a second spelling: this is a "may I dial this?"
        // question and that function is its one author. Its three refusals are each
        // exactly the case here -- splitting is not parsing, an empty host names nobody,
        // and a bare port would send the client back to itself.
        //
        // Non-empty only, so it COMPOSES with the two rules that already judge this
        // flag rather than duplicating either: the emptiness refusal below, and
        // `main.cpp`'s socket-activation rule making it mandatory there.
        //
        // A ROW rather than a check in a tier, for this table's standing reason:
        // `--install-service` returns long before any tier exists, so a registration
        // would otherwise bake the typo in and replay it at every boot.
        { .refuses = [](NodeConfig const& c) { return !c.advertise.empty() && !ParseDialEndpoint(c.advertise).has_value(); },
          .message = "--advertise is not an address clients can dial: it must be host:port (or [v6]:port), with a "
                     "host that names a machine and a port in range. A worker whose advertised address does not "
                     "parse registers, heartbeats, is leased out, and is never reached -- with no error at either "
                     "end." },
        // The wildcard row's sibling, and it must stay a SEPARATE row. Since the bind
        // merged, `--advertise` falls back to the `Node` surface, which defaults to
        // loopback on a node that does not schedule -- so the shape an operator now
        // reaches by typing nothing is `127.0.0.1`, not `0.0.0.0`. It fails exactly as
        // silently and it is a different mistake: the wildcard is usually a bind
        // address pasted into the wrong flag, this one is having named no address at
        // all. One message for both would tell each operator about the other's error.
        //
        // Ordered after the wildcard so a configuration that is somehow both is
        // reported as the wildcard, which is the more specific diagnosis: first match
        // wins.
        //
        // The admission gate is IDENTICAL to the row above and deliberately so -- it
        // is what keeps the one-machine install working, where advertising loopback is
        // not a mistake but the correct answer. Widening the endpoint test while
        // relaxing this one would refuse the deployment an operator gets by installing
        // the package.
        { .scope = &WorkerComponent,
          .refuses =
              [](NodeConfig const& c) {
                  return !SchedulersOf(c, AsConfigured).empty() && NamesAnAdmissionRoute(c)
                         && AdvertisesLoopbackFromAReachableBind(c);
              },
          .message = "this node admits other machines -- by key, or by --fleet-open -- so that they can dial this "
                     "worker, --listen-node accepts from the network, and --advertise names loopback -- so every peer is "
                     "told to dial "
                     "ITSELF. This worker would register, heartbeat, be leased out and never be reached, with no "
                     "error at either end. Give --advertise=<this host>:6674, an address peers can dial. (A node "
                     "that binds loopback AND advertises loopback is a single-machine fleet and is fine; it is the "
                     "disagreement between the two that cannot work.)" },
        // The rule's third spelling, and the one an operator reaches by DOING WHAT THE
        // ROW ABOVE TELLS THEM: name --advertise with a routable address and stop.
        // Both rows above then pass while the surface is still on loopback, so the
        // worker is registered, leased out and unreachable -- the same silent shape,
        // entered through the remedy for it.
        //
        // A refusal that steers an operator into an unrefused failure is worse than no
        // refusal, which is why this is part of the same change rather than a
        // follow-up: the configuration could not arise before #290 stage 3, because
        // --bind defaulted to the wildcard.
        //
        // Disjoint from its siblings by construction -- `AdvertisesPastALoopbackBind`
        // is false whenever either of them is true -- so the order of the three is not
        // load-bearing and no configuration can be answered by the wrong one.
        { .scope = &WorkerComponent,
          .refuses =
              [](NodeConfig const& c) {
                  return !SchedulersOf(c, AsConfigured).empty() && NamesAnAdmissionRoute(c)
                         && AdvertisesPastALoopbackBind(c);
              },
          .message = "this node admits other machines -- by key, or by --fleet-open -- and --advertise names an "
                     "address they can dial, but --listen-node binds loopback, so this worker would never accept the "
                     "connections it told them to make: it registers, heartbeats, is leased out, and every "
                     "dispatched compile fails to connect with no error at either end. Give "
                     "--listen-node=0.0.0.0:6674 so it accepts from the network." },
        // The rule's FOURTH spelling, and the only one an operator reaches by typing
        // neither flag. The three rows above all judge a flag somebody WROTE, so a
        // worker started with neither `--advertise` nor `--listen-node` sailed past
        // all of them and registered loopback with a remote scheduler.
        // `NodeServiceRejection` refuses that at INSTALL time and nothing refused it
        // at a hand start, which is the reverse of the asymmetry #166 composed the two
        // tables to delete.
        //
        // #463 asked instead whether `--listen-node` should widen itself for a fleet
        // participant so the case stops arising. The zero-config defaults answered yes for
        // every node (`NodeSurfaceDefaultHost`), so what still reaches this row is a
        // loopback bind TYPED beside a remote scheduler.
        //
        // The admission gate is its three siblings' for the reason they give. Their
        // `!SchedulersOf(c, AsConfigured).empty()` half is not repeated: `SchedulerIsRemote` asks a
        // strictly stronger question, and a clause that can never decide anything is
        // one a reader has to prove harmless every time they meet it.
        //
        // So a worker registering with a scheduler on another machine, with a loopback advertise, IS
        // refused, on every
        // worker that can start: each keeps a state directory, which is a key route, and a
        // ticket holder the scheduler leases this worker to would be told to dial itself.
        // What keeps the single-machine install working is `SchedulerIsRemote`, which that
        // install answers no -- its scheduler is on loopback too. The route gate is defence
        // in depth for a node with no route at all, which today cannot start (it could
        // hold no identity); were that ever allowed, such a worker would admit its own
        // machine alone, and answering it here would tell an operator about an address
        // when their problem is a policy.
        //
        // The message names both flags, because naming only `--advertise` is what
        // steers an operator into the row above -- and it says what the ENDPOINT is
        // rather than which flag was omitted, since an explicit `--advertise=127.0.0.1`
        // reaches this row too and a sentence about flags nobody typed would be false
        // for it.
        { .scope = &WorkerComponent,
          .refuses = [](NodeConfig const& c) { return NamesAnAdmissionRoute(c) && AdvertisesLoopbackToARemoteScheduler(c); },
          .message = "this node admits other machines -- by key, or by --fleet-open -- and this worker registers "
                     "with a scheduler on a machine that is not this one, and would register LOOPBACK with it: "
                     "--advertise resolves to an address only this machine can dial, whether it was named or left "
                     "to fall back to --listen-node. Every client the scheduler leases it to would "
                     "dial ITSELF -- the worker registers, heartbeats, is leased out and is never reached, with no "
                     "error at either end. Give --listen-node=0.0.0.0:6674 and --advertise=<this host>:6674." },
        // `ConsensusTier::Start` decided this from inside the tier, which the install
        // path returns long before reaching -- so a registration naming no reachable
        // self was written happily and then died at every boot (#168).
        // The shape rule for a node that HAS asked for consensus: it must name the
        // endpoint its peers dial. `RunsConsensus` since #1022 -- the predicate moved
        // from `--node-id` to `--listen-raft` and this rule did not, because "a member
        // must name itself" is true whichever flag turns the mode on.
        { .refuses =
              [](NodeConfig const& c) {
                  // `--raft-self` is how a node says it (#1024): its identity is MINTED,
                  // and nothing else names where its peers dial it.
                  //
                  // Asked of the config as PARSED, before `ApplyNodeIdentity` turns the
                  // flag into a member -- which is what keeps this rule a pure function
                  // of the command line and lets `--install-service` reach it, and is
                  // also why the rule cannot simply look for the member afterwards.
                  //
                  // Through `ConsensusDialAddressOf` (#1328) rather than a spelling of its
                  // own: the worksheet prints NOT STATED from that answer, and a node it
                  // prints so must be exactly a node this row refuses.
                  auto const dial = ConsensusDialAddressOf(c);
                  return !dial.has_value() && dial.error() == ConsensusDialGap::Unstated;
              },
          .message = ConsensusNamesNoDialAddressRefusal },
        // A host with no port is not an endpoint, and the port is the half this flag
        // deliberately does not carry.
        { .refuses = [](NodeConfig const& c) { return !c.raftSelf.empty() && !RunsConsensus(c); },
          .message = "--raft-self names the host this node's peers dial, and the port comes from --listen-raft, "
                     "which is also what turns consensus ON. Without it there is no port to pair the host with "
                     "and no consensus for the pair to name a member of." },
        // The other half of the same flag group: the cluster's addresses given with
        // the switch that turns consensus on left off. `--cluster-dir` is deliberately
        // NOT here -- `FleetHistoryPath` reads it for the dashboard's history file, so
        // a node running no consensus still has a use for it.
        //
        // `--listen-raft` is gone from this predicate rather than kept, because it is
        // now the switch: naming it with no `--node-id` is the ORDINARY case #1022
        // exists to make possible, not a mistake.
        // On provenance, since discovery is on by default: a node that turns consensus off with
        // an empty `--listen-raft=` turns the defaulted discovery off with it (discovery starts only
        // beside consensus), and refusing it for an address it never typed would describe the wrong
        // problem. A TYPED `--discovery` on such a node is the silent no-op this row exists for.
        { .refuses =
              [](NodeConfig const& c) {
                  return c.discoveryAddressExplicit && !c.discoveryAddress.empty() && !RunsConsensus(c);
              },
          .message = DiscoveryNeedsConsensusRefusal },
        { .refuses = [](NodeConfig const& c) { return c.discoveryReplyPort != 0 && c.discoveryAddress.empty(); },
          .message = "--discovery-reply-port is where discovery is ANSWERED, and --discovery is not set. A port "
                     "pinned for a service that is off is a port nothing will ever bind, so this is a typo or a "
                     "half-finished configuration rather than an instruction." },
        { .refuses =
              [](NodeConfig const& c) {
                  if (c.discoveryReplyPort == 0)
                      return false;
                  auto const beacon = SplitHostPort(c.discoveryAddress);
                  return beacon.has_value() && ParseTcpPort(beacon->second) == c.discoveryReplyPort;
              },
          .message = "--discovery-reply-port names the --discovery port, and they are the two halves this node "
                     "keeps APART. It listens on the beacon port, which every node on the segment shares, and "
                     "answers somewhere only it holds -- because just one of the sockets sharing a port is handed "
                     "a unicast. Pointing both at one port is the configuration that made two nodes on a host see "
                     "each other and never finish proving the key. Pick another, or leave it unset." },
        // Newly reachable since the surfaces merged (#290), and silent without a row:
        // the scheduler verbs are answered on `--listen-node`, so emptying that flag
        // closes the port the scheduler would have been reached on. Before the merge
        // these were two flags naming two ports and neither could cancel the other; now
        // one can, and a node would start, log a scheduler, and answer nobody.
        { .refuses = [](NodeConfig const& c) { return ServesScheduler(c) && c.nodeListen.empty(); },
          .message = "this node's mode serves the fleet's scheduler, and --listen-node is empty: the scheduler verbs "
                     "are answered on the node's 0xFC port, beside the cache verbs, and an empty --listen-node "
                     "closes it. There would be nothing left for a peer to dial." },
        { .refuses = [](NodeConfig const& c) { return c.dashboard && c.adminListen.empty(); },
          .message = "--dashboard needs --admin-listen: the dashboard is served on the admin surface, and "
                     "without one there is no port for it to answer on. It would start, log nothing wrong, "
                     "and serve the page to nobody." },
        { .refuses = [](NodeConfig const& c) { return c.dashboard && !ServesScheduler(c); },
          .message = "--dashboard needs a node that serves the fleet's scheduler -- which its mode does, and only "
                     "while it runs consensus: a node that runs no scheduler never leads a fleet, so the page could "
                     "only ever say it is not the leader. The fleet-wide facts live where leadership does." },
        // The fleet's two readers are the dashboard page and the live-stats fleet stream, and both are served
        // only by a node that runs the scheduler -- `--dashboard` already requires it. Keyed on the scheduler
        // rather than on `--dashboard`, which was right until the stream arrived (#1399) and then refused the
        // one way a leader with no admin surface can admit a remote watcher.
        { .refuses = [](NodeConfig const& c) { return !c.dashboardTokenFile.empty() && !ServesScheduler(c); },
          .message = "--dashboard-token-file guards the fleet -- the dashboard and the live-stats fleet stream -- "
                     "and this node serves no scheduler (its mode does only while it runs consensus), so it serves "
                     "neither. A secret an operator provisioned, being read by nobody, is the silent no-op this "
                     "list exists to refuse." },
        { .refuses = [](NodeConfig const& c) { return c.tlsSelfSigned && !c.tlsCertFile.empty(); },
          .message = "--tls-self-signed and --tls-cert contradict each other: one generates a certificate and "
                     "the other names one. Silently preferring either would serve an identity the operator did "
                     "not choose, which is the whole thing a certificate is for." },
        { .refuses =
              [](NodeConfig const& c) { return (c.tlsSelfSigned || !c.tlsCertFile.empty()) && c.adminListen.empty(); },
          .message = "--tls-self-signed and --tls-cert serve the admin surface, and --admin-listen is not set. "
                     "TLS material nothing terminates is the silent no-op this list exists to refuse." },
        { .refuses = [](NodeConfig const& c) { return c.tlsCertFile.empty() != c.tlsKeyFile.empty(); },
          .message = "--tls-cert and --tls-key are both or neither: a certificate with no key cannot terminate "
                     "TLS, and this node would otherwise start and serve the admin surface in the clear while "
                     "an operator believed it was encrypted." },
        // A fact about the BUILD and the configuration together, and nothing else -- so a row, not the
        // admin tier's `#else` where it lived alone: there `--print-surfaces` accepted the line and
        // `--install-service` baked it into a registration that refused at every boot.
        { .refuses = [](NodeConfig const& c) { return !BuildServesTls && (c.tlsSelfSigned || !c.tlsCertFile.empty()); },
          .message = TlsUnavailableRefusal },
        // The rule that keeps a fleet map off an open port. Loopback needs no
        // credential -- reaching it already means being on the machine -- but a
        // bind an operator deliberately exposed does, and HTTPS alone does not
        // supply it: TLS authenticates the SERVER to the browser and says nothing
        // about who the browser is.
        { .refuses =
              [](NodeConfig const& c) {
                  if (!c.dashboard || c.adminListen.empty() || !c.dashboardTokenFile.empty())
                      return false;
                  // The address `AdminEndpoint::Start` will actually take, asked of
                  // the surface's own row rather than derived here from the same
                  // constant. This is the ONE security decision that hangs on the
                  // loopback default, so a second author of the resolution is the
                  // last place to keep one -- and the admin row could grow a
                  // condition the way the raft row already has (`--node-id`), at
                  // which point a copy would judge an address the surface no longer
                  // binds and silently stop requiring a credential.
                  auto const endpoints = RowFor(NodeSurface::Admin).Resolve(c);
                  return !endpoints.empty() && !IsLoopbackHost(endpoints.front().host);
              },
          .message = "--dashboard on a non-loopback --admin-listen needs --dashboard-token-file: the page is a "
                     "map of every member's hostname, endpoint and capacity, and an operator who bound it to "
                     "the network is publishing that to whoever asks. A bare port binds loopback and needs no "
                     "credential." },
        // **A worker on a node that can never form**: with the consensus port closed there is no
        // formation runtime, no scheduler of its own and no fleet to join, so the worker would
        // register nowhere. The tier found that at start (`WorkerTier::Start`, "nowhere to
        // register"), after an install had already baked it into a registration that replays it at
        // every boot. LATE in the table, so a flag the operator typed that needs consensus
        // (`--raft-self`, `--discovery`, `--dashboard`) is named first: dropping `--listen-raft=`
        // answers both, and the typed flag is the more specific diagnosis.
        { .scope = &WorkerComponent,
          .refuses = [](NodeConfig const& c) { return WorkerConsensusClosed(c); },
          .message = WorkerWithConsensusClosedRefusal },
        // And one that then runs nothing at all. Asked of the configuration, like every
        // row here, so an install is refused rather than a boot: `ConfiguresCacheTier` is
        // the tier's own question, and `RunsConsensus` the consensus tier's.
        { .refuses =
              [](NodeConfig const& c) {
                  return !RunsWorker(c) && !ServesScheduler(c) && !RunsConsensus(c) && !ConfiguresCacheTier(c);
              },
          .message = NodeRunsNothingRefusal },
    });

    if (auto rejection = FirstRefusal(Rules, cfg))
        return rejection;

    // **After the rows above, and that ordering is the decision** (#594). Those judge
    // the advertised HOST -- a wildcard, a loopback address peers cannot reach -- and
    // this judges its PORT. Where both apply the host is the larger problem: an endpoint
    // no peer can reach at all is worse than one they reach at the wrong port, so their
    // sentence is the one to give.
    //
    // Not a row in the table because it has to NAME the port this node actually serves.
    // An operator who typed the wrong one of their own ports needs the right one, not a
    // restatement -- and a `Rule`'s message is static prose. That is the same reason the
    // surface-grammar walk at the top of this function is written out rather than
    // tabulated: it echoes what the operator wrote, which is the half a table row cannot
    // do.
    if (auto rejection = AdvertisedPortRejection(cfg); rejection.has_value())
        return rejection;

    return std::nullopt;
}

std::optional<std::string> NodeInstallRejection(NodeConfig const& cfg)
{
    // Composed rather than merged, so each table keeps the contract its own callers
    // rely on: this is the only place that says an install must satisfy both, and a
    // new row in either reaches the install path without anybody remembering to add
    // it here.
    //
    // The install-time table is asked first because its messages name the action --
    // "is required to install a service" reads as an answer to `--install-service`
    // in a way a startup rule does not, and only one refusal is ever printed.
    if (auto rejection = NodeServiceRejection(cfg))
        return rejection;

    // Every startup rule is a pure invariant of the parsed configuration -- no
    // clock, no filesystem, no port -- so each is decided the moment the command
    // line is typed. `--install-service` bakes that command line in and replays it
    // at every boot, which makes a startup rule strictly MORE worth refusing here
    // than at a start: a start refuses once, in front of the operator who typed it,
    // while a registration refuses forever into a log nobody reads.
    return StartupPolicyRejection(cfg);
}

std::string HelpText(UsageColor color)
{
    UsageRows optionRows;
    AddOptionRows(optionRows, NodeOptions());

    auto const blocks = std::to_array<UsageBlock>({ { .entries = optionRows.Rows() } });
    std::span<UsageBlock const> const allBlocks { blocks };
    auto const sections = std::to_array<UsageSection>({
        { .subject = "fastcache-compile-node - a compile worker for fastcached distributed builds." },
        { .title = "OPTIONS", .blocks = allBlocks.subspan(0, 1) },
    });
    auto const substitutions = std::to_array<UsageSubstitution>({
        { .token = "{duration-units}", .value = DurationUnitList() },
    });
    return RenderUsage({ .sections = sections }, color, substitutions);
}

} // namespace FastCache::Node
