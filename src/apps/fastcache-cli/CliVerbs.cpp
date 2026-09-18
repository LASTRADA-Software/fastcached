// SPDX-License-Identifier: Apache-2.0
#include "CliVerbs.hpp"
#include "FleetDocument.hpp"
#include "FleetReach.hpp"
#include "LiveSession.hpp"
#include "LiveStats.hpp"
#include "NodeStatusText.hpp"

#include <FastCache/Cli/Duration.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Distributed/MembershipWire.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Cli
{

namespace
{
    /// The version this binary was built as.
    ///
    /// Defined by the build so the value cannot drift from the tag. Guarded rather than
    /// assumed, because a `version` verb reporting a placeholder is worse than one that
    /// does not build.
#if !defined(FASTCACHE_CLI_VERSION)
    #error "FASTCACHE_CLI_VERSION must be defined by the build"
#endif
    constexpr std::string_view ClientVersion = FASTCACHE_CLI_VERSION;

    /// Turn a failed exchange into an answer.
    ///
    /// `Unreachable` and `Transport` share one exit code deliberately: both mean *you
    /// did not get an answer*, and both are acted on the same way. The advisory still
    /// tells them apart, because *nothing is listening* and *the connection broke
    /// part-way* send an operator to different places. `Malformed` does NOT join them:
    /// a reply this client cannot read is a different problem with a different fix.
    /// @param error What went wrong.
    /// @return The answer.
    [[nodiscard]] Answer FromExchangeError(ExchangeError const& error)
    {
        switch (error.kind)
        {
            case ExchangeFailure::Unreachable:
            case ExchangeFailure::Transport:
                return Concluded(Outcome::Unreachable, error.detail);
            case ExchangeFailure::Malformed:
                return Concluded(Outcome::Protocol, error.detail);
            case ExchangeFailure::Last:
                break;
        }
        return Concluded(Outcome::Protocol, error.detail);
    }

    /// Turn a server error reply into an answer.
    ///
    /// `NOAUTH` gets a pointed remark naming the variable, because the bare server
    /// sentence ("Authentication required.") does not say how to supply one and this is
    /// the single most likely first-run failure.
    /// @param reply The error reply.
    /// @return The answer, always `Refused`.
    [[nodiscard]] Answer FromRespError(RespValue const& reply)
    {
        auto answer = Concluded(Outcome::Refused, reply.text);
        if (ErrorCodeWord(reply.text) == ErrorCode::NoAuth)
            answer.advisories.emplace_back("this server requires a credential; set FASTCACHE_TOKEN or pass --token-file");
        return answer;
    }

    /// Turn an unexpected reply shape into an answer.
    ///
    /// Its own outcome rather than a refusal: the server answered, and what it said was
    /// not something this client can read. That is `Protocol`, and it is the one
    /// outcome that suggests a version mismatch rather than a misconfiguration.
    /// @param verb What was asked.
    /// @param expected What the handler wanted, in words.
    /// @param reply What arrived.
    /// @return The answer.
    [[nodiscard]] Answer WrongShape(std::string_view verb, std::string_view expected, RespValue const& reply)
    {
        return Concluded(Outcome::Protocol,
                         std::format("{}: expected {} and the server sent {}", verb, expected, RespTypeName(reply.type)));
    }

    /// Send one command and pre-classify the two failures every handler shares.
    ///
    /// Handlers therefore only ever see a reply that arrived and is not an error, which
    /// is what keeps each of them to a few lines.
    /// @param context The invocation.
    /// @param argv The command and its arguments.
    /// @return The reply, or the answer to return instead.
    [[nodiscard]] std::expected<RespValue, Answer> Ask(VerbContext const& context, std::vector<std::string> argv)
    {
        auto reply = context.resp->Call(argv);
        if (!reply.has_value())
            return std::unexpected(FromExchangeError(reply.error()));
        if (IsError(*reply))
            return std::unexpected(FromRespError(*reply));
        return std::move(*reply);
    }

    /// The verb's protocol command followed by its operands.
    /// @param context The invocation.
    /// @return The argument vector.
    [[nodiscard]] std::vector<std::string> CommandWithOperands(VerbContext const& context)
    {
        std::vector<std::string> argv;
        argv.reserve(context.operands.size() + 1);
        argv.emplace_back(context.verb->protocolCommand);
        argv.insert(argv.end(), context.operands.begin(), context.operands.end());
        return argv;
    }

    /// What the operator is told when a cached VALUE could not be shown as text.
    ///
    /// A named constant because `BinaryCellAdvisory` below is suppressed by comparing
    /// against it -- by equality, never by sniffing the text for a word, so the
    /// suppression is parsing rather than guessing and reveals itself by failing
    /// rather than by quietly not firing if either spelling moves.
    constexpr std::string_view BinaryValueAdvisory =
        "the value is not valid UTF-8, so it is shown base64-encoded; --raw writes the bytes";

    /// ...and when anything ELSE in the answer could not.
    ///
    /// A key, in practice. After the UTF-8 question was folded into `TextCell`, a
    /// non-UTF-8 key became a base64 cell with nothing saying why, where a value had
    /// been explained since it was first classified -- and base64 in a `key` column
    /// with no accompanying sentence is a genuine puzzle for an operator, who has no
    /// reason to suspect the key rather than the tool.
    ///
    /// It says a byte string is ORDINARY rather than a fault, because it is: the
    /// protocol never promised a key was text, so this is a rendering decision and not
    /// a report of damage.
    constexpr std::string_view BinaryCellAdvisory =
        "some text in this answer is not valid UTF-8, so it is shown base64-encoded rather than repaired";

    /// Whether any cell of @p value could not be shown as text.
    /// @param value The answer's value.
    /// @return True when some cell is `Binary`.
    [[nodiscard]] bool HasBinaryCell(Value const& value)
    {
        auto const binary = [](Cell const& cell) {
            return cell.kind == CellKind::Binary;
        };
        if (binary(value.scalar))
            return true;
        if (std::ranges::any_of(value.fields, [&](Field const& field) { return binary(field.value); }))
            return true;
        return std::ranges::any_of(value.rows,
                                   [&](std::vector<Cell> const& row) { return std::ranges::any_of(row, binary); });
    }

    /// Say so when an answer carries bytes that could not be shown as text.
    ///
    /// At the ONE place every answer passes through rather than at the handlers that
    /// happen to produce such cells, for the same reason the UTF-8 question itself was
    /// folded into `TextCell`: a diagnostic each handler has to remember is one the
    /// next handler forgets, which is how a key came to be classified and never
    /// explained while a value was both.
    ///
    /// THE LIMIT, stated rather than left to be discovered: when a key AND its value
    /// are both binary, only the value's advisory is printed, because that one is more
    /// specific and naming both would be two sentences about one observation. The
    /// operator still sees base64 in both columns under a sentence that says base64.
    /// @param answer The finished answer; may gain one advisory.
    void NoteBinaryCells(Answer& answer, Wire wire)
    {
        if (!HasBinaryCell(answer.value))
            return;
        if (std::ranges::find(answer.advisories, BinaryValueAdvisory) != answer.advisories.end())
            return;

        auto const note = WireTable[static_cast<std::size_t>(wire)].binaryNote;
        if (note.empty())
            answer.advisories.emplace_back(BinaryCellAdvisory);
        else
            answer.advisories.emplace_back(std::format("{}; {}", BinaryCellAdvisory, note));
    }

    /// A value cell plus the remark that goes with it when the bytes are not text.
    /// @param bytes The value.
    /// @param answer The answer being built; gains an advisory for a binary value.
    /// @return The cell.
    [[nodiscard]] Cell ValueCell(std::string_view bytes, Answer& answer)
    {
        auto cell = TextCell(std::string { bytes });
        // Once per ANSWER, not once per value: `mget k1 k2 k3` over three binary
        // values printed the identical stderr line three times. The sentence says
        // nothing a second copy adds, and `NoteBinaryCells` below already
        // de-duplicated by exact comparison -- the asymmetry was live in the same
        // function block.
        if (cell.kind == CellKind::Binary
            && std::ranges::find(answer.advisories, BinaryValueAdvisory) == answer.advisories.end())
            answer.advisories.emplace_back(BinaryValueAdvisory);
        return cell;
    }

    // --- handlers, in table order -------------------------------------------------

    /// `get <key>`: one value, or a miss.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Fetch(VerbContext const& context)
    {
        auto reply = Ask(context, CommandWithOperands(context));
        if (!reply.has_value())
            return std::move(reply.error());

        if (IsNull(*reply))
        {
            auto answer = Answered(ScalarValue(AbsentCell()), Outcome::Negative);
            answer.advisories.emplace_back(std::format("no such key: {}", context.operands[0]));
            return answer;
        }
        if (reply->type != RespType::BulkString && reply->type != RespType::SimpleString)
            return WrongShape(context.verb->name, "a value", *reply);

        Answer answer;
        if (context.options.raw)
        {
            // The operator asked for the bytes, so they are handed over untouched and
            // the value model is bypassed. This is the only escape from the
            // text/base64 classification, and it exists because piping a cached blob
            // to a file is an ordinary thing to want.
            answer.rawPayload = reply->text;
            answer.value = EmptyValue();
            return answer;
        }
        answer.value = ScalarValue(ValueCell(reply->text, answer));
        return answer;
    }

    /// `mget <key>...`: a table of keys and values.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer FetchMany(VerbContext const& context)
    {
        auto reply = Ask(context, CommandWithOperands(context));
        if (!reply.has_value())
            return std::move(reply.error());
        if (reply->type != RespType::Array)
            return WrongShape(context.verb->name, "an array", *reply);
        if (reply->items.size() != context.operands.size())
            return Concluded(Outcome::Protocol,
                             std::format("{}: asked for {} keys and the server answered about {}",
                                         context.verb->name,
                                         context.operands.size(),
                                         reply->items.size()));

        Answer answer;
        std::vector<std::vector<Cell>> rows;
        rows.reserve(reply->items.size());
        auto found = std::size_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, reply->items.size()))
        {
            auto const& item = reply->items[index];
            auto cell = IsNull(item) ? AbsentCell() : ValueCell(item.text, answer);
            if (!IsNull(item))
                ++found;
            rows.push_back(std::vector<Cell> { TextCell(context.operands[index]), std::move(cell) });
        }

        answer.value = TableValue(std::vector<std::string> { "key", "value" }, std::move(rows));
        // Every key a miss is a negative answer; some found is affirmative. Reporting
        // the count is what keeps the two from needing the exit code to tell them
        // apart in a script that asked about ten keys.
        if (found == 0)
        {
            answer.outcome = Outcome::Negative;
            answer.advisories.emplace_back("none of the keys exist");
        }
        else if (found != reply->items.size())
        {
            answer.advisories.emplace_back(std::format("{} of {} keys exist", found, reply->items.size()));
        }
        return answer;
    }

    /// The expiry a store sends: `--ttl`, refused where the verb's wire would read it as something else.
    ///
    /// Asked of the wire's `relativeTtlCeiling` column rather than of the verb, so both store paths --
    /// RESP's `EX` and memcached's `exptime` -- take one rule, and only a wire that has a bound refuses.
    /// @param context The invocation.
    /// @return The expiry to send, nullopt when none was named, or the refusal.
    [[nodiscard]] std::expected<std::optional<std::chrono::seconds>, Answer> TtlToSend(VerbContext const& context)
    {
        auto const& wire = WireTable[static_cast<std::size_t>(context.verb->wire)];
        auto const& ttl = context.options.ttl;
        if (ttl.has_value() && wire.relativeTtlCeiling.has_value() && *ttl > *wire.relativeTtlCeiling)
        {
            auto const ceiling = FormatDuration(*wire.relativeTtlCeiling);
            return std::unexpected(
                Concluded(Outcome::Usage,
                          std::format("`--ttl={}`: the {} wire reads an expiry past {} as a date, not a length of time, so "
                                      "`--ttl` is at most {} there",
                                      FormatDuration(*ttl),
                                      wire.name,
                                      ceiling,
                                      ceiling)));
        }
        return ttl;
    }

    /// `set <key> <value>`: store, honouring `--ttl`, `--nx` and `--xx`.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Store(VerbContext const& context)
    {
        auto ttl = TtlToSend(context);
        if (!ttl.has_value())
            return std::move(ttl.error());
        auto argv = CommandWithOperands(context);
        if (ttl->has_value())
        {
            argv.emplace_back("EX");
            argv.emplace_back(std::format("{}", (*ttl)->count()));
        }
        if (context.options.onlyIfAbsent)
            argv.emplace_back("NX");
        if (context.options.onlyIfPresent)
            argv.emplace_back("XX");

        auto reply = Ask(context, std::move(argv));
        if (!reply.has_value())
            return std::move(reply.error());

        // A null here is the server saying the NX/XX condition was not met. That is an
        // answer, and it is `no` -- not a failure, and not a success either.
        if (IsNull(*reply))
        {
            auto answer = Concluded(Outcome::Negative);
            answer.advisories.emplace_back(context.options.onlyIfAbsent ? "the key already exists, so --nx stored nothing"
                                                                        : "the key does not exist, so --xx stored nothing");
            return answer;
        }
        if (reply->type != RespType::SimpleString)
            return WrongShape(context.verb->name, "an acknowledgement", *reply);
        return Answered(EmptyValue());
    }

    /// `del`, `exists`, `incr`, `decr`, `incrby`, `decrby`: one number back.
    ///
    /// One handler for six rows, which is what the `protocolCommand` column is for.
    /// Zero is a *negative* answer for the counting verbs and an ordinary value for the
    /// arithmetic ones, so the row's own name decides -- an `incr` that lands on zero
    /// has not failed.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Counting(VerbContext const& context)
    {
        auto reply = Ask(context, CommandWithOperands(context));
        if (!reply.has_value())
            return std::move(reply.error());
        if (reply->type != RespType::Integer)
            return WrongShape(context.verb->name, "an integer", *reply);

        auto answer = Answered(ScalarValue(NumberCell(reply->integer)));
        auto const counts = context.verb->name == "del" || context.verb->name == "exists";
        if (counts && reply->integer == 0)
        {
            answer.outcome = Outcome::Negative;
            answer.advisories.emplace_back("no key matched");
        }
        return answer;
    }

    /// `expire`, `persist`: the server answers whether it did anything.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Flagged(VerbContext const& context)
    {
        auto reply = Ask(context, CommandWithOperands(context));
        if (!reply.has_value())
            return std::move(reply.error());
        if (reply->type != RespType::Integer)
            return WrongShape(context.verb->name, "an integer", *reply);

        if (reply->integer == 0)
        {
            auto answer = Concluded(Outcome::Negative);
            answer.advisories.emplace_back(
                std::format("nothing changed; {} may not exist, or already had no expiry", context.operands[0]));
            return answer;
        }
        return Answered(EmptyValue());
    }

    /// `ttl <key>`: how long a key has left.
    ///
    /// **A record of three fields rather than one number, and that is the whole point
    /// of this handler.** RESP answers `-2` for *no such key*, `-1` for *exists and has
    /// no expiry*, and a count otherwise. Rendered as a bare scalar, the first two both
    /// come out as an absent cell and are distinguishable only by the exit code -- a
    /// state collapse on stdout, in the one verb whose whole job is that distinction.
    /// Naming `exists` separately means all three states are visible in every format.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Lifetime(VerbContext const& context)
    {
        auto reply = Ask(context, CommandWithOperands(context));
        if (!reply.has_value())
            return std::move(reply.error());
        if (reply->type != RespType::Integer)
            return WrongShape(context.verb->name, "an integer", *reply);

        constexpr std::int64_t NoSuchKey = -2;
        constexpr std::int64_t NoExpiry = -1;

        auto const key = TextCell(context.operands[0]);
        if (reply->integer == NoSuchKey)
        {
            auto answer = Answered(RecordValue(std::vector<Field> {
                                       Field { .name = "key", .value = key },
                                       Field { .name = "exists", .value = BooleanCell(false) },
                                       Field { .name = "ttl", .value = AbsentCell() },
                                   }),
                                   Outcome::Negative);
            answer.advisories.emplace_back(std::format("no such key: {}", context.operands[0]));
            return answer;
        }

        auto const ttl = reply->integer == NoExpiry ? AbsentCell() : NumberCell(reply->integer);
        auto answer = Answered(RecordValue(std::vector<Field> {
            Field { .name = "key", .value = key },
            Field { .name = "exists", .value = BooleanCell(true) },
            Field { .name = "ttl", .value = ttl },
        }));
        if (reply->integer == NoExpiry)
            answer.advisories.emplace_back("the key exists and has no expiry set");
        return answer;
    }

    /// `flush`: discard the keyspace.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Flush(VerbContext const& context)
    {
        auto argv = std::vector<std::string> { context.options.everything ? std::string { "FLUSHALL" }
                                                                          : std::string { context.verb->protocolCommand } };
        auto reply = Ask(context, std::move(argv));
        if (!reply.has_value())
            return std::move(reply.error());
        if (reply->type != RespType::SimpleString)
            return WrongShape(context.verb->name, "an acknowledgement", *reply);
        return Answered(EmptyValue());
    }

    /// `ping`, `echo`: whatever the server says back, as text.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Echoed(VerbContext const& context)
    {
        auto reply = Ask(context, CommandWithOperands(context));
        if (!reply.has_value())
            return std::move(reply.error());
        if (reply->type != RespType::SimpleString && reply->type != RespType::BulkString)
            return WrongShape(context.verb->name, "a string", *reply);

        Answer answer;
        answer.value = ScalarValue(ValueCell(reply->text, answer));
        return answer;
    }

    /// Read `INFO` and hand back its payload.
    /// @param context The invocation.
    /// @return The payload, or the answer to return instead.
    [[nodiscard]] std::expected<std::string, Answer> ReadInfo(VerbContext const& context)
    {
        auto reply = Ask(context, std::vector<std::string> { "INFO" });
        if (!reply.has_value())
            return std::unexpected(std::move(reply.error()));
        // A bulk string under RESP2, a verbatim string under RESP3. Accepting both
        // costs one comparison and means this does not break if the client ever sends
        // `HELLO 3`.
        if (reply->type != RespType::BulkString && reply->type != RespType::Verbatim)
            return std::unexpected(WrongShape("info", "a text payload", *reply));
        return reply->text;
    }

    /// `info`: the server's own `INFO` payload as a record.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Info(VerbContext const& context)
    {
        auto const body = ReadInfo(context);
        if (!body.has_value())
            return body.error();

        auto record = ParseInfo(*body);
        if (record.fields.empty())
            return Concluded(Outcome::Protocol, "the server's INFO payload carried no fields");
        return Answered(std::move(record));
    }

    /// `version`: both ends, side by side.
    ///
    /// Both, because the interesting question is never one of them -- it is whether
    /// they agree. A stale binary talking to an upgraded daemon is exactly the shape
    /// that has cost this project a served-but-wrong object before, and an operator
    /// cannot see it from one number.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Versions(VerbContext const& context)
    {
        auto fields = std::vector<Field> {
            Field { .name = "client", .value = TextCell(std::string { ClientVersion }) },
        };

        auto const body = ReadInfo(context);
        if (!body.has_value())
        {
            // The client half is knowable without a server, so it is still reported;
            // the server half is absent rather than blank, and the reason travels as a
            // remark. Reporting only the failure would throw away the half that
            // answered.
            fields.push_back(Field { .name = "server", .value = AbsentCell() });
            auto answer = Answered(RecordValue(std::move(fields)), body.error().outcome);
            answer.advisories = body.error().advisories;
            answer.advisories.emplace_back("the server's version could not be read");
            return answer;
        }

        auto const record = ParseInfo(*body);
        auto const* const server = FindField(record, "fastcached_version");
        fields.push_back(Field { .name = "server", .value = server == nullptr ? AbsentCell() : server->value });
        if (auto const* const dialect = FindField(record, "redis_version"); dialect != nullptr)
            fields.push_back(Field { .name = "resp_dialect", .value = dialect->value });

        auto answer = Answered(RecordValue(std::move(fields)));
        if (server == nullptr)
            answer.advisories.emplace_back("the server's INFO payload named no fastcached_version");
        return answer;
    }

    /// `stats`: whichever source answers, said out loud.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Stats(VerbContext const& context)
    {
        auto const attempts = context.stats->Gather();
        return ChooseStats(attempts);
    }

    // --- memcached text -----------------------------------------------------------

    /// What a one-line memcached status concludes.
    struct McStatusSpec
    {
        std::string_view word;     ///< As the server writes it.
        Outcome outcome;           ///< What it means for the exit code.
        std::string_view advisory; ///< A sentence for the operator, or empty.
    };

    /// The status words this daemon writes, and what each concludes.
    ///
    /// A table because the mapping IS the whole of it: `STORED` and `NOT_STORED` differ
    /// by a row, not by a handler, and five verbs share every row. Read out of
    /// `MemcachedText.cpp`'s own writers rather than from memcached's documentation.
    ///
    /// **There is no default row on purpose.** A word with no row is reported as a
    /// protocol failure naming the word, not guessed at -- an unknown status mapped to
    /// `Affirmative` by a fallback would report success for whatever a future server
    /// says, which is the failure this client is least able to notice.
    constexpr auto McStatusTable = std::to_array<McStatusSpec>({
        { .word = "STORED", .outcome = Outcome::Affirmative, .advisory = "" },
        { .word = "TOUCHED", .outcome = Outcome::Affirmative, .advisory = "" },
        { .word = "DELETED", .outcome = Outcome::Affirmative, .advisory = "" },
        { .word = "OK", .outcome = Outcome::Affirmative, .advisory = "" },
        { .word = "NOT_STORED",
          .outcome = Outcome::Negative,
          .advisory = "nothing was stored: `add` needs the key absent, and `replace`, `append` and "
                      "`prepend` need it present" },
        { .word = "EXISTS",
          .outcome = Outcome::Negative,
          .advisory = "the value changed since that cas token was issued, so nothing was stored" },
        { .word = "NOT_FOUND", .outcome = Outcome::Negative, .advisory = "no such key" },
    });

    /// A readable name for one of the `me` inspector's flags.
    struct McFlagSpec
    {
        std::string_view flag; ///< As `MemcachedMeta.cpp` writes it.
        std::string_view name; ///< What `inspect` calls it.
    };

    /// `me`'s flags, renamed for a reader.
    ///
    /// Renaming rather than relaying: `exp`, `la` and `cls` are memcached's spellings,
    /// and this verb exists precisely to be the readable per-key diagnostic. A flag with
    /// no row keeps its WIRE name rather than being dropped, so a server that grows one
    /// is visible here the day it does instead of silently absent.
    constexpr auto McFlagTable = std::to_array<McFlagSpec>({
        { .flag = "exp", .name = "ttl_seconds" },
        { .flag = "la", .name = "last_access_seconds" },
        { .flag = "cas", .name = "cas" },
        { .flag = "fetch", .name = "fetched" },
        { .flag = "cls", .name = "slab_class" },
        { .flag = "size", .name = "value_bytes" },
    });

    /// One `stats` sub-command this client offers.
    struct McStatsSub
    {
        std::string_view name;    ///< As the wire spells it.
        std::string_view summary; ///< What it reports.
    };

    /// The `stats` families worth asking this daemon for.
    ///
    /// An allowlist, and **`reset` is deliberately not on it**: `MemcachedText.cpp`
    /// answers `RESET` while resetting nothing -- its own comment says it acknowledges
    /// the command rather than lying about state -- so relaying it would report a reset
    /// that did not happen. `conns` IS offered although this daemon keeps no connection
    /// registry, because its empty answer is a TRUE one and renders as an empty table.
    constexpr auto McStatsSubs = std::to_array<McStatsSub>({
        { .name = "settings", .summary = "configured limits and policy" },
        { .name = "items", .summary = "per-slab item counts" },
        { .name = "slabs", .summary = "slab allocator figures" },
        { .name = "sizes", .summary = "one approximate size bucket" },
        { .name = "conns", .summary = "connections, which this daemon does not track" },
    });

    /// The status row @p status matches, or nullptr.
    /// @param status The whole status line.
    /// @return The row, or nullptr.
    [[nodiscard]] McStatusSpec const* FindMcStatus(std::string_view status) noexcept
    {
        for (auto const& row: McStatusTable)
            if (row.word == status)
                return &row;
        return nullptr;
    }

    /// What `inspect` calls the flag @p wireName.
    /// @param wireName The flag as the server spelled it.
    /// @return The readable name, or @p wireName when there is no row.
    [[nodiscard]] std::string_view McFlagName(std::string_view wireName) noexcept
    {
        for (auto const& row: McFlagTable)
            if (row.flag == wireName)
                return row.name;
        return wireName;
    }

    /// Turn a memcached error reply into an answer.
    ///
    /// The server's own sentence is the advisory, because it is more specific than
    /// anything this client could infer. The one addition is the auth explanation: the
    /// bare `CLIENT_ERROR authentication required` does not say that this protocol has
    /// no AUTH verb, so an operator reads it as *supply a credential* and there is no
    /// way to. Keyed on the wire's `authenticable` column rather than on the verb, since
    /// it is the protocol that lacks the verb.
    /// @param context The invocation.
    /// @param reply The error reply.
    /// @return The answer, always `Refused`.
    [[nodiscard]] Answer FromMemcachedError(VerbContext const& context, McReply const& reply)
    {
        auto answer = Concluded(Outcome::Refused, reply.status);
        auto const& wire = WireTable[static_cast<std::size_t>(context.verb->wire)];
        if (!wire.authenticable && reply.status.contains("authentication required"))
            answer.advisories.emplace_back(
                std::format("this daemon requires a password, and the memcached text protocol has no AUTH "
                            "verb -- so `{}` cannot be run against it at all. The RESP verbs authenticate and "
                            "reach the same --addr; `get`, `set`, `del`, `ttl` and `expire` cover most of what "
                            "`{}` is for.",
                            context.verb->name,
                            context.verb->name));
        return answer;
    }

    /// Turn a one-line status reply into an answer.
    /// @param context The invocation.
    /// @param reply The reply.
    /// @return The answer.
    [[nodiscard]] Answer FromMemcachedStatus(VerbContext const& context, McReply const& reply)
    {
        auto const* const row = FindMcStatus(reply.status);
        if (row == nullptr)
            return Concluded(Outcome::Protocol,
                             std::format("{}: the server answered `{}`, which this client has no reading for",
                                         context.verb->name,
                                         reply.status));
        auto answer = Concluded(row->outcome);
        if (!row->advisory.empty())
            answer.advisories.emplace_back(row->advisory);
        return answer;
    }

    /// Send @p request over the memcached wire and hand back the reply.
    /// @param context The invocation.
    /// @param request The encoded command.
    /// @return The reply, or the answer that replaces it.
    [[nodiscard]] std::expected<McReply, Answer> AskMemcached(VerbContext const& context, std::string const& request)
    {
        auto reply = context.memcached->Send(request);
        if (!reply.has_value())
            return std::unexpected(FromExchangeError(reply.error()));
        if (reply->kind == McLeadToken::Error)
            return std::unexpected(FromMemcachedError(context, *reply));
        return std::move(*reply);
    }

    /// Refuse the first of @p count leading operands this wire cannot carry.
    ///
    /// **Before anything is sent.** The text protocol has no quoting and no escaping, so
    /// a key with a space arrives as two tokens and silently addresses a different key,
    /// and one carrying a CR ends the line early and injects whatever follows as a
    /// command. Sending it and letting the server complain is not an option for the
    /// second of those.
    /// @param context The invocation.
    /// @param count How many leading operands are tokens rather than payload.
    /// @return The refusal, or nullopt when every one is expressible.
    [[nodiscard]] std::optional<Answer> RefuseUnsendableToken(VerbContext const& context, std::size_t count)
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, std::min(count, context.operands.size())))
            if (!ValidTextToken(context.operands[index]))
                return Concluded(Outcome::Usage,
                                 std::format("`{}` cannot travel on the memcached text protocol: it is empty, or "
                                             "carries a space or a control character, and this wire has no quoting",
                                             context.operands[index]));
        return std::nullopt;
    }

    /// The verb's protocol command followed by its operands, encoded for this wire.
    /// @param context The invocation.
    /// @return The bytes to send.
    [[nodiscard]] std::string MemcachedCommandWithOperands(VerbContext const& context)
    {
        auto const args = std::vector<std::string> { context.operands.begin(), context.operands.end() };
        return EncodeMemcachedCommand(context.verb->protocolCommand, args);
    }

    /// `touch <key> <seconds>`: move a key's expiry without reading it.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Touch(VerbContext const& context)
    {
        if (auto refusal = RefuseUnsendableToken(context, context.operands.size()); refusal.has_value())
            return std::move(*refusal);
        auto reply = AskMemcached(context, MemcachedCommandWithOperands(context));
        if (!reply.has_value())
            return std::move(reply.error());
        return FromMemcachedStatus(context, *reply);
    }

    /// `gat <seconds> <key>...` and `gats <seconds> <key>...`.
    ///
    /// The operand order mirrors the WIRE, where the expiry comes first -- which is the
    /// opposite way round from `touch`, on this daemon and on memcached. Reordering it
    /// to match `touch` would make a packet capture and a `--help` line disagree, and
    /// this is a protocol client; either spelling fails loudly on the other's input,
    /// since one operand must be a number.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer FetchAndTouch(VerbContext const& context)
    {
        if (auto refusal = RefuseUnsendableToken(context, context.operands.size()); refusal.has_value())
            return std::move(*refusal);
        auto reply = AskMemcached(context, MemcachedCommandWithOperands(context));
        if (!reply.has_value())
            return std::move(reply.error());
        if (reply->kind != McLeadToken::Value && reply->status != "END")
            return Concluded(
                Outcome::Protocol,
                std::format("{}: expected values, and the server answered `{}`", context.verb->name, reply->status));

        Answer answer;
        auto const wantsCas = context.verb->protocolCommand == "gats";
        std::vector<std::string> columns { "key", "value" };
        if (wantsCas)
            columns.emplace_back("cas");

        std::vector<std::vector<Cell>> rows;
        rows.reserve(reply->values.size());
        for (auto const& value: reply->values)
        {
            std::vector<Cell> row;
            row.push_back(TextCell(value.key));
            row.push_back(ValueCell(value.data, answer));
            if (wantsCas)
                row.push_back(value.hasCas ? NumberCell(value.cas) : AbsentCell());
            rows.push_back(std::move(row));
        }
        answer.value = TableValue(std::move(columns), std::move(rows));

        // **This wire SKIPS a miss rather than naming it**, so a short table reads as a
        // complete answer unless the count is said out loud. `mget` gets that for free
        // over RESP, which answers a null per key; here there is nothing to count but
        // the difference, and an operator who asked about four keys and sees three rows
        // has no way to tell which one is gone without it.
        auto const asked = context.operands.size() - 1;
        if (reply->values.empty())
        {
            answer.outcome = Outcome::Negative;
            answer.advisories.emplace_back("none of the keys exist");
        }
        else if (reply->values.size() != asked)
        {
            answer.advisories.emplace_back(
                std::format("{} of {} keys exist; this protocol does not name the misses", reply->values.size(), asked));
        }
        return answer;
    }

    /// `add`, `replace`, `append`, `prepend`, `cas`: store, conditionally.
    ///
    /// One handler for five rows, keyed on `protocolCommand` exactly as `Counting` is.
    /// `--ttl` is a modifier of the three rows that HONOUR it: `append` and `prepend`
    /// reach `CacheEngine::Append`/`Prepend`, which take no expiry at all, so accepting
    /// `--ttl` there would silently discard it.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer StoreText(VerbContext const& context)
    {
        if (auto refusal = RefuseUnsendableToken(context, 1); refusal.has_value())
            return std::move(*refusal);

        std::uint64_t casToken = 0;
        auto const isCas = context.verb->protocolCommand == "cas";
        if (isCas && !ParseWholeUnsigned(context.operands[2], casToken))
            return Concluded(
                Outcome::Usage,
                std::format("`{}` is not a cas token; `inspect <key>` reports the current one", context.operands[2]));

        auto ttl = TtlToSend(context);
        if (!ttl.has_value())
            return std::move(ttl.error());
        // No `--ttl` is an `exptime` of 0, which memcached reads as *never expires*.
        auto const exptime = ttl->value_or(std::chrono::seconds::zero()).count();
        auto reply =
            AskMemcached(context,
                         EncodeMemcachedStorage(
                             context.verb->protocolCommand, context.operands[0], 0, exptime, context.operands[1], casToken));
        if (!reply.has_value())
            return std::move(reply.error());
        return FromMemcachedStatus(context, *reply);
    }

    /// `inspect <key>`: the `me` inspector's per-key facts.
    ///
    /// Nothing else in this tree exposes a key's last-access time, its cas token or its
    /// stored size, which is why this verb exists.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Inspect(VerbContext const& context)
    {
        if (auto refusal = RefuseUnsendableToken(context, 1); refusal.has_value())
            return std::move(*refusal);
        auto reply = AskMemcached(context, MemcachedCommandWithOperands(context));
        if (!reply.has_value())
            return std::move(reply.error());

        if (reply->kind != McLeadToken::Meta)
        {
            // `EN` is the miss and is the only other shape this verb gets. Checked as
            // *not a meta reply* rather than as *equals EN*, so an unexpected status is
            // still reported rather than read as a hit with no flags.
            if (reply->status != "EN")
                return Concluded(Outcome::Protocol,
                                 std::format("inspect: expected `ME` or `EN`, and the server answered `{}`", reply->status));
            auto answer = Answered(ScalarValue(AbsentCell()), Outcome::Negative);
            answer.advisories.emplace_back(std::format("no such key: {}", context.operands[0]));
            return answer;
        }

        std::vector<Field> fields;
        fields.reserve(reply->metaFlags.size() + 1);
        fields.push_back(Field { .name = "key", .value = TextCell(reply->metaKey) });
        for (auto const& flag: reply->metaFlags)
            fields.push_back(Field { .name = std::string { McFlagName(flag.name) }, .value = TextCell(flag.value) });
        return Answered(RecordValue(std::move(fields)));
    }

    /// `mc-stats [<family>]`: one of the memcached `stats` families, as a table.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer TextStats(VerbContext const& context)
    {
        if (!context.operands.empty())
        {
            auto const named =
                std::ranges::any_of(McStatsSubs, [&](McStatsSub const& row) { return row.name == context.operands[0]; });
            if (!named)
            {
                // Listed from the table rather than written out, so a family added there
                // is offered AND named in the refusal by the same edit.
                std::string names;
                for (auto const& row: McStatsSubs)
                {
                    if (!names.empty())
                        names += ", ";
                    names += row.name;
                }
                return Concluded(Outcome::Usage,
                                 std::format("`{}` is not a stats family this client offers; the families are {}",
                                             context.operands[0],
                                             names));
            }
        }

        auto reply = AskMemcached(context, MemcachedCommandWithOperands(context));
        if (!reply.has_value())
            return std::move(reply.error());

        if (reply->kind != McLeadToken::Stat)
        {
            // A bare `END` is an EMPTY RESULT rather than a status, and `stats conns`
            // answers exactly that on this daemon. It renders as an empty table so
            // every format says *nothing here* in its own vocabulary -- `[]` in JSON, a
            // header row and no rows in the human one -- rather than saying nothing at
            // all, which reads as the command having failed.
            if (reply->status != "END")
                return FromMemcachedStatus(context, *reply);
            auto answer = Answered(TableValue(std::vector<std::string> { "name", "value" }, {}), Outcome::Negative);
            answer.advisories.emplace_back(std::format("the server reported no rows for `{}`",
                                                       context.operands.empty() ? "stats" : context.operands[0]));
            return answer;
        }

        std::vector<std::vector<Cell>> rows;
        rows.reserve(reply->stats.size());
        Answer answer;
        for (auto const& stat: reply->stats)
            rows.push_back(std::vector<Cell> { TextCell(stat.name), ValueCell(stat.value, answer) });
        answer.value = TableValue(std::vector<std::string> { "name", "value" }, std::move(rows));
        return answer;
    }

    /// `cache-memlimit <megabytes>`: change the cache's byte budget at runtime.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer MemoryLimit(VerbContext const& context)
    {
        if (auto refusal = RefuseUnsendableToken(context, context.operands.size()); refusal.has_value())
            return std::move(*refusal);
        auto reply = AskMemcached(context, MemcachedCommandWithOperands(context));
        if (!reply.has_value())
            return std::move(reply.error());

        auto answer = FromMemcachedStatus(context, *reply);
        if (answer.outcome == Outcome::Affirmative)
            answer.advisories.emplace_back(
                "the new limit is in force now and is NOT persisted: it lasts until this daemon restarts, "
                "which then reads --max-memory again");
        return answer;
    }

    /// What to call one reported surface.
    /// @param surface The wire tag.
    /// @return A stable lower-case name.
    [[nodiscard]] std::string_view NameOfSurface(CompileCacheWire::WireSurface surface) noexcept
    {
        switch (surface)
        {
            case CompileCacheWire::WireSurface::Admin:
                return "admin";
            case CompileCacheWire::WireSurface::Raft:
                return "raft";
            case CompileCacheWire::WireSurface::Discovery:
                return "discovery";
        }
        // A tag this build does not name never reaches here -- `DecodeNodeStatus` SKIPS
        // one rather than refusing the whole reply, which is what lets an older client
        // read a newer node at all. Closed anyway, because falling off the end of a
        // function returning a view is a dangling one.
        return "unknown";
    }

    /// What to call one enrollment-window state.
    /// @param state The wire tag.
    /// @return A stable lower-case name.
    [[nodiscard]] std::string_view NameOfEnrollmentState(CompileCacheWire::WireEnrollmentState state) noexcept
    {
        switch (state)
        {
            case CompileCacheWire::WireEnrollmentState::Closed:
                return "closed";
            case CompileCacheWire::WireEnrollmentState::Open:
                return "open";
        }
        // Unreachable for `NameOfSchedulerRole`'s reason: `DecodeNodeRuntime` leaves a
        // state this build has no name for disengaged rather than passing it through.
        return "unknown";
    }

    /// What to call one cordon state.
    /// @param state The wire tag.
    /// @return A stable lower-case name.
    [[nodiscard]] std::string_view NameOfCordonState(CompileCacheWire::WireCordonState state) noexcept
    {
        switch (state)
        {
            case CompileCacheWire::WireCordonState::Serving:
                return "serving";
            case CompileCacheWire::WireCordonState::Draining:
                return "draining";
            case CompileCacheWire::WireCordonState::Drained:
                return "drained";
        }
        // Unreachable for `NameOfEnrollmentState`'s reason.
        return "unknown";
    }

    /// Add an optional unsigned field, or nothing at all when the node did not say.
    ///
    /// **A helper rather than four copies of the same `if`.** Each of these is a fact a
    /// node may legitimately not have -- no worker tier, no scheduler, no heartbeat yet
    /// -- and *absent is not zero* has to hold for every one of them. Written once, a
    /// later field cannot be the one that renders a `0` for a fact nobody stated.
    /// @param record Where the field is appended.
    /// @param name What to call it.
    /// @param value The fact, or nothing.
    void AddOptionalNumber(std::vector<Field>& record, std::string_view name, std::optional<std::uint32_t> value)
    {
        if (value.has_value())
            record.push_back({ .name = std::string { name }, .value = NumberCell(static_cast<std::uint64_t>(*value)) });
    }

    /// Turn a decoded node status into the reported record.
    ///
    /// Pure, and separate from the handler, so the reported SHAPE is testable without a
    /// socket -- which is the half that matters: the field names an operator greps for
    /// are a contract, and a handler that builds them inline puts that contract
    /// somewhere only an end-to-end run can reach.
    /// @param fields What the node said.
    /// @return The record.
    [[nodiscard]] Value NodeStatusRecord(CompileCacheWire::NodeStatusFields const& fields)
    {
        std::vector<Field> record;
        record.push_back({ .name = "version", .value = TextCell(fields.version) });

        // **Absent, not empty.** A node running no consensus has no minted identity, and
        // an empty string renders as a value somebody could paste into `--raft-peer`.
        record.push_back({ .name = "node-id", .value = fields.nodeId.empty() ? AbsentCell() : TextCell(fields.nodeId) });
        record.push_back({ .name = "uptime-seconds", .value = NumberCell(fields.uptimeSeconds) });
        record.push_back({ .name = "components", .value = TextCell(DescribeComponents(fields.components)) });

        // **What the worker is DOING, which `components` cannot say.** That mask carries
        // a `worker` bit which is a constant on this binary -- it compiles, that is what
        // it is for -- so it reports a node that is still walking its include trees and
        // one that is serving compiles identically. These three separate them.
        //
        // Three cells rather than one sentence like `surveying (0 of 3)`, because this
        // record is also rendered as JSON and CSV: a consumer gets two numbers it can
        // compare, not a string it has to parse back. The counts stand or fall WITH the
        // state -- `0 of 0` under no state is the collapse the tri-state exists to end --
        // so a node that said nothing gets three absent fields rather than three zeroes.
        if (fields.runtime.toolchains.has_value())
        {
            record.push_back({ .name = "toolchains",
                               .value = TextCell(std::string { NameOfToolchainState(*fields.runtime.toolchains) }) });
            record.push_back({ .name = "toolchains-served",
                               .value = NumberCell(static_cast<std::uint64_t>(fields.runtime.toolchainsServed)) });
            record.push_back({ .name = "toolchains-discovered",
                               .value = NumberCell(static_cast<std::uint64_t>(fields.runtime.toolchainsDiscovered)) });
        }

        // What this node's worker is offering and spending. Both or neither: a slot
        // count with no in-flight figure invites the reading that the node is idle.
        AddOptionalNumber(record, "compile-slots", fields.runtime.compileSlots);
        AddOptionalNumber(record, "compiles-in-flight", fields.runtime.compilesInFlight);

        // Whether an operator cordoned this worker, and whether it has drained (#1303) --
        // the field somebody waiting to reboot this machine polls. Absent on a node that
        // runs no worker, for the enrollment state's reason below: `serving` would be a
        // reassuring answer about a worker that does not exist.
        if (fields.runtime.cordon.has_value())
            record.push_back(
                { .name = "cordon", .value = TextCell(std::string { NameOfCordonState(*fields.runtime.cordon) }) });

        // Whether this node is getting through to a scheduler. `2 of 3` is ordinary
        // mid-survey and alarming an hour later, which is why both numbers are reported
        // rather than a ratio or a bare bool.
        AddOptionalNumber(record, "registrars-registered", fields.runtime.registrarsRegistered);
        AddOptionalNumber(record, "registrars-total", fields.runtime.registrarsTotal);

        // **Absent means NEVER, and that is not a long time ago.** A node whose
        // `--scheduler` has never answered and one that registered an hour ago are the
        // two states an operator is separating; rendering a `0` or a `-1` for the first
        // reports the healthy answer for both.
        if (fields.runtime.lastRegistrationSecondsAgo.has_value())
            record.push_back({ .name = "last-registration-seconds-ago",
                               .value = NumberCell(*fields.runtime.lastRegistrationSecondsAgo) });

        // A leading scheduler and a following one both report `scheduler` in the
        // component mask, and a follower's registry is empty and reads exactly like an
        // idle fleet. A node running no scheduler reports no role at all, which is what
        // keeps `undecided` meaning *an election is in progress*.
        if (fields.runtime.schedulerRole.has_value())
        {
            record.push_back({ .name = "scheduler-role",
                               .value = TextCell(std::string { NameOfSchedulerRole(*fields.runtime.schedulerRole) }) });
            // Empty is a READING here -- no leader is known -- so the field is present
            // and ABSENT rather than missing, which would say this node could not tell.
            record.push_back(
                { .name = "leader",
                  .value = fields.runtime.leaderEndpoint.empty() ? AbsentCell() : TextCell(fields.runtime.leaderEndpoint) });
        }

        // **The window an operator needs to know they left open.** The node encodes
        // these two and the wire carries them; until now nothing rendered either, so
        // the one place an operator is told to look reported nothing at all -- filled,
        // carried and drawn nowhere, which is the wiring this project asserts rather
        // than assumes.
        //
        // It is also what the counters cannot say. Both enrollment series are rendered
        // by every node and read zero on a machine that has no window, because a
        // counter is a tally and zero is the truth about events that never happened --
        // so *no window here* and *a window that nothing has come through* are one
        // number, and this field is where that distinction is kept.
        //
        // ABSENT is the whole point of the shape: a node running no consensus says
        // nothing, exactly as the toolchain trio does, rather than reporting a
        // reassuring `closed` for a window that does not exist. Both cells or neither,
        // since a state with no count invites the reading that nobody is waiting.
        if (fields.runtime.enrollment.has_value())
        {
            record.push_back({ .name = "enrollment",
                               .value = TextCell(std::string { NameOfEnrollmentState(*fields.runtime.enrollment) }) });
            AddOptionalNumber(record, "enrollment-pending", fields.runtime.enrollmentPending);
        }

        // **Where peers DIAL this node's consensus port** (#1328) -- the half of
        // `--cluster-admit`'s receipt an operator holds against the machine being brought
        // in. Named as the receipt names it, and deliberately not a `*-port` field below:
        // those are what the node BOUND, and a consensus bind is routinely the wildcard,
        // which is the address that comparison must not be made against. Absent on a node
        // running no consensus, for the enrollment state's reason.
        if (fields.runtime.consensusEndpoint.has_value())
            record.push_back({ .name = std::string { CompileCacheWire::ConsensusEndpointField },
                               .value = TextCell(*fields.runtime.consensusEndpoint) });

        // How many `--cluster-forget-client` tombstones this node is ENFORCING (#1471), which
        // is the field an operator reads after issuing one. Through `AddOptionalNumber`, so
        // absent renders no row at all rather than a `0`: a node running no consensus has no
        // committed set for a forget to have reached, and a zero there answers a different
        // question than "the cluster forgets nobody".
        //
        // It is also how a forget that has not PROPAGATED is visible. A node behind on the log
        // reports a lower number than the leader, and both report one -- so an operator
        // comparing two machines sees a difference rather than two plausible zeroes, which is
        // the reading the counters cannot give them.
        AddOptionalNumber(record, "forgotten-clients", fields.runtime.forgottenClients);

        // One field per surface the node actually opened. A surface it does not run gets
        // no field at all rather than a zero port -- the same rule the node applies when
        // encoding, held on both sides so neither can quietly invent a number.
        for (auto const& surface: fields.surfaces)
            record.push_back({ .name = std::format("{}-port", NameOfSurface(surface.surface)),
                               .value = NumberCell(static_cast<std::uint64_t>(surface.port)) });

        // The admin surface's scheme, and only when there IS an admin surface: a client
        // that guessed `http://` against a TLS one fails in a way that reads as the
        // surface being down.
        auto const admin = std::ranges::find(
            fields.surfaces, CompileCacheWire::WireSurface::Admin, &CompileCacheWire::SurfaceReport::surface);
        if (admin != fields.surfaces.end())
            record.push_back({ .name = "admin-tls", .value = BooleanCell(admin->tls) });

        return RecordValue(std::move(record));
    }

    /// Ask the node one fieldless verb and hand back its reply.
    ///
    /// One door for both node verbs, so the refusal wording and the outcome mapping
    /// cannot be spelled two ways -- which is how three surfaces in this tree came to
    /// disagree about one refusal.
    /// @param context What to run against.
    /// @param request The framed request.
    /// @return The reply, or the answer to give instead.
    [[nodiscard]] std::expected<NodeReply, Answer> AskNode(VerbContext const& context, std::span<std::byte const> request)
    {
        auto reply = context.node->Send(request);
        if (!reply.has_value())
            return std::unexpected(FromExchangeError(reply.error()));

        if (reply->status != CompileCacheWire::Status::Ok)
            return std::unexpected(
                Concluded(Outcome::Refused, ExplainRefusal(context.verb->name, context.node->Address(), *reply)));

        return std::move(*reply);
    }

    /// `node` -- what this endpoint IS.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer NodeStatus(VerbContext const& context)
    {
        auto const reply = AskNode(context, CompileCacheWire::EncodeNodeStatusRequest());
        if (!reply.has_value())
            return reply.error();

        auto const fields = CompileCacheWire::DecodeNodeStatus(reply->payload);
        if (!fields.has_value())
            return Concluded(
                Outcome::Protocol,
                std::format("{} answered node-status with a body this client cannot read", context.node->Address()));

        return Answered(NodeStatusRecord(*fields));
    }

    /// What to call one admission verdict.
    ///
    /// The word an operator would use, not the enumerator: `refused` rather than `Outsider`,
    /// which reads as a status this tool invented.
    /// @param verdict The wire tag.
    /// @return A stable lower-case name.
    [[nodiscard]] std::string_view NameOfMembership(CompileCacheWire::WireMembership verdict) noexcept
    {
        switch (verdict)
        {
            case CompileCacheWire::WireMembership::Outsider:
                return "refused";
            case CompileCacheWire::WireMembership::Member:
                return "admitted";
            case CompileCacheWire::WireMembership::Forgotten:
                return "forgotten";
        }
        // Unreachable for `NameOfCordonState`'s reason: `DecodeAdmissionExplanation` REFUSES a
        // verdict this build cannot name rather than passing it through, because a verdict is
        // the answer itself -- an unknown route can be counted, an unknown verdict cannot.
        return "unknown";
    }

    /// One row of `AdmissionRouteNames`.
    struct AdmissionRouteName
    {
        Distributed::MembershipParticipant route; ///< The route.
        std::string_view name;                    ///< What an operator calls it.
    };

    /// How each admission route is spelled for a person.
    ///
    /// **The flag or the act, never the enumerator.** An operator told `--fleet-member` knows
    /// which line to edit; one told `FleetMemberList` has to go and look it up.
    ///
    /// Keyed on the PARTICIPANT rather than on the wire bit, and that is the guard rather than a
    /// preference: a bitmask enum has no `Last`, so a table keyed on one cannot be checked for
    /// completeness. Keyed here, `RowsInEnumeratorOrder` fails the build when a route is added
    /// and not spelled -- where a bit-keyed array would compile and render the new route as *a
    /// route this client cannot name*, blaming the node's version for this client's omission.
    /// That is the confident wrong signal the verb exists to remove, arriving inside the verb.
    constexpr EnumTable<Distributed::MembershipParticipant, AdmissionRouteName> AdmissionRouteNames { {
        { .route = Distributed::MembershipParticipant::FleetMemberList, .name = "--fleet-member" },
        { .route = Distributed::MembershipParticipant::ClusterMembers, .name = "the cluster's member set" },
        { .route = Distributed::MembershipParticipant::ClientTombstone, .name = "--cluster-forget-client" },
        { .route = Distributed::MembershipParticipant::OpenPolicy, .name = "--fleet-open" },
        { .route = Distributed::MembershipParticipant::ProvenKeyHolder, .name = "proved the cluster key" },
    } };

    static_assert(RowsInEnumeratorOrder(AdmissionRouteNames, &AdmissionRouteName::route),
                  "AdmissionRouteNames must hold one row per MembershipParticipant, in enumerator order");

    /// Every route in @p decidedBy, as an operator reads them.
    ///
    /// **A route this build cannot name is COUNTED, never dropped.** The node may be newer, and
    /// under-reporting authorship during an upgrade would say *fewer things decided this than
    /// did* -- which is the reading that sends somebody to change a route that was never
    /// consulted. Counted, the operator knows to ask a client of the node's own version.
    /// @param decidedBy The bitmask the node sent.
    /// @return The routes, joined, or empty when nothing decided.
    [[nodiscard]] std::string DescribeAdmissionRoutes(std::uint32_t decidedBy)
    {
        auto described = std::string {};
        auto named = std::uint32_t { 0 };
        for (auto const& row: Distributed::MembershipWireRoutes)
        {
            auto const bit = row.bit;
            if ((decidedBy & bit) == 0)
                continue;
            named |= bit;
            if (!described.empty())
                described += ", ";
            described += AdmissionRouteNames[static_cast<std::size_t>(row.route)].name;
        }

        if (auto const unnamed = decidedBy & ~named; unnamed != 0)
        {
            if (!described.empty())
                described += ", ";
            described += std::format("{} route(s) this client is too old to name", std::popcount(unnamed));
        }
        return described;
    }

    /// `explain-admission <host>` -- which routes admit or refuse that host, as THIS node folds it.
    ///
    /// **Every route that decided, not just the winner**
    /// ([#1471](https://github.com/LASTRADA-Software/fastcached/issues/1471)). An operator who
    /// drops a host from `--fleet-member` and finds it still served needs to know the cluster
    /// admits it too; naming only the winning route answers a question they did not ask, and
    /// sends them to edit a file that will change nothing.
    ///
    /// The verdict does NOT reach the exit code. `refused` is a successful answer to *why is this
    /// host refused*, and mapping it to a non-zero status would fail a script that was only
    /// asking -- while collapsing `refused` and `forgotten` onto one number, which is the
    /// distinction the whole verb exists to carry.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer ExplainAdmission(VerbContext const& context)
    {
        auto const reply = AskNode(context, CompileCacheWire::EncodeExplainAdmissionRequest(context.operands[0]));
        if (!reply.has_value())
            return reply.error();

        auto const fields = CompileCacheWire::DecodeAdmissionExplanation(reply->payload);
        if (!fields.has_value())
            return Concluded(
                Outcome::Protocol,
                std::format("{} answered explain-admission with a body this client cannot read", context.node->Address()));

        auto routes = DescribeAdmissionRoutes(fields->decidedBy);
        auto answer = Answered(RecordValue(
            { Field { .name = "host", .value = TextCell(std::string { context.operands[0] }) },
              Field { .name = "verdict", .value = TextCell(std::string { NameOfMembership(fields->verdict) }) },
              // An empty set is a READING -- no route had an opinion -- so the field is present
              // and ABSENT, exactly as `leader` is when no leader is known. Dropping the field
              // would say this node cannot answer that half; a `0` or an empty string would read
              // as a value nobody wrote.
              Field { .name = "decided-by", .value = routes.empty() ? AbsentCell() : TextCell(std::move(routes)) } }));

        if (fields->verdict == CompileCacheWire::WireMembership::Forgotten)
            answer.advisories.emplace_back(
                "a forgotten client outranks every admission route, so listing this host in `--fleet-member` will "
                "not bring it back; `--cluster-admit-client` on the node binary clears the tombstone");
        return answer;
    }

    /// `cordon` or `uncordon`: take this machine's worker out of the fleet, or put it back.
    ///
    /// **Answered with the state the worker is in NOW**, and the running count beside it,
    /// because the next thing an operator does is wait: `draining` with a count says what
    /// is being waited for, and `drained` says a stop abandons nothing. Asking twice answers
    /// the same way, so a script is never told it failed for repeating itself.
    /// @param context What to run against.
    /// @param action Cordon, or lift it.
    /// @return The answer.
    [[nodiscard]] Answer AskCordon(VerbContext const& context, CompileCacheWire::CordonAction action)
    {
        auto const reply = AskNode(context, CompileCacheWire::EncodeCordonRequest(action));
        if (!reply.has_value())
            return reply.error();

        auto const fields = CompileCacheWire::DecodeCordonFields(reply->payload);
        if (!fields.has_value())
            return Concluded(Outcome::Protocol,
                             std::format("{} answered {} with a body this client cannot read",
                                         context.node->Address(),
                                         context.verb->name));

        auto answer = Answered(RecordValue(
            { Field { .name = "cordon", .value = TextCell(std::string { NameOfCordonState(fields->state) }) },
              Field { .name = "compiles-in-flight", .value = NumberCell(static_cast<std::uint64_t>(fields->inFlight)) } }));
        if (fields->state == CompileCacheWire::WireCordonState::Draining)
            answer.advisories.emplace_back(
                std::format("{} compile(s) still running; `node` reports `cordon drained` once they have been delivered",
                            fields->inFlight));
        if (action == CompileCacheWire::CordonAction::Cordon)
            answer.advisories.emplace_back(
                "the cordon is NOT persisted: it lasts until `uncordon`, or until this node restarts and takes work again");
        return answer;
    }

    /// `cordon` -- refuse new compiles here and let the running ones finish.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer Cordon(VerbContext const& context)
    {
        return AskCordon(context, CompileCacheWire::CordonAction::Cordon);
    }

    /// `uncordon` -- take compiles here again.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer Uncordon(VerbContext const& context)
    {
        return AskCordon(context, CompileCacheWire::CordonAction::Lift);
    }

    /// `version`, answered by a node instead of by RESP `INFO`.
    ///
    /// **This is the verb the whole fallback exists for.** `fastcache-compile-node`
    /// speaks no RESP, so `version` reported *the server closed the connection without
    /// answering* against the one binary an operator most often points this tool at --
    /// true, and useless. A node knows its own version and will say so over `0xFC`.
    ///
    /// The client half is reported either way, because the interesting question is never
    /// one of the numbers: it is whether they AGREE. A stale binary talking to an
    /// upgraded node is exactly the shape that has cost this project a served-but-wrong
    /// object before, and an operator cannot see it from one number.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer VersionsFromNode(VerbContext const& context)
    {
        auto fields = std::vector<Field> {
            Field { .name = "client", .value = TextCell(std::string { ClientVersion }) },
        };

        auto const reply = AskNode(context, CompileCacheWire::EncodeNodeStatusRequest());
        if (!reply.has_value())
        {
            fields.push_back(Field { .name = "server", .value = AbsentCell() });
            auto answer = Answered(RecordValue(std::move(fields)), reply.error().outcome);
            answer.advisories = reply.error().advisories;
            return answer;
        }

        auto const status = CompileCacheWire::DecodeNodeStatus(reply->payload);
        if (!status.has_value())
        {
            fields.push_back(Field { .name = "server", .value = AbsentCell() });
            auto answer = Answered(RecordValue(std::move(fields)), Outcome::Protocol);
            answer.advisories.push_back(
                std::format("{} answered node-status with a body this client cannot read", context.node->Address()));
            return answer;
        }

        fields.push_back(Field { .name = "server", .value = TextCell(status->version) });
        // What KIND of server, so the two numbers are not read as two builds of the same
        // binary. A node and a daemon version the same way and are different programs.
        fields.push_back(Field { .name = "server_kind", .value = TextCell("fastcache-compile-node") });
        return Answered(RecordValue(std::move(fields)));
    }

    /// `del`, answered by a node's cache tier instead of by RESP `DEL` (#1276).
    ///
    /// One `cache-drop` per key, because the verb names one key, and the SAME contract `del`
    /// has over RESP: the answer is how many were removed, and none is the answer *no* -- exit
    /// 1, which is an answer and not an error, so a repair run twice reads as done twice.
    ///
    /// A refusal stops the loop at the key it met and says how many were removed before it.
    /// Carrying on would send every remaining key into the same refusal -- a node that refuses
    /// one caller for locality refuses it every key -- and would bury the one sentence that
    /// explains why under a count.
    ///
    /// **The upstream advisory is unconditional, and phrased as a condition.** A node's tier
    /// reads through to the shared cache it was given, and a drop reaches this tier only, so a
    /// key the shared cache still holds comes back on the next fetch. Whether this node HAS
    /// one is not on the wire; saying *if* costs a line and saying nothing costs an operator a
    /// repair that silently undid itself.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer DropFromNode(VerbContext const& context)
    {
        constexpr std::string_view UpstreamAdvisory =
            "this removed the keys from the node's own cache tier only; if the node reads through to a shared "
            "cache (--upstream), that cache still holds them and refills this tier on the next fetch, so run "
            "`del` against it as well";

        std::int64_t removed = 0;
        for (auto const& key: context.operands)
        {
            auto const reply = context.node->Send(CompileCacheWire::EncodeCacheDrop(key));
            auto refusal = std::optional<Answer> {};
            if (!reply.has_value())
                refusal = FromExchangeError(reply.error());
            else if (reply->status == CompileCacheWire::Status::Ok)
                ++removed;
            else if (reply->status != CompileCacheWire::Status::Miss)
                refusal = Concluded(Outcome::Refused, ExplainRefusal(context.verb->name, context.node->Address(), *reply));

            if (refusal.has_value())
            {
                refusal->advisories.push_back(
                    std::format("{} of {} key(s) were removed before `{}`", removed, context.operands.size(), key));
                return std::move(*refusal);
            }
        }

        auto answer = Answered(ScalarValue(NumberCell(removed)));
        if (removed == 0)
        {
            answer.outcome = Outcome::Negative;
            answer.advisories.emplace_back("no key matched");
        }
        answer.advisories.emplace_back(UpstreamAdvisory);
        return answer;
    }

    /// `node-metrics` -- every figure the node's `/metrics` renders, over `0xFC` (#1406).
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer NodeMetrics(VerbContext const& context)
    {
        auto const reply = AskNode(context, CompileCacheWire::EncodeNodeMetricsRequest());
        if (!reply.has_value())
            return reply.error();

        auto record = DecodeNodeMetrics(reply->payload);
        if (!record.has_value())
            return Concluded(Outcome::Protocol,
                             std::format("{} answered node-metrics with a reading that {}",
                                         context.node->Address(),
                                         DescribeReadingFault(record.error())));

        return Answered(*std::move(record));
    }

    // -----------------------------------------------------------------------------
    // The cluster verbs.
    //
    // Four verbs on the wire (`ClusterStatus`, `ClusterSet`, `ClusterForget`,
    // `ClusterAdmit`), served on the node's ONE `0xFC` port since #290 -- so these need
    // no new flag, no new address and no new transport. The working client that existed
    // before them lives inside `fastcache-compile-node`, which is the problem: the
    // natural way to run cluster admin on machines 2..40 is from the machine being
    // provisioned, and `RunClusterAdmin` needs `--scheduler`, which is ALSO a startup
    // flag -- so getting it into the unit file once points that node at one scheduler
    // forever, because a registration replays its command line. A `--fleet-member`
    // client (a laptop, a CI runner) has no node binary at all and could never ask.
    //
    // **Lifted, not duplicated, and the lift is smaller than it looks.** The encoders
    // are already `CompileCacheWire`'s and the decoder is already `Cluster::`, both of
    // them the single source of truth; the only rule that had two authors was reading a
    // `NotLeader`, which is `LeaderRedirectTarget` now and is asked in `ExplainRefusal`
    // so every node verb gets one spelling. `RenderClusterState` is deliberately NOT
    // lifted: it is a fixed plain-text report for a person, and this tool's entire
    // reason to exist is `--format=json|tsv|csv` and a cell that can say ABSENT. Two
    // presentations of one wire is not duplication; two encodings would be.

    /// The agreed members, as a table.
    ///
    /// Pure and separate from the handler so the reported SHAPE -- the column names an
    /// operator greps for, which are a contract -- is testable without a socket.
    /// @param state What the cluster has agreed.
    /// @return The table.
    [[nodiscard]] Value ClusterMembersTable(Cluster::ClusterState const& state)
    {
        std::vector<std::vector<Cell>> rows;
        rows.reserve(state.members.size());
        for (auto const& member: state.members)
            // **Absent, not empty.** An empty `schedulerEndpoint` is the ORDINARY state
            // of every member that has never led -- a leader announces its own record on
            // election -- so it is a member that has not said, never a member reachable
            // at the empty string. A blank cell would read as a rendering fault, and a
            // zero would be a claim.
            //
            // And `scheduler-state` says which absence (#1340): never announced, or
            // cleared by a re-admit. A column of its own rather than a word in the
            // endpoint cell, so `scheduler` stays an address or ABSENT in every format.
            rows.push_back({ TextCell(member.id),
                             TextCell(member.raftEndpoint),
                             member.schedulerEndpoint.empty() ? AbsentCell() : TextCell(member.schedulerEndpoint),
                             TextCell(std::string { Cluster::SchedulerEndpointStateName(member) }) });

        return TableValue({ "id", "raft", "scheduler", "scheduler-state" }, std::move(rows));
    }

    /// Every setting this build knows, with what the cluster has agreed for it.
    ///
    /// **Both questions in one table**, because an operator's real question is usually
    /// *what CAN I set* and a report listing only what somebody had already set answers
    /// it wrongly by omission -- which is the reasoning `RenderClusterState` records for
    /// printing its own `known settings:` section.
    ///
    /// A setting the cluster has agreed and this build does not KNOW still gets a row,
    /// with no summary. That is the state a fleet mid-upgrade is in -- a leader running
    /// a newer build -- and dropping the row would hide a live fact because the reader
    /// is the older binary.
    /// @param state What the cluster has agreed.
    /// @return The table.
    [[nodiscard]] Value ClusterSettingsTable(Cluster::ClusterState const& state)
    {
        auto const agreed = [&state](std::string_view name) -> Cell {
            auto const at = std::ranges::find(state.settings, name, &Cluster::Setting::name);
            // Absent means *the cluster has agreed nothing for this*, which is a
            // different fact from a value that happens to be empty.
            return at == state.settings.end() ? AbsentCell() : TextCell(at->value);
        };

        std::vector<std::vector<Cell>> rows;
        // An upper bound, and an exact one whenever the leader agrees nothing this build
        // does not know -- which is every fleet that is not mid-upgrade.
        rows.reserve(Cluster::SettingTable.size() + state.settings.size());
        for (auto const& row: Cluster::SettingTable)
            rows.push_back({ TextCell(std::string { row.name }), agreed(row.name), TextCell(std::string { row.summary }) });

        for (auto const& setting: state.settings)
        {
            if (std::ranges::any_of(Cluster::SettingTable, [&setting](auto const& row) { return row.name == setting.name; }))
                continue;
            rows.push_back({ TextCell(setting.name), TextCell(setting.value), AbsentCell() });
        }

        return TableValue({ "name", "value", "summary" }, std::move(rows));
    }

    /// Ask the endpoint for the agreed cluster state.
    ///
    /// One door for both read verbs, so a refusal is worded once and the *format this
    /// build cannot read* refusal cannot be spelled two ways.
    /// @param context What to run against.
    /// @return The state, or the answer to give instead.
    [[nodiscard]] std::expected<Cluster::ClusterState, Answer> AskClusterState(VerbContext const& context)
    {
        auto const reply = AskNode(context, CompileCacheWire::EncodeClusterStatus());
        if (!reply.has_value())
            return std::unexpected(reply.error());

        auto state = Cluster::DecodeState(reply->payload);
        if (!state.has_value())
            // A leader running a build whose state format this one does not know.
            // REFUSED rather than rendered as an empty cluster: a partial read looks
            // exactly like a fleet that admits nobody, and that would be read as a fact.
            // The decoder's reason rides along, so another build's encoding is named by
            // its version rather than reading as damage.
            return std::unexpected(
                Concluded(Outcome::Protocol,
                          std::format("{} answered cluster-status with a body this client cannot read: {}",
                                      context.node->Address(),
                                      state.error().context)));

        return std::move(*state);
    }

    /// `cluster-members` -- who the cluster has agreed is in it.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer ClusterMembers(VerbContext const& context)
    {
        auto const state = AskClusterState(context);
        if (!state.has_value())
            return state.error();
        return Answered(ClusterMembersTable(*state));
    }

    /// `cluster-settings` -- what every member must agree on.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer ClusterSettings(VerbContext const& context)
    {
        auto const state = AskClusterState(context);
        if (!state.has_value())
            return state.error();
        return Answered(ClusterSettingsTable(*state));
    }

    /// What to report for a cluster change the leader accepted.
    ///
    /// **Appended, not committed, and the wording says so.** The leader cannot know the
    /// difference until a majority answers, and a tool claiming otherwise is the one
    /// thing a report like this must not do.
    /// @return The record.
    [[nodiscard]] Value ClusterChangeAccepted()
    {
        return RecordValue({ Field { .name = "accepted", .value = BooleanCell(true) },
                             Field { .name = "state", .value = TextCell("replicating") } });
    }

    /// `cluster-set <name> <value>` -- change a replicated setting.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer ClusterSet(VerbContext const& context)
    {
        auto const reply = AskNode(context,
                                   CompileCacheWire::EncodeClusterSet(CompileCacheWire::ClusterSetRequest {
                                       .name = context.operands[0], .value = context.operands[1] }));
        if (!reply.has_value())
            return reply.error();
        return Answered(ClusterChangeAccepted());
    }

    /// `cluster-forget <id>` -- remove a member.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer ClusterForget(VerbContext const& context)
    {
        auto const reply = AskNode(context, CompileCacheWire::EncodeClusterForget(context.operands[0]));
        if (!reply.has_value())
            return reply.error();
        return Answered(ClusterChangeAccepted());
    }

    /// `cluster-admit <id> <raft-endpoint>` -- add a member, or move one.
    ///
    /// **The one cluster verb that answers with more than *accepted*, and the
    /// difference is the whole of
    /// [#1296](https://github.com/LASTRADA-Software/fastcached/issues/1296).** The
    /// address typed here and the one the joining node answers consensus on --
    /// `--raft-self` with `--listen-raft` -- are two spellings of one address, and
    /// nothing compared them. When they disagree the member is in the committed
    /// configuration and contacts nobody, which at three members or more presents as an
    /// election storm that settles: the symptom points at consensus rather than at the
    /// character that was mistyped.
    ///
    /// So the leader's half goes on the screen. What it RECORDED it knows the instant it
    /// builds the command, with no majority involved; whether a majority has taken it it
    /// cannot know at all. The field names carry that distinction rather than a comment
    /// doing it, because a field name is what an operator reads: **recorded**, **as
    /// received**, and a state that says **appended, not committed** -- which is
    /// `SchedulerService::Offer`'s own phrase and is deliberately not reworded here.
    /// Never *admitted*, *added* or *in force*; an echo that reads as in-force when it
    /// is merely taken-down is a confident wrong signal, and this codebase rates that
    /// worse than a vague right one.
    ///
    /// `ClusterChangeAccepted` is untouched and still answers the other two verbs: the
    /// hazard is `AddMember`'s alone, since it is the only command carrying an ADDRESS.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer ClusterAdmit(VerbContext const& context)
    {
        auto const reply = AskNode(context,
                                   CompileCacheWire::EncodeClusterAdmit(CompileCacheWire::ClusterAdmitRequest {
                                       .memberId = context.operands[0], .raftEndpoint = context.operands[1] }));
        if (!reply.has_value())
            return reply.error();

        auto const receipt = CompileCacheWire::DecodeClusterAdmitReceipt(reply->payload);
        if (!receipt.has_value())
            // REFUSED rather than rendered with blank cells, which is the missing string
            // this change exists to prevent arriving through the renderer: an operator
            // comparing two empty columns finds them equal. Deliberately not reported as
            // *an older node* either -- `MinSupportedVersion` equals `CurrentVersion`, so
            // one speaking another version is refused by name at the header and never
            // reaches here.
            return Concluded(
                Outcome::Protocol,
                std::format("{} answered cluster-admit with a receipt this client cannot read", context.node->Address()));

        return Answered(
            RecordValue({ Field { .name = "recorded", .value = BooleanCell(true) },
                          Field { .name = "member-id-as-received", .value = TextCell(receipt->memberId) },
                          Field { .name = std::format("{}-as-recorded", CompileCacheWire::ConsensusEndpointField),
                                  .value = TextCell(receipt->raftEndpoint) },
                          Field { .name = "state", .value = TextCell("appended, not committed") } }));
    }

    /// A reply from the node that leads, and every node asked on the way to it.
    struct LeaderReply
    {
        NodeReply reply;                ///< The leader's `Ok`.
        std::vector<std::string> asked; ///< Where the request went, in order; the last one answered.
    };

    /// How a refusal a fleet read does not follow is told.
    /// @param context What was run.
    /// @param endpoint Who refused.
    /// @param reply The refusal.
    /// @return The answer.
    [[nodiscard]] Answer FleetRefusal(VerbContext const& context, std::string_view endpoint, NodeReply const& reply)
    {
        auto const code = reply.code.value_or(CompileCacheWire::ErrorCode::MalformedFrame);

        // The leader's own words, as they are: they list every key this build serves, one per
        // line, which a parenthesised suffix would mangle.
        if (code == CompileCacheWire::ErrorCode::UnknownFleetSelector)
        {
            auto const said = std::string_view { reply.detail };
            // `npos + 1` is zero, so a detail of nothing but newlines trims to nothing.
            auto const words = said.substr(0, said.find_last_not_of('\n') + 1);
            return Concluded(Outcome::Refused, std::format("{} refused `{}`: {}", endpoint, context.verb->name, words));
        }

        auto explained = ExplainRefusal(context.verb->name, endpoint, reply);
        if (code == CompileCacheWire::ErrorCode::Unauthenticated)
            explained += DashboardCredentialRemedy(!context.dashboardToken.empty());
        return Concluded(Outcome::Refused, std::move(explained));
    }

    /// Send @p request, following `NotLeader` to the node that leads.
    ///
    /// **`DecideLeaderHop`, the rule a fleet subscription follows too**: a refusal naming an address
    /// is an instruction, at most `MaxLeaderRedirects` times; anything else is relayed. The bound is
    /// what stops two nodes each naming the other stale leader from bouncing the request forever.
    /// @param context What to run against; `node` is the first node asked.
    /// @param request The framed request.
    /// @return The leader's reply with the nodes asked, or the answer to give instead.
    [[nodiscard]] std::expected<LeaderReply, Answer> AskLeader(VerbContext const& context,
                                                               std::span<std::byte const> request)
    {
        std::unique_ptr<INodeExchange> dialled;
        auto* at = context.node;
        std::vector<std::string> asked;
        for (auto const hopsTaken: std::views::iota(0, MaxLeaderRedirects + 1))
        {
            asked.emplace_back(at->Address());
            auto reply = at->Send(request);
            if (!reply.has_value())
                return std::unexpected(FromExchangeError(reply.error()));
            if (reply->status == CompileCacheWire::Status::Ok)
                return LeaderReply { .reply = *std::move(reply), .asked = std::move(asked) };

            auto const hop =
                DecideLeaderHop(reply->code.value_or(CompileCacheWire::ErrorCode::MalformedFrame), reply->detail, hopsTaken);
            if (hop.kind == LeaderHopKind::NotARedirect || context.dial == nullptr)
                return std::unexpected(FleetRefusal(context, at->Address(), *reply));
            if (hop.kind == LeaderHopKind::Exhausted)
                break;

            auto next = context.dial->Dial(hop.next);
            if (!next.has_value())
                return std::unexpected(FromExchangeError(next.error()));
            dialled = *std::move(next);
            at = dialled.get();
        }

        // Past the bound: every node asked, in order, so an operator sees which name each other.
        std::string path;
        for (auto const& each: asked)
            path += path.empty() ? each : std::format(" -> {}", each);
        return std::unexpected(Concluded(
            Outcome::Unreachable,
            std::format(
                "followed {} leader redirects without an answer ({}); none of them leads", MaxLeaderRedirects, path)));
    }

    /// `fleet <section>` -- one of the leader's fleet tables, in a terminal.
    ///
    /// One `fleet-text` request over the `0xFC` connection every node verb uses (#1391): the leader
    /// renders the table with the function `/fleet.txt` answers from, so a node needs no admin
    /// surface for this, and a follower's `NotLeader` is followed to the leader it names.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer Fleet(VerbContext const& context)
    {
        auto const request = CompileCacheWire::EncodeFleetTextRequest(
            CompileCacheWire::FleetTextRequest { .section = context.operands[0],
                                                 .range = context.options.range.value_or(std::string {}),
                                                 .dashboardToken = std::string { context.dashboardToken } });
        auto read = AskLeader(context, request);
        if (!read.has_value())
            return std::move(read.error());

        auto table = FleetTable(CompileCacheWire::AsStringView(read->reply.payload));
        if (!table.has_value())
            return Concluded(Outcome::Protocol, std::format("the fleet table could not be read: {}", table.error()));

        auto answer = Answered(*std::move(table));

        // Which section's column tables scale these columns, said by the verb that fetched them
        // (#1488). Before this the human table printed the leader's raw integers, so
        // `heartbeat-age` read `14223` -- which under that column name says a worker was last
        // heard from almost four hours ago when the truth was fourteen seconds, and `/fleet`
        // rendered the same cell `14 s`. Wrong, and wrong toward a false alarm.
        //
        // From the OPERAND rather than from the document's marker: the operand is what this
        // client asked for, while a marker is text a leader sent, and a leader naming a section
        // this build does not know must still render its table -- raw, since an unknown scale is
        // not a reason to refuse. `FleetSectionFromKey` answering nothing is exactly that case.
        answer.columnScales = Distributed::FleetSectionFromKey(context.operands[0]);

        // On stderr: the table on stdout is the same whoever answered it, and a script reading it
        // must not have to skip a line -- while an operator who named a follower still learns
        // where the leader is.
        if (read->asked.size() > 1)
            answer.advisories.push_back(std::format(
                "{} does not lead the fleet; the table is from the leader at {}", read->asked.front(), read->asked.back()));
        return answer;
    }

    /// The verbs, in the order `--help` documents them.
    constexpr auto VerbTable = std::to_array<VerbSpec>({
        { .name = "get",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "read one value; a miss exits 1",
          .protocolCommand = "GET",
          .modifiers = Modifier::Raw,
          .handler = &Fetch,
          // No `0xFC` equivalent: a compile node holds no user keyspace. Spelled out
          // rather than left to default -- clang and gcc reject the omission under
          // this project's pedantic flags and MSVC does not say a word, which is a
          // shape that builds clean on Windows and fails four CI legs.
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "mget",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = VariadicOperands,
          .operands = " <key>...",
          .summary = "read several values as a table",
          .protocolCommand = "MGET",
          .modifiers = Modifier::None,
          .handler = &FetchMany,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "set",
          .wire = Wire::Resp,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <value>",
          .summary = "store a value; see --ttl, --nx, --xx",
          .protocolCommand = "SET",
          .modifiers = Modifier::Ttl | Modifier::Exclusivity,
          .handler = &Store,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "del",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = VariadicOperands,
          .operands = " <key>...",
          .summary = "delete keys; reports how many existed",
          .protocolCommand = "DEL",
          .modifiers = Modifier::None,
          .handler = &Counting,
          // A compile node has no RESP, and its cache tier is where a wrong object an operator
          // has to remove actually lives (#1276): one `cache-drop` per key, same count contract.
          .nodeFallback = &DropFromNode,
          .session = nullptr },
        { .name = "exists",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = VariadicOperands,
          .operands = " <key>...",
          .summary = "count how many of the named keys exist",
          .protocolCommand = "EXISTS",
          .modifiers = Modifier::None,
          .handler = &Counting,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "incr",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "add one and report the result",
          .protocolCommand = "INCR",
          .modifiers = Modifier::None,
          .handler = &Counting,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "decr",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "subtract one and report the result",
          .protocolCommand = "DECR",
          .modifiers = Modifier::None,
          .handler = &Counting,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "incrby",
          .wire = Wire::Resp,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <delta>",
          .summary = "add <delta> and report the result",
          .protocolCommand = "INCRBY",
          .modifiers = Modifier::None,
          .handler = &Counting,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "decrby",
          .wire = Wire::Resp,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <delta>",
          .summary = "subtract <delta> and report the result",
          .protocolCommand = "DECRBY",
          .modifiers = Modifier::None,
          .handler = &Counting,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "ttl",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "how long a key has left; names `exists` separately, because\n"
                     "a key that is gone and one with no expiry are not the same",
          .protocolCommand = "TTL",
          .modifiers = Modifier::None,
          .handler = &Lifetime,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "expire",
          .wire = Wire::Resp,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <seconds>",
          .summary = "set a key's expiry",
          .protocolCommand = "EXPIRE",
          .modifiers = Modifier::None,
          .handler = &Flagged,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "persist",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "remove a key's expiry",
          .protocolCommand = "PERSIST",
          .modifiers = Modifier::None,
          .handler = &Flagged,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "flush",
          .wire = Wire::Resp,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "discard the keyspace; --all clears every database",
          .protocolCommand = "FLUSHDB",
          .modifiers = Modifier::Everything,
          .handler = &Flush,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "ping",
          .wire = Wire::Resp,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "check the cache answers at all",
          .protocolCommand = "PING",
          .modifiers = Modifier::None,
          .handler = &Echoed,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "echo",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <text>",
          .summary = "have the server repeat <text> back",
          .protocolCommand = "ECHO",
          .modifiers = Modifier::None,
          .handler = &Echoed,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "info",
          .wire = Wire::Resp,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "the server's own INFO payload, as a record",
          .protocolCommand = "INFO",
          .modifiers = Modifier::None,
          .handler = &Info,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "version",
          .wire = Wire::Resp,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "this client's version and the server's, side by side",
          .protocolCommand = "INFO",
          .modifiers = Modifier::None,
          .handler = &Versions,
          // The one row with a fallback today. A node answers *what are you running*
          // as readily as a daemon does, and this is the verb an operator reaches for
          // first when an address does not behave.
          .nodeFallback = &VersionsFromNode,
          .session = nullptr },
        { .name = "stats",
          .wire = Wire::Stats,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "counters from the richest source that answers, which is\n"
                     "named in the output as `source`",
          .protocolCommand = "",
          .modifiers = Modifier::None,
          .handler = &Stats,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "live-stats",
          // `Stats` for ADMISSION only: `0xFC` identifies the endpoint, and RESP opening
          // beside it is what lets a refusal say *a Redis answered, a fastcache did not*.
          // Every reading then arrives on the session's own subscription (#1399).
          .wire = Wire::Stats,
          .minOperands = 0,
          .maxOperands = 1,
          // Derived from `LiveSubjectTable` at compile time, unlike `fleet`'s list below,
          // because #134 §1 asks for exactly that and a derived list cannot drift where a
          // test only notices after it has.
          .operands = LiveSubjectOperands,
          .summary = "watch a cache, a node or the fleet, one sample per interval;\n"
                     "the subject is inferred when not named, and `fleet` never is",
          .protocolCommand = "",
          .modifiers = Modifier::Interval | Modifier::Samples,
          .handler = &LiveStatsVerb,
          // No second attempt exists to make: identifying the endpoint IS the subject
          // decision, and it happens before any sample (#134 §1.3).
          .nodeFallback = nullptr,
          .session = &RunLiveStatsSession },

        // The `0xFC` verbs. These are the ONLY ones a `fastcache-compile-node` answers:
        // that binary speaks no RESP and no memcached text, so every row above this
        // comment is refused BY NAME against one rather than left to close silently.
        { .name = "node",
          .wire = Wire::Node,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          // Kinds of thing, never the field list. The enumerating form was already
          // wrong -- #1294 added the toolchain survey, registration state, capacity
          // and consensus role while this went on naming five fields as an inventory
          // -- and it was wrong the way a list always becomes wrong: a summary that
          // enumerates is a second place the fields live, so it drifts the moment the
          // verb answers one more.
          .summary = "what this endpoint is, what it is running, and how far\n"
                     "it has got towards being useful to the fleet",
          .protocolCommand = "node-status",
          .modifiers = Modifier::None,
          .handler = &NodeStatus,
          .nodeFallback = nullptr,
          .session = nullptr },
        // Asked of the node on THIS machine: the node refuses a cordon from anywhere else,
        // because whether a machine serves the fleet is decided on that machine (#1303).
        { .name = "cordon",
          .wire = Wire::Node,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "stop this machine's worker taking new compiles and let the\n"
                     "running ones finish; lasts until uncordon or a restart",
          .protocolCommand = "cordon",
          .modifiers = Modifier::None,
          .handler = &Cordon,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "uncordon",
          .wire = Wire::Node,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "let this machine's worker take compiles again",
          .protocolCommand = "cordon",
          .modifiers = Modifier::None,
          .handler = &Uncordon,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "fleet",
          .wire = Wire::Node,
          .minOperands = 1,
          .maxOperands = 1,
          // Spelled out rather than joined from `FleetSectionTable`, because this is a
          // `constexpr` table and the join would be a compile-time string built into a
          // buffer -- more machinery than the fact is worth. What keeps it honest is
          // `the fleet verb offers every section the server serves` in
          // `CliVerbs_test.cpp`, which walks that table: a section added and not
          // spelled here reddens rather than going quietly missing from the help.
          .operands = " <kpi|machines|workers|leases|members|forgotten|tiers|series>",
          .summary = "one of the leader's fleet tables, read over 0xFC from\n"
                     "the node that leads -- no admin surface, browser or JSON parser",
          .protocolCommand = "fleet-text",
          .modifiers = Modifier::Range,
          .handler = &Fleet,
          // No fallback: an endpoint that is not a node has no fleet to report.
          .nodeFallback = nullptr,
          .session = nullptr },
        // Asked of ANY node, about any host: the answer is that node's own fold, which is the
        // point -- two nodes disagreeing about one host is the finding, and a verb that could
        // only be asked of one of them could not surface it.
        { .name = "explain-admission",
          .wire = Wire::Node,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <host>",
          .summary = "why this node admits or refuses a host, naming every\n"
                     "route that decided rather than only the winning one",
          .protocolCommand = "explain-admission",
          .modifiers = Modifier::None,
          .handler = &ExplainAdmission,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "node-metrics",
          .wire = Wire::Node,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "every figure the node's /metrics renders, zeroes\n"
                     "included -- a counter is a tally, so zero is a reading",
          .protocolCommand = "node-metrics",
          .modifiers = Modifier::None,
          .handler = &NodeMetrics,
          .nodeFallback = nullptr,
          .session = nullptr },

        // The cluster verbs. FIVE rows over FOUR wire verbs, because `ClusterStatus`
        // answers two questions an operator asks separately -- who is in the cluster,
        // and what it has agreed -- and this tool's unit is a table that `--format=json`
        // can carry. One verb returning a members table and a settings table would have
        // to nest one inside the other, which `Field::value` is a `Cell` precisely to
        // forbid; one verb returning a union of both would need a discriminating column
        // nobody wants to filter on.
        { .name = "cluster-members",
          .wire = Wire::Node,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "who the cluster has agreed is a member, and where each\n"
                     "answers; a member that has never led shows no scheduler",
          .protocolCommand = "cluster-status",
          .modifiers = Modifier::None,
          .handler = &ClusterMembers,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "cluster-settings",
          .wire = Wire::Node,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "every setting this build knows and what the cluster has\n"
                     "agreed for it; absent means nothing has been agreed",
          .protocolCommand = "cluster-status",
          .modifiers = Modifier::None,
          .handler = &ClusterSettings,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "cluster-set",
          .wire = Wire::Node,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <name> <value>",
          .summary = "change a replicated setting; the leader refuses a name\n"
                     "it does not know rather than agreeing to nothing",
          .protocolCommand = "cluster-set",
          .modifiers = Modifier::None,
          .handler = &ClusterSet,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "cluster-forget",
          .wire = Wire::Node,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <member-id>",
          .summary = "remove a member from the agreed set",
          .protocolCommand = "cluster-forget",
          .modifiers = Modifier::None,
          .handler = &ClusterForget,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "cluster-admit",
          .wire = Wire::Node,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <member-id> <raft-endpoint>",
          .summary = "add a member, or record that one has moved",
          .protocolCommand = "cluster-admit",
          .modifiers = Modifier::None,
          .handler = &ClusterAdmit,
          .nodeFallback = nullptr,
          .session = nullptr },

        // The memcached text verbs. Everything below this line is unavailable against a
        // daemon with `--requirepass` set, because that protocol has no AUTH verb --
        // `WireSpec::authenticable` carries the fact and `FromMemcachedError` explains
        // it when the server says so.
        { .name = "touch",
          .wire = Wire::Memcached,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <seconds>",
          .summary = "move a key's expiry without reading the value",
          .protocolCommand = "touch",
          .modifiers = Modifier::None,
          .handler = &Touch,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "gat",
          .wire = Wire::Memcached,
          .minOperands = 2,
          .maxOperands = VariadicOperands,
          .operands = " <seconds> <key>...",
          .summary = "read values and move their expiry in one step; the expiry\n"
                     "comes FIRST here, as it does on the wire",
          .protocolCommand = "gat",
          .modifiers = Modifier::None,
          .handler = &FetchAndTouch,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "gats",
          .wire = Wire::Memcached,
          .minOperands = 2,
          .maxOperands = VariadicOperands,
          .operands = " <seconds> <key>...",
          .summary = "`gat` with each value's cas token, for a later `cas`",
          .protocolCommand = "gats",
          .modifiers = Modifier::None,
          .handler = &FetchAndTouch,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "add",
          .wire = Wire::Memcached,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <value>",
          .summary = "store only if the key is absent; exits 1 if it exists",
          .protocolCommand = "add",
          .modifiers = Modifier::Ttl,
          .handler = &StoreText,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "replace",
          .wire = Wire::Memcached,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <value>",
          .summary = "store only if the key exists; exits 1 if it does not",
          .protocolCommand = "replace",
          .modifiers = Modifier::Ttl,
          .handler = &StoreText,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "append",
          .wire = Wire::Memcached,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <value>",
          .summary = "add bytes to the end of an existing value; no --ttl,\n"
                     "because the server keeps the entry's own expiry",
          .protocolCommand = "append",
          .modifiers = Modifier::None,
          .handler = &StoreText,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "prepend",
          .wire = Wire::Memcached,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <value>",
          .summary = "add bytes to the front of an existing value",
          .protocolCommand = "prepend",
          .modifiers = Modifier::None,
          .handler = &StoreText,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "cas",
          .wire = Wire::Memcached,
          .minOperands = 3,
          .maxOperands = 3,
          .operands = " <key> <value> <cas>",
          .summary = "store only if the value still carries <cas>; exits 1 when\n"
                     "it has changed. `gats` and `inspect` report the token",
          .protocolCommand = "cas",
          .modifiers = Modifier::Ttl,
          .handler = &StoreText,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "inspect",
          .wire = Wire::Memcached,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "a key's ttl, last access, cas token and stored size --\n"
                     "nothing else in this project reports the last three",
          .protocolCommand = "me",
          .modifiers = Modifier::None,
          .handler = &Inspect,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "mc-stats",
          .wire = Wire::Memcached,
          .minOperands = 0,
          .maxOperands = 1,
          .operands = " [settings|items|slabs|sizes|conns]",
          // "which `stats` ABOVE cannot reach" until #1301, and that was a claim about
          // where the row sat in a flat list. The list is grouped by wire now and
          // `stats` renders in the LAST group, below this one -- so the sentence
          // survived the change by no longer being about anything. A summary names the
          // other verb, never where it is printed.
          .summary = "the memcached `stats` families, which the `stats` verb\n"
                     "cannot reach; no argument gives the 24-field default set",
          .protocolCommand = "stats",
          .modifiers = Modifier::None,
          .handler = &TextStats,
          .nodeFallback = nullptr,
          .session = nullptr },
        { .name = "cache-memlimit",
          .wire = Wire::Memcached,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <megabytes>",
          .summary = "change the cache's byte budget now; NOT persisted, so a\n"
                     "restart reads --max-memory again",
          .protocolCommand = "cache_memlimit",
          .modifiers = Modifier::None,
          .handler = &MemoryLimit,
          .nodeFallback = nullptr,
          .session = nullptr },
    });
} // namespace

std::span<VerbSpec const> Verbs() noexcept
{
    return VerbTable;
}

VerbSpec const* FindVerb(std::string_view name) noexcept
{
    // A plain loop rather than `std::ranges::find_if`, and deliberately: the iterator
    // of a `std::array` is a raw pointer on libstdc++ and libc++ and a class type on
    // MSVC, so no single spelling of `auto` satisfies both the compilers and
    // clang-tidy's `readability-qualified-auto`. The test client records the same
    // collision. Not iterating at all sidesteps it rather than picking a side.
    for (auto const& verb: VerbTable)
        if (verb.name == name)
            return &verb;
    return nullptr;
}

bool OperandCountAccepted(VerbSpec const& verb, std::size_t count) noexcept
{
    if (count < verb.minOperands)
        return false;
    return verb.maxOperands == VariadicOperands || count <= verb.maxOperands;
}

std::string DescribeOperandArity(VerbSpec const& verb)
{
    auto const plural = [](std::uint8_t n) {
        return n == 1 ? "operand" : "operands";
    };
    if (verb.maxOperands == VariadicOperands)
        return std::format("at least {} {}", verb.minOperands, plural(verb.minOperands));
    if (verb.minOperands == verb.maxOperands)
        return std::format("exactly {} {}", verb.minOperands, plural(verb.minOperands));
    return std::format("{} to {} operands", verb.minOperands, verb.maxOperands);
}

Answer RunNodeFallback(VerbSpec const& verb, VerbContext const& context, RemoteKind kind, Answer primary)
{
    auto scoped = context;
    scoped.verb = &verb;

    // A verb whose question a node can answer, asked of a node. Everything else falls
    // through to the identification below -- including a verb WITH a fallback met by an
    // endpoint that is not a node, which must not be asked a question it cannot hear.
    if (kind == RemoteKind::CompileNode && verb.nodeFallback != nullptr && context.node != nullptr)
        return verb.nodeFallback(scoped);

    auto const explanation = ExplainRemoteKind(verb.name, context.node == nullptr ? "" : context.node->Address(), kind);
    if (explanation.empty())
        // The probe explained nothing the caller does not already know. Returned
        // UNCHANGED rather than annotated, because a second sentence about one fault
        // makes it read as two.
        return primary;

    // The identification goes FIRST: an operator reading downwards wants *this is a
    // compile node* before *the server closed the connection*, which is the consequence
    // rather than the cause.
    primary.advisories.insert(primary.advisories.begin(), explanation);

    // And the SAME correction for the reader that has no eyes. The sentence above is the
    // operator's half; the exit code is the script's, and it is the published half of
    // this tool's contract. A probe that came back with a FRAME proves the endpoint was
    // reached and answered, so leaving `Unreachable` standing would go on saying *the
    // server could not be reached* about an address that is serving -- and `unreachable`
    // is the one code that reads as RETRY, which is advice that can never come true here.
    //
    // Keyed on the KIND, never on the explanation being non-empty: what corrects an
    // outcome is the evidence, and a table that decides it from whether some text was
    // produced would silently change the exit code the day a sentence is reworded.
    if (auto const established = EstablishedBy(kind); established.has_value())
        primary.outcome = *established;

    return primary;
}

Answer RunVerb(VerbSpec const& verb, VerbContext const& context)
{
    auto scoped = context;
    scoped.verb = &verb;

    auto const& wire = WireTable[static_cast<std::size_t>(verb.wire)];
    if (!wire.available(scoped))
        return Concluded(Outcome::Unreachable, std::string { wire.unavailable });

    auto answer = verb.handler(scoped);
    NoteBinaryCells(answer, verb.wire);
    return answer;
}

} // namespace FastCache::Cli
