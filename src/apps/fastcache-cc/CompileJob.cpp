// SPDX-License-Identifier: Apache-2.0
#include "CmdLine.hpp"
#include "CompileJob.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <utility>

namespace FastCache::Cc
{

namespace
{
    /// Extensions a job's source name may imply, and nothing else.
    ///
    /// A fixed table rather than "take whatever is after the last dot": the value
    /// reaches a compiler's command line, and an extension is one of the few places
    /// a driver will happily accept something surprising. `.S` is here because
    /// preprocessed assembly is a real translation unit a build system produces.
    constexpr std::array<std::string_view, 7> KnownExtensions {
        ".cpp", ".cc", ".cxx", ".c", ".m", ".mm", ".S",
    };

    /// What a name falls back to when the client's cannot be used.
    constexpr std::string_view DefaultStem = "tu";
    constexpr std::string_view DefaultExtension = ".cpp";

    /// The longest stem a client may ask for.
    ///
    /// A real source name is far shorter; the cap is here because the string comes
    /// off a socket and a path has a limit on every platform this runs on.
    constexpr std::size_t MaxStemLength = 64;

    /// Names Windows resolves to a DEVICE rather than to a file, with or without an
    /// extension: `CON.cpp` opens the console.
    ///
    /// Compared case-insensitively against the stem, which is why the table carries
    /// one spelling each. A worker on POSIX is unaffected and checks anyway -- the
    /// scratch file is written by whichever host is doing the compiling, and a rule
    /// that holds only on the host that happens to be running the test is the kind
    /// this repository keeps finding the hard way.
    constexpr std::array<std::string_view, 22> ReservedDeviceNames {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };

    /// Whether a stem may be used as a file name inside the scratch directory.
    /// @param stem The name without its extension.
    /// @return True when it is safe to create.
    [[nodiscard]] bool IsSafeStem(std::string_view stem)
    {
        if (stem.empty() || stem.size() > MaxStemLength || stem.front() == '.')
            return false;
        constexpr std::string_view Allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-";
        if (!std::ranges::all_of(stem, [&](char c) { return Allowed.contains(c); }))
            return false;
        // Folded through PathCanon's byte rule rather than `std::tolower`, which is
        // locale-dependent: under a Turkish locale `std::tolower('I')` is not `i`,
        // so `LPT1` would be reserved on one worker and allowed on the next.
        std::string folded { stem };
        std::ranges::transform(folded, folded.begin(), [](char c) { return PathCanon::AsciiLower(c); });
        return std::ranges::find(ReservedDeviceNames, folded) == ReservedDeviceNames.end();
    }

    /// Read a whole file as bytes.
    /// Read a whole file as bytes.
    ///
    /// Sized from the stream and read in one call, deliberately NOT through
    /// `std::istreambuf_iterator`. GCC 14 at -O2 inlines that iterator's `sgetc`
    /// and then reports `-Werror=null-dereference` inside `<streambuf>` itself --
    /// a false positive, but one this project cannot silence, since warnings are
    /// errors and the rule is to fix them at the source rather than suppress them.
    /// Seeking is also one allocation and one read instead of a per-character
    /// loop, so the workaround is the better implementation regardless.
    [[nodiscard]] std::optional<std::vector<std::byte>> ReadBytes(std::filesystem::path const& path)
    {
        std::ifstream in { path, std::ios::binary | std::ios::ate };
        if (!in)
            return std::nullopt;
        auto const size = in.tellg();
        if (size < 0)
            return std::nullopt;
        in.seekg(0, std::ios::beg);

        std::vector<std::byte> out(static_cast<std::size_t>(size));
        if (!out.empty() && !in.read(reinterpret_cast<char*>(out.data()), size))
            return std::nullopt;
        return out;
    }
} // namespace

namespace
{
    /// How an allowlist row matches an argument.
    enum class ArgMatch : std::uint8_t
    {
        /// The argument must equal the spelling exactly. Bare switches (`-g`, `/c`)
        /// and the language tokens the client passes as their own argument
        /// (`c++-cpp-output`).
        Exact,
        /// The spelling is a prefix: the argument equals it or continues it with a
        /// fused value or a longer flag name (`-O2` under `-O`, `-std=c++23` under
        /// `-std=`, `-Wall` under `-W`).
        Prefix,
    };

    /// Whether a row admits an argument or carves one back out of a prefix that
    /// admits it.
    enum class ArgRule : std::uint8_t
    {
        /// Accept the argument.
        Allow,
        /// Refuse it, overriding any `Allow` prefix it also matches. Every `Deny` row
        /// is a program-invoking or code-loading flag that shares a leading shape
        /// with an otherwise-safe `Allow` prefix, and each names why below. A `Deny`
        /// is checked before any `Allow`, so the carve-out cannot be out-voted.
        Deny,
    };

    /// One entry in the per-family allowlist of accepted argument shapes.
    struct AllowedArg
    {
        DriverFamily families; ///< Which driver families this row applies to.
        std::string_view spelling;
        ArgMatch match;
        ArgRule rule;
    };

    /// The flag shapes a distributed compile legitimately carries, per family.
    ///
    /// This is the accepted set the file header describes: code generation, language,
    /// preprocessor-define and diagnostic options, which is everything that still
    /// means something once the headers are inlined and the macros expanded. It is
    /// deliberately broader than what `RemoteCompileArgs` emits today — a build using
    /// an option we do not list falls back to a local compile, so too narrow a table
    /// is a silent performance regression, while too broad a one is the hole this
    /// file exists to close.
    ///
    /// **No blanket prefix admits a program.** A naive `-f` prefix re-admits
    /// `-fplugin=`; a naive `-W` prefix re-admits `-Wa,`/`-Wl,`/`-Wp,`, which hand
    /// options to the assembler, linker and preprocessor; a naive `-X` prefix
    /// re-admits `-Xclang -load`. So the families that contain a program-invoking
    /// member carry an explicit `Deny` row for exactly that member, checked first, and
    /// `-X` is not a prefix at all — nothing under it is code generation, so
    /// `-Xclang`, `-Xassembler`, `-Xlinker` and their kin are simply not listed and
    /// refused by default. Anything path-valued (`-I`, `-isystem`, `-include`, `-B`,
    /// `--sysroot`, `-specs=`, MSVC `/Fo`, `/FI`, `@response`) is likewise absent, so
    /// it too is refused.
    ///
    /// A `Deny` row is not a denylist in the sense this file rejects: the default here
    /// is refusal, so a program-invoking flag we forget to `Deny` under an `Allow`
    /// prefix is the only residual risk, and it is bounded to the handful of prefixes
    /// with a dangerous member. The rulebook records that these prefixes are audited
    /// against upstream when a new code-loading `-f` or sub-tool pass-through appears.
    constexpr std::array AllowedArgs {
        // -- the target the CLIENT states so this worker cannot pick its own. Both
        // families: clang-cl (MSVC family) is handed `--target=<triple>` first, and a
        // GNU clang the same. Refusing it would make every dispatched clang compile a
        // RejectedArgument the moment the pin lands.
        AllowedArg {
            .families = DriverFamily::Any, .spelling = "--target=", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },

        // -- GNU: gcc / g++ / clang / clang++ ---------------------------------------
        // Code-generation and standard selection. Nothing dangerous shares these
        // prefixes.
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-O", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-g", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-std=", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-m", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-D", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-U", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        // Warnings. `-W` is an `Allow` prefix, but three `-W*` spellings are not
        // warnings at all — they hand a comma-separated option list to a sub-tool —
        // so each is a `Deny`, checked first.
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-W", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-Wa,", .match = ArgMatch::Prefix, .rule = ArgRule::Deny },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-Wl,", .match = ArgMatch::Prefix, .rule = ArgRule::Deny },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-Wp,", .match = ArgMatch::Prefix, .rule = ArgRule::Deny },
        // Feature flags. `-f` is an `Allow` prefix, and `-fplugin` / `-fplugin-arg-`
        // load a shared object into the driver, so `-fplugin` is a `Deny` covering
        // both.
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-f", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Gnu, .spelling = "-fplugin", .match = ArgMatch::Prefix, .rule = ArgRule::Deny },
        // Bare switches that carry no value and reach no file.
        AllowedArg {
            .families = DriverFamily::Gnu, .spelling = "-pthread", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Gnu, .spelling = "-pedantic", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu,
                     .spelling = "-pedantic-errors",
                     .match = ArgMatch::Exact,
                     .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-ansi", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-w", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-pg", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-pipe", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-undef", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Gnu, .spelling = "-trigraphs", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Gnu, .spelling = "-nostdinc", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Gnu, .spelling = "-nostdinc++", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-c", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        // The language, as the client states it for a preprocessed input: `-x`
        // followed by one of the `*-cpp-output` tokens, which arrive as their own
        // argument (`RemoteCompileArgs` appends `PreprocessedInputFlagsFor`). `-x` is
        // an `Allow` prefix so the fused `-xc++-cpp-output` form is covered too, and
        // the bare value tokens are `Exact` rows because they carry no introducer.
        AllowedArg { .families = DriverFamily::Gnu, .spelling = "-x", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Gnu, .spelling = "cpp-output", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Gnu, .spelling = "c++-cpp-output", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu,
                     .spelling = "objective-c-cpp-output",
                     .match = ArgMatch::Exact,
                     .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Gnu,
                     .spelling = "objective-c++-cpp-output",
                     .match = ArgMatch::Exact,
                     .rule = ArgRule::Allow },

        // -- MSVC: cl / clang-cl ----------------------------------------------------
        // Optimization, standard, warnings, defines, code generation. None of these
        // prefixes has a program-invoking member: `/analyze:plugin` loads a plugin
        // and is deliberately absent (so refused), and `/link` starts linker options
        // and is likewise absent.
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/O", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Msvc, .spelling = "/std:", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/W", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/w", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/D", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/U", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/EH", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/M", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/G", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/Z", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/RTC", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Msvc, .spelling = "/arch:", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/fp:", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/Q", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Msvc, .spelling = "/diagnostics:", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/vd", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/vm", .match = ArgMatch::Prefix, .rule = ArgRule::Allow },
        // Bare switches.
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/c", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Msvc, .spelling = "/nologo", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Msvc, .spelling = "/utf-8", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Msvc, .spelling = "/bigobj", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Msvc, .spelling = "/permissive", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Msvc, .spelling = "/permissive-", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/sdl", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/sdl-", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Msvc, .spelling = "/openmp", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/J", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg {
            .families = DriverFamily::Msvc, .spelling = "/showIncludes", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        // The language, stated for a preprocessed input (`RemoteCompileArgs` appends
        // `/TC` or `/TP`).
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/TC", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
        AllowedArg { .families = DriverFamily::Msvc, .spelling = "/TP", .match = ArgMatch::Exact, .rule = ArgRule::Allow },
    };

    /// Whether an allowlist row matches @p arg for driver family @p family.
    /// @param row The row.
    /// @param arg The argument, known non-empty.
    /// @param family The worker's driver family.
    /// @return True when the row applies and its shape matches.
    [[nodiscard]] bool ArgRowMatches(AllowedArg const& row, std::string_view arg, DriverFamily family)
    {
        if (!Overlaps(row.families, family))
            return false;
        return row.match == ArgMatch::Exact ? arg == row.spelling : arg.starts_with(row.spelling);
    }
} // namespace

bool IsAcceptableJobArgument(std::string_view arg, DriverFamily family)
{
    if (arg.empty())
        return true; // an empty argument names nothing and reaches no file

    // A carve-out wins over any prefix that admits the same argument: `-fplugin=x`
    // matches both the `-f` Allow prefix and the `-fplugin` Deny, and the Deny is the
    // one that must decide. Checked first, so the ordering of the Allow rows below it
    // cannot matter.
    for (AllowedArg const& row: AllowedArgs)
        if (row.rule == ArgRule::Deny && ArgRowMatches(row, arg, family))
            return false;

    return std::ranges::any_of(
        AllowedArgs, [&](AllowedArg const& row) { return row.rule == ArgRule::Allow && ArgRowMatches(row, arg, family); });
}

std::string SafeSourceName(std::string_view sourceName)
{
    // One component. A colon counts as a separator here even on POSIX: `C:x` is a
    // path on a Windows worker and an ordinary file name nowhere that matters, and
    // the cost of being wrong is a name, while the cost of being right is nothing.
    auto const separator = sourceName.find_last_of("/\\:");
    auto const name = separator == std::string_view::npos ? sourceName : sourceName.substr(separator + 1);

    auto const dot = name.find_last_of('.');
    auto const stem = dot == std::string_view::npos ? name : name.substr(0, dot);
    auto const extension = dot == std::string_view::npos ? std::string_view {} : name.substr(dot);

    auto const safeExtension =
        std::ranges::find(KnownExtensions, extension) != KnownExtensions.end() ? extension : DefaultExtension;

    if (!IsSafeStem(stem))
        return std::string { DefaultStem } + std::string { safeExtension };
    return std::string { stem } + std::string { safeExtension };
}

CompileJobRunner::CompileJobRunner(IProcessRunner& runner,
                                   std::filesystem::path scratchRoot,
                                   std::map<std::string, std::string> toolchains):
    _runner { runner },
    _scratchRoot { std::move(scratchRoot) },
    _toolchains { std::move(toolchains) }
{
}

std::vector<std::string> CompileJobRunner::Fingerprints() const
{
    std::shared_lock const guard { _toolchainsMutex };
    std::vector<std::string> out;
    out.reserve(_toolchains.size());
    for (auto const& [fingerprint, compiler]: _toolchains)
        out.push_back(fingerprint);
    return out;
}

void CompileJobRunner::ReplaceToolchains(std::map<std::string, std::string> toolchains)
{
    std::unique_lock const guard { _toolchainsMutex };
    _toolchains = std::move(toolchains);
}

std::expected<CompileOutcome, JobError> CompileJobRunner::Run(CompileJob const& job)
{
    // The compiler comes from THIS worker's configuration, keyed by the fingerprint
    // the client named. The client never names a program. A fingerprint this worker
    // does not have is refused rather than served with a default -- the scheduler
    // should not have sent it, and a client that reached this port directly did not
    // go through scheduling at all.
    //
    // Copied OUT of the map, under the lock, rather than kept as an iterator. The
    // map can be replaced while this job runs -- a node re-surveys when a compiler
    // is patched underneath it (#238) -- and the two uses below are far downstream,
    // after the scratch directory is created and the whole preprocessed source is
    // written. An iterator held across that is a dangling read on the line that
    // decides which program executes. A job already admitted therefore finishes
    // against the compiler it looked up, which is also the honest answer: it is what
    // the client was told it would get.
    std::string compiler;
    {
        std::shared_lock const guard { _toolchainsMutex };
        auto const found = _toolchains.find(job.fingerprint);
        if (found == _toolchains.end())
            return std::unexpected(JobError { .reason = JobRefusal::UnknownFingerprint, .detail = {} });
        compiler = found->second;
    }

    // Derived from the worker's OWN configured compiler, never from anything the
    // client sent -- the same rule that governs which program runs. Used both to
    // vet the client's arguments against this family's allowlist and to spell the
    // output flag far below, so it is computed once here.
    auto const family = DriverOf(ClassifyCompiler(compiler)).family;

    // Checked again here, on the receiving side, and against an ALLOWLIST -- see
    // `IsAcceptableJobArgument`. The client's filter protects an honest client from
    // dispatching something that would not work; this one protects the worker from a
    // client that is not honest. Trusting the client's check would mean the worker is
    // secured by code running on the caller's machine -- and the client's check is a
    // denylist that admits every program-invoking option that carries no path
    // separator, which is exactly the surface this must not expose.
    if (auto const offender =
            std::ranges::find_if(job.args, [&](std::string const& arg) { return !IsAcceptableJobArgument(arg, family); });
        offender != job.args.end())
        return std::unexpected(JobError {
            .reason = JobRefusal::RejectedArgument,
            .detail =
                std::format("argument {} is not on this worker's accepted-flag list for its driver family", *offender) });

    // Every path below is the worker's. Nothing the client sent decides where a byte
    // lands -- not the source name, not the object name, not the directory.
    std::error_code ec;
    auto const scratch = _scratchRoot / std::format("job-{}", _nextJob.fetch_add(1, std::memory_order_relaxed));
    std::filesystem::create_directories(scratch, ec);
    if (ec)
        return std::unexpected(JobError { .reason = JobRefusal::ScratchUnavailable, .detail = {} });

    // Removed however this returns, including on a refusal below: a worker that
    // leaked a directory per job would fill its disk in a long-running build.
    class ScratchGuard
    {
      public:
        explicit ScratchGuard(std::filesystem::path path) noexcept:
            _path { std::move(path) }
        {
        }
        ~ScratchGuard()
        {
            std::error_code ignored;
            std::filesystem::remove_all(_path, ignored);
        }
        ScratchGuard(ScratchGuard const&) = delete;
        ScratchGuard& operator=(ScratchGuard const&) = delete;
        ScratchGuard(ScratchGuard&&) = delete;
        ScratchGuard& operator=(ScratchGuard&&) = delete;

      private:
        std::filesystem::path _path;
    };
    ScratchGuard const guard { scratch };

    auto const source = scratch / SafeSourceName(job.sourceName);
    auto const object = scratch / "tu.o";
    {
        std::ofstream out { source, std::ios::binary };
        if (!out)
            return std::unexpected(JobError { .reason = JobRefusal::ScratchUnavailable, .detail = {} });
        out.write(job.preprocessed.data(), static_cast<std::streamsize>(job.preprocessed.size()));
        if (!out.good())
            return std::unexpected(JobError { .reason = JobRefusal::ScratchUnavailable, .detail = {} });
    }

    std::vector<std::string> argv;
    argv.reserve(job.args.size() + 5);
    argv.push_back(compiler);
    argv.insert(argv.end(), job.args.begin(), job.args.end());
    // The compile action and the output are the worker's to name, which is why the
    // client's `RemoteCompileArgs` dropped both rather than passing them through.
    //
    // The OUTPUT flag comes from the driver family, because `-o` is not universal:
    // `cl` does not accept it. Hard-coding it meant MSVC quietly wrote `tu.obj`
    // beside the source, exited 0, and this worker then found nothing at the path
    // it had asked for and refused the job as ScratchUnavailable -- so
    // distribution never worked on Windows, and said "storage write failed" about
    // it. `-c` needs no such treatment: MSVC drivers accept `-` for every option,
    // so it means the same to both families.
    //
    // `family` was derived once at lookup from the worker's OWN configured compiler,
    // never from anything the client sent -- the same rule that governs which program
    // runs, and which vetted the arguments above.
    argv.emplace_back("-c");
    argv.push_back(source.string());
    // Fused, which both families accept and which is the only form MSVC documents
    // for `/Fo`.
    argv.push_back(std::string { ObjectOutputPrefixFor(family) } + object.string());

    auto run = _runner.RunCaptureSplit(argv);
    if (run.exitCode == NotSpawned)
        // The compiler could not be spawned at all. Deliberately NOT reported as a
        // failed compile: the client must be able to tell "this worker is broken"
        // from "your code does not compile", because only the second is its answer.
        return std::unexpected(JobError { .reason = JobRefusal::SpawnFailed, .detail = {} });

    CompileOutcome outcome {
        .exitCode = run.exitCode, .object = {}, .stdoutText = std::move(run.out), .stderrText = std::move(run.err)
    };
    if (run.exitCode == 0)
    {
        auto bytes = ReadBytes(object);
        if (!bytes.has_value())
            // The compiler said it succeeded and produced nothing readable. Refused
            // rather than returned as an empty object, which the client would write
            // to disk and cache.
            return std::unexpected(JobError { .reason = JobRefusal::ScratchUnavailable, .detail = {} });
        outcome.object = *std::move(bytes);
    }
    return outcome;
}

} // namespace FastCache::Cc
