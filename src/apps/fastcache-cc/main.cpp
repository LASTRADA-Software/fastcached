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
#include "IProcessRunner.hpp"
#include "ITcpClient.hpp"
#include "Stats.hpp"

#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Endian.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

// --- process exec -----------------------------------------------------------

/// The process runner used by every spawn in this file. Created once; the
/// concrete implementation (CreateProcess on Windows, posix_spawn elsewhere)
/// is chosen behind the IProcessRunner seam.
[[nodiscard]] Cc::IProcessRunner& ProcessRunner()
{
    static std::unique_ptr<Cc::IProcessRunner> const runner = Cc::MakeProcessRunner();
    return *runner;
}

using Cc::CompileRun;

/// Run `argv` with stdout and stderr captured separately.
/// @param argv Full invocation; argv[0] is the compiler.
/// @return Exit code plus both captured streams.
[[nodiscard]] CompileRun RunCaptureSplit(std::span<std::string const> argv)
{
    return ProcessRunner().RunCaptureSplit(argv);
}

/// Run `argv` with stdout and stderr merged. Used where the split does not
/// matter — the compiler-id probe.
/// @param argv Full invocation; argv[0] is the compiler.
/// @return (exit code, combined output).
[[nodiscard]] std::pair<int, std::string> RunCaptureCombined(std::span<std::string const> argv)
{
    auto const run = ProcessRunner().RunCaptureCombined(argv);
    return { run.exitCode, run.out };
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

// --- TCP client -------------------------------------------------------------

/// Thin owning wrapper over the injected ITcpClient seam, so call sites keep
/// the `TcpClient::Connect(addr)` -> optional shape they already use while the
/// platform socket code lives behind the interface.
class TcpClient
{
  public:
    /// Connect to `hostPort`.
    /// @param hostPort "host:port"; hostnames and [::1]:port are accepted.
    /// @return A connected client, or nullopt when unreachable.
    [[nodiscard]] static std::optional<TcpClient> Connect(std::string_view hostPort)
    {
        auto impl = Cc::ConnectTcp(hostPort);
        if (impl == nullptr)
            return std::nullopt;
        return TcpClient { std::move(impl) };
    }

    /// @see Cc::ITcpClient::SendAll
    [[nodiscard]] bool SendAll(std::span<std::byte const> bytes)
    {
        return _impl->SendAll(bytes);
    }

    /// @see Cc::ITcpClient::RecvExactly
    [[nodiscard]] std::optional<std::vector<std::byte>> RecvExactly(std::size_t count)
    {
        return _impl->RecvExactly(count);
    }

  private:
    explicit TcpClient(std::unique_ptr<Cc::ITcpClient> impl):
        _impl { std::move(impl) }
    {
    }

    std::unique_ptr<Cc::ITcpClient> _impl;
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

/// Index of the depfile inside a stored value's text regions.
///
/// Regions are positional: 0 = stdout, 1 = stderr, 2 = the GNU depfile. A value
/// stored before depfile support simply has no region 2, so it decodes and
/// replays exactly as before — the addition is backward compatible.
constexpr std::size_t DepFileRegionIndex = 2;

/// Number of captured streams replayed to our own stdout/stderr. Regions beyond
/// these are files, not streams, and must never be replayed.
constexpr std::size_t ReplayRegionCount = 2;

/// Read the depfile a GNU-driver compile just wrote, for storing alongside the
/// object.
///
/// A depfile is the build system's record of which headers a translation unit
/// depends on. Ninja and Make read it to decide whether to recompile; without
/// it the TU appears to depend on nothing and stops rebuilding when its headers
/// change. It therefore has to be reproduced on a hit exactly like the object.
///
/// @param cmd The parsed compile command.
/// @return The depfile text, or nullopt when the line requested none (or it is
///         unreadable, in which case there is simply nothing to cache).
[[nodiscard]] std::optional<std::string> ReadDepFile(Cc::ParsedCommand const& cmd)
{
    if (cmd.depPath.empty())
        return std::nullopt;
    auto const bytes = ReadFileBytes(std::filesystem::path { cmd.depPath });
    if (!bytes.has_value())
        return std::nullopt;
    return std::string { reinterpret_cast<char const*>(bytes->data()), bytes->size() };
}

/// Write the cached depfile back, localized to this machine's layout.
///
/// Called on every hit. A miss wrote its own depfile as a side effect of running
/// the real compiler; a hit runs no compiler, so without this the file the build
/// system depends on would simply be absent (or, worse, left stale from an
/// earlier build).
///
/// @param cmd     The parsed compile command (its depPath is the destination).
/// @param regions The decoded value's text regions.
/// @param layout  This machine's roots, for localizing the recorded paths.
/// @return True when there was nothing to do or the write succeeded.
[[nodiscard]] bool RestoreDepFile(Cc::ParsedCommand const& cmd,
                                  std::vector<TextRegion> const& regions,
                                  PathCanon::Layout const& layout)
{
    if (cmd.depPath.empty() || regions.size() <= DepFileRegionIndex)
        return true; // nothing requested, or a value stored before depfile support

    auto const& region = regions[DepFileRegionIndex];
    auto localized = PathCanon::LocalizeRegion(region.bytes, region.grammar, layout);
    std::string const& text = localized.has_value() ? *localized : region.bytes;

    std::ofstream out { std::filesystem::path { cmd.depPath }, std::ios::binary };
    if (!out)
        return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
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
    constexpr auto VolatileMacros = std::to_array<std::string_view>({ "__TIME__", "__DATE__", "__TIMESTAMP__" });
    return std::ranges::any_of(VolatileMacros, [text](std::string_view const macro) { return text.contains(macro); });
}

// --- preprocess + compiler identity ----------------------------------------

/// Preprocess the source (compiler-native, no line markers) for the cache key.
///
/// The per-driver spelling lives in the CmdLine driver table: MSVC drivers use
/// `/EP` (preprocess to stdout with no `#line` directives), GNU drivers `-E`.
/// Either way the compile action and any dependency-writing flags are dropped.
///
/// @param cmd The parsed compile command.
/// @param originalArgs The original full invocation.
/// @return The preprocessed text, or nullopt when the probe failed.
[[nodiscard]] std::optional<std::string> Preprocess(Cc::ParsedCommand const& cmd, std::span<std::string const> originalArgs)
{
    auto const pp = Cc::PreprocessCommand(cmd, originalArgs);
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
    // Passed as argv, not a shell string: the runner spawns the process
    // directly, so a compiler path containing spaces needs no quoting.
    std::array<std::string, 2> const probe { compiler, "--version" };
    auto const [code, out] = RunCaptureCombined(probe);
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

    // The depfile is a file, not a stream: restore it before replaying, so a
    // failure to write it is not reported after the build has already seen the
    // compiler's output.
    if (!RestoreDepFile(cmd, decoded->textRegions, layout))
        return std::nullopt;

    // Region 0 = stdout, region 1 = stderr, per the STORE ordering.
    std::string_view replayOut;
    std::string_view replayErr;
    std::array<std::string, ReplayRegionCount> localizedText;
    for (std::size_t idx = 0; idx < decoded->textRegions.size() && idx < localizedText.size(); ++idx)
    {
        auto const& region = decoded->textRegions[idx];
        auto localized = PathCanon::LocalizeRegion(region.bytes, region.grammar, layout);
        if (localized.has_value())
            localizedText[idx] = *std::move(localized);
        else
            localizedText[idx] = region.bytes;
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

    // GNU drivers report nothing on either stream — their dependencies go to the
    // depfile. Without this, direct mode could never populate on POSIX: every
    // compile would pay for a manifest lookup that always missed.
    if (includes.empty())
        if (auto const depText = ReadDepFile(cmd))
            includes = Cc::ParseDepFilePaths(*depText);

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

/// DIRECT MODE. Preprocessing a translation unit to derive its key costs ~1.4 s
/// on a large codebase (it expands ~25 MB of headers); re-hashing the project
/// headers a previous compile recorded costs ~18 ms. So before preprocessing,
/// try to reach the object through a stored manifest instead.
///
/// Any failure here just means we preprocess as before: direct mode never
/// decides a compile is uncacheable, only that it cannot shortcut.
///
/// @param cfg Launcher config.
/// @param cmd The parsed compile command.
/// @param layout This machine's source-root / build-tree layout.
/// @param relativizedArgs The command line with checkout-rooted paths tokenized.
/// @param toolchainStamp The compiler identity folded into the manifest key.
/// @return The exit code if the object was served, nullopt to keep going.
[[nodiscard]] std::optional<int> TryDirectMode(Config const& cfg,
                                               Cc::ParsedCommand const& cmd,
                                               PathCanon::Layout const& layout,
                                               std::vector<std::string> const& relativizedArgs,
                                               std::string const& toolchainStamp)
{
    auto const directStarted = std::chrono::steady_clock::now();
    // Every early return records how long the attempt took, so the statistics
    // show the cost of a direct-mode miss as well as a direct-mode hit.
    auto const giveUp = [directStarted]() -> std::optional<int> {
        g_directMs = MsSince(directStarted);
        return std::nullopt;
    };

    auto const canonicalSource = PathCanon::Canonicalize(cmd.source, layout);
    if (!canonicalSource.has_value())
        return giveUp();

    auto const manifestKey = Cc::ComputeManifestKey(*canonicalSource, relativizedArgs, toolchainStamp);
    auto const manifestBytes = FetchRaw(cfg.addr, manifestKey);
    if (!manifestBytes.has_value())
        return giveUp();

    // Unwrap the compile-value envelope the manifest was stored in.
    auto const envelope = DecodeCompileValue(*manifestBytes);
    auto const manifestSpan =
        envelope.has_value() ? std::span<std::byte const> { envelope->objectBlob } : std::span<std::byte const> {};
    auto const manifest =
        Cc::DecodeManifest(std::string_view { reinterpret_cast<char const*>(manifestSpan.data()), manifestSpan.size() });
    if (!manifest.has_value() || manifest->objectKey.empty() || !Cc::ValidateManifest(*manifest, layout, toolchainStamp))
        return giveUp();

    g_directMs = MsSince(directStarted);
    // Follow the manifest's pointer to the object, which is stored exactly once
    // under its ordinary preprocessed key.
    auto served = TryServeFromCache(cfg, cmd, manifest->objectKey, layout);
    if (served.has_value())
        g_directHit = true;
    return served;
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

    if (cfg.direct && !SourceReferencesVolatileMacro(cmd.source))
        if (auto served = TryDirectMode(cfg, cmd, layout, relativizedArgs, toolchainStamp))
            return served;

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

            // Reproduce the depfile the compiler would have written. Skipping it
            // silently breaks incremental builds: Ninja/Make would see no header
            // dependencies for this TU and stop rebuilding it when they change.
            if (!RestoreDepFile(cmd, decoded->textRegions, consumer))
            {
                Warn("could not write depfile on hit");
                return std::nullopt;
            }

            // Region 0 = stdout, region 1 = stderr (see STORE below).
            std::string_view replayOut;
            std::string_view replayErr;
            std::array<std::string, ReplayRegionCount> localizedText;
            for (std::size_t idx = 0; idx < decoded->textRegions.size() && idx < localizedText.size(); ++idx)
            {
                auto const& region = decoded->textRegions[idx];
                auto localized = PathCanon::LocalizeRegion(region.bytes, region.grammar, consumer);
                if (localized.has_value())
                    localizedText[idx] = *std::move(localized);
                else
                    localizedText[idx] = region.bytes;
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
    value.textRegions.push_back({ .grammar = IncludeGrammar(), .bytes = run.out });
    value.textRegions.push_back({ .grammar = IncludeGrammar(), .bytes = run.err });

    // Region 2, when present, is the GNU depfile the compile just wrote. It is
    // tagged with the depfile grammar so the daemon canonicalizes the header
    // paths inside it — a depfile full of this machine's absolute paths would be
    // useless (and wrong) when replayed on another checkout.
    if (auto const depText = ReadDepFile(cmd))
        value.textRegions.push_back({ .grammar = PathCanon::Grammar::GccDepfile, .bytes = *depText });

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
