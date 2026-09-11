// SPDX-License-Identifier: Apache-2.0
#include "CliVerbs.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
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

    /// A value cell plus the remark that goes with it when the bytes are not text.
    /// @param bytes The value.
    /// @param answer The answer being built; gains an advisory for a binary value.
    /// @return The cell.
    [[nodiscard]] Cell ValueCell(std::string_view bytes, Answer& answer)
    {
        auto cell = TextOrBinaryCell(bytes);
        if (cell.kind == CellKind::Binary)
            answer.advisories.emplace_back(
                "the value is not valid UTF-8, so it is shown base64-encoded; --raw writes the bytes");
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

    /// `set <key> <value>`: store, honouring `--ttl`, `--nx` and `--xx`.
    /// @param context The invocation.
    /// @return The answer.
    [[nodiscard]] Answer Store(VerbContext const& context)
    {
        auto argv = CommandWithOperands(context);
        if (context.options.ttlSeconds != TtlUnset)
        {
            argv.emplace_back("EX");
            argv.emplace_back(std::format("{}", context.options.ttlSeconds));
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

        auto const ttl = context.options.ttlSeconds == TtlUnset ? std::int64_t { 0 } : context.options.ttlSeconds;
        auto reply =
            AskMemcached(context,
                         EncodeMemcachedStorage(
                             context.verb->protocolCommand, context.operands[0], 0, ttl, context.operands[1], casToken));
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

    /// Which `NodeComponentBit` each reported component name stands for.
    ///
    /// A table because the set grows: a bit this build does not name is reported under
    /// its NUMBER rather than dropped, so an older client meeting a newer node says
    /// *there is something here I do not know about* instead of quietly under-reporting
    /// what the node runs.
    struct ComponentBit
    {
        std::uint32_t bit;     ///< The mask bit.
        std::string_view name; ///< What to call it.
    };

    constexpr std::array<ComponentBit, 4> ComponentBits { {
        { .bit = CompileCacheWire::NodeComponentBit::CacheTier, .name = "cache-tier" },
        { .bit = CompileCacheWire::NodeComponentBit::Worker, .name = "worker" },
        { .bit = CompileCacheWire::NodeComponentBit::Scheduler, .name = "scheduler" },
        { .bit = CompileCacheWire::NodeComponentBit::Consensus, .name = "consensus" },
    } };

    /// The components @p mask names, as a comma-separated list.
    ///
    /// **An empty MASK renders as the word `none` rather than as an absent cell**: a
    /// node that runs no component is a reading, not a missing one, and the two must
    /// not render alike.
    /// @param mask What the node reported.
    /// @return The list.
    [[nodiscard]] std::string DescribeComponents(std::uint32_t mask)
    {
        std::string out;
        std::uint32_t named = 0;
        for (auto const& row: ComponentBits)
            if ((mask & row.bit) != 0)
            {
                named |= row.bit;
                if (!out.empty())
                    out += ", ";
                out += row.name;
            }

        // Whatever is left is a component this build has no name for. Reported as the
        // residual mask, because *some bits I do not understand* is a fact an operator
        // can act on -- upgrade the client -- and silence is not.
        if (auto const unknown = mask & ~named; unknown != 0)
        {
            if (!out.empty())
                out += ", ";
            out += std::format("unknown(0x{:x})", unknown);
        }

        return out.empty() ? std::string { "none" } : out;
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

    /// `node-metrics` -- every counter this node's build carries.
    /// @param context What to run against.
    /// @return The answer.
    [[nodiscard]] Answer NodeMetrics(VerbContext const& context)
    {
        auto const reply = AskNode(context, CompileCacheWire::EncodeNodeMetricsRequest());
        if (!reply.has_value())
            return reply.error();

        auto record = DecodeNodeCounters(reply->payload);
        if (!record.has_value())
            return Concluded(
                Outcome::Protocol,
                std::format("{} answered node-metrics with a body this client cannot read", context.node->Address()));

        return Answered(std::move(*record));
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
          .nodeFallback = nullptr },
        { .name = "mget",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = VariadicOperands,
          .operands = " <key>...",
          .summary = "read several values as a table",
          .protocolCommand = "MGET",
          .modifiers = Modifier::None,
          .handler = &FetchMany,
          .nodeFallback = nullptr },
        { .name = "set",
          .wire = Wire::Resp,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <value>",
          .summary = "store a value; see --ttl, --nx, --xx",
          .protocolCommand = "SET",
          .modifiers = Modifier::Ttl | Modifier::Exclusivity,
          .handler = &Store,
          .nodeFallback = nullptr },
        { .name = "del",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = VariadicOperands,
          .operands = " <key>...",
          .summary = "delete keys; reports how many existed",
          .protocolCommand = "DEL",
          .modifiers = Modifier::None,
          .handler = &Counting,
          .nodeFallback = nullptr },
        { .name = "exists",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = VariadicOperands,
          .operands = " <key>...",
          .summary = "count how many of the named keys exist",
          .protocolCommand = "EXISTS",
          .modifiers = Modifier::None,
          .handler = &Counting,
          .nodeFallback = nullptr },
        { .name = "incr",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "add one and report the result",
          .protocolCommand = "INCR",
          .modifiers = Modifier::None,
          .handler = &Counting,
          .nodeFallback = nullptr },
        { .name = "decr",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "subtract one and report the result",
          .protocolCommand = "DECR",
          .modifiers = Modifier::None,
          .handler = &Counting,
          .nodeFallback = nullptr },
        { .name = "incrby",
          .wire = Wire::Resp,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <delta>",
          .summary = "add <delta> and report the result",
          .protocolCommand = "INCRBY",
          .modifiers = Modifier::None,
          .handler = &Counting,
          .nodeFallback = nullptr },
        { .name = "decrby",
          .wire = Wire::Resp,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <delta>",
          .summary = "subtract <delta> and report the result",
          .protocolCommand = "DECRBY",
          .modifiers = Modifier::None,
          .handler = &Counting,
          .nodeFallback = nullptr },
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
          .nodeFallback = nullptr },
        { .name = "expire",
          .wire = Wire::Resp,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <seconds>",
          .summary = "set a key's expiry",
          .protocolCommand = "EXPIRE",
          .modifiers = Modifier::None,
          .handler = &Flagged,
          .nodeFallback = nullptr },
        { .name = "persist",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "remove a key's expiry",
          .protocolCommand = "PERSIST",
          .modifiers = Modifier::None,
          .handler = &Flagged,
          .nodeFallback = nullptr },
        { .name = "flush",
          .wire = Wire::Resp,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "discard the keyspace; --all clears every database",
          .protocolCommand = "FLUSHDB",
          .modifiers = Modifier::Everything,
          .handler = &Flush,
          .nodeFallback = nullptr },
        { .name = "ping",
          .wire = Wire::Resp,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "check the cache answers at all",
          .protocolCommand = "PING",
          .modifiers = Modifier::None,
          .handler = &Echoed,
          .nodeFallback = nullptr },
        { .name = "echo",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <text>",
          .summary = "have the server repeat <text> back",
          .protocolCommand = "ECHO",
          .modifiers = Modifier::None,
          .handler = &Echoed,
          .nodeFallback = nullptr },
        { .name = "info",
          .wire = Wire::Resp,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "the server's own INFO payload, as a record",
          .protocolCommand = "INFO",
          .modifiers = Modifier::None,
          .handler = &Info,
          .nodeFallback = nullptr },
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
          .nodeFallback = &VersionsFromNode },
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
          .nodeFallback = nullptr },

        // The `0xFC` verbs. These are the ONLY ones a `fastcache-compile-node` answers:
        // that binary speaks no RESP and no memcached text, so every row above this
        // comment is refused BY NAME against one rather than left to close silently.
        { .name = "node",
          .wire = Wire::Node,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "what the endpoint is: version, identity, uptime, the\n"
                     "components it runs and the ports it opened",
          .protocolCommand = "node-status",
          .modifiers = Modifier::None,
          .handler = &NodeStatus,
          .nodeFallback = nullptr },
        { .name = "node-metrics",
          .wire = Wire::Node,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "every counter the endpoint's build carries, zeroes\n"
                     "included -- a counter is a tally, so zero is a reading",
          .protocolCommand = "node-metrics",
          .modifiers = Modifier::None,
          .handler = &NodeMetrics,
          .nodeFallback = nullptr },

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
          .nodeFallback = nullptr },
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
          .nodeFallback = nullptr },
        { .name = "gats",
          .wire = Wire::Memcached,
          .minOperands = 2,
          .maxOperands = VariadicOperands,
          .operands = " <seconds> <key>...",
          .summary = "`gat` with each value's cas token, for a later `cas`",
          .protocolCommand = "gats",
          .modifiers = Modifier::None,
          .handler = &FetchAndTouch,
          .nodeFallback = nullptr },
        { .name = "add",
          .wire = Wire::Memcached,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <value>",
          .summary = "store only if the key is absent; exits 1 if it exists",
          .protocolCommand = "add",
          .modifiers = Modifier::Ttl,
          .handler = &StoreText,
          .nodeFallback = nullptr },
        { .name = "replace",
          .wire = Wire::Memcached,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <value>",
          .summary = "store only if the key exists; exits 1 if it does not",
          .protocolCommand = "replace",
          .modifiers = Modifier::Ttl,
          .handler = &StoreText,
          .nodeFallback = nullptr },
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
          .nodeFallback = nullptr },
        { .name = "prepend",
          .wire = Wire::Memcached,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <value>",
          .summary = "add bytes to the front of an existing value",
          .protocolCommand = "prepend",
          .modifiers = Modifier::None,
          .handler = &StoreText,
          .nodeFallback = nullptr },
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
          .nodeFallback = nullptr },
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
          .nodeFallback = nullptr },
        { .name = "mc-stats",
          .wire = Wire::Memcached,
          .minOperands = 0,
          .maxOperands = 1,
          .operands = " [settings|items|slabs|sizes|conns]",
          .summary = "the memcached `stats` families, which `stats` above cannot\n"
                     "reach; no argument gives the 24-field default set",
          .protocolCommand = "stats",
          .modifiers = Modifier::None,
          .handler = &TextStats,
          .nodeFallback = nullptr },
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
          .nodeFallback = nullptr },
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

    return verb.handler(scoped);
}

} // namespace FastCache::Cli
