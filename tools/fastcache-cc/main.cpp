// SPDX-License-Identifier: Apache-2.0
//
// fastcache-cc — an sccache-style compiler launcher over the fastcached 0xFC
// compile-cache protocol.
//
// Invoked as `fastcache-cc <compiler> <args...>` (e.g. via
// CMAKE_CXX_COMPILER_LAUNCHER). On a cache HIT it reproduces the object file
// and replays the compiler's stdout/stderr (localizing /showIncludes header
// paths to this machine's layout) so the build behaves as if it compiled. On a
// MISS it runs the real compiler, stores the canonicalized result, and passes
// the output through. On ANY cache error it falls back to a plain real compile
// and (when FASTCACHE_VERBOSE is set) prints a one-line diagnostic — the build
// never breaks because the cache is unavailable.
//
// Config (environment):
//   FASTCACHE_ADDR       host:port of fastcached (required to use the cache)
//   FASTCACHE_SRCROOT    checkout source root (for keying + canonicalization)
//   FASTCACHE_BUILDTREE  build output root
//   FASTCACHE_COHORT     optional cohort id (default "default")
//   FASTCACHE_VERBOSE    if set, print fall-back diagnostics to stderr
//   FASTCACHE_NO_STATS   if set, do not record invocations to the statistics log
//
// Run `fastcache-cc --help` for the flag and environment reference, and
// `--stats` for the recorded per-machine cache statistics.
//
// Contains no project-specific data; it compiles whatever it is pointed at.

#include "CacheKey.hpp"
#include "CmdLine.hpp"
#include "DirectManifest.hpp"
#include "Stats.hpp"

#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Endian.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <fcntl.h>
    #include <io.h>
    #include <windows.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
#endif

namespace
{
using namespace FastCache;

// Supplied by the build; defaulted so the tool still compiles standalone.
#if !defined(FASTCACHE_CC_VERSION)
    #define FASTCACHE_CC_VERSION "unknown"
#endif

constexpr std::byte CompileCacheMagic { 0xFC };
constexpr std::byte OpStore { 0x01 };
constexpr std::byte OpFetch { 0x02 };
constexpr std::byte StatusOk { 0x01 };

// --- config ----------------------------------------------------------------

struct Config
{
    std::string addr;
    std::string srcRoot;
    std::string buildTree;
    std::string cohort { "default" };
    bool verbose { false };
    bool stats { true };  ///< Record each invocation to the per-user log.
    bool direct { true }; ///< Try the manifest shortcut before preprocessing.
};

/// Read an environment variable, or a fallback when unset/empty. Uses the
/// secure CRT `getenv_s` on Windows so the build stays warning-clean under /WX.
[[nodiscard]] std::string EnvOr(char const* name, std::string_view fallback)
{
#if defined(_WIN32)
    std::size_t size = 0;
    if (::getenv_s(&size, nullptr, 0, name) != 0 || size == 0)
        return std::string { fallback };
    std::string buffer(size, '\0');
    if (::getenv_s(&size, buffer.data(), buffer.size(), name) != 0)
        return std::string { fallback };
    buffer.resize(size > 0 ? size - 1 : 0); // drop the trailing NUL
    return buffer.empty() ? std::string { fallback } : buffer;
#else
    char const* v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? std::string { v } : std::string { fallback };
#endif
}

/// Whether an environment variable is set (to any non-empty value).
[[nodiscard]] bool EnvSet(char const* name)
{
    return !EnvOr(name, "").empty();
}

[[nodiscard]] Config LoadConfig()
{
    Config c;
    c.addr = EnvOr("FASTCACHE_ADDR", "");
    c.srcRoot = EnvOr("FASTCACHE_SRCROOT", "");
    c.buildTree = EnvOr("FASTCACHE_BUILDTREE", "");
    c.cohort = EnvOr("FASTCACHE_COHORT", "default");
    c.verbose = EnvSet("FASTCACHE_VERBOSE");
    c.stats = !EnvSet("FASTCACHE_NO_STATS");
    c.direct = !EnvSet("FASTCACHE_NO_DIRECT");
    return c;
}

bool g_verbose = false;

/// What this invocation ended up doing, for the statistics log. Collected here
/// rather than returned through the call chain so the cache flow keeps its
/// "exit code or fall back" shape; main() writes the record once, at the end.
Cc::Outcome g_outcome = Cc::Outcome::Unavailable;
std::string g_outcomeDetail;
std::uint64_t g_valueBytes = 0;

/// Phase timings for the statistics record, accumulated as the flow proceeds.
/// Same rationale as the outcome globals above: keeps the cache flow's signature
/// unchanged while letting main() write one complete record at the end.
std::uint64_t g_preprocessMs = 0;
std::uint64_t g_cacheMs = 0;

/// Direct-mode accounting: how long the manifest shortcut took, and whether it
/// succeeded (so the report can separate a direct hit from a preprocessed one).
std::uint64_t g_directMs = 0;
bool g_directHit = false;

/// Milliseconds elapsed since `start`, for the phase counters above.
[[nodiscard]] std::uint64_t MsSince(std::chrono::steady_clock::time_point start)
{
    auto const delta = std::chrono::steady_clock::now() - start;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
}

/// Print a one-line fall-back diagnostic when FASTCACHE_VERBOSE is set.
void Warn(std::string_view reason)
{
    // A fall-back reason distinguishes "deliberately not cacheable" from "the
    // cache let us down" — the two need different responses, so the statistics
    // report separates them.
    bool const deliberate = reason.starts_with("uses __TIME__");
    g_outcome = deliberate ? Cc::Outcome::Uncacheable : Cc::Outcome::Unavailable;
    g_outcomeDetail = reason;
    if (g_verbose)
        std::cerr << "fastcache-cc: cache unavailable (" << reason << "); running real compiler\n";
}

/// Emit a HIT/MISS trace line (stderr) when FASTCACHE_VERBOSE is set. Useful in
/// real use to see the cache working, and the signal the E2E harness asserts on.
void TraceOutcome(std::string_view outcome, std::string_view key)
{
    g_outcome = (outcome == "HIT") ? Cc::Outcome::Hit : Cc::Outcome::Miss;
    g_outcomeDetail.clear();
    if (g_verbose)
        std::cerr << "fastcache-cc: " << outcome << " key=" << key << '\n';
}

// --- shell quoting / process exec ------------------------------------------

/// Quote one argument for a Windows command line following the CommandLineToArgvW
/// rules: backslashes are literal except when they precede a `"`, where each
/// must be doubled, and a run of backslashes immediately before the closing
/// quote must also be doubled — otherwise a path ending in `\` (common for
/// MSVC/SDK include dirs) would escape the closing quote and split the arg.
/// @param a The raw argument.
/// @return The argument, quoted if it contains whitespace or a quote.
[[nodiscard]] std::string Quote(std::string_view a)
{
    if (!a.empty() && a.find_first_of(" \t\"") == std::string_view::npos)
        return std::string { a };
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char const c: a)
    {
        if (c == '\\')
        {
            ++backslashes;
            out += c;
            continue;
        }
        if (c == '"')
        {
            // Double the run of backslashes that precede this quote, then escape it.
            out.append(backslashes, '\\');
            out += '\\';
            out += '"';
        }
        else
        {
            out += c;
        }
        backslashes = 0;
    }
    // Double any trailing backslashes so they do not escape the closing quote.
    out.append(backslashes, '\\');
    out += '"';
    return out;
}

/// Join argv into a single command-line string.
[[nodiscard]] std::string JoinCommand(std::span<std::string const> argv)
{
    std::string cmd;
    for (auto const& a: argv)
    {
        if (!cmd.empty())
            cmd += ' ';
        cmd += Quote(a);
    }
    return cmd;
}

/// Combined-output capture (stdout+stderr merged via 2>&1). Used only where the
/// separation does not matter — preprocessing and the compiler-id probe.
/// @return (exit code, combined output). Exit -1 means the process could not
///         be spawned.
[[nodiscard]] std::pair<int, std::string> RunCaptureCombined(std::string const& command)
{
    std::string out;
#if defined(_WIN32)
    FILE* pipe = _popen((command + " 2>&1").c_str(), "rb");
#else
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
    if (pipe == nullptr)
        return { -1, {} };
    std::array<char, 4096> buf {};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0)
        out.append(buf.data(), n);
#if defined(_WIN32)
    int const code = _pclose(pipe);
#else
    int const code = pclose(pipe);
#endif
    return { code, out };
}

/// The result of running the real compiler with stdout and stderr captured
/// separately, so a cache hit can replay each on its correct stream.
struct CompileRun
{
    int exitCode { -1 };
    std::string out; ///< captured stdout
    std::string err; ///< captured stderr
};

#if defined(_WIN32)
/// Drain a pipe read-end fully into `dst`.
void DrainPipe(HANDLE readEnd, std::string& dst)
{
    std::array<char, 4096> buf {};
    DWORD n = 0;
    while (ReadFile(readEnd, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0)
        dst.append(buf.data(), n);
}
#endif

/// Run the real compiler with stdout and stderr captured SEPARATELY. Full
/// fidelity: a hit can replay each stream on its own channel (warnings on
/// stderr, etc.). Uses CreateProcess + two pipes on Windows.
/// @param argv Full invocation (argv[0] = compiler).
/// @return exit code + captured streams (exitCode -1 on spawn failure).
[[nodiscard]] CompileRun RunCaptureSplit(std::span<std::string const> argv)
{
    CompileRun result;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outR = nullptr;
    HANDLE outW = nullptr;
    HANDLE errR = nullptr;
    HANDLE errW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0))
        return result;
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outW;
    si.hStdError = errW;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::string cmd = JoinCommand(argv);
    std::vector<char> mutableCmd { cmd.begin(), cmd.end() };
    mutableCmd.push_back('\0');

    PROCESS_INFORMATION pi {};
    BOOL const ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    // Parent closes the write ends so its reads see EOF when the child exits.
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok)
    {
        CloseHandle(outR);
        CloseHandle(errR);
        return result;
    }
    // Drain both pipes CONCURRENTLY. Sequential draining deadlocks (and then
    // captures a truncated, run-varying amount) whenever the not-yet-drained
    // stream fills its ~64 KB pipe buffer — which preprocessed output routinely
    // does. A truncated/varying stdout capture would make the cache key
    // nondeterministic and defeat all caching, so this must be correct.
    std::thread errThread { [&] { DrainPipe(errR, result.err); } };
    DrainPipe(outR, result.out);
    errThread.join();
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit = 0;
    GetExitCodeProcess(pi.hProcess, &exit);
    result.exitCode = static_cast<int>(exit);
    CloseHandle(outR);
    CloseHandle(errR);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    static_cast<void>(argv);
#endif
    return result;
}

/// Replay captured child bytes on one of our own streams verbatim.
///
/// Writes through the C `FILE*` in binary mode rather than `std::cout`/`std::cerr`:
/// the captured bytes already contain the compiler's own CRLF line endings, and a
/// text-mode stream would translate each '\n' again, emitting CR CR LF. That shows
/// up as a blank line between every real line and corrupts the /showIncludes bytes
/// Ninja parses for dependencies.
void ReplayVerbatim(std::string_view bytes, std::FILE* stream)
{
    if (bytes.empty())
        return;
#if defined(_WIN32)
    int const previousMode = _setmode(_fileno(stream), _O_BINARY);
#endif
    std::fwrite(bytes.data(), 1, bytes.size(), stream);
    std::fflush(stream);
#if defined(_WIN32)
    if (previousMode != -1)
        _setmode(_fileno(stream), previousMode);
#endif
}

/// Replay a captured stdout/stderr pair verbatim on our own streams.
void ReplayStreams(std::string_view out, std::string_view err)
{
    // Flush the iostreams first: our own diagnostics go through std::cerr, and
    // mixing them with these raw writes would otherwise reorder the output.
    std::cout.flush();
    std::cerr.flush();
    ReplayVerbatim(out, stdout);
    ReplayVerbatim(err, stderr);
}

/// Run the real compiler with the given argv, streaming its combined output to
/// our stdout, and return its exit code. Used for both fallback and miss when
/// we do NOT need to capture (fallback) — the miss path uses RunCapture.
[[nodiscard]] int RunPassthrough(std::span<std::string const> argv)
{
    auto const run = RunCaptureSplit(argv);
    ReplayStreams(run.out, run.err);
    return run.exitCode == -1 ? 1 : run.exitCode;
}

// --- file helpers -----------------------------------------------------------

[[nodiscard]] std::optional<std::vector<std::byte>> ReadFileBytes(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary };
    if (!in)
        return std::nullopt;
    std::vector<char> raw { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    std::vector<std::byte> bytes;
    bytes.reserve(raw.size());
    for (char const c: raw)
        bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

[[nodiscard]] bool WriteFileBytes(std::filesystem::path const& path, std::span<std::byte const> bytes)
{
    std::ofstream out { path, std::ios::binary };
    if (!out)
        return false;
    out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

// --- minimal blocking TCP client -------------------------------------------

class TcpClient
{
  public:
    [[nodiscard]] static std::optional<TcpClient> Connect(std::string_view hostPort)
    {
        auto const colon = hostPort.rfind(':');
        if (colon == std::string_view::npos)
            return std::nullopt;
        std::string const host { hostPort.substr(0, colon) };
        std::string const portStr { hostPort.substr(colon + 1) };
#if defined(_WIN32)
        WSADATA wsa {};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return std::nullopt;
        SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCKET)
        {
            WSACleanup();
            return std::nullopt;
        }
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(std::atoi(portStr.c_str())));
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1
            || connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            closesocket(fd);
            WSACleanup();
            return std::nullopt;
        }
        return TcpClient { fd };
#else
        return std::nullopt;
#endif
    }

    ~TcpClient()
    {
#if defined(_WIN32)
        if (_fd != INVALID_SOCKET)
        {
            closesocket(_fd);
            WSACleanup();
        }
#endif
    }

    TcpClient(TcpClient const&) = delete;
    TcpClient& operator=(TcpClient const&) = delete;
    TcpClient(TcpClient&& o) noexcept
    {
#if defined(_WIN32)
        _fd = o._fd;
        o._fd = INVALID_SOCKET;
#endif
    }
    TcpClient& operator=(TcpClient&&) = delete;

    [[nodiscard]] bool SendAll(std::span<std::byte const> bytes)
    {
#if defined(_WIN32)
        std::size_t sent = 0;
        while (sent < bytes.size())
        {
            int const n =
                send(_fd, reinterpret_cast<char const*>(bytes.data()) + sent, static_cast<int>(bytes.size() - sent), 0);
            if (n <= 0)
                return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
#else
        return false;
#endif
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> RecvExactly(std::size_t count)
    {
        std::vector<std::byte> out(count);
#if defined(_WIN32)
        std::size_t got = 0;
        while (got < count)
        {
            int const n = recv(_fd, reinterpret_cast<char*>(out.data()) + got, static_cast<int>(count - got), 0);
            if (n <= 0)
                return std::nullopt;
            got += static_cast<std::size_t>(n);
        }
#endif
        return out;
    }

  private:
#if defined(_WIN32)
    explicit TcpClient(SOCKET fd):
        _fd(fd)
    {
    }
    SOCKET _fd { INVALID_SOCKET };
#endif
};

// --- protocol framing helpers ----------------------------------------------

void AppendU32(std::vector<std::byte>& out, std::uint32_t n)
{
    std::array<std::byte, sizeof(std::uint32_t)> buf {};
    WriteBigEndian<std::uint32_t>(buf, n);
    out.insert(out.end(), buf.begin(), buf.end());
}

void AppendField(std::vector<std::byte>& out, std::string_view s)
{
    AppendU32(out, static_cast<std::uint32_t>(s.size()));
    auto const* p = reinterpret_cast<std::byte const*>(s.data());
    out.insert(out.end(), p, p + s.size());
}

void AppendField(std::vector<std::byte>& out, std::span<std::byte const> bytes)
{
    AppendU32(out, static_cast<std::uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// The grammar to tag the include-bearing stream with, per compiler flavor.
[[nodiscard]] PathCanon::Grammar IncludeGrammar()
{
    return PathCanon::Grammar::ShowIncludes;
}

/// True if the source file at `path` references a compile-time-varying macro
/// (`__TIME__` / `__DATE__` / `__TIMESTAMP__`). Such a TU cannot be cached
/// because it re-keys every second. Reads the file text and looks for the
/// macro token as a substring — a superset check: a false positive only costs
/// a (correct) cache skip, never incorrectness.
/// @param path Source file path.
/// @return True if any volatile macro token appears in the file.
[[nodiscard]] bool SourceReferencesVolatileMacro(std::string const& path)
{
    auto const bytes = ReadFileBytes(std::filesystem::path { path });
    if (!bytes.has_value())
        return false; // cannot read → let the normal flow handle it
    std::string_view const text { reinterpret_cast<char const*>(bytes->data()), bytes->size() };
    for (std::string_view const macro: { "__TIME__", "__DATE__", "__TIMESTAMP__" })
        if (text.find(macro) != std::string_view::npos)
            return true;
    return false;
}

// --- preprocess + compiler identity ----------------------------------------

/// Preprocess the source (compiler-native, no line markers) for the cache key.
/// cl/clang-cl both support `/EP /P`-style, but `/EP` alone writes preprocessed
/// output to stdout without `#line` directives — ideal for a stable key.
[[nodiscard]] std::optional<std::string> Preprocess(Cc::ParsedCommand const& cmd, std::span<std::string const> originalArgs)
{
    // Rebuild the argv replacing the compile action with preprocess-to-stdout.
    // /EP = preprocess to stdout, no #line. /c and /Fo are dropped.
    std::vector<std::string> pp;
    pp.push_back(cmd.compiler);
    pp.emplace_back("/nologo");
    pp.emplace_back("/EP");
    for (auto const& a: originalArgs.subspan(1)) // skip compiler
    {
        if (a == "/c" || a == "/showIncludes" || a.starts_with("/Fo") || a == "/Fo" || a == "-o" || a == "-c")
            continue;
        pp.push_back(a);
    }
    // Capture stdout ONLY. Merging stderr (2>&1) would fold the compiler's
    // diagnostic lines into the hashed text, and the interleave point of two
    // independently-buffered streams is not stable run-to-run — which would
    // make the key nondeterministic and defeat all caching. The preprocessed
    // source is on stdout; stderr (warnings) is irrelevant to the key.
    auto const run = RunCaptureSplit(pp);
    if (run.exitCode != 0)
        return std::nullopt;
    return run.out;
}

/// A stable-ish compiler identity: its version banner. cl prints it on /? or on
/// a bare invocation; we use the first line of `<compiler> --version`-ish. To
/// stay cheap and portable we just use the compiler basename + the banner cl
/// emits to stderr when run with no input. Best-effort; folded into the key.
[[nodiscard]] std::string CompilerId(std::string const& compiler)
{
    // `cl` with no args prints its version banner (to stderr). clang-cl honours
    // `--version`. Try --version first; fall back to the basename.
    auto const [code, out] = RunCaptureCombined(Quote(compiler) + " --version");
    if (code == 0 && !out.empty())
        return out.substr(0, out.find('\n'));
    auto const slash = compiler.find_last_of("/\\");
    return slash == std::string::npos ? compiler : compiler.substr(slash + 1);
}

// --- the cache flow ---------------------------------------------------------

/// FETCH one key and return its raw stored bytes, or nullopt on miss or any
/// transport failure. Used for manifests, whose payload is not a compile-value.
/// @param addr Daemon address.
/// @param key  The key to fetch.
/// @return The stored bytes on hit.
[[nodiscard]] std::optional<std::vector<std::byte>> FetchRaw(std::string const& addr, std::string const& key)
{
    auto client = TcpClient::Connect(addr);
    if (!client.has_value())
        return std::nullopt;

    std::vector<std::byte> frame;
    frame.push_back(CompileCacheMagic);
    frame.push_back(OpFetch);
    AppendField(frame, key);
    if (!client->SendAll(frame))
        return std::nullopt;

    auto const status = client->RecvExactly(1);
    if (!status.has_value() || (*status)[0] != StatusOk)
        return std::nullopt;

    auto const lenBytes = client->RecvExactly(sizeof(std::uint32_t));
    if (!lenBytes.has_value())
        return std::nullopt;
    return client->RecvExactly(ReadBigEndian<std::uint32_t>(*lenBytes));
}

/// STORE bytes under `key`, best-effort. A manifest that fails to store only
/// costs the next compile its shortcut, so failures are silent unless verbose.
///
/// The payload MUST be a compile-value frame: the daemon decodes every STORE as
/// one (to canonicalize its text regions) and rejects anything else outright. A
/// manifest therefore travels as a compile-value whose object blob is the manifest
/// and whose text-region list is empty — nothing to canonicalize, and no protocol
/// change needed.
/// @param addr Daemon address.
/// @param cfg  Launcher config (cohort and layout travel with the store).
/// @param key  The key to store under.
/// @param body The bytes to store.
void StoreRaw(std::string const& addr, Config const& cfg, std::string const& key, std::string_view body)
{
    auto client = TcpClient::Connect(addr);
    if (!client.has_value())
        return;

    std::vector<std::byte> frame;
    frame.push_back(CompileCacheMagic);
    frame.push_back(OpStore);
    AppendField(frame, key);
    AppendField(frame, cfg.cohort);
    AppendField(frame, cfg.srcRoot);
    AppendField(frame, cfg.buildTree);
    AppendField(frame, std::span<std::byte const> { reinterpret_cast<std::byte const*>(body.data()), body.size() });
    if (!client->SendAll(frame))
        return;

    // Check the ack rather than discarding it: a rejected STORE is silent otherwise,
    // and a manifest that never lands makes direct mode look simply ineffective.
    auto const ack = client->RecvExactly(1);
    if (g_verbose && (!ack.has_value() || (*ack)[0] != StatusOk))
        std::cerr << "fastcache-cc: STORE rejected (raw) key=" << key << " bytes=" << body.size() << '\n';
}

/// Fetch `key`, and if it holds a compile value, materialize it: write the object
/// and replay the captured streams with paths localized to this machine.
/// @param cfg    Launcher config.
/// @param cmd    The parsed compile command (object path, source).
/// @param key    The object key to serve.
/// @param layout This machine's roots, for localizing the replayed text.
/// @return The exit code to return on a hit, or nullopt when not served.
[[nodiscard]] std::optional<int> TryServeFromCache(Config const& cfg,
                                                   Cc::ParsedCommand const& cmd,
                                                   std::string const& key,
                                                   PathCanon::Layout const& layout)
{
    auto const payload = FetchRaw(cfg.addr, key);
    if (!payload.has_value())
        return std::nullopt;

    auto decoded = DecodeCompileValue(*payload);
    if (!decoded.has_value())
        return std::nullopt;
    if (!WriteFileBytes(cmd.objPath, decoded->objectBlob))
        return std::nullopt;

    // Region 0 = stdout, region 1 = stderr, per the STORE ordering.
    std::string_view replayOut;
    std::string_view replayErr;
    std::array<std::string, 2> localizedText;
    for (std::size_t idx = 0; idx < decoded->textRegions.size() && idx < localizedText.size(); ++idx)
    {
        auto const& region = decoded->textRegions[idx];
        auto localized = PathCanon::LocalizeRegion(region.bytes, region.grammar, layout);
        localizedText[idx] = localized.has_value() ? std::move(*localized) : region.bytes;
        if (idx == 0)
            replayOut = localizedText[idx];
        else
            replayErr = localizedText[idx];
    }
    ReplayStreams(replayOut, replayErr);
    g_valueBytes = decoded->objectBlob.size();
    TraceOutcome("HIT", key);
    return 0;
}

/// Record the direct-mode manifest for a compile whose object bytes are already
/// stored, so the next compile of this TU can skip preprocessing.
///
/// Called from BOTH the hit and the miss path. On a miss the include text comes
/// from the compiler that just ran; on a hit it comes from the cached value's
/// replayed streams — either way it names the same headers, and neither requires
/// an extra compiler invocation. The object is stored a second time under the
/// manifest-derived key so the direct path resolves without depending on the
/// preprocessed key existing.
/// @param cfg             Launcher config.
/// @param cmd             The parsed compile command.
/// @param layout          This machine's roots.
/// @param relativizedArgs The relativized compile arguments.
/// @param toolchainStamp  The toolchain identity.
/// @param includeTextOut      Captured stdout (may carry the include notes).
/// @param includeTextErr      Captured stderr (cl puts include notes here).
/// @param objectKeyForPointer Key the object is already stored under; recorded in
///                            the manifest so the object is never stored twice.
void RecordManifest(Config const& cfg,
                    Cc::ParsedCommand const& cmd,
                    PathCanon::Layout const& layout,
                    std::vector<std::string> const& relativizedArgs,
                    std::string const& toolchainStamp,
                    std::string_view includeTextOut,
                    std::string_view includeTextErr,
                    std::string_view objectKeyForPointer)
{
    auto const canonicalSource = PathCanon::Canonicalize(cmd.source, layout);
    if (!canonicalSource.has_value())
        return;

    // Either stream may carry the notes (clang-cl uses stdout, cl uses stderr);
    // parse both rather than guessing which compiler produced this value.
    auto includes = Cc::ParseIncludePaths(includeTextOut);
    auto const fromErr = Cc::ParseIncludePaths(includeTextErr);
    includes.insert(includes.end(), fromErr.begin(), fromErr.end());
    if (includes.empty())
        return;

    // The manifest points at the object's ordinary key rather than causing a second
    // copy to be stored: L1 keeps values uncompressed, so duplicating objects would
    // double RAM pressure (27 GB -> 54 GB measured) where compression cannot help.
    auto const manifest = Cc::BuildManifest(includes, layout, toolchainStamp, objectKeyForPointer);
    if (!manifest.has_value())
    {
        if (g_verbose)
            std::cerr << "fastcache-cc: manifest not built (uncanonicalizable include)\n";
        return;
    }

    auto const manifestKey = Cc::ComputeManifestKey(*canonicalSource, relativizedArgs, toolchainStamp);

    // Wrap the manifest as a text-region-free compile value so the daemon's STORE
    // (which decodes every payload as one) accepts it.
    auto const manifestEncoded = Cc::EncodeManifest(*manifest);
    CompileValue manifestValue;
    manifestValue.objectBlob.assign(reinterpret_cast<std::byte const*>(manifestEncoded.data()),
                                    reinterpret_cast<std::byte const*>(manifestEncoded.data()) + manifestEncoded.size());
    auto const manifestFrame = EncodeCompileValue(manifestValue);
    StoreRaw(cfg.addr,
             cfg,
             manifestKey,
             std::string_view { reinterpret_cast<char const*>(manifestFrame.data()), manifestFrame.size() });
    if (g_verbose)
        std::cerr << "fastcache-cc: MANIFEST stored key=" << manifestKey << " entries=" << manifest->entries.size() << '\n';
}

/// Try to serve `cmd` from the cache; returns the process exit code if handled
/// (hit or miss-then-stored), or std::nullopt to signal "fall back to a plain
/// real compile" (any cache error).
[[nodiscard]] std::optional<int> RunCached(Config const& cfg,
                                           Cc::ParsedCommand const& cmd,
                                           std::span<std::string const> argv)
{
    if (cfg.addr.empty() || cfg.srcRoot.empty() || cfg.buildTree.empty())
    {
        Warn("missing FASTCACHE_ADDR/SRCROOT/BUILDTREE");
        return std::nullopt;
    }

    PathCanon::Layout const layout { .sourceRoot = cfg.srcRoot, .buildTree = cfg.buildTree };
    auto const relativizedArgs = Cc::RelativizeArgs(argv.subspan(1), cfg.srcRoot, cfg.buildTree);
    auto const toolchainStamp = CompilerId(cmd.compiler);

    // DIRECT MODE. Preprocessing a translation unit to derive its key costs ~1.4 s
    // on this codebase (it expands ~25 MB of headers); re-hashing the project
    // headers a previous compile recorded costs ~18 ms. So before preprocessing,
    // try to reach the object through a stored manifest instead.
    if (cfg.direct && !SourceReferencesVolatileMacro(cmd.source))
    {
        auto const directStarted = std::chrono::steady_clock::now();
        auto const canonicalSource = PathCanon::Canonicalize(cmd.source, layout);
        if (canonicalSource.has_value())
        {
            auto const manifestKey = Cc::ComputeManifestKey(*canonicalSource, relativizedArgs, toolchainStamp);
            auto manifestBytes = FetchRaw(cfg.addr, manifestKey);
            if (manifestBytes.has_value())
            {
                // Unwrap the compile-value envelope the manifest was stored in.
                auto const envelope = DecodeCompileValue(*manifestBytes);
                auto const manifestSpan = envelope.has_value() ? std::span<std::byte const> { envelope->objectBlob }
                                                               : std::span<std::byte const> {};
                auto manifest = Cc::DecodeManifest(
                    std::string_view { reinterpret_cast<char const*>(manifestSpan.data()), manifestSpan.size() });
                if (manifest.has_value() && !manifest->objectKey.empty()
                    && Cc::ValidateManifest(*manifest, layout, toolchainStamp))
                {
                    g_directMs = MsSince(directStarted);
                    // Follow the manifest's pointer to the object, which is stored
                    // exactly once under its ordinary preprocessed key.
                    if (auto const served = TryServeFromCache(cfg, cmd, manifest->objectKey, layout))
                    {
                        g_directHit = true;
                        return *served;
                    }
                }
            }
        }
        // Any failure here just means we preprocess as before; direct mode never
        // decides a compile is uncacheable, only that it cannot shortcut.
        g_directMs = MsSince(directStarted);
    }

    auto const preprocessStarted = std::chrono::steady_clock::now();
    auto const preprocessed = Preprocess(cmd, argv);
    if (!preprocessed.has_value())
    {
        Warn("preprocess failed");
        return std::nullopt;
    }

    // Skip translation units that reference a time/date macro. `__TIME__` /
    // `__DATE__` / `__TIMESTAMP__` expand to a run-varying (second-granular)
    // string, so such a TU re-keys on every compile and can never hit —
    // caching it only churns the store. Preprocessing has already *expanded*
    // the macro (its name is gone from the output), so we scan the source file
    // text itself, matching sccache's refusal to cache these. Direct use in the
    // TU is the overwhelmingly common case; header-introduced use is rare and
    // its only cost is a permanent miss, never incorrectness.
    if (SourceReferencesVolatileMacro(cmd.source))
    {
        Warn("uses __TIME__/__DATE__/__TIMESTAMP__; not caching (non-deterministic)");
        return std::nullopt;
    }

    Cc::KeyInputs const inputs {
        .compilerId = toolchainStamp,
        .preprocessed = *preprocessed,
        .relativizedArgs = relativizedArgs,
    };
    std::string const key = Cc::ComputeKey(inputs);
    g_preprocessMs = MsSince(preprocessStarted);

    auto const cacheStarted = std::chrono::steady_clock::now();

    // FETCH.
    {
        auto client = TcpClient::Connect(cfg.addr);
        if (!client.has_value())
        {
            Warn("connect failed");
            return std::nullopt;
        }
        std::vector<std::byte> frame;
        frame.push_back(CompileCacheMagic);
        frame.push_back(OpFetch);
        AppendField(frame, key);
        if (!client->SendAll(frame))
        {
            Warn("fetch send failed");
            return std::nullopt;
        }
        auto const status = client->RecvExactly(1);
        if (!status.has_value())
        {
            Warn("fetch recv failed");
            return std::nullopt;
        }
        if ((*status)[0] == StatusOk)
        {
            auto const lenBytes = client->RecvExactly(sizeof(std::uint32_t));
            if (!lenBytes.has_value())
            {
                Warn("fetch length recv failed");
                return std::nullopt;
            }
            auto const payload = client->RecvExactly(ReadBigEndian<std::uint32_t>(*lenBytes));
            if (!payload.has_value())
            {
                Warn("fetch payload recv failed");
                return std::nullopt;
            }
            auto decoded = DecodeCompileValue(*payload);
            if (!decoded.has_value())
            {
                Warn("fetch decoded malformed");
                return std::nullopt;
            }

            // HIT: write the object, replay the streams (localizing paths).
            if (!WriteFileBytes(cmd.objPath, decoded->objectBlob))
            {
                Warn("could not write object on hit");
                return std::nullopt;
            }
            PathCanon::Layout const consumer { .sourceRoot = cfg.srcRoot, .buildTree = cfg.buildTree };
            // Region 0 = stdout, region 1 = stderr (see STORE below).
            std::string_view replayOut;
            std::string_view replayErr;
            std::array<std::string, 2> localizedText;
            for (std::size_t idx = 0; idx < decoded->textRegions.size() && idx < localizedText.size(); ++idx)
            {
                auto const& region = decoded->textRegions[idx];
                auto localized = PathCanon::LocalizeRegion(region.bytes, region.grammar, consumer);
                localizedText[idx] = localized.has_value() ? std::move(*localized) : region.bytes;
                if (idx == 0)
                    replayOut = localizedText[idx];
                else
                    replayErr = localizedText[idx];
            }
            ReplayStreams(replayOut, replayErr);
            g_valueBytes = decoded->objectBlob.size();
            g_cacheMs = MsSince(cacheStarted);

            // Backfill the direct-mode manifest from the hit we just served.
            //
            // Without this, direct mode could never populate on a cache that already
            // holds preprocessed-key entries: manifests would only ever be written by
            // the miss path, so a warm cache would preprocess forever. The localized
            // /showIncludes text names exactly the headers this object depends on, so
            // no compiler run is needed to record them.
            if (cfg.direct)
                RecordManifest(cfg, cmd, layout, relativizedArgs, toolchainStamp, replayOut, replayErr, key);

            TraceOutcome("HIT", key);
            return 0;
        }
        // MISS — fall through to compile.
    }
    g_cacheMs = MsSince(cacheStarted);
    TraceOutcome("MISS", key);

    // MISS: run the real compiler with SEPARATE stdout/stderr capture, then STORE.
    auto const run = RunCaptureSplit(argv);
    // Always surface the compiler's output on its true streams and its exit code.
    ReplayStreams(run.out, run.err);
    int const code = run.exitCode;
    if (code != 0)
        return code; // do not cache a failed compile

    auto const objectBytes = ReadFileBytes(cmd.objPath);
    if (!objectBytes.has_value())
    {
        Warn("object missing after compile; not caching");
        return code;
    }

    CompileValue value;
    value.objectBlob = *objectBytes;
    // Two regions, one per stream, in a fixed order (0=stdout, 1=stderr) so the
    // hit path replays each on its correct channel. clang-cl emits
    // /showIncludes on stdout, cl on stderr — we tag BOTH with the ShowIncludes
    // grammar so whichever stream carries include notes gets canonicalized; a
    // non-matching line in either region is preserved verbatim.
    value.textRegions.push_back({ IncludeGrammar(), run.out });
    value.textRegions.push_back({ IncludeGrammar(), run.err });

    auto const encoded = EncodeCompileValue(value);
    auto client = TcpClient::Connect(cfg.addr);
    if (!client.has_value())
    {
        Warn("connect failed for store");
        return code; // compile already succeeded; just skip caching
    }
    std::vector<std::byte> frame;
    frame.push_back(CompileCacheMagic);
    frame.push_back(OpStore);
    AppendField(frame, key);
    AppendField(frame, cfg.cohort);
    AppendField(frame, cfg.srcRoot);
    AppendField(frame, cfg.buildTree);
    AppendField(frame, std::span<std::byte const> { encoded.data(), encoded.size() });
    // Best-effort store: a failure here must never fail the build (the compile
    // already succeeded). Surface the outcome under verbose so store rejections
    // (e.g. a value over the server's --storage-max-value) are diagnosable.
    if (client->SendAll(frame))
    {
        auto const ack = client->RecvExactly(1);
        if (g_verbose)
        {
            if (ack.has_value() && (*ack)[0] == StatusOk)
                std::cerr << "fastcache-cc: STORED key=" << key << " bytes=" << encoded.size() << '\n';
            else
                std::cerr << "fastcache-cc: STORE rejected key=" << key << " bytes=" << encoded.size() << '\n';
        }
    }
    else if (g_verbose)
    {
        std::cerr << "fastcache-cc: STORE send failed key=" << key << '\n';
    }

    // Record the direct-mode manifest so the NEXT compile of this TU can reach the
    // object without preprocessing. The include set comes from the /showIncludes
    // output the compile just produced, so this costs no extra compiler run.
    //
    // The object is stored a second time under the manifest-derived key. That
    // duplication is deliberate: the two keys answer different questions (one from
    // preprocessed text, one from header hashes) and both must resolve to the
    // object, so the direct path never depends on the preprocessed path having run
    // on this machine.
    if (cfg.direct)
        RecordManifest(cfg, cmd, layout, relativizedArgs, toolchainStamp, run.out, run.err, key);
    return code;
}

} // namespace

/// Print usage to `stream`. Written out rather than generated so the wording can
/// explain *why* a flag exists, which a flag table alone cannot.
void PrintHelp(std::ostream& stream)
{
    stream << R"(fastcache-cc - a compiler launcher over the fastcached compile cache.

USAGE
  fastcache-cc <compiler> <args...>     Front a compile (as CMAKE_<LANG>_COMPILER_LAUNCHER).
  fastcache-cc --stats [options]        Report cache statistics for this machine.
  fastcache-cc --clear-stats            Discard the statistics log.
  fastcache-cc --help | --version       This text / the launcher version.

STATS OPTIONS
  --cohort <id>     Report only this cohort (also --cohort=<id>).
  --clear-stats     Discard the log instead of reporting (--reset is a synonym).

ENVIRONMENT
  FASTCACHE_ADDR        host:port of the fastcached daemon. Unset means every
                        compile runs uncached -- the build still succeeds, so
                        check this before concluding the cache is working.
  FASTCACHE_SRCROOT     Checkout source root, for keying and path canonicalization.
  FASTCACHE_BUILDTREE   Build output root.
  FASTCACHE_COHORT      Prefetch grouping id (default "default"). Not part of the
                        cache key, so it never partitions the cache.
  FASTCACHE_VERBOSE     Print HIT/MISS and fall-back diagnostics to stderr.
  FASTCACHE_NO_STATS    Do not record invocations to the statistics log.
  FASTCACHE_NO_DIRECT   Disable direct mode (always preprocess to derive the key).
                        Direct mode is on by default: it reaches a cached object by
                        re-hashing the project headers a previous compile recorded,
                        which is far cheaper than preprocessing the translation unit.

ADDR, SRCROOT and BUILDTREE must ALL be set to cache; any missing one makes the
launcher run the real compiler and report "missing FASTCACHE_ADDR/SRCROOT/BUILDTREE"
under FASTCACHE_VERBOSE.

Any cache error falls back to a plain real compile: caching is an optimization
and never breaks a build.
)";
}

/// Delete the statistics log, reporting the outcome. @return Process exit code.
[[nodiscard]] int ClearStats()
{
    auto const path = Cc::LogPath();
    if (Cc::ResetLog())
    {
        std::cout << "fastcache-cc: statistics cleared" << (path.empty() ? "" : " (") << path << (path.empty() ? "" : ")")
                  << '\n';
        return 0;
    }
    std::cerr << "fastcache-cc: could not clear statistics log";
    if (!path.empty())
        std::cerr << " (" << path << ')';
    std::cerr << '\n';
    return 1;
}

/// Print the statistics report (`--stats`) and return the process exit code.
[[nodiscard]] int RunStatsReport(std::span<std::string const> args)
{
    std::string cohortFilter;
    for (std::size_t i = 1; i < args.size(); ++i)
    {
        if (args[i] == "--cohort" && i + 1 < args.size())
            cohortFilter = args[i + 1];
        else if (args[i].starts_with("--cohort="))
            cohortFilter = args[i].substr(std::string_view { "--cohort=" }.size());
        else if (args[i] == "--reset" || args[i] == "--clear-stats")
            return ClearStats();
    }
    std::cout << Cc::FormatReport(cohortFilter);
    return 0;
}

int main(int argc, char** argv)
{
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 1; i < argc; ++i) // argv[0] is fastcache-cc itself; drop it
        args.emplace_back(argv[i]);

    // No arguments is a usage error (exit 2); an explicit --help is a successful
    // query and prints to stdout so it can be paged or redirected.
    if (args.empty())
    {
        PrintHelp(std::cerr);
        return 2;
    }

    if (args[0] == "--help" || args[0] == "-h" || args[0] == "/?")
    {
        PrintHelp(std::cout);
        return 0;
    }

    if (args[0] == "--version")
    {
        std::cout << "fastcache-cc " << FASTCACHE_CC_VERSION << '\n';
        return 0;
    }

    if (args[0] == "--stats")
        return RunStatsReport(std::span<std::string const> { args });

    if (args[0] == "--clear-stats" || args[0] == "--reset")
        return ClearStats();

    Config const cfg = LoadConfig();
    g_verbose = cfg.verbose;

    auto const cmd = Cc::ParseCommand(std::span<std::string const> { args });
    if (!cmd.parsedOk)
    {
        // Not a cacheable compile (a link or preprocess-only step). Recording it
        // would dilute the hit rate with lines that were never candidates.
        return RunPassthrough(std::span<std::string const> { args });
    }

    auto const started = std::chrono::steady_clock::now();
    auto const handled = RunCached(cfg, cmd, std::span<std::string const> { args });
    int const code = handled.has_value() ? *handled : RunPassthrough(std::span<std::string const> { args });

    if (cfg.stats)
    {
        auto const elapsed = std::chrono::steady_clock::now() - started;
        Cc::AppendRecord({
            .outcome = g_outcome,
            .cohort = cfg.cohort,
            .source = cmd.source,
            .valueBytes = g_valueBytes,
            .elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()),
            .detail = g_outcomeDetail,
            .preprocessMs = g_preprocessMs,
            .cacheMs = g_cacheMs,
            .directMs = g_directMs,
            .directHit = g_directHit,
        });
    }
    return code;
}
