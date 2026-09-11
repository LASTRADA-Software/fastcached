// SPDX-License-Identifier: Apache-2.0
/// @file main.cpp
/// `fastcache-cli`'s entry point, and deliberately almost nothing else.
///
/// **This translation unit is in no test target**, which is a property the tree has
/// paid for twice (#370, #909) and the reason every decision this tool makes lives
/// somewhere else: the command line in `CliCommand`, the verbs in `CliVerbs`, the
/// stats ladder's choice in `StatsSource`, the rendering in `CliFormat`. What is left
/// here is acquisition -- read the environment, read a file, open a socket, write to a
/// stream -- and the switch that hands over to the parts that can be tested.

#include "CliCommand.hpp"
#include "CliFormat.hpp"
#include "CliVerbs.hpp"
#include "SocketExchange.hpp"
#include "StatsGatherer.hpp"

#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/Terminal.hpp>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
    #include <fcntl.h>
    #include <io.h>
#endif

namespace
{

using namespace FastCache;
using namespace FastCache::Cli;

/// This binary's own name, for the one-line diagnostics.
constexpr std::string_view ProgramName = "fastcache-cli";

/// Read one environment variable.
///
/// A plain function pointer rather than a lambda so it can be handed to
/// `ApplyEnvironment`, whose parameter is a function pointer precisely so a test can
/// substitute a scripted environment.
/// @param name The variable.
/// @return Its value, or nullopt when unset.
[[nodiscard]] std::optional<std::string> LookupEnvironment(std::string_view name)
{
    return ReadEnvironmentVariable(name);
}

/// Read a secret out of a file, trimming the trailing newline an editor leaves.
///
/// The path is not provenance-gated: a secret reached BY PATH is the file's to protect,
/// not the command line's, which is why `--token-file` is preferable to
/// `$FASTCACHE_TOKEN` -- an environment variable is visible to anything that can read
/// this process's environment.
/// @param path The file.
/// @return The secret, or the reason it could not be read.
[[nodiscard]] std::expected<std::string, std::string> ReadSecretFile(std::string const& path)
{
    std::ifstream file { path, std::ios::binary };
    if (!file.is_open())
        return std::unexpected(std::format("cannot read {}", path));

    std::ostringstream buffer;
    buffer << file.rdbuf();
    auto secret = buffer.str();
    while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r'))
        secret.pop_back();
    if (secret.empty())
        return std::unexpected(std::format("{} is empty", path));
    return secret;
}

/// Write bytes to stdout with no translation.
///
/// **Windows opens stdout in text mode, which turns every `\n` into `\r\n`.** For
/// `get --raw`, whose whole purpose is to hand over the value's bytes unaltered, that
/// silently corrupts any value containing a newline -- and it corrupts it on exactly
/// one platform, so a fixture that only runs on POSIX would never see it.
/// @param bytes What to write.
void WriteRaw(std::string_view bytes)
{
#if defined(_WIN32)
    (void) ::_setmode(::_fileno(stdout), _O_BINARY);
#endif
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
}

/// Print the remarks an answer carried.
/// @param answer The answer.
/// @param quiet Whether the operator asked for silence.
void ReportAdvisories(Answer const& answer, bool quiet)
{
    if (quiet)
        return;
    for (auto const& advisory: answer.advisories)
        std::cerr << ProgramName << ": " << advisory << '\n';
}

/// Whether to colour, resolving `auto` against the actual stdout.
///
/// The one place the terminal is asked. `StdoutSupportsColor` also enables
/// virtual-terminal processing on Windows as a side effect, so it must run before
/// anything is written.
/// @param choice What the operator asked for.
/// @return Whether to emit escapes.
[[nodiscard]] UsageColor ResolveColor(ColorChoice choice)
{
    switch (choice)
    {
        case ColorChoice::Always:
            return UsageColor::Colored;
        case ColorChoice::Never:
            return UsageColor::Plain;
        case ColorChoice::Auto:
        case ColorChoice::Last:
            break;
    }
    return StdoutSupportsColor() ? UsageColor::Colored : UsageColor::Plain;
}

/// Print a usage failure and the help text.
///
/// The help goes to **stderr**, plain. Deliberately: `StdoutSupportsColor` probes
/// stdout, and a build system or a pipe usually captures stderr, so colouring text
/// that is about to be captured helps nobody -- the same reasoning `fastcache-cc`
/// records for its own usage errors.
/// @param diagnostic What was wrong.
/// @return The process exit code.
[[nodiscard]] int ReportUsageError(std::string_view diagnostic)
{
    std::cerr << ProgramName << ": " << diagnostic << "\n\n" << HelpText();
    return ExitCodeOf(Outcome::Usage);
}

/// Run the verb the command named and write its answer.
/// @param command The parsed command.
/// @param verb The verb it named.
/// @return The process exit code.
[[nodiscard]] int RunAndReport(Command const& command, VerbSpec const& verb)
{
    // WHICH connections to open is the wire row's answer, never a ladder here: this
    // file is in no test target (#370, #909), so a decision taken in it is one nothing
    // can hold to. `Stats` asks for RESP too, because `INFO` is the ladder's fallback
    // rung and lives on that connection.
    auto const& wire = WireTable[static_cast<std::size_t>(verb.wire)];

    std::unique_ptr<SocketExchange> resp;
    std::vector<std::string> openingRemarks;
    if (wire.needsResp)
    {
        auto exchange = SocketExchange::Open(command.cache, command.timeouts, command.credential);
        if (exchange.has_value())
        {
            resp = std::move(*exchange);
            auto const carried = resp->Advisories();
            openingRemarks.assign(carried.begin(), carried.end());
        }
        else if (verb.wire != Wire::Stats)
        {
            // A verb that needs the cache and could not reach it is finished here.
            // `stats` is the exception: `/metrics` is on another port and may well
            // answer, so the ladder is still given its chance and reports this rung as
            // having failed.
            std::cerr << ProgramName << ": " << exchange.error().detail << '\n';
            return ExitCodeOf(Outcome::Unreachable);
        }
        else
        {
            openingRemarks.push_back(exchange.error().detail);
        }
    }

    std::unique_ptr<MemcachedExchange> memcached;
    if (wire.needsMemcached)
    {
        // No credential is presented, and none can be: this protocol has no AUTH verb.
        // `WireSpec::authenticable` carries that fact and the refusal, when the server
        // makes one, is explained rather than relayed bare.
        auto exchange = MemcachedExchange::Open(command.cache, command.timeouts);
        if (!exchange.has_value())
        {
            std::cerr << ProgramName << ": " << exchange.error().detail << '\n';
            return ExitCodeOf(Outcome::Unreachable);
        }
        memcached = std::move(*exchange);
    }

    std::unique_ptr<NodeExchange> node;
    if (wire.needsNode)
    {
        auto exchange = NodeExchange::Open(command.cache, command.timeouts, command.credential);
        if (exchange.has_value())
        {
            node = std::move(*exchange);
            auto const carried = node->Advisories();
            openingRemarks.insert(openingRemarks.end(), carried.begin(), carried.end());
        }
        else if (verb.wire != Wire::Stats)
        {
            std::cerr << ProgramName << ": " << exchange.error().detail << '\n';
            return ExitCodeOf(Outcome::Unreachable);
        }
        else
        {
            // `stats` again: the `0xFC` rung is one of three and the other two may well
            // answer, so a rung that could not be opened is REPORTED rather than fatal.
            // The same reasoning as the RESP arm above, and the same reason the ladder
            // exists at all.
            openingRemarks.push_back(exchange.error().detail);
        }
    }

    auto gatherer = LadderGatherer {
        command.admin,
        command.cache,
        command.timeouts,
        command.credential.Configured() ? std::optional<std::string> { command.credential.secret } : std::nullopt,
        resp.get(),
        node.get()
    };

    auto const context = VerbContext { .operands = command.operands,
                                       .options = command.verbOptions,
                                       .resp = resp.get(),
                                       .memcached = memcached.get(),
                                       .node = node.get(),
                                       .stats = &gatherer };

    auto answer = RunVerb(verb, context);

    // **The endpoint is IDENTIFIED only when its own wire could not answer**, which is
    // what keeps this off the common path: a `get` against a healthy daemon pays for no
    // probe at all. What is here is acquisition alone -- open a socket, ask one question
    // -- because this file is in no test target (#370, #909); `RunNodeFallback` owns
    // every decision, including whether the identification is worth reporting.
    //
    // `Protocol` joins `Unreachable` deliberately. A `fastcache-compile-node` meeting a
    // RESP client does not always close in silence: whatever arrives is bytes this
    // client cannot read as a reply, and reporting *the reply could not be read* for a
    // binary that speaks another protocol entirely sends an operator hunting a codec
    // bug.
    auto const unanswered = answer.outcome == Outcome::Unreachable || answer.outcome == Outcome::Protocol;
    if (unanswered && verb.wire != Wire::Node && node == nullptr)
    {
        if (auto probe = NodeExchange::Open(command.cache, command.timeouts, command.credential); probe.has_value())
        {
            auto const kind = ProbeRemote(**probe);
            auto probed = context;
            probed.node = probe->get();
            answer = RunNodeFallback(verb, probed, kind, std::move(answer));
        }
        // A probe that could not even connect adds nothing: the primary answer already
        // says the address did not answer, and a second sentence about one fault makes
        // it read as two. Deliberately silent rather than logged.
    }

    // The connection's own remarks come first: they are about the whole exchange
    // rather than about this one answer, and an operator reading downwards wants
    // "your credential was ignored" before "the key does not exist".
    answer.advisories.insert(answer.advisories.begin(), openingRemarks.begin(), openingRemarks.end());

    if (answer.rawPayload.has_value())
        WriteRaw(*answer.rawPayload);
    else
        std::cout << RenderValue(answer.value,
                                 RenderOptions { .format = command.format,
                                                 .color = ResolveColor(command.color),
                                                 .absentOverride = command.absentOverride });

    ReportAdvisories(answer, command.quiet);
    return ExitCodeOf(answer.outcome);
}

} // namespace

/// Entry point.
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return One of `OutcomeTable`'s exit codes.
int main(int argc, char* argv[])
{
    std::vector<std::string> args;
    args.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
    for (auto index = 1; index < argc; ++index)
        args.emplace_back(argv[index]);

    // Defaults, then the environment, then argv -- in that order, each overriding the
    // last, and "the command line wins" is which step runs second rather than a
    // per-field merge.
    Command seed;
    ApplyEnvironment(seed, &LookupEnvironment);
    if (seed.action == Action::UsageError)
        return ReportUsageError(seed.diagnostic);

    auto command = ParseCommand(args, std::move(seed));

    switch (command.action)
    {
        case Action::ShowHelp:
            std::cout << HelpText(ResolveColor(command.color));
            return ExitCodeOf(Outcome::Affirmative);
        case Action::ShowVersion:
            std::cout << ProgramName << ' ' << FASTCACHE_CLI_VERSION << '\n';
            return ExitCodeOf(Outcome::Affirmative);
        case Action::UsageError:
            return ReportUsageError(command.diagnostic);
        case Action::RunVerb:
        case Action::Last:
            break;
    }

    if (!command.tokenFile.empty())
    {
        auto const secret = ReadSecretFile(command.tokenFile);
        if (!secret.has_value())
            return ReportUsageError(secret.error());
        command.credential.secret = *secret;
    }

    auto const* const verb = FindVerb(command.verb);
    if (verb == nullptr)
        // Unreachable: `ParseCommand` refuses an unknown verb. Answered rather than
        // asserted, because a `main` that can only be right is better than one that
        // can only crash.
        return ReportUsageError(std::format("unknown command `{}`", command.verb));

    return RunAndReport(command, *verb);
}
