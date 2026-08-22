// SPDX-License-Identifier: Apache-2.0
#include "CompileJob.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <system_error>

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

bool IsAcceptableJobArgument(std::string_view arg)
{
    if (arg.empty())
        return true; // an empty argument names nothing and reaches no file
    if (arg.starts_with('@'))
        return false;

    // One introducer is skipped before the separator is looked for, exactly as
    // `CouldNameAFile` does on the client -- and for the reason AGENT.md records: `/`
    // starts an option for an MSVC driver and an absolute path everywhere else.
    // Testing the raw argument refuses `/O2` and therefore every MSVC job, which the
    // test asserting `/O2` is acceptable is what caught.
    //
    // Both introducers are accepted here rather than the driver's own set, because a
    // worker is told a FINGERPRINT rather than a command line and has no driver to
    // ask. That is marginally more permissive than the client's per-family rule --
    // `/x` is treated as an option here where a GNU client would call it a path --
    // and it does not matter: the check that carries the weight is the one below,
    // and a bare `/x` still has to survive it.
    constexpr std::string_view Introducers = "-/";
    auto const body = Introducers.contains(arg.front()) ? arg.substr(1) : arg;
    return !body.contains('/') && !body.contains('\\');
}

std::string_view SafeSourceExtension(std::string_view sourceName)
{
    auto const dot = sourceName.find_last_of('.');
    if (dot != std::string_view::npos)
    {
        auto const extension = sourceName.substr(dot);
        if (std::ranges::find(KnownExtensions, extension) != KnownExtensions.end())
            return extension;
    }
    return ".cpp";
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
    std::vector<std::string> out;
    out.reserve(_toolchains.size());
    for (auto const& [fingerprint, compiler]: _toolchains)
        out.push_back(fingerprint);
    return out;
}

std::expected<CompileOutcome, JobRefusal> CompileJobRunner::Run(CompileJob const& job)
{
    // The compiler comes from THIS worker's configuration, keyed by the fingerprint
    // the client named. The client never names a program. A fingerprint this worker
    // does not have is refused rather than served with a default -- the scheduler
    // should not have sent it, and a client that reached this port directly did not
    // go through scheduling at all.
    auto const toolchain = _toolchains.find(job.fingerprint);
    if (toolchain == _toolchains.end())
        return std::unexpected(JobRefusal::UnknownFingerprint);

    // Checked again here, on the receiving side. The client's filter protects an
    // honest client from dispatching something that would not work; this one
    // protects the worker from a client that is not honest. Trusting the client's
    // check would mean the worker is secured by code running on the caller's machine.
    if (!std::ranges::all_of(job.args, IsAcceptableJobArgument))
        return std::unexpected(JobRefusal::RejectedArgument);

    // Every path below is the worker's. Nothing the client sent decides where a byte
    // lands -- not the source name, not the object name, not the directory.
    std::error_code ec;
    auto const scratch = _scratchRoot / std::format("job-{}", _nextJob++);
    std::filesystem::create_directories(scratch, ec);
    if (ec)
        return std::unexpected(JobRefusal::ScratchUnavailable);

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

    auto const source = scratch / std::format("tu{}", SafeSourceExtension(job.sourceName));
    auto const object = scratch / "tu.o";
    {
        std::ofstream out { source, std::ios::binary };
        if (!out)
            return std::unexpected(JobRefusal::ScratchUnavailable);
        out.write(job.preprocessed.data(), static_cast<std::streamsize>(job.preprocessed.size()));
        if (!out.good())
            return std::unexpected(JobRefusal::ScratchUnavailable);
    }

    std::vector<std::string> argv;
    argv.reserve(job.args.size() + 5);
    argv.push_back(toolchain->second);
    argv.insert(argv.end(), job.args.begin(), job.args.end());
    // The compile action and the output are the worker's to name, which is why the
    // client's `RemoteCompileArgs` dropped both rather than passing them through.
    argv.emplace_back("-c");
    argv.push_back(source.string());
    argv.emplace_back("-o");
    argv.push_back(object.string());

    auto run = _runner.RunCaptureSplit(argv);
    if (run.exitCode == -1)
        // The compiler could not be spawned at all. Deliberately NOT reported as a
        // failed compile: the client must be able to tell "this worker is broken"
        // from "your code does not compile", because only the second is its answer.
        return std::unexpected(JobRefusal::SpawnFailed);

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
            return std::unexpected(JobRefusal::ScratchUnavailable);
        outcome.object = *std::move(bytes);
    }
    return outcome;
}

} // namespace FastCache::Cc
