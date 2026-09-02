// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// The three ways a `0xFC` surface may answer a refusal, and nothing else.
///
/// `CompileCacheWire::EncodeErrorReply` takes a code and knows nothing about a sink,
/// so counting a refusal was a thing each author had to remember. Six sites forgot
/// ([#327](https://github.com/LASTRADA-Software/fastcached/issues/327)), then five
/// more accumulated on the merged listener through an entire surface migration
/// ([#447](https://github.com/LASTRADA-Software/fastcached/issues/447)) -- and a
/// refusal answered while nothing rises is indistinguishable, on `/metrics`, from a
/// port nobody is talking to.
///
/// ## Why there are three and not two
///
/// `Refuse` alone was not enough, and the reason is the defect that outlived #327's
/// fix. Some refusals are DELIBERATELY not counted: the in-flight byte budget says a
/// surface is momentarily full, the peer retries past it, and summed into a
/// credential series it would make that series unreadable. That is a considered
/// position and a correct one -- but it was spelled as a bare `EncodeErrorReply`,
/// which is also how "forgot" is spelled. **A scan cannot tell a decision from an
/// omission when the two produce identical text**, which is the silence-reads-as-
/// coverage failure this file's own check exists to close, one level down
/// ([#492](https://github.com/LASTRADA-Software/fastcached/issues/492)).
///
/// So the three spellings are three different FACTS, each of which somebody has
/// asserted:
///
/// | Spelling                | The claim being made                                     |
/// |-------------------------|----------------------------------------------------------|
/// | `Refuse`                | A rise here means something an operator acts on.          |
/// | `RefuseWithoutCounter`  | A rise here would mean nothing. Here is why.              |
/// | `RefuseUntriaged`       | Nobody has decided yet. Here is the issue that will.      |
///
/// The third exists because the alternative was worse. Converting an undecided site
/// to `RefuseWithoutCounter` with a placeholder reason spells *forgot* using the
/// vocabulary of *decided*, and the next reader cannot tell those apart either --
/// only now they look considered. `RefuseUntriaged` is provisional by construction:
/// it carries an issue number, `worker-refusals-counted` counts every one of them and
/// reports the total on each run, so the backlog is visible, monotonic, and cannot be
/// added to silently.
///
/// ## Why this header is here
///
/// `SurfaceRefusal` and `Refuse` lived in `fastcache-cc`'s private
/// `WorkerProtocol.hpp`, and the node's generic frame listener -- which is not a
/// compile surface at all -- included that header for two symbols. Worse, the check
/// that guards the rule could then only be pointed at a hand-kept list of the files
/// somebody remembered, which is exactly how the five sites accumulated. With the
/// primitive in one shared place, coverage is a property of the type: every file in
/// the tree must reach a refusal through this header, and the check needs no list.
///
/// It is header-only on purpose. `Refuse` is one increment and one call, its only
/// dependencies are `CompileCacheWire.hpp` (header-only and dependency-free, because
/// the launcher does not link `FastCache`) and `IMetricsSink.hpp` (standard library
/// only), so this costs `_fc_cc_core` no row and the build no translation unit.
/// `CompileCacheWire.hpp` itself is untouched and stays dependency-free.

/// One refusal a `0xFC` surface answers with AND counts: the wire code a client acts
/// on and the counter an operator watches, as ONE row.
///
/// **The row is the REFUSAL, not the code**, and that distinction is load-bearing.
/// Two refusals on the compile surface both answer `MalformedFrame` and must not
/// share a counter -- a truncated frame is a framing fault or a hostile peer, an
/// undecodable payload is a version or encoding mismatch between two ends that agree
/// on the framing. One code, because a client acts on both identically; two counters,
/// because an operator does not. A table keyed on the code could not hold both, which
/// is why `EnumTable<ErrorCode, Counter>` is the wrong instrument here (#327).
struct SurfaceRefusal
{
    CompileCacheWire::ErrorCode code; ///< What the client is told.
    IMetricsSink::Counter counter;    ///< What the operator sees rise.
};

/// One refusal a surface answers and deliberately does not count.
///
/// The `rationale` is never sent and never read at run time. It is a **forcing
/// function**: the author cannot write the call without answering "would a rise here
/// mean something happened", which is the question #447 extracted and #491 is about.
/// This project already spells decisions that way -- `StartupPolicyRejection` carries
/// a per-row reason, and the option table carries `why` text nothing transmits.
struct UncountedRefusal
{
    CompileCacheWire::ErrorCode code; ///< What the client is told.

    /// Why no counter moves. Never sent, never read at run time -- see the type's own
    /// documentation for why it is here at all.
    ///
    /// **Not spelled `why`**, which on `CompileCacheWire::RefusedVerb` is the text a
    /// CLIENT is sent. The two meet in one expression at several call sites, and one
    /// word carrying two opposite contracts is how a reader comes to send this one.
    std::string_view rationale;
};

/// One refusal nobody has decided the counter policy for yet.
///
/// Distinct from `UncountedRefusal` because "not yet decided" and "decided not to" are
/// different facts, and collapsing them is how a backlog becomes invisible. Every one
/// of these is counted by `worker-refusals-counted` and reported on every run, so the
/// set can only shrink deliberately and can never be added to unnoticed.
struct UntriagedRefusal
{
    CompileCacheWire::ErrorCode code; ///< What the client is told.
    std::uint32_t issue;              ///< The issue that will decide whether this counts.
};

/// Answer a refusal, and record it when anything is collecting.
///
/// Taking a ROW rather than a code is what makes the counter impossible to forget:
/// there is no argument to pass a bare `ErrorCode` to.
///
/// **The pointer overload is the one that encodes, and it takes a pointer because
/// `SessionContext::metrics` is one.** That member is optional by contract -- "a
/// scheduler must schedule whether or not anyone is scraping it" -- and its own
/// documentation refuses a null-object default, because a silently-discarding sink and
/// a genuinely absent one would then be indistinguishable at the call site. That is
/// this project's absent-is-not-zero rule stated at a seam.
///
/// So the guard lives HERE rather than at each caller: `CompileCacheHandler` alone has
/// twelve refusals, and twelve hand-written null checks are twelve chances to write
/// `*metrics` instead. Callers pass the pointer they were handed and say nothing about
/// nullability.
///
/// It is also why the whole `Refuse` family spells `EncodeErrorReply` exactly ONCE:
/// `worker-refusals-counted` requires one encoder call per spelling, and an overload
/// that encoded separately would have raised the call count without raising the name
/// count -- which is the evasion that check exists to catch, arriving through the
/// header it guards.
/// @param metrics Where the refusal is recorded, or null when nothing collects.
/// @param refusal Which refusal, as one row.
/// @param detail Words for a person, or empty when there are none to add.
/// @return The encoded reply.
[[nodiscard]] inline std::vector<std::byte> Refuse(IMetricsSink* metrics,
                                                   SurfaceRefusal const& refusal,
                                                   std::string_view detail = {})
{
    if (metrics != nullptr)
        metrics->Increment(refusal.counter);
    return CompileCacheWire::EncodeErrorReply(refusal.code, detail);
}

/// Answer a refusal, and record it.
///
/// The reference overload, for a surface that holds a sink outright rather than
/// reaching one through a session. Delegates, so there is one encoder call for both.
/// @param metrics Where the refusal is recorded.
/// @param refusal Which refusal, as one row.
/// @param detail Words for a person, or empty when there are none to add.
/// @return The encoded reply.
[[nodiscard]] inline std::vector<std::byte> Refuse(IMetricsSink& metrics,
                                                   SurfaceRefusal const& refusal,
                                                   std::string_view detail = {})
{
    return Refuse(&metrics, refusal, detail);
}

/// Answer a refusal that is deliberately not counted.
///
/// @param refusal Which refusal, and why nothing rises for it.
/// @param detail Words for a person, or empty when there are none to add.
/// @return The encoded reply.
[[nodiscard]] inline std::vector<std::byte> RefuseWithoutCounter(UncountedRefusal const& refusal,
                                                                 std::string_view detail = {})
{
    return CompileCacheWire::EncodeErrorReply(refusal.code, detail);
}

/// Answer a refusal whose counter policy is still undecided.
///
/// Behaves exactly as `RefuseWithoutCounter` today. The difference is what it CLAIMS,
/// and the claim is what the check tallies.
///
/// @param refusal Which refusal, and the issue that will decide it.
/// @param detail Words for a person, or empty when there are none to add.
/// @return The encoded reply.
[[nodiscard]] inline std::vector<std::byte> RefuseUntriaged(UntriagedRefusal const& refusal, std::string_view detail = {})
{
    return CompileCacheWire::EncodeErrorReply(refusal.code, detail);
}

} // namespace FastCache::Cc
