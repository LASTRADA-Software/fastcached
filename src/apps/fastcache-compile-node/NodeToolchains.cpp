// SPDX-License-Identifier: Apache-2.0
#include "NodeToolchains.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <ranges>
#include <string>
#include <thread>
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

    /// How many toolchains are fingerprinted at once.
    ///
    /// Bounded rather than one thread per toolchain, because each is a recursive
    /// directory walk hashing every file it finds: ten toolchains would otherwise
    /// put ten of those on the disk at one moment and finish later than four would
    /// have. Four is enough to hide the slowest one's latency and small enough to
    /// stay polite on the shared machine a developer is sitting at -- which is the
    /// deployment `--node-class=workstation` exists for.
    constexpr std::size_t MaxFingerprintThreads = 4;

    /// One toolchain's computed identity, and whether it means anything.
    struct Identity
    {
        std::string fingerprint; ///< The digest a client must match.
        /// True when the digest carries no information about WHICH compiler this is.
        ///
        /// A banner that is itself the fallback name, over an empty include tree, is
        /// not a weak identity but no identity: `KeyDigest("toolchain-v1").Field("cl")`
        /// is a value this repository could print with no compiler installed, and
        /// every MSVC toolset in existence produces it. `ToolchainProbe.hpp` permits
        /// a banner-only fingerprint and argues it can "only cause two
        /// genuinely-identical toolchains to be treated as identical, never two
        /// different ones" -- and that argument has an unstated precondition, that the
        /// banner is a real version string. This is that precondition, checked.
        bool degenerate { false };
    };

    /// Compute every entry's identity, several at a time.
    ///
    /// **The INJECTED runner is what every worker uses**, rather than one made per
    /// thread. Reaching for `MakeProcessRunner` here would spawn real compilers from
    /// a function whose caller passed in a seam -- which is not an optimization but a
    /// hole: every test that scripts this would silently be interrogating the machine
    /// it runs on, and pass or fail on what happens to be installed there.
    /// `IProcessRunner` requires implementations to tolerate concurrent calls for
    /// exactly this, and the fingerprint cache is separately documented as tolerating
    /// concurrent writers (a temp file plus a rename, sixteen launchers deep on a
    /// cold `-j16` build), so nothing here needs a lock.
    ///
    /// Results are collected POSITIONALLY, so neither the answer nor the log that
    /// follows depends on which thread finished first.
    ///
    /// @param entries The toolchains to identify.
    /// @param runner Process-spawning seam, called from several threads at once.
    /// @param host The machine's filesystem, registry and environment.
    /// @param logger Startup log.
    /// @return One identity per entry, in the same order.
    [[nodiscard]] std::vector<Identity> FingerprintAll(std::vector<ToolchainEntry> const& entries,
                                                       Cc::IProcessRunner& runner,
                                                       Cc::IToolchainHost& host,
                                                       ILogger& logger)
    {
        std::vector<Identity> fingerprints(entries.size());
        std::atomic<std::size_t> next { 0 };

        auto identify = [&] {
            for (auto index = next.fetch_add(1); index < entries.size(); index = next.fetch_add(1))
            {
                auto const& entry = entries[index];
                if (!entry.fingerprint.empty())
                {
                    // An operator pinned it by hand, which is the documented way to
                    // give a worker an identity this process could not compute.
                    // Second-guessing it here would refuse the escape hatch.
                    fingerprints[index] = Identity { .fingerprint = entry.fingerprint, .degenerate = false };
                    continue;
                }

                // The same computation the launcher performs, through the same
                // functions -- which is the point. A worker that derived its
                // identity differently from its clients would register
                // successfully, heartbeat happily, and never be matched, with
                // nothing anywhere reporting why.
                //
                // Logged BEFORE the walk rather than after, because the walk is the
                // slow part and an operator watching a cold start needs to know the
                // process is working rather than wedged.
                logger.Logf(LogLevel::Info, "computing the toolchain fingerprint for {}", entry.compiler);
                auto const banner = Cc::CompilerBanner(runner, entry.compiler);
                auto const flavor = Cc::ClassifyCompiler(entry.compiler);
                auto const& driver = Cc::DriverOf(flavor);
                fingerprints[index].fingerprint =
                    Cc::CachedToolchainFingerprint(runner, host, entry.compiler, banner, driver);

                // The roots are re-derived only when the banner turned out to be the
                // FALLBACK, which is the cheap half of the test and the one that
                // gates it. A driver that answered `--version` has a real identity
                // whatever its roots, and re-deriving for it would spawn the verbose
                // probe a second time on every GNU toolchain.
                if (banner == Cc::NormalizedCompilerName(entry.compiler))
                    fingerprints[index].degenerate = Cc::DiscoverIncludePaths(runner, host, entry.compiler, driver).empty();
            }
        };

        // No thread at all for one toolchain, which is the common case: spawning one
        // to do what this thread could have done is pure latency, and most machines
        // a worker runs on hold a single compiler.
        if (entries.size() <= 1)
        {
            identify();
            return fingerprints;
        }

        std::vector<std::jthread> workers;
        workers.reserve(std::min(entries.size(), MaxFingerprintThreads));
        for ([[maybe_unused]] auto const worker: std::views::iota(std::size_t { 0 }, workers.capacity()))
            workers.emplace_back(identify);

        // Cleared EXPLICITLY, and it is not tidiness. `jthread` joins when it is
        // destroyed, and locals are destroyed AFTER the return value is
        // constructed -- so leaving the join to scope exit would copy
        // `fingerprints` while the workers were still writing into it.
        workers.clear();
        return fingerprints;
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

    // Computed CONCURRENTLY, and the cost is why. A cold fingerprint is a full walk
    // of the include tree -- about two seconds over 288 MB on an ordinary Xcode
    // toolchain, per `ToolchainProbe.hpp` -- and a machine the node surveyed itself
    // routinely holds four or five. Sequentially that is a node sitting silent for
    // half a minute at first boot before it reaches its scheduler, on exactly the
    // start where an operator is watching. Warm starts read the cache and are
    // instant, so this buys nothing on any boot after the first; it is the first one
    // that decides whether the feature looks like it works.
    auto const fingerprints = FingerprintAll(entries, runner, host, logger);

    // Indexed rather than zipped: `std::views::zip` is C++23 and not uniformly
    // available across the four standard libraries this project builds against.
    std::map<std::string, std::string> toolchains;
    for (auto const index: std::views::iota(std::size_t { 0 }, entries.size()))
    {
        auto const& entry = entries[index];
        auto const& identity = fingerprints[index];

        // Refused rather than registered, and this is the backstop that survives the
        // next layout nobody anticipated. A worker whose digest carries no
        // information about which compiler it is registers cleanly, heartbeats
        // happily, and is matched by every client on a different toolchain -- or by
        // none at all. Both directions are silent from both ends.
        if (identity.degenerate)
        {
            logger.Logf(LogLevel::Error,
                        "refusing {}: it could not be asked its version and no include roots were found, so its "
                        "fingerprint would carry nothing about which compiler it is. Pin one with "
                        "--toolchain=<fingerprint>={} if that is deliberate",
                        entry.compiler,
                        entry.compiler);
            continue;
        }

        auto const& fingerprint = identity.fingerprint;

        // Reported unconditionally, including for an explicit override, and IN
        // TABLE ORDER however the work was scheduled. A fingerprint mismatch is
        // invisible from both ends -- the scheduler just says no worker matches --
        // so the one place a worker's own digest can be seen is this log, next to
        // `fastcache-cc --print-toolchain-fingerprint` on the client. A log whose
        // order depended on which thread finished first would be a poor place to
        // compare two machines.
        logger.Logf(LogLevel::Info, "serving {} as {}", entry.compiler, fingerprint);
        toolchains.emplace(fingerprint, entry.compiler);
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
