// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/ConfigReloader.hpp>
#include <FastCache/Config/SecretProvenance.hpp>

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
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
    ///         in @p files order; empty when nothing changed for the worse.
    [[nodiscard]] std::vector<std::string> Observe(std::span<std::filesystem::path const> files);

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

/// Report the daemon's secret-file exposure now, and again at every reload.
///
/// **One call for both moments, deliberately.** #384's startup check and #753's
/// reload check are one rule asked twice, and writing them apart is the shape #396
/// and #726 already paid for -- two copies of one rule that drift on the question
/// nobody re-reads. It also has to be one call for a reason stronger than tidiness:
/// the startup answer is what SEEDS the memory a reload compares against, so a
/// subscription attached without the initial observation would report a standing
/// exposure a second time at the first SIGHUP.
///
/// The subscriber ignores the `previous` snapshot the reloader hands it. That is not
/// an oversight -- see `SecretExposureWatcher` for why a configuration snapshot
/// cannot answer a question about a file's mode.
///
/// The watcher is owned by the subscription rather than by the caller, so there is no
/// object whose lifetime a call site could get wrong. @p report is not: it is called
/// from `Reload()`, so it must outlive @p reloader.
///
/// @param reloader The pipeline whose snapshots this follows. Its current snapshot is
///        observed before this returns.
/// @param cli The command-line parse, for its provenance bits. Copied, because argv
///        does not change under a running process and a reload must ask exactly the
///        question the start asked.
/// @param report Where a warning goes. Must outlive @p reloader.
void WatchSecretExposure(ConfigReloader& reloader, CliResult cli, SecretExposureReport report);

} // namespace FastCache
