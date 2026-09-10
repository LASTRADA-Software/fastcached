// SPDX-License-Identifier: Apache-2.0
#include "CliVerbs.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <ranges>
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
          .handler = &Fetch },
        { .name = "mget",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = VariadicOperands,
          .operands = " <key>...",
          .summary = "read several values as a table",
          .protocolCommand = "MGET",
          .modifiers = Modifier::None,
          .handler = &FetchMany },
        { .name = "set",
          .wire = Wire::Resp,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <value>",
          .summary = "store a value; see --ttl, --nx, --xx",
          .protocolCommand = "SET",
          .modifiers = Modifier::Ttl | Modifier::Exclusivity,
          .handler = &Store },
        { .name = "del",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = VariadicOperands,
          .operands = " <key>...",
          .summary = "delete keys; reports how many existed",
          .protocolCommand = "DEL",
          .modifiers = Modifier::None,
          .handler = &Counting },
        { .name = "exists",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = VariadicOperands,
          .operands = " <key>...",
          .summary = "count how many of the named keys exist",
          .protocolCommand = "EXISTS",
          .modifiers = Modifier::None,
          .handler = &Counting },
        { .name = "incr",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "add one and report the result",
          .protocolCommand = "INCR",
          .modifiers = Modifier::None,
          .handler = &Counting },
        { .name = "decr",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "subtract one and report the result",
          .protocolCommand = "DECR",
          .modifiers = Modifier::None,
          .handler = &Counting },
        { .name = "incrby",
          .wire = Wire::Resp,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <delta>",
          .summary = "add <delta> and report the result",
          .protocolCommand = "INCRBY",
          .modifiers = Modifier::None,
          .handler = &Counting },
        { .name = "decrby",
          .wire = Wire::Resp,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <delta>",
          .summary = "subtract <delta> and report the result",
          .protocolCommand = "DECRBY",
          .modifiers = Modifier::None,
          .handler = &Counting },
        { .name = "ttl",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "how long a key has left; names `exists` separately, because\n"
                     "a key that is gone and one with no expiry are not the same",
          .protocolCommand = "TTL",
          .modifiers = Modifier::None,
          .handler = &Lifetime },
        { .name = "expire",
          .wire = Wire::Resp,
          .minOperands = 2,
          .maxOperands = 2,
          .operands = " <key> <seconds>",
          .summary = "set a key's expiry",
          .protocolCommand = "EXPIRE",
          .modifiers = Modifier::None,
          .handler = &Flagged },
        { .name = "persist",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <key>",
          .summary = "remove a key's expiry",
          .protocolCommand = "PERSIST",
          .modifiers = Modifier::None,
          .handler = &Flagged },
        { .name = "flush",
          .wire = Wire::Resp,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "discard the keyspace; --all clears every database",
          .protocolCommand = "FLUSHDB",
          .modifiers = Modifier::Everything,
          .handler = &Flush },
        { .name = "ping",
          .wire = Wire::Resp,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "check the cache answers at all",
          .protocolCommand = "PING",
          .modifiers = Modifier::None,
          .handler = &Echoed },
        { .name = "echo",
          .wire = Wire::Resp,
          .minOperands = 1,
          .maxOperands = 1,
          .operands = " <text>",
          .summary = "have the server repeat <text> back",
          .protocolCommand = "ECHO",
          .modifiers = Modifier::None,
          .handler = &Echoed },
        { .name = "info",
          .wire = Wire::Resp,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "the server's own INFO payload, as a record",
          .protocolCommand = "INFO",
          .modifiers = Modifier::None,
          .handler = &Info },
        { .name = "version",
          .wire = Wire::Resp,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "this client's version and the server's, side by side",
          .protocolCommand = "INFO",
          .modifiers = Modifier::None,
          .handler = &Versions },
        { .name = "stats",
          .wire = Wire::Stats,
          .minOperands = 0,
          .maxOperands = 0,
          .operands = "",
          .summary = "counters from the richest source that answers, which is\n"
                     "named in the output as `source`",
          .protocolCommand = "",
          .modifiers = Modifier::None,
          .handler = &Stats },
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

Answer RunVerb(VerbSpec const& verb, VerbContext const& context)
{
    auto scoped = context;
    scoped.verb = &verb;

    auto const& wire = WireTable[static_cast<std::size_t>(verb.wire)];
    auto const available = verb.wire == Wire::Resp ? scoped.resp != nullptr : scoped.stats != nullptr;
    if (!available)
        return Concluded(Outcome::Unreachable, std::string { wire.unavailable });

    return verb.handler(scoped);
}

} // namespace FastCache::Cli
