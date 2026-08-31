// SPDX-License-Identifier: Apache-2.0
#include "NodeToolchains.hpp"
#include "ToolchainHashProgress.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <CmdLine.hpp>
#include <IParallelFor.hpp>
#include <ParallelFor.hpp>
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

        /// What that identity was computed from, so it can be rechecked without
        /// paying for it again. Left empty for an operator's override, which is
        /// never probed and must never be re-derived.
        ToolchainWitness witness;
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
    /// @param clock Where the hash phase's progress rate reads elapsed time (#354).
    /// @param logger Startup log.
    /// @return One survey per entry, in the same order.
    [[nodiscard]] std::vector<SurveyedToolchain> FingerprintAll(std::stop_token const& stop,
                                                                std::vector<ToolchainEntry> const& entries,
                                                                Cc::IProcessRunner& runner,
                                                                Cc::IToolchainHost& host,
                                                                IClock const& clock,
                                                                ILogger& logger)
    {
        std::vector<SurveyedToolchain> fingerprints(entries.size());
        std::atomic<std::size_t> next { 0 };

        // One instance shared by every identifying thread, and that is safe: `Run`
        // holds no state across calls -- it creates and joins its own threads each
        // time -- and the only member is a width nobody writes.
        //
        // Constructed here rather than injected from `main` because this function is
        // already where the survey's own parallelism is decided, immediately below.
        // How the walk underneath is spread is the same decision in the same place;
        // the SEAM lives one layer down at `ProbeToolchainFiles`, which is where the
        // correctness requirement is and where tests substitute a serial one.
        //
        // The nesting is bounded and lopsided: this fans out over toolchains, of
        // which there is normally one, while the inner one fans out over thousands
        // of files. The inner is the work.
        Cc::ThreadedParallelFor parallel;

        // A decorator, so the walk announces itself without `ToolchainProbe` learning
        // about a logger. `ProbeToolchainFiles` enumerates every root SERIALLY and
        // only then hands the file list here, so this line lands exactly between
        // those two phases and separates them for free: a stall before it is
        // discovery or enumeration, a stall after it is hashing.
        //
        // Reached from every identifying thread when there is more than one
        // toolchain, so nothing here may race. It writes only through `logger`, and
        // `Run` is called once per walk, which makes this one line per toolchain
        // rather than a stream.
        struct AnnouncingParallelFor final: Cc::IParallelFor
        {
            // A constructor rather than designated initialisers: an override makes
            // this polymorphic, and a polymorphic class is not an aggregate.
            AnnouncingParallelFor(Cc::IParallelFor& parallelFor,
                                  IClock const& source,
                                  ILogger& sink,
                                  std::stop_token const& stopping) noexcept:
                inner { parallelFor },
                clock { source },
                logger { sink },
                stop { stopping }
            {
            }

            Cc::IParallelFor& inner;
            IClock const& clock;
            ILogger& logger;
            /// Observed per hashed file; see `Run`. A reference because the token
            /// outlives this wrapper -- it belongs to the thread being stopped.
            std::stop_token const& stop;

            [[nodiscard]] bool Run(std::size_t count, std::function<void(std::size_t)> const& slice) override
            {
                logger.Logf(LogLevel::Info, "hashing {} toolchain file(s)", count);

                // And then, while it runs, how fast (#354). The line above marks the
                // phase; these mark its RATE, which is what tells a cold cache from a
                // scanner from a per-file open cost. Eight sightings of this phase
                // stalling produced eight artefacts of the same shape and separated
                // none of those, because a phase that logs nothing while it runs
                // cannot say whether it is slowing down.
                //
                // Wrapping the slice rather than the whole run: a rate is only useful
                // during the walk, and a figure printed after it finishes is one the
                // failing case never reaches.
                ToolchainHashProgress progress { count, ToolchainHashProgress::DefaultInterval, clock, logger };
                return inner.Run(count, [&slice, &progress, this](std::size_t index) {
                    // The finest grain a stop can be observed at, and it has to be
                    // this fine: ONE toolchain's walk is the thing measured past
                    // 300 s, so a check only between toolchains would still make a
                    // stopping node wait out a whole one (#365).
                    if (stop.stop_requested())
                        return;
                    slice(index);
                    progress.Observe();
                });
            }
        };
        AnnouncingParallelFor announcing { parallel, clock, logger, stop };

        auto identify = [&] {
            for (auto index = next.fetch_add(1); index < entries.size(); index = next.fetch_add(1))
            {
                // Between toolchains as well as between files. The per-file check
                // bounds one walk; this stops the survey reaching for the next
                // machine-wide toolchain at all.
                if (stop.stop_requested())
                    return;
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

                // The three lines this one begins exist because a 300s stall here
                // reported as ONE undifferentiated silence (#354). The comment above
                // already says an operator needs to know the process is working
                // rather than wedged -- and then nothing else was ever logged, so
                // "working" and "wedged" still looked identical for five minutes.
                //
                // Sampling cannot close that gap: the banner probe is a `cl` that
                // takes 15-36ms warm, and no process-tree poll cheap enough to run
                // for five minutes will see it. What locates a stall is saying which
                // phase ended.
                logger.Logf(LogLevel::Info, "read the compiler banner for {}", entry.compiler);
                auto const flavor = Cc::ClassifyCompiler(entry.compiler);
                auto const& spec = Cc::DriverOf(flavor);
                fingerprints[index].identity =
                    Cc::CachedToolchainFingerprint(runner, host, entry.compiler, banner, spec, announcing);
                logger.Logf(LogLevel::Info, "computed the toolchain fingerprint for {}", entry.compiler);

                // The evidence this identity rests on, kept so that noticing it has
                // moved costs no spawn at all (#238). It is the same banner, the same
                // resolved path and the same roots `CachedToolchainFingerprint` just
                // folded, so the stamp recorded here is by construction the one it
                // wrote to its own cache file.
                //
                // The roots are asked for a SECOND time, and that is the price of
                // this being computed outside the function that already had them.
                // One extra driver spawn per toolchain, on a start that is already
                // walking the include tree, buys zero spawns on every heartbeat for
                // the life of the process -- which is the cost that actually matters,
                // and the one #188 is separately trying to remove from the launcher.
                //
                // Resolved on the search path for the reason `ComputeToolchainStamp`
                // documents: a bare `cc` or `cl` cannot be stat'd from an arbitrary
                // working directory, so stamping the typed spelling would quietly
                // yield no stamp for exactly the toolchains a package manager
                // upgrades.
                auto const resolved = host.ResolveOnSearchPath(entry.compiler).value_or(entry.compiler);
                auto discovered = Cc::DiscoverIncludePaths(runner, host, entry.compiler, spec);

                // A probe that did not RUN leaves no witness, rather than one over an
                // empty root set. The identity above was computed from whatever the
                // FIRST probe found, so a second probe that failed would leave this
                // node watching a narrower set of roots than its own fingerprint
                // covers -- and it would then stop noticing an SDK-side change for
                // that toolchain, permanently and in silence. Not watching a
                // toolchain is visible in the next survey; watching the wrong
                // evidence is not.
                //
                // Asked as `answered` rather than as an empty `roots`, for the reason
                // `IncludeSearchRoots` documents: several mechanisms legitimately find
                // no roots at all, and only a driver that could not be spawned means
                // the answer is missing.
                if (discovered.answered)
                {
                    auto stamp = Cc::ComputeToolchainStamp(banner, resolved, discovered.roots);
                    fingerprints[index].witness = ToolchainWitness { .compiler = resolved,
                                                                     .banner = banner,
                                                                     .roots = std::move(discovered.roots),
                                                                     .stamp = std::move(stamp) };
                }

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

std::optional<DiscoveredToolchains> DiscoverToolchainEntries(NodeConfig const& cfg,
                                                             Cc::IToolchainDiscovery* discovery,
                                                             Cc::IProcessRunner& runner,
                                                             ILogger& logger)
{
    // One fact, stated once. The set is the machine's when the operator named none
    // and there is something to ask -- which is also what decides whether the count
    // at the end is worth saying out loud, and which of three refusals is honest
    // when nothing survives the fingerprinting.
    auto const source = [&] {
        if (!cfg.toolchains.empty())
            return ToolchainSource::OperatorNamed;
        if (discovery != nullptr)
            return ToolchainSource::MachineSearched;
        return ToolchainSource::NothingToSearch;
    }();

    std::vector<ToolchainEntry> entries;
    if (source == ToolchainSource::MachineSearched)
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

    return DiscoveredToolchains { .entries = std::move(entries), .source = source };
}

SurveyResult FingerprintToolchains(DiscoveredToolchains const& discovered,
                                   Cc::IProcessRunner& runner,
                                   Cc::IToolchainHost& host,
                                   IClock const& clock,
                                   ILogger& logger,
                                   std::stop_token const& stop)
{
    auto const& entries = discovered.entries;

    // Computed CONCURRENTLY, and the cost is why. A cold fingerprint is a full walk
    // of the include tree -- about two seconds over 288 MB on an ordinary Xcode
    // toolchain, per `ToolchainProbe.hpp` -- and a machine the node surveyed itself
    // routinely holds four or five. Sequentially that is a node sitting silent for
    // half a minute at first boot before it reaches its scheduler, on exactly the
    // start where an operator is watching. Warm starts read the cache and are
    // instant, so this buys nothing on any boot after the first; it is the first one
    // that decides whether the feature looks like it works.
    auto fingerprints = FingerprintAll(stop, entries, runner, host, clock, logger);

    // Asked after the walk rather than instead of it: the check inside skips the work
    // per file, so a cancelled survey returns promptly carrying entries that were
    // never probed. Serving those would be a node offering whichever half of its
    // toolchains happened to finish first, and calling them "nothing to serve" would
    // exit a stopping node with a configuration error it does not have -- in the log
    // an operator reads to find out why the stop took a while.
    if (stop.stop_requested())
    {
        logger.Logf(LogLevel::Info, "toolchain survey abandoned: this node is stopping");
        return SurveyResult { .outcome = SurveyOutcome::Cancelled, .served = {} };
    }

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
        // Moved out rather than copied: a witness carries a vector of include roots
        // per toolchain, and the survey entry is never read again.
        auto const [existing, inserted] =
            toolchains.emplace(fingerprint,
                               ServedToolchain { .compiler = entry.compiler,
                                                 .label = std::move(fingerprints[index].label),
                                                 .witness = std::move(fingerprints[index].witness) });
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
        switch (discovered.source)
        {
            case ToolchainSource::MachineSearched:
                logger.Logf(LogLevel::Error,
                            "no toolchain to serve: found no compiler on this machine. Searched: {}. Name one with "
                            "--toolchain, or install a compiler where this worker can find it",
                            SearchedLayouts());
                break;
            case ToolchainSource::OperatorNamed:
                logger.Logf(LogLevel::Error,
                            "no toolchain to serve: every compiler named with --toolchain was refused, for the reason "
                            "given above each");
                break;
            case ToolchainSource::NothingToSearch:
                logger.Logf(LogLevel::Error,
                            "no toolchain to serve: --no-toolchain-discovery was given and no --toolchain, so this "
                            "worker was told to serve nothing");
                break;
        }
        return SurveyResult { .outcome = SurveyOutcome::NothingToServe, .served = {} };
    }

    // Said out loud when the machine answered, because the set is then something
    // nobody typed: an operator reading this log has to be able to tell "the fleet
    // decided" from "I configured that".
    if (discovered.source == ToolchainSource::MachineSearched)
        logger.Logf(LogLevel::Info,
                    "discovered {} toolchain(s) on this machine; pass --toolchain to serve a narrower set",
                    toolchains.size());

    return SurveyResult { .outcome = SurveyOutcome::Served, .served = std::move(toolchains) };
}

std::optional<std::map<std::string, ServedToolchain>> ResolveToolchains(NodeConfig const& cfg,
                                                                        Cc::IToolchainDiscovery* discovery,
                                                                        Cc::IProcessRunner& runner,
                                                                        Cc::IToolchainHost& host,
                                                                        IClock const& clock,
                                                                        ILogger& logger)
{
    // The whole survey, for every caller that can afford to wait for it: the
    // re-survey on the heartbeat thread, `--print-toolchain-fingerprint`, and every
    // test that wants one answer. Node startup is the one caller that cannot, and it
    // is the reason the two halves are separable at all (#365).
    auto discovered = DiscoverToolchainEntries(cfg, discovery, runner, logger);
    if (!discovered.has_value())
        return std::nullopt;

    // A default-constructed token, which is never stopped: every caller that reaches
    // the WHOLE survey runs it to completion by definition -- the re-survey on the
    // heartbeat thread, `--print-toolchain-fingerprint`, and the tests. The node's
    // FIRST survey is the one that can be cancelled, and it calls the two halves
    // itself rather than coming through here.
    auto surveyed = FingerprintToolchains(*discovered, runner, host, clock, logger, std::stop_token {});
    if (surveyed.outcome != SurveyOutcome::Served)
        return std::nullopt;
    return std::move(surveyed.served);
}

std::vector<std::string> StaleToolchains(std::map<std::string, ServedToolchain> const& served)
{
    std::vector<std::string> stale;
    for (auto const& [fingerprint, toolchain]: served)
    {
        auto const& witness = toolchain.witness;
        if (!witness.Watchable())
            continue;

        // The same function the launcher validates its own fingerprint cache with,
        // over the inputs recorded at survey time. Asking it rather than comparing
        // fields by hand is what keeps a node and its clients agreeing about what
        // counts as the same toolchain -- a second notion of toolchain identity is
        // the defect this whole subsystem is built to avoid.
        if (Cc::ComputeToolchainStamp(witness.banner, witness.compiler, witness.roots) != witness.stamp)
            stale.push_back(fingerprint);
    }
    return stale;
}

ToolchainRefresh RefreshToolchains(std::map<std::string, ServedToolchain> const& served,
                                   NodeConfig const& cfg,
                                   Cc::IToolchainDiscovery* discovery,
                                   Cc::IProcessRunner& runner,
                                   Cc::IToolchainHost& host,
                                   IClock const& clock,
                                   ILogger& logger,
                                   RecheckDepth depth)
{
    auto const stale = StaleToolchains(served);
    if (stale.empty() && depth == RecheckDepth::WhenEvidenceMoved)
        return {};

    for (auto const& fingerprint: stale)
        logger.Logf(LogLevel::Info,
                    "the toolchain behind {} changed on this machine; re-deriving what this worker serves",
                    fingerprint);

    // The full survey, not a cheaper re-derivation of the stale entries alone. An
    // upgrade can add a compiler, remove one, or make two that were distinct
    // identical -- and the set is what this worker registers, so deriving it any way
    // but the startup way would give a node two identities depending on when it was
    // asked.
    auto refreshed = ResolveToolchains(cfg, discovery, runner, host, clock, logger);

    // `ResolveToolchains` refuses an empty result, which at startup is fatal and here
    // is a state the node has to be able to reach: the machine's only compiler was
    // removed, or the upgrade left it unrunnable. It has already said so. Serving
    // nothing is the correct answer -- every lease is then refused and every client
    // compiles locally -- and it is the one answer that is never a wrong object.
    auto next = std::move(refreshed).value_or(std::map<std::string, ServedToolchain> {});

    // Only the REMOVALS are announced here. `ResolveToolchains` has just logged a
    // "serving {} as {}" line for everything it kept, so naming the additions again
    // would print every one of them twice on a refresh. What an operator needs from
    // this trip is the half the re-survey cannot say: what this node has stopped
    // being able to honour.
    for (auto const& [fingerprint, toolchain]: served)
        if (!next.contains(fingerprint))
            logger.Logf(LogLevel::Info, "no longer serving {} ({})", fingerprint, toolchain.compiler);

    // An unconditional sweep that found the machine exactly as it was left is not a
    // change, and must not be reported as one: it runs on a timer, and answering
    // "changed" would re-register every worker in the fleet on that timer for no
    // reason. Nothing is lost by discarding its witnesses -- no stamp moved, so they
    // say what the ones already held say.
    if (stale.empty() && std::ranges::equal(std::views::keys(served), std::views::keys(next)))
        return {};

    // Otherwise `changed` follows the STAMP as much as the fingerprint set, and the
    // difference matters: an upgrade that moves a file's mtime without changing what
    // the digest covers leaves the set identical, and returning "nothing changed"
    // there would leave the old witness in place and re-run this whole survey on
    // every heartbeat for the life of the process. The refreshed witnesses are the
    // point of that trip.
    return ToolchainRefresh { .changed = true, .served = std::move(next) };
}

} // namespace FastCache::Node
