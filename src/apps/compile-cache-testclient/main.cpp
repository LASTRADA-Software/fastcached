// SPDX-License-Identifier: Apache-2.0
//
// Standalone compile-cache test client — the reference localizer.
//
// Drives a real compiler with /showIncludes, frames the result as a
// CompileValue, and STOREs/FETCHes it against a running fastcached over the
// custom 0xFC protocol. On FETCH it localizes the canonical paths back to the
// consumer's layout (linking the same PathCanon the server canonicalizes with,
// so the parity contract is exercised, not reimplemented).
//
// This tool exists purely for cross-depth validation (see run-crossdepth.ps1).
// It contains NO project-specific source; it compiles whatever source file it
// is pointed at and prints only synthetic counts/paths under the caller's
// control.

#include "TestClientCli.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/TcpClient.hpp>
#include <FastCache/Platform/Terminal.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <IProcessRunner.hpp>
#include <ReplayGuard.hpp>

namespace
{

using namespace FastCache;

namespace Wire = FastCache::CompileCacheWire;

/// Print an error and exit non-zero.
[[noreturn]] void Die(std::string_view message)
{
    std::cerr << "testclient: " << message << '\n';
    std::exit(2);
}

/// Read an entire file into a byte vector.
[[nodiscard]] std::vector<std::byte> ReadFileBytes(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary };
    if (!in)
        Die("cannot open file: " + path.string());
    std::vector<char> raw { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    std::vector<std::byte> bytes;
    bytes.reserve(raw.size());
    for (char const c: raw)
        bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

/// Write a byte vector to a file.
void WriteFileBytes(std::filesystem::path const& path, std::span<std::byte const> bytes)
{
    std::ofstream out { path, std::ios::binary };
    if (!out)
        Die("cannot write file: " + path.string());
    out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// Which family of command line a compiler takes, and where it reports the
/// headers it read.
///
/// Two rows, because that is how many spellings exist -- and it is a table
/// rather than a branch for the reason every other driver table here is one: a
/// third row is the whole cost of a third driver.
struct DriverSpec
{
    std::string_view objectFlag;      ///< Fused with the object path.
    std::string_view compileOnly;     ///< Compile, do not link.
    std::string_view dependencyFlag;  ///< Ask for the dependency record.
    std::string_view dependencyValue; ///< Fused with the depfile path, or empty.
    PathCanon::Grammar grammar;       ///< The grammar the record is written in.
};

/// MSVC drivers report includes as `/showIncludes` notes on their output; GNU
/// drivers write a depfile. The two are different grammars, and `PathCanon`
/// already knows both -- so a value stored from either localizes on either.
constexpr auto MsvcDriver = DriverSpec { .objectFlag = "/Fo",
                                         .compileOnly = "/c",
                                         .dependencyFlag = "/showIncludes",
                                         .dependencyValue = {},
                                         .grammar = PathCanon::Grammar::ShowIncludes };

constexpr auto GnuDriver = DriverSpec { .objectFlag = "-o",
                                        .compileOnly = "-c",
                                        .dependencyFlag = "-MD",
                                        .dependencyValue = "-MF",
                                        .grammar = PathCanon::Grammar::GccDepfile };

/// Pick the driver family from the compiler's name.
///
/// By name and not by host: this tool is pointed at whatever `--compiler` says,
/// and `clang-cl` on Linux takes MSVC spellings exactly as it does on Windows.
/// @param compiler The `--compiler` value, as typed.
/// @return The spellings that compiler accepts.
[[nodiscard]] DriverSpec DriverFor(std::string_view compiler)
{
    auto const leaf = std::filesystem::path { compiler }.filename().string();
    return leaf.starts_with("cl") || leaf.starts_with("clang-cl") ? MsvcDriver : GnuDriver;
}

/// Run a compiler, capturing stdout and stderr together.
///
/// Through `IProcessRunner` and an explicit argument vector rather than a shell
/// string. Two reasons and both matter: a command processor is what
/// `bugprone-command-processor` refuses, and hand-quoting a path into a shell
/// string is a bug waiting for the first directory with a space in it.
/// @param argv Full invocation; argv[0] is the compiler.
/// @return Exit code and the combined output.
[[nodiscard]] std::pair<int, std::string> RunCapture(std::vector<std::string> const& argv)
{
    auto const runner = Cc::MakeProcessRunner();
    if (runner == nullptr)
        Die("failed to create a process runner");
    auto run = runner->RunCaptureCombined(argv);
    if (run.exitCode < 0)
        Die("failed to spawn compiler: " + argv.front());
    return { run.exitCode, std::move(run.out) };
}

// --- The daemon connection ------------------------------------------------

/// How long to wait for the daemon, both to answer the dial and to answer each
/// later call.
///
/// Bounded rather than left to the OS, because this tool is run from scripts and
/// from CI: a probe that parks forever on a daemon which accepted and then went
/// quiet reports as a suite timeout naming nothing, which is the least useful way
/// a failure can present itself.
constexpr auto DaemonTimeout = std::chrono::seconds { 30 };

/// Connect to the daemon, or exit with a diagnosis.
///
/// This used to be a hand-written `TcpClient` here, and it was the third
/// implementation of one job in this tree -- the one nobody built, so nobody
/// noticed it was `inet_pton(AF_INET)`-only (a hostname could never work),
/// unbounded, unprotected against SIGPIPE, and did not compile on POSIX at all
/// (issue #84). It is `Net/TcpClient` now, like everything else.
///
/// @param a The parsed command line.
/// @return The connected socket; never null.
[[nodiscard]] std::unique_ptr<ISocket> Dial(TestClient::Args const& a)
{
    auto socket = ConnectTcp(a.host, a.port, DaemonTimeout, DaemonTimeout);
    if (!socket.has_value())
        Die("connect failed (" + socket.error().context + ") -- is fastcached running?");
    return std::move(*socket);
}

/// Send every byte, or exit.
///
/// `SyncRun` over a blocking socket resolves inline, so the task is never left
/// suspended -- the one thing `SyncRun` refuses to read from.
/// @param socket Connected socket.
/// @param bytes What to send.
void SendOrDie(ISocket& socket, std::span<std::byte const> bytes)
{
    if (!SyncRun(SendAll(&socket, bytes)))
        Die("send failed");
}

/// Receive exactly `count` bytes, or exit.
/// @param socket Connected socket.
/// @param count How many bytes are required.
/// @return The bytes.
[[nodiscard]] std::vector<std::byte> RecvOrDie(ISocket& socket, std::size_t count)
{
    auto got = SyncRun(RecvExactly(&socket, count));
    if (!got.has_value())
        Die("recv failed (peer closed early)");
    return std::move(*got);
}

/// Read one whole reply frame: the fixed header, then exactly the payload it
/// declares. Every status is drained the same way, so a refusal leaves the
/// connection in the same state a hit does.
/// @param client Connected client.
/// @return The decoded status and its payload.
[[nodiscard]] std::pair<Wire::Status, std::vector<std::byte>> RecvReply(ISocket& client)
{
    auto const headerBytes = RecvOrDie(client, Wire::ReplyHeaderSize);
    auto const header = Wire::DecodeReplyHeader(headerBytes);
    if (!header.has_value())
        Die("server sent a reply this build cannot parse");
    auto payload = header->payloadLength > 0 ? RecvOrDie(client, header->payloadLength) : std::vector<std::byte> {};
    return { header->status, std::move(payload) };
}

/// Turn a refusal into a diagnosable message naming the server's own code.
[[nodiscard]] std::string DescribeRefusal(std::span<std::byte const> payload)
{
    auto const decoded = Wire::DecodeErrorPayload(payload);
    if (!decoded.has_value())
        return "rejected (no reason given)";
    auto const* descriptor = Wire::Describe(decoded->first);
    auto const name = descriptor != nullptr ? descriptor->name : std::string_view { "unknown" };
    return std::string { "rejected (" } + std::string { name } + "): " + std::string { decoded->second };
}

// --- store: compile, frame, STORE -----------------------------------------

int DoStore(TestClient::Args const& a)
{
    std::filesystem::path const objPath = a.object.empty() ? (std::filesystem::temp_directory_path() / "cc-testclient.obj")
                                                           : std::filesystem::path { a.object };

    auto const driver = DriverFor(a.compiler);
    auto const depPath = std::filesystem::path { objPath }.replace_extension(".d");

    std::vector<std::string> argv { a.compiler, std::string { driver.compileOnly }, std::string { driver.dependencyFlag } };
    if (driver.dependencyValue.empty())
        argv.emplace_back(std::string { driver.objectFlag } + objPath.string());
    else
    {
        argv.emplace_back(driver.objectFlag);
        argv.emplace_back(objPath.string());
        argv.emplace_back(driver.dependencyValue);
        argv.emplace_back(depPath.string());
    }
    argv.emplace_back(a.source);

    auto const [code, output] = RunCapture(argv);
    if (code != 0)
    {
        std::cerr << "compiler failed (exit " << code << "):\n" << output << '\n';
        return 3;
    }

    // An MSVC driver puts its notes in the captured output; a GNU driver wrote a
    // file. Reading the record from where THIS driver put it is the whole
    // difference, and getting it wrong stores a region with nothing in it -- which
    // still round-trips, so nothing would fail.
    std::string record = output;
    if (!driver.dependencyValue.empty())
    {
        auto const bytes = ReadFileBytes(depPath);
        record.assign(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        std::filesystem::remove(depPath);
    }

    CompileValue value;
    value.objectBlob = ReadFileBytes(objPath);
    value.textRegions.push_back({ .grammar = driver.grammar, .bytes = record });

    auto const encoded = EncodeCompileValue(value);

    auto const frame = Wire::EncodeStore(Wire::StoreRequest { .key = a.key,
                                                              .prefetchGroup = a.prefetchGroup,
                                                              .srcRoot = a.srcRoot,
                                                              .buildTree = a.buildTree,
                                                              .value = std::span<std::byte const> { encoded } });

    auto const client = Dial(a);
    SendOrDie(*client, frame);
    auto const [status, payload] = RecvReply(*client);
    if (status != Wire::Status::Ok)
        Die("STORE " + DescribeRefusal(payload));

    std::cout << "STORE ok key=" << a.key << " objectBytes=" << value.objectBlob.size() << '\n';
    return 0;
}

// --- fetch: FETCH, localize, verify ----------------------------------------

int DoFetch(TestClient::Args const& a)
{
    auto const client = Dial(a);
    SendOrDie(*client, Wire::EncodeFetch(a.key));

    auto const [status, payload] = RecvReply(*client);
    if (status == Wire::Status::Error)
        Die("FETCH " + DescribeRefusal(payload));
    if (status != Wire::Status::Ok)
    {
        std::cout << "FETCH miss key=" << a.key << '\n';
        return 4;
    }

    auto decoded = DecodeCompileValue(payload);
    if (!decoded.has_value())
        Die("FETCH returned a malformed compile-value");

    // Localize every region to the CONSUMER layout (srcRoot/buildTree passed on
    // the fetch command line), then require every dependency it names to exist.
    PathCanon::Layout const consumer { .sourceRoot = a.srcRoot, .buildTree = a.buildTree };
    std::vector<TextRegion> localizedRegions;
    localizedRegions.reserve(decoded->textRegions.size());
    for (auto const& region: decoded->textRegions)
    {
        auto localized = PathCanon::LocalizeRegion(region.bytes, region.grammar, consumer);
        if (!localized.has_value())
            Die("localization failed");
        localizedRegions.push_back({ .grammar = region.grammar, .bytes = std::move(*localized) });
    }

    // Through the launcher's own `ReplayedDependencyPaths` rather than a scanner
    // written here. This used to look for "Note: including file:" and nothing
    // else, which meant it understood MSVC and was blind to a GNU depfile -- so on
    // POSIX it reported `includePathsChecked=0` and validated nothing at all,
    // while still exiting 0. It also had none of the exclusions that function
    // documents as load-bearing: a toolchain path outside both roots, or a token
    // the region walker declined to localize, would have been reported as missing
    // when neither is this build's to have.
    auto const required = Cc::ReplayedDependencyPaths(localizedRegions, consumer);
    std::size_t missing = 0;
    for (auto const& path: required)
    {
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path { path }, ec))
        {
            ++missing;
            std::cerr << "  UNRESOLVED: " << path << '\n';
        }
    }
    auto const checked = required.size();

    // Write the object out so the caller can byte-compare it if desired.
    if (!a.object.empty())
        WriteFileBytes(std::filesystem::path { a.object }, decoded->objectBlob);

    std::cout << "FETCH ok key=" << a.key << " objectBytes=" << decoded->objectBlob.size()
              << " includePathsChecked=" << checked << " unresolved=" << missing << '\n';
    return missing == 0 ? 0 : 5;
}

} // namespace

int main(int argc, char** argv)
{
    // argv[0] is the program itself; the sub-command leads what remains.
    std::span<char const* const> const args { argv + 1, argc > 0 ? static_cast<std::size_t>(argc - 1) : 0 };

    auto const parsed = TestClient::ParseArgs(args);
    if (!parsed.has_value())
    {
        std::cerr << "compile-cache-testclient: " << parsed.error().ToString() << "\n\n" << TestClient::HelpText();
        return 2;
    }

    switch (parsed->action)
    {
        case TestClient::Action::ShowHelp:
            // The color decision belongs at the call site; on Windows this call
            // also enables virtual-terminal processing as a side effect.
            std::cout << TestClient::HelpText(StdoutSupportsColor() ? UsageColor::Colored : UsageColor::Plain);
            return 0;
        case TestClient::Action::Store:
            return DoStore(*parsed);
        case TestClient::Action::Fetch:
            return DoFetch(*parsed);
    }
    return 2;
}
