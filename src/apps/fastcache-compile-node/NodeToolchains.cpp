// SPDX-License-Identifier: Apache-2.0
#include "NodeToolchains.hpp"

#include <algorithm>
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
    /// `Cc::NotSpawned` is the one answer that means "could not spawn"; a compiler that
    /// ran and then failed has run, and that is the question.
    ///
    /// The argv is `Cc::VersionProbeCommand`'s and not a second spelling of it, because
    /// the very next thing asked of this compiler is its banner, through the same
    /// command. Two spellings would have this judge spawnability from an invocation it
    /// then never uses -- and they diverged once already, when `--version` was hard-coded
    /// here while the banner probe learned that `cl` answers only when asked bare.
    ///
    /// @param runner Process-spawning seam.
    /// @param compiler The candidate.
    /// @return True when the process started.
    [[nodiscard]] bool CanSpawn(Cc::IProcessRunner& runner, std::string const& compiler)
    {
        return runner.RunCaptureCombined(Cc::VersionProbeCommand(compiler)).exitCode != Cc::NotSpawned;
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

    /// What the survey learned about one entry: its identity, and the label a person
    /// reads it by.
    ///
    /// Two fields rather than a field on `Cc::ToolchainIdentity`, because they answer
    /// different questions and only one of them is load-bearing: the fingerprint
    /// decides whether a client matches this worker, while the label decides nothing
    /// at all and exists to be read (#194). Putting a display string on the identity
    /// type would invite exactly the second way of deciding what a compiler IS that
    /// `ToolchainDiscovery` warns against.
    struct SurveyedToolchain
    {
        Cc::ToolchainIdentity identity;
        std::string label; ///< Empty when there was no banner to derive one from.
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
    /// @return One survey per entry, in the same order.
    [[nodiscard]] std::vector<SurveyedToolchain> FingerprintAll(std::vector<ToolchainEntry> const& entries,
                                                                Cc::IProcessRunner& runner,
                                                                Cc::IToolchainHost& host,
                                                                ILogger& logger)
    {
        std::vector<SurveyedToolchain> fingerprints(entries.size());
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
                    // No label: an override is never probed, so there is no banner
                    // to read one out of. Empty travels as "did not say" (#194), which
                    // is the honest answer -- and is why the fingerprint column stays
                    // beside the label rather than being replaced by it.
                    fingerprints[index].identity =
                        Cc::ToolchainIdentity { .fingerprint = entry.fingerprint, .defect = Cc::IdentityDefect::None };
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
                fingerprints[index].identity =
                    Cc::CachedToolchainFingerprint(runner, host, entry.compiler, banner, Cc::DriverOf(flavor));

                // Derived from the banner that was just computed, rather than by
                // asking the compiler a second time: the digest is a hash OF this
                // string, so the readable name was already in hand and was being
                // thrown away (#194).
                fingerprints[index].label = Cc::ToolchainLabel(entry.compiler, banner);
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

        // Bounded by the COMPUTED count, not by `workers.capacity()`. `reserve`
        // promises only `capacity() >= n`, so reading the bound back out of the
        // vector makes the thread count an allocator detail -- and the number it
        // would then not be bounded by is the one `MaxFingerprintThreads` documents
        // as "polite on the machine a developer is sitting at".
        auto const threads = std::min(entries.size(), MaxFingerprintThreads);
        std::vector<std::jthread> workers;
        workers.reserve(threads);
        for ([[maybe_unused]] auto const worker: std::views::iota(std::size_t { 0 }, threads))
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

std::optional<std::map<std::string, ServedToolchain>> ResolveToolchains(NodeConfig const& cfg,
                                                                        Cc::IToolchainDiscovery* discovery,
                                                                        Cc::IProcessRunner& runner,
                                                                        Cc::IToolchainHost& host,
                                                                        ILogger& logger)
{
    // One fact, stated once. The set is the machine's when the operator named none
    // and there is something to ask -- which is also what decides whether the count
    // at the end is worth saying out loud.
    bool const discovered = cfg.toolchains.empty() && discovery != nullptr;

    std::vector<ToolchainEntry> entries;
    if (discovered)
    {
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

            // Whether the PINNED half is text is asked by `ParseToolchain`, in the
            // option table, so that `--install-service` cannot bake in a fingerprint
            // no scheduler will accept: this function is not reached on that path.
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
    std::map<std::string, ServedToolchain> toolchains;
    for (auto const index: std::views::iota(std::size_t { 0 }, entries.size()))
    {
        auto const& entry = entries[index];
        auto const& identity = fingerprints[index].identity;

        // Refused rather than registered, and this is the backstop that survives the
        // next layout nobody anticipated. A worker whose digest does not identify its
        // toolchain registers cleanly, heartbeats happily, and is matched by every
        // client on a different toolchain -- or by none at all. Both directions are
        // silent from both ends, which is why the refusal has to be loud.
        //
        // The reason and the remedy come from `IdentityDefectTable` rather than being
        // written out here: `--print-toolchain-fingerprint` reports the same defects
        // to the same operator, and the two were already two spellings of one fact.
        if (!identity.Usable())
        {
            auto const& explanation = Cc::ExplainDefect(identity.defect);
            logger.Logf(LogLevel::Error, "refusing {}: {}. {}", entry.compiler, explanation.reason, explanation.remedy);
            continue;
        }

        auto const& fingerprint = identity.fingerprint;

        // The log follows the MAP rather than the loop, because the map is what the
        // worker will actually serve and the two do not always agree. Two compilers
        // can reach one fingerprint -- `clang` and `clang++` do, since clang's banner
        // does not name its own argv[0] the way a GNU driver's does, and the include
        // probe forces `-x c++` for both -- and `emplace` keeps the first. Saying
        // "serving" for the second was a line naming a binding this worker does not
        // have, in the one log an operator reads to find out why the scheduler is not
        // matching them.
        //
        // Reported for an explicit override too, and IN TABLE ORDER however the work
        // was scheduled: a fingerprint mismatch is invisible from both ends, so this
        // log is where two machines' digests get compared, and one ordered by which
        // thread finished first would be a poor place to do it.
        auto const [existing, inserted] = toolchains.emplace(
            fingerprint, ServedToolchain { .compiler = entry.compiler, .label = fingerprints[index].label });
        if (inserted)
            logger.Logf(LogLevel::Info, "serving {} as {}", entry.compiler, fingerprint);
        else
            logger.Logf(LogLevel::Info,
                        "{} is the same toolchain as {} ({}); serving it once",
                        entry.compiler,
                        existing->second.compiler,
                        fingerprint);
    }

    // Refused HERE, and the refusal is the point of the function rather than an
    // afterthought its caller performs. Left to run, a worker with nothing to serve
    // is the worst shape this system has: nothing registers, so the scheduler never
    // hears of it; the heartbeat reports "0 of 0 toolchain(s) registered" and calls
    // that a complete success; and the ready line says the node is up. A supervisor
    // sees a healthy unit, an operator sees a green fleet, and every build compiles
    // locally with no error at either end.
    //
    // The message names where it looked ONLY when there was a search. A worker that
    // was told which compilers to serve, and had every one of them rejected, must
    // not be handed a list of places it never looked -- that reads as "your compiler
    // is not installed" when the answer is "the one you named was refused, a line
    // above". The search list is derived from the layout table rather than written
    // out, so a layout added to the table necessarily appears in it.
    if (toolchains.empty())
    {
        if (discovered)
            logger.Logf(LogLevel::Error,
                        "no toolchain to serve: found no compiler on this machine. Searched: {}. Name one with "
                        "--toolchain, or install a compiler where this worker can find it",
                        SearchedLayouts());
        else if (!cfg.toolchains.empty())
            logger.Logf(LogLevel::Error,
                        "no toolchain to serve: every compiler named with --toolchain was refused, for the reason "
                        "given above each");
        else
            logger.Logf(LogLevel::Error,
                        "no toolchain to serve: --no-toolchain-discovery was given and no --toolchain, so this "
                        "worker was told to serve nothing");
        return std::nullopt;
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
