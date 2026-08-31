// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <IProcessRunner.hpp>
#include <ToolchainDiscovery.hpp>
#include <ToolchainHost.hpp>

namespace FastCache::Node
{

/// One toolchain this worker will serve, before its identity is computed.
struct ToolchainEntry
{
    std::string fingerprint; ///< Empty when the node must compute it.
    std::string compiler;    ///< Path to the compiler.
};

/// How the set of compilers to serve was arrived at.
///
/// Carried from the cheap half of the survey to the expensive one because it is the
/// only thing that makes a "nothing to serve" refusal honest, and there are three
/// ways to arrive at that: the machine was searched and holds nothing, every named
/// compiler was rejected, or nothing was named and nothing was to be searched. The
/// first names WHERE it looked; the others must not, because reciting places nobody
/// looked in reads as "your compiler is not installed" when the answer is "the one
/// you named was refused, a line above".
///
/// An enum rather than the `bool discovered` this replaced, and not only for the
/// third case: a bool passed between two functions is a bool one of them can be
/// handed wrongly, and the three messages are three states.
enum class ToolchainSource : std::uint8_t
{
    /// Discovery answered and the set is the machine's own.
    MachineSearched,
    /// Every entry came from a `--toolchain`. The operator's list wins whole.
    OperatorNamed,
    /// `--no-toolchain-discovery` and no `--toolchain`: this worker was told to
    /// serve nothing, which is a configuration answer rather than a search result.
    NothingToSearch,
};

/// Which compilers this node will serve, before any of them has been fingerprinted.
///
/// The **cheap** half of the survey, and the split exists because the two halves cost
/// three orders of magnitude apart. Deciding the set is a spawn per candidate;
/// identifying them walks every byte under every include root, which has been
/// measured at 5136 files and over 300 s on a cold Windows CI runner
/// ([#354](https://github.com/LASTRADA-Software/fastcached/issues/354)). Keeping the
/// cheap half synchronous is what lets a misconfigured node still be refused at
/// startup, promptly, while the expensive half moves off the path that blocks the
/// node from serving anything at all
/// ([#365](https://github.com/LASTRADA-Software/fastcached/issues/365)).
struct DiscoveredToolchains
{
    std::vector<ToolchainEntry> entries; ///< What to fingerprint, in table order.
    ToolchainSource source;              ///< How the set was chosen.
};

/// What a toolchain's fingerprint was computed FROM, so it can be rechecked cheaply.
///
/// A node fingerprints once at startup and then lives for weeks, while the launcher
/// recomputes per invocation -- so a compiler patched in place under a running
/// service leaves the node advertising the pre-upgrade digest and spawning the
/// post-upgrade compiler. Clients then receive objects built by a compiler they did
/// not key against and store them in the shared cache under the old key, where the
/// whole fleet reads them (#238).
///
/// Rechecking that by re-deriving the fingerprint would cost a driver spawn and a
/// walk of the include tree. What this holds instead is the three INPUTS
/// `ComputeToolchainStamp` folds, so re-asking is a stat of the binary plus one stat
/// per include root and spawns nothing at all. The expensive half is paid only once
/// the cheap half says something moved.
///
/// Empty for an operator's `<fingerprint>=<compiler>` override, which is never
/// probed and must never be second-guessed: pinning a digest by hand is how an
/// operator forces a fleet to agree while a machine is being repaired, and a node
/// that re-derived it would undo exactly that. `Watchable()` is how that is asked.
struct ToolchainWitness
{
    /// The compiler as it was STAT'd -- resolved on the search path, not as typed.
    ///
    /// A bare `cc` or `cl` cannot be stat'd from an arbitrary working directory, so
    /// stamping the typed spelling would yield an empty stamp and silently give up
    /// on the toolchain most likely to be upgraded by a package manager.
    std::string compiler;

    std::string banner;             ///< The version line the fingerprint folded.
    std::vector<std::string> roots; ///< The include search roots it folded.

    /// What those three hashed to when this node surveyed the machine.
    ///
    /// Empty means the toolchain could not be stamped at all -- an unstattable
    /// compiler, or a root whose name this process cannot decode. That is not a
    /// change and must never be read as one: an empty stamp recomputes to empty, so
    /// such a toolchain is simply never found stale, which is the same "refusing to
    /// stamp is refusing to cache" trade `ComputeToolchainStamp` documents.
    std::string stamp;

    /// Whether this toolchain can be rechecked at all.
    ///
    /// Asked rather than compared against an empty field, so what counts as watchable
    /// stays one decision the day a second reason to skip a toolchain appears.
    /// @return True when a stamp exists to compare against.
    [[nodiscard]] bool Watchable() const noexcept
    {
        return !stamp.empty();
    }
};

/// One toolchain this worker serves, once its identity is known.
struct ServedToolchain
{
    std::string compiler; ///< Path to the compiler.

    /// What a person calls it, e.g. `cl 19.44.35207`. Empty when nothing said.
    ///
    /// **Display only.** The fingerprint decides every match; this decides nothing,
    /// and both are reported because they answer different questions -- the digest is
    /// what a launcher compares, and this is what an operator reads (#194). A machine
    /// with two MSVC toolsets showed two opaque hashes and no way to tell which was
    /// which, and the fingerprint deliberately stopped being derivable by hand.
    ///
    /// Empty for an operator's `<fingerprint>=<compiler>` override, which is never
    /// probed and so has no banner to read a label out of. Empty means "did not say"
    /// everywhere it travels, and is rendered as absent rather than as a blank.
    std::string label;

    /// What this toolchain's identity was derived from, for rechecking it later.
    ///
    /// Carried HERE rather than in a second map keyed by the same fingerprint. Two
    /// maps that must agree about which toolchains exist are two maps that will
    /// eventually disagree, and the one that decides what this worker serves is
    /// this one.
    ToolchainWitness witness;
};

/// Split a `--toolchain` value into its fingerprint and compiler.
///
/// Two accepted shapes, and the bare one is what operators should use:
///
///   `<compiler>`               -- the node computes the fingerprint itself
///   `<fingerprint>=<compiler>` -- an explicit override
///
/// The bare form exists because the fingerprint stopped being something a person
/// can derive. It used to be the compiler's `--version` line, which an operator
/// could read off a terminal; it is now a digest over the whole include tree, and
/// requiring that to be pasted into a config would make every toolchain update a
/// manual two-step that silently un-registers a worker when somebody forgets.
///
/// The override is kept because it is the only way to run a worker whose compiler
/// this process cannot execute -- a cross-compiler, or a wrapper that must not be
/// spawned at configuration time -- and because pinning a fingerprint by hand is
/// how an operator forces a fleet to agree while a machine is being repaired.
///
/// Split on the FIRST `=`, since a fingerprint is hex and contains none. A
/// compiler path containing `=` is therefore only reachable through the override
/// form, which is the documented escape hatch rather than a silent mis-parse --
/// and is why a DISCOVERED path never comes through here.
///
/// @param spec The flag's value.
/// @return The entry, or nullopt when it is empty or malformed.
[[nodiscard]] std::optional<ToolchainEntry> SplitToolchain(std::string_view spec);

/// The layouts discovery searches, for a refusal that can be acted on.
///
/// Off the shared table rather than a list written by hand, so a row added there
/// necessarily appears in the diagnosis -- a hand-written list is maintained by the
/// same person who forgot to add the row.
///
/// @return The layout names, comma-separated.
[[nodiscard]] std::string SearchedLayouts();

/// Every toolchain this worker will serve, from the operator or from the machine.
///
/// Lives here rather than in `main.cpp` because every rule below is a decision with
/// a failure mode, and `main.cpp` is in no test target -- which is exactly how the
/// two defects this function was written with (a discovered path re-parsed through
/// the operator's `=` grammar, and an empty result reported as a healthy worker)
/// got as far as review.
///
/// **The operator's list wins whole.** Naming any `--toolchain` pins the worker to
/// exactly that set; naming none, with discovery on, means "serve what this machine
/// has". The two are never merged, because a merged set would quietly re-add a
/// compiler an operator had deliberately narrowed away.
///
/// **A discovered compiler that cannot be spawned is dropped**, with a line naming
/// it and the layout that found it. That is the `SpawnFailed` refusal a client
/// otherwise meets at job time, moved to startup where an operator can see it. An
/// operator-NAMED toolchain is not probed: the `<fingerprint>=<compiler>` override
/// exists precisely for a compiler this process cannot execute.
///
/// **A worker with nothing to serve is refused here**, not reported as an empty set
/// for the caller to judge. Left to run it is the worst shape this system has:
/// nothing registers, the heartbeat calls "0 of 0 toolchain(s)" a success, and the
/// ready line says the node is up -- a healthy unit, a green fleet, and every build
/// compiling locally with no error at either end. Refusing is also what makes the
/// message testable, and there are three of them because there are three ways to
/// arrive: the machine was searched and holds nothing, every named compiler was
/// rejected, or nothing was named and nothing was to be searched. The first names
/// where it looked; the others must NOT, because reciting places nobody looked in
/// reads as "your compiler is not installed".
///
/// @param cfg What the operator asked for.
/// @param discovery Where the machine's own compilers come from; null when
///        `--no-toolchain-discovery` was given.
/// @param runner Process-spawning seam, for the compiler probes.
/// @param host The machine's filesystem, registry and environment.
/// @param clock Where the hash phase's progress rate reads elapsed time (#354). It
///        is the only thing here that reads a clock, and it reads one because a
///        phase that has been observed running past 300 s without finishing cannot
///        be diagnosed by anything that does not know how fast it is going.
/// @param logger Startup log.
/// @return Fingerprint to what this worker serves under it -- never empty -- or
///         nullopt when a `--toolchain` value is malformed or there is nothing to
///         serve.
[[nodiscard]] std::optional<std::map<std::string, ServedToolchain>> ResolveToolchains(NodeConfig const& cfg,
                                                                                      Cc::IToolchainDiscovery* discovery,
                                                                                      Cc::IProcessRunner& runner,
                                                                                      Cc::IToolchainHost& host,
                                                                                      IClock const& clock,
                                                                                      ILogger& logger);

/// Decide WHICH compilers this node will serve, without identifying any of them.
///
/// `ResolveToolchains` is this followed by `FingerprintToolchains`, and every rule
/// above still holds -- the two are split by COST, not by policy. This half spawns
/// each discovered candidate once to prove it can be executed and parses each
/// `--toolchain`; it walks nothing.
///
/// A node that cannot serve anything is NOT refused here, and that is deliberate
/// rather than an omission. Discovery finding four compilers does not mean four will
/// be served: a fingerprint that does not identify its toolchain is dropped, so
/// "nothing to serve" can only be decided once the expensive half has run, and it is
/// decided there.
///
/// @param cfg What the operator asked for.
/// @param discovery Where the machine's own compilers come from; null when
///        `--no-toolchain-discovery` was given.
/// @param runner Process-spawning seam, for the "can this be executed" probe.
/// @param logger Startup log.
/// @return The set and how it was chosen, or nullopt when a `--toolchain` value is
///         malformed -- the one refusal this half can reach, and the reason it is
///         worth keeping on the startup path.
[[nodiscard]] std::optional<DiscoveredToolchains> DiscoverToolchainEntries(NodeConfig const& cfg,
                                                                           Cc::IToolchainDiscovery* discovery,
                                                                           Cc::IProcessRunner& runner,
                                                                           ILogger& logger);

/// What identifying the discovered compilers came to.
///
/// Three states, not an `optional`, because the third is real and is not either of
/// the others. A survey that was ABANDONED because the node is stopping has produced
/// no answer, and folding it into "nothing to serve" would exit a stopping node with
/// a configuration error it does not have -- in the log an operator reads to find out
/// why `systemctl stop` took a while.
enum class SurveyOutcome : std::uint8_t
{
    /// The walk finished and this node serves what `served` holds.
    Served,
    /// The walk finished and nothing survived it. A startup refusal; see
    /// `FingerprintToolchains`.
    NothingToServe,
    /// The node was asked to stop mid-walk. `served` is meaningless.
    Cancelled,
};

/// The outcome and, when there is one, the answer.
struct SurveyResult
{
    SurveyOutcome outcome { SurveyOutcome::NothingToServe }; ///< What happened.
    std::map<std::string, ServedToolchain> served;           ///< Empty unless `Served`.
};

/// Identify the discovered compilers, and refuse a node that would serve none.
///
/// The **expensive** half: a driver spawn and a full walk of the include tree per
/// entry, concurrently. See `DiscoveredToolchains` for what that costs and why the
/// split exists.
///
/// @param discovered What `DiscoverToolchainEntries` returned.
/// @param runner Process-spawning seam, for the compiler probes.
/// @param host The machine's filesystem, registry and environment.
/// @param clock Where the hash phase's progress rate reads elapsed time (#354).
/// @param logger Startup log.
/// @return Fingerprint to what this worker serves under it -- never empty -- or
///         nullopt when there is nothing to serve, which is refused HERE with the
///         message that fits `discovered.source`.
/// @param stop Observed between toolchains and between hashed files, so a node told
///        to stop mid-walk stops. It is not a courtesy: since #365 this runs on the
///        heartbeat thread, whose `jthread` destructor joins before `main` returns,
///        and the stop handlers are installed a few lines after it starts. An
///        unobservable walk therefore makes `systemctl stop` wait out the whole
///        survey -- minutes on a cold machine -- and a supervisor answers that with
///        SIGKILL and no diagnostic. Pass a default-constructed token where nothing
///        can cancel, which is every caller but the node's.
[[nodiscard]] SurveyResult FingerprintToolchains(DiscoveredToolchains const& discovered,
                                                 Cc::IProcessRunner& runner,
                                                 Cc::IToolchainHost& host,
                                                 IClock const& clock,
                                                 ILogger& logger,
                                                 std::stop_token const& stop);

/// Which of these toolchains no longer match the machine they were derived from.
///
/// The cheap half of #238, and it spawns **nothing**: every input the stamp folds
/// was recorded when the toolchain was surveyed, so this is a stat of each compiler
/// binary plus one stat per include search root. That is what makes it affordable on
/// every heartbeat, and re-deriving a fingerprint -- a driver spawn and a walk of the
/// whole include tree -- is paid only once this has said something moved.
///
/// What it catches is what `ComputeToolchainStamp` covers, which is the pair of cases
/// that actually happen: the binary's size and mtime catch a distribution upgrading
/// `gcc` in place, and each root's own mtime catches a Windows SDK update that adds
/// or removes headers while leaving `cl.exe` untouched. A header edited in place
/// under an unchanged directory is deliberately not covered -- a system toolchain's
/// headers are installed rather than edited, and the alternative is the multi-second
/// walk this exists to avoid.
///
/// A toolchain that is not `Watchable()` is skipped rather than reported: an
/// operator's pinned `<fingerprint>=<compiler>` is never probed and must never be
/// second-guessed, and a compiler that could not be stamped at all yields an empty
/// stamp that would otherwise compare equal forever anyway.
///
/// @param served What this worker is serving now, witnesses included.
/// @return The fingerprints whose evidence moved, sorted; empty when nothing did.
[[nodiscard]] std::vector<std::string> StaleToolchains(std::map<std::string, ServedToolchain> const& served);

/// What rechecking this node's toolchains against the machine concluded.
struct ToolchainRefresh
{
    /// Whether the machine moved and `served` is therefore the new answer.
    ///
    /// False means nothing changed and `served` is empty -- deliberately, so a caller
    /// cannot install a stale copy by forgetting to ask. It is the common case: this
    /// runs on every heartbeat and answers false on almost all of them.
    bool changed { false };

    /// What this worker must serve from now on, witnesses refreshed.
    ///
    /// **May legitimately be empty while `changed` is true.** That is the machine
    /// whose only compiler was removed or broken by the upgrade, and it is a state
    /// this node has to be able to reach: serving nothing is correct, and serving a
    /// fingerprint it can no longer honour is the wrong-object path itself.
    std::map<std::string, ServedToolchain> served;
};

/// How hard a recheck should look.
enum class RecheckDepth : std::uint8_t
{
    /// Ask the witnesses, and survey only if one of them moved.
    ///
    /// The cadence a heartbeat runs at: stats only, no process spawned.
    WhenEvidenceMoved,

    /// Survey regardless of what the witnesses say.
    ///
    /// The way back from a machine that is serving LESS than it should. A recheck
    /// driven by witnesses can only ever notice what it is already watching, and a
    /// toolchain that leaves the served set takes its witness with it -- so a
    /// compiler dropped by a transient probe failure, or removed and then
    /// reinstalled, would never be looked at again. A node serving nothing at all is
    /// the terminal case: with no witnesses left it could not recover without being
    /// restarted, which is exactly the "the compiler may come back with the next
    /// package" promise the rest of this makes.
    ///
    /// It costs a survey, so it belongs on a slow cadence rather than a heartbeat.
    Unconditional,
};

/// Re-derive what this node serves, if the machine changed underneath it.
///
/// The whole of #238's decision, in one place a test can drive. A node fingerprints
/// once at startup and then lives for weeks while the launcher recomputes per
/// invocation, so a compiler patched in place leaves the node advertising the
/// pre-upgrade digest and spawning the post-upgrade compiler. Clients receive objects
/// built by a compiler they did not key against and store them in the shared cache
/// under the old key, where the whole fleet then reads them.
///
/// **It re-registers under the new fingerprint AND stops serving the old one, and
/// those are not alternatives.** Stopping is the load-bearing half -- it is the
/// wrong-object path -- but stopping alone would take a machine out of the fleet on
/// every routine upgrade, and a staggered rollout across an estate would empty the
/// fleet one machine at a time with nothing anywhere saying so. That is the same
/// silent-success shape the rest of this system is built to refuse, only slower.
///
/// A client holding a lease for a fingerprint that has just been dropped is refused
/// `UnknownFingerprint` by the worker and compiles locally. That answer, its wire
/// code and its counter already exist; nothing new is invented for this.
///
/// The re-survey is `ResolveToolchains` itself rather than a second, cheaper
/// derivation. A node whose identity was computed one way at startup and another way
/// afterwards would drift from its own clients exactly when an operator is least
/// able to see it.
///
/// @param served What this worker is serving now, witnesses included.
/// @param cfg What the operator asked for.
/// @param discovery Where the machine's own compilers come from; null when
///        `--no-toolchain-discovery` was given.
/// @param runner Process-spawning seam, for the compiler probes.
/// @param host The machine's filesystem, registry and environment.
/// @param clock Where the re-survey's progress rate reads elapsed time (#354).
/// @param logger Where the change is announced.
/// @param depth Whether to survey unconditionally, or only on moved evidence.
/// @return What changed, and what to serve; `changed` false when nothing moved.
///
/// An unconditional sweep that finds the machine unchanged still answers `changed`
/// false. It is a recovery path, not a reason to re-register a fleet's worth of
/// workers every time it runs.
[[nodiscard]] ToolchainRefresh RefreshToolchains(std::map<std::string, ServedToolchain> const& served,
                                                 NodeConfig const& cfg,
                                                 Cc::IToolchainDiscovery* discovery,
                                                 Cc::IProcessRunner& runner,
                                                 Cc::IToolchainHost& host,
                                                 IClock const& clock,
                                                 ILogger& logger,
                                                 RecheckDepth depth = RecheckDepth::WhenEvidenceMoved);

} // namespace FastCache::Node
