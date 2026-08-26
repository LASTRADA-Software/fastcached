// SPDX-License-Identifier: Apache-2.0
#include "NodeToolchains.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include <CmdLine.hpp>
#include <ToolchainProbe.hpp>

namespace FastCache::Node
{

namespace
{
    /// Whether a compiler can actually be launched.
    ///
    /// Asked of DISCOVERED compilers only. A candidate that cannot be spawned must not
    /// become a registered toolchain: that is the `SpawnFailed` refusal a client
    /// currently meets at job time, moved to startup where it belongs and where an
    /// operator can see it. An operator-named toolchain is deliberately NOT asked --
    /// the `<fingerprint>=<compiler>` override exists precisely for a compiler this
    /// process cannot execute, such as a cross-compiler or a wrapper that must not run
    /// at configuration time.
    ///
    /// `exitCode == -1` is the one answer that means "could not spawn"; a compiler that
    /// ran and rejected `--version` (which `cl` does) has run, and that is the question.
    ///
    /// @param runner Process-spawning seam.
    /// @param compiler The candidate.
    /// @return True when the process started.
    [[nodiscard]] bool CanSpawn(Cc::IProcessRunner& runner, std::string const& compiler)
    {
        std::array<std::string, 2> const probe { compiler, "--version" };
        return runner.RunCaptureCombined(probe).exitCode != -1;
    }
} // namespace

std::optional<ToolchainEntry> SplitToolchain(std::string_view spec)
{
    if (spec.empty())
        return std::nullopt;

    auto const eq = spec.find('=');
    if (eq == std::string_view::npos)
        return ToolchainEntry { .fingerprint = {}, .compiler = std::string { spec } };
    if (eq == 0 || eq + 1 >= spec.size())
        return std::nullopt;
    return ToolchainEntry { .fingerprint = std::string { spec.substr(0, eq) },
                            .compiler = std::string { spec.substr(eq + 1) } };
}

std::string SearchedLayouts()
{
    std::string names;
    for (auto const& layout: Cc::ToolchainLayouts())
    {
        if (!names.empty())
            names += ", ";
        names += layout.name;
    }
    return names;
}

std::optional<std::map<std::string, std::string>> ResolveToolchains(NodeConfig const& cfg,
                                                                    Cc::IToolchainDiscovery* discovery,
                                                                    Cc::IProcessRunner& runner,
                                                                    Cc::IToolchainHost& host,
                                                                    ILogger& logger)
{
    std::vector<ToolchainEntry> entries;
    bool discovered = false;

    if (cfg.toolchains.empty() && discovery != nullptr)
    {
        discovered = true;
        for (auto const& candidate: discovery->Discover())
        {
            // Refused HERE rather than at the first job. A compiler that is present
            // and cannot be run is a real state -- a broken symlink, a stub shipped
            // by a package whose payload is missing, a binary for another
            // architecture -- and registering it would hand the scheduler a worker
            // that fails everything sent to it.
            if (!CanSpawn(runner, candidate.compiler))
            {
                logger.Logf(
                    LogLevel::Warn, "ignoring {} found by {}: it cannot be executed", candidate.compiler, candidate.layout);
                continue;
            }
            logger.Logf(LogLevel::Info, "found {} ({})", candidate.compiler, candidate.layout);

            // Built directly rather than pushed back through `SplitToolchain`. That
            // grammar reserves the first `=`, which is right for something an
            // operator typed and wrong for a path this process found itself: a
            // compiler under `/opt/gcc=13/bin` would register the fingerprint
            // `/opt/gcc` for the compiler `13/bin/gcc`, and one with a leading or
            // trailing `=` would abort startup as "malformed --toolchain", naming a
            // flag nobody passed.
            entries.push_back(ToolchainEntry { .fingerprint = {}, .compiler = candidate.compiler });
        }
    }
    else
        for (auto const& spec: cfg.toolchains)
        {
            auto split = SplitToolchain(spec);
            if (!split.has_value())
            {
                logger.Logf(
                    LogLevel::Error, "malformed --toolchain '{}'; expected <compiler> or <fingerprint>=<compiler>", spec);
                return std::nullopt;
            }
            entries.push_back(*std::move(split));
        }

    std::map<std::string, std::string> toolchains;
    for (auto const& entry: entries)
    {
        auto fingerprint = entry.fingerprint;
        if (fingerprint.empty())
        {
            // The same computation the launcher performs, through the same
            // functions -- which is the point. A worker that derived its identity
            // differently from its clients would register successfully, heartbeat
            // happily, and never be matched, with nothing anywhere reporting why.
            //
            // Logged at info because it is slow the first time (a full walk of the
            // include tree, seconds) and instant afterwards, and an operator
            // watching a worker start deserves to know which of the two is
            // happening rather than wondering whether it has hung.
            logger.Logf(LogLevel::Info, "computing the toolchain fingerprint for {}", entry.compiler);
            auto const banner = Cc::CompilerBanner(runner, entry.compiler);
            auto const flavor = Cc::ClassifyCompiler(entry.compiler);
            fingerprint = Cc::CachedToolchainFingerprint(runner, host, entry.compiler, banner, Cc::DriverOf(flavor));
        }

        // Reported unconditionally, including for an explicit override. A
        // fingerprint mismatch is invisible from both ends -- the scheduler just
        // says no worker matches -- so the one place the worker's own digest can
        // be seen is its startup log, next to `fastcache-cc
        // --print-toolchain-fingerprint` on the client.
        logger.Logf(LogLevel::Info, "serving {} as {}", entry.compiler, fingerprint);
        toolchains.emplace(std::move(fingerprint), entry.compiler);
    }

    // Said out loud when the machine answered, because the set is then something
    // nobody typed: an operator reading this log has to be able to tell "the fleet
    // decided" from "I configured that".
    if (discovered)
        logger.Logf(LogLevel::Info,
                    "discovered {} toolchain(s) on this machine; pass --toolchain to serve a narrower set",
                    toolchains.size());
    return toolchains;
}

} // namespace FastCache::Node
