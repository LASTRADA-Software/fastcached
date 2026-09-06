// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/ConfigReloader.hpp>
#include <FastCache/Config/SecretProvenance.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace FastCache
{

/// What was last said about a set of secret-bearing files, so a repeat is silent.
///
/// #384's readability check runs at a START, and `requirepass` is `Reloadable::Yes`
/// -- so a configuration file can **gain** a secret after startup and that path was
/// silent in exactly the way the ticket was written to close
/// ([#753](https://github.com/LASTRADA-Software/fastcached/issues/753)).
///
/// **It re-asks the filesystem and compares against a REMEMBERED answer. It does not
/// diff the two configuration snapshots, and that is the whole design.** A file's
/// MODE is in no configuration, so a reload that loosens permissions on a file whose
/// `requirepass:` never moved produces two byte-identical snapshots -- and an
/// implementation reasoning from the reloader's previous-and-current pair would
/// conclude nothing changed and never look at the file. That is one of the two
/// transitions #753 requires; the other, a secret appearing in a file whose mode was
/// always loose, IS visible in the snapshots, which is what makes the snapshot-diffing
/// version look correct while covering half the ticket.
///
/// **The answer, not the sentence.** What is remembered is `(path, exposure)`, so a
/// file whose exposure changes KIND -- group-readable, then world-readable, with
/// different remedies -- is a transition and is reported. Comparing rendered text
/// would agree today and would stop the day a hint loses information.
///
/// **A standing exposure is said once**, because a warning repeated at every SIGHUP
/// is the alarm-nobody-reads failure arriving by a different route -- the same trade
/// #741 records for a true positive on a shipped default. An exposure that CLEARS is
/// silent too: this reports what an operator must act on, not a status line.
///
/// A path that leaves the set is FORGOTTEN, so a secret removed and later put back
/// into the same still-exposed file warns again. It is a fresh transition into
/// exposure and the operator's last warning was about a state that stopped being
/// true in between.
///
/// Not thread-safe, and it does not need to be: `ConfigReloaderOf` calls subscribers
/// synchronously on the thread that invoked `Reload()`, which is the one signal
/// thread, and the startup observation runs before any of them.
class SecretExposureWatcher
{
  public:
    /// Ask about @p files and report only what is newly wrong.
    ///
    /// @param files The files this process holds secrets in, in the order to report.
    ///        The CALLER applies whatever provenance gate it has -- see
    ///        `SecretFileWarnings`, which explains why a path-reached secret has none.
    /// @return A sentence per file whose exposure differs from the last observation,
    ///         in @p files order; empty when every subject's exposure is what it was.
    ///         A file that has become LESS exposed but is still exposed is a
    ///         difference too, and is reported -- it needs a different remedy from
    ///         the one the operator was last handed.
    [[nodiscard]] std::vector<std::string> Observe(std::span<std::filesystem::path const> files);

    /// The memory rule alone, over findings somebody else acquired.
    ///
    /// **Published so the rule is testable where `chmod` is not.** `Observe` is this
    /// composed with `SecretFileExposures`, and fusing the two would put every
    /// behavioural test of say-once, report-on-kind-change and forget-on-leave behind
    /// a POSIX guard and a real file mode -- leaving the rule that decides whether an
    /// operator is TOLD anything untested on the platform where the acquisition half
    /// is a security descriptor rather than three bits.
    ///
    /// It is the same acquisition-versus-decision split `Platform/FileTrust` draws
    /// between `ObserveSecretFile` and `ClassifySecretFile`, one level up, and for the
    /// same stated reason: the branch a developer cannot run is still exercised against
    /// a constructed input. Not a dependency-injection seam and not an attempt at one
    /// -- there is no `IFileSystem` in this tree, deliberately.
    ///
    /// **It MUTATES the memory**, so a caller that renders the result and a caller that
    /// discards it are the same caller as far as the next call is concerned.
    ///
    /// @param found What is unfit now, in the order to report it.
    /// @return A sentence per finding whose exposure differs from the last observation.
    [[nodiscard]] std::vector<std::string> Transitions(std::span<SecretFileFinding const> found);

  private:
    /// What the last `Observe` found, in the order it found it.
    ///
    /// A vector rather than a map: the set is a handful of files, the order is the
    /// caller's and worth keeping, and a linear scan over five entries needs no
    /// justification.
    std::vector<SecretFileFinding> _reported;
};

/// Where a warning goes. A sink rather than a logger, so this layer takes no
/// dependency on which of the three log destinations a binary happens to be using.
using SecretExposureReport = std::function<void(std::string_view)>;

/// Which files a configuration's secrets live in.
///
/// The one thing the two executables do not share. `fastcached` answers it with
/// `DaemonSecretFiles`, `fastcache-compile-node` with `Node::NodeSecretFiles`, and
/// both apply whatever provenance gate they have before returning -- see
/// `SecretFileWarnings` for why a path-reached secret has none.
///
/// **Whatever it captures is duplicated per reload.** `ConfigReloaderOf::Reload()`
/// copies its subscriber list on every SIGHUP, so this holds the one bit or path the
/// subject list reads, never a whole parse result. Measured on the daemon, over one
/// `Reload()`'s subscriber-list copy and counting the CAPTURE alone: 7 allocations and
/// 828 bytes for a captured `CliResult` -- which carries a whole `Config` -- against 1
/// allocation and 56 bytes for one bool. The figures live here and nowhere else; the
/// daemon overload below points at them rather than restating them.
/// @tparam ConfigT The configuration this executable reloads.
template <typename ConfigT>
using SecretSubjectFiles = std::function<std::vector<std::filesystem::path>(ConfigT const&)>;

/// Report @p reloader's current snapshot's secret-file exposure now, and again at
/// every reload.
///
/// **One call for both moments, deliberately.** #384's startup check and #753's
/// reload check are one rule asked twice, and writing them apart is the shape #396
/// and #726 already paid for -- two copies of one rule that drift on the question
/// nobody re-reads. It also has to be one call for a reason stronger than tidiness:
/// the startup answer is what SEEDS the memory a reload compares against, so a
/// subscription attached without the initial observation would report a standing
/// exposure a second time at the first SIGHUP.
///
/// **A template over the configuration type**, because both binaries have this
/// exposure and only one of them had the check
/// ([#868](https://github.com/LASTRADA-Software/fastcached/issues/868)). #753 declined
/// to generalise it on the reading that the worker could not adopt a `Subscribe` at
/// all; that reading was one frame too wide. `ApplyReloadRequest`'s decline is about a
/// subscriber capturing **`WorkerBody`'s** locals -- the reloader is declared in
/// `main` and outlives that frame, and `Subscribe` has no unsubscribe. A subscription
/// attached from `main`, beside the reloader, capturing only what `main` owns, is the
/// arrangement that decline describes as safe, and it is exactly what `DaemonBody`
/// already does. The lifetime rule is unchanged; what moved is which frame attaches.
///
/// The subscriber ignores the `previous` snapshot the reloader hands it. That is not
/// an oversight -- see `SecretExposureWatcher` for why a configuration snapshot
/// cannot answer a question about a file's mode.
///
/// The watcher is owned by the subscription rather than by the caller, so there is no
/// object whose lifetime a call site could get wrong. @p subjects and @p report are
/// MOVED into that subscription and are owned by it too -- what a call site still owns
/// is whatever the callables REFER to, and that is what has to outlive @p reloader,
/// because the subscription calls them from `Reload()`. Stating the requirement
/// against the arguments themselves would be unfollowable as written: both are
/// ordinarily temporary lambdas that die at the end of the call. `DaemonBody` is the
/// shape to copy -- it hands over `[&logger]` and declares `logger` BEFORE the
/// reloader, so the referent is destroyed after it.
///
/// @tparam ConfigT The configuration this executable reloads.
/// @param reloader The pipeline whose snapshots this follows. Its current snapshot is
///        observed before this returns.
/// @param subjects Which files hold this process's secrets, gate applied.
/// @param report Where a warning goes. Moved into the subscription; whatever it
///        REFERS to must outlive @p reloader.
template <typename ConfigT>
void WatchSecretExposure(ConfigReloaderOf<ConfigT>& reloader,
                         std::type_identity_t<SecretSubjectFiles<ConfigT>> subjects,
                         SecretExposureReport report)
{
    // Owned by the closures rather than by the caller, so no call site can hand this
    // a lifetime shorter than the reloader's. `Subscribe` has no unsubscribe, so a
    // watcher living in the caller's frame would be a hazard nobody could see.
    auto const watcher = std::make_shared<SecretExposureWatcher>();

    auto observe = [watcher, subjects = std::move(subjects), report = std::move(report)](ConfigT const& cfg) {
        // Through the caller's subject list rather than deriving one here: the start
        // and every reload must ask the SAME question, and two derivations of one
        // gate is the shape this whole arrangement exists to avoid.
        for (auto const& warning: watcher->Observe(subjects(cfg)))
            report(warning);
    };

    // The startup observation, and it is what SEEDS the memory a reload compares
    // against: a subscription attached WITHOUT it would report a standing exposure a
    // second time at the first SIGHUP, because the memory would still be empty.
    //
    // The claim is about the observation HAPPENING, never about its order relative to
    // `Subscribe` -- observing second would seed the memory just as well, and no
    // reload can be observed before this call returns anyway. It is written first
    // because a rule that does not depend on the order should not be spelled as one:
    // a comment claiming a reordering breaks something is a reason that outruns the
    // fact it was drawn from, which is the failure this file's own rulebook records.
    observe(*reloader.Current());

    // Moved rather than copied: the seeding call above is the last use here, and the
    // subscriber list is where this closure lives from now on.
    reloader.Subscribe([observe = std::move(observe)](auto const& /*previous*/, auto const& current) { observe(*current); });
}

/// Report @p cfg's secret-file exposure ONCE, for a process with no file to reload.
///
/// **No watcher, and that is the whole difference.** A remembered answer exists to
/// keep a second observation quiet; a process started entirely from argv has no
/// second moment, so there is nothing to remember and nothing that could repeat. It
/// still has secret files -- `fastcache-compile-node` reaches four of them by path
/// and needs no configuration file to name any of them -- so the observation itself
/// is not optional.
///
/// Named rather than left as a loop at the call site, so the two arms of "is there a
/// file to reload" read as one vocabulary and neither can quietly stop reporting.
///
/// @tparam ConfigT The configuration in force.
/// @param cfg What this process was told to be.
/// @param subjects Which files hold its secrets, gate applied.
/// @param report Where a warning goes.
template <typename ConfigT>
void ReportSecretExposure(ConfigT const& cfg,
                          std::type_identity_t<SecretSubjectFiles<ConfigT>> const& subjects,
                          SecretExposureReport const& report)
{
    for (auto const& warning: SecretFileWarnings(subjects(cfg)))
        report(warning);
}

/// The daemon's `WatchSecretExposure`, with its subject list supplied.
///
/// A named overload rather than a lambda at `DaemonBody`'s call site, so
/// `DaemonSecretFiles` stays this header's business and the one thing the daemon has
/// to hand over is the provenance bit.
///
/// **The BIT, never the whole `CliResult`**, for the reason and the figures
/// `SecretSubjectFiles` carries -- stated there once and pointed at from here, because
/// a measurement restated in two places is two claims that drift apart. Copied rather
/// than referenced either way, because argv does not change under a running process
/// and a reload must ask exactly the question the start asked.
///
/// @param reloader The pipeline whose snapshots this follows. Its current snapshot is
///        observed before this returns.
/// @param secretNamedOnCommandLine Whether argv supplied `--requirepass`; the parse's
///        own `CliResult::requirePassExplicit`.
/// @param report Where a warning goes. Moved into the subscription; whatever it
///        REFERS to must outlive @p reloader.
void WatchSecretExposure(ConfigReloader& reloader, bool secretNamedOnCommandLine, SecretExposureReport report);

} // namespace FastCache
