// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string_view>

/// A worker's private scratch root, claimed exclusively for the life of the process.
///
/// ## What this closes
///
/// A worker derived its scratch root from `temp_directory_path() /
/// "fastcache-compile-node"` and numbered jobs beneath it from a counter starting at
/// 1 in every process. So a second node on one host -- a service plus a hand-started
/// debug run, two instances an operator started, a container sharing a mount --
/// derived the IDENTICAL `job-1`, and with it the source path and the hard-coded
/// `tu.o` inside it. `create_directories` succeeds on a directory that already
/// exists, so the second process was told nothing (#279).
///
/// Both outcomes are bad and neither is loud:
///
/// - One worker's `ScratchGuard` removes the shared directory under the other, whose
///   compile then fails. The client falls back to compiling locally, so the BUILD
///   stays correct and distribution silently stops -- with nothing in any log saying
///   why. This is the same shape as #232, where the fleet cached nothing while every
///   counter read zero.
/// - Or the two compiles share `tu.o` and one answers with the other's object, which
///   its client then caches under its own key. A compile cache that answers with the
///   wrong object is worse than one that answers with nothing.
///
/// ## Claiming IS the check
///
/// Liveness is never decided by inspecting a process. A claim is an OS-level
/// exclusive hold on a lock file, taken non-blocking, and kept for as long as the
/// returned `IScratchClaim` lives. Three things follow, and each removes a hazard
/// that a pid-inspecting design would have:
///
/// - **No race.** There is no window between "is the owner alive?" and "I take it",
///   because acquiring the lock IS the answer.
/// - **`_Exit` stops being special.** `WorkerServer`'s abandoned-drain path
///   (`std::_Exit`, #239) bypasses static destructors, so the directory is leaked --
///   but the OS releases the lock however the process died. A leaked root is then a
///   root whose lock is FREE, which is exactly what "reclaimable" should mean. No
///   reaper has to reason about staleness.
/// - **A recycled pid cannot confuse it.** Nothing reads a pid to decide anything.
///
/// This is `CowTree::FilePageStore`'s rule applied to a directory rather than a
/// store file, and it diverges from that one deliberately in a single place -- see
/// `ScratchClaimRefusal::Unavailable`.
namespace FastCache::Node
{

/// Why a worker could not claim a scratch root.
///
/// Two, and they send an operator to different places: every root being held is a
/// machine running more nodes than it has roots for, while being unable to make one
/// is a disk or a permission. Collapsing them into one "no scratch" would tell
/// nobody which.
enum class ScratchClaimRefusal : std::uint8_t
{
    /// Every candidate root is held by another LIVE process.
    InUse = 0,

    /// The filesystem refused, or cannot lock at all.
    ///
    /// **Including "this filesystem does not support locking", which is the one
    /// place this deliberately parts company with `CowTree::FilePageStore`.** That
    /// one opens unguarded and says so, because refusing would stop a deployment
    /// that works today and a second opener is a possibility rather than a
    /// certainty. Here the reverse holds: an unclaimed root IS the defect, two
    /// nodes on one host collide by construction rather than by accident, and the
    /// failure is silent on exactly the machines least able to diagnose it. So
    /// there is no unguarded path at all -- no code below returns a claim it does
    /// not hold a lock for.
    ///
    /// The remedy is real and needs no flag: `TEMP`/`TMPDIR` relocates the root, so
    /// an operator on a filesystem that cannot lock points it at one that can.
    Unavailable,

    Last, ///< Not a reason, and has no row: the length of a table keyed by one.
};

/// What a refusal is called, and what an operator should do about it.
struct ScratchClaimRefusalRow
{
    ScratchClaimRefusal refusal; ///< The outcome this row describes.
    std::string_view name;       ///< Stable short name, for a log line.
    std::string_view remedy;     ///< What to actually do, in one sentence.
};

/// One row per `ScratchClaimRefusal`, in enumerator order.
inline constexpr EnumTable<ScratchClaimRefusal, ScratchClaimRefusalRow> ScratchClaimRefusalTable { {
    { .refusal = ScratchClaimRefusal::InUse,
      .name = "scratch-roots-exhausted",
      .remedy = "every scratch root on this machine is held by another running compile node; "
                "stop one, or point TEMP at a directory of its own for this node" },
    { .refusal = ScratchClaimRefusal::Unavailable,
      .name = "scratch-unavailable",
      .remedy = "the scratch directory could not be created or could not be locked; check the "
                "disk and its permissions, and point TEMP at a local filesystem if this one is "
                "a network mount that cannot lock" },
} };

static_assert(RowsInEnumeratorOrder(ScratchClaimRefusalTable, &ScratchClaimRefusalRow::refusal),
              "ScratchClaimRefusalTable must hold one row per ScratchClaimRefusal, in enumerator order");

/// The row describing @p refusal.
/// @param refusal Why the claim failed.
/// @return Its descriptor.
[[nodiscard]] constexpr ScratchClaimRefusalRow const& DescribeScratchClaimRefusal(ScratchClaimRefusal refusal) noexcept
{
    return ScratchClaimRefusalTable[static_cast<std::size_t>(refusal)];
}

/// An exclusive claim on one scratch root, released when this object is destroyed.
///
/// The claim is the object's whole purpose, so it is move-only by being
/// non-copyable and held through a `unique_ptr`: a claim that could be copied would
/// be a claim released twice, and the second release frees a root this process is
/// still compiling into.
class IScratchClaim
{
  public:
    IScratchClaim() = default;
    IScratchClaim(IScratchClaim const&) = delete;
    IScratchClaim(IScratchClaim&&) = delete;
    IScratchClaim& operator=(IScratchClaim const&) = delete;
    IScratchClaim& operator=(IScratchClaim&&) = delete;
    virtual ~IScratchClaim() = default;

    /// The directory this claim covers. Empty for no claim, which cannot happen:
    /// a claimant either returns a held claim or a refusal.
    /// @return The claimed root.
    [[nodiscard]] virtual std::filesystem::path const& Root() const noexcept = 0;

    /// Whether this root had to be emptied because a previous owner left it behind.
    ///
    /// Reported rather than merely done, because it is the one interesting thing
    /// that happens silently: a rise means processes are dying without running their
    /// cleanup, which is visible nowhere else.
    /// @return True when the root held leftovers from a dead owner.
    [[nodiscard]] virtual bool Reclaimed() const noexcept = 0;
};

/// Where a worker's private scratch root comes from.
///
/// Injected rather than called directly, per this project's rule for anything that
/// touches the filesystem -- and here the rule earns its keep twice over, because
/// the two paths that matter most are the two a test cannot otherwise reach: a root
/// held by a foreign LIVE process, and a root left behind by a foreign DEAD one.
/// Exercising those by spawning processes would confine them to the end-to-end run,
/// which is the shape of a guard nobody can regression-test.
class IScratchClaimant
{
  public:
    IScratchClaimant() = default;
    IScratchClaimant(IScratchClaimant const&) = delete;
    IScratchClaimant(IScratchClaimant&&) = delete;
    IScratchClaimant& operator=(IScratchClaimant const&) = delete;
    IScratchClaimant& operator=(IScratchClaimant&&) = delete;
    virtual ~IScratchClaimant() = default;

    /// Claim one root beneath @p base, exclusively, for as long as the result lives.
    ///
    /// @param base The directory the roots live under; created if absent.
    /// @param maxRoots How many candidates to try before giving up.
    /// @return The held claim, or why there is none.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IScratchClaim>, ScratchClaimRefusal> Claim(
        std::filesystem::path const& base, std::size_t maxRoots) = 0;
};

/// How many candidate roots a claimant walks before reporting `InUse`.
///
/// A bound rather than an unbounded scan, so a machine in a pathological state fails
/// by name instead of walking its filesystem forever. Sixty-four is far beyond any
/// real deployment -- one node per machine is the norm and a handful is an unusual
/// one -- so reaching it means something is wrong rather than something is busy.
inline constexpr std::size_t DefaultMaxScratchRoots = 64;

/// The claimant a worker uses: a lock file per root, held by the OS.
/// @return The production claimant.
[[nodiscard]] std::unique_ptr<IScratchClaimant> MakeLockFileScratchClaimant();

} // namespace FastCache::Node
