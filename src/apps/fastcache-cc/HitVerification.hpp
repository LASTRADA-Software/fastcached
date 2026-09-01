// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ObjectEquivalence.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// Deciding whether a cache hit is one of the sampled ones, and what came of it.
///
/// **The one mechanism that turns a wrong object from invisible into loud**
/// ([#423](https://github.com/LASTRADA-Software/fastcached/issues/423), split out of
/// [#368](https://github.com/LASTRADA-Software/fastcached/issues/368)). A wrong
/// object under a correct key is this project's worst failure class, and until now it
/// was detectable only by luck: #368 was found because the stale object happened to
/// crash, and one that still linked and still passed would have left no trace at all.
///
/// The file is separate from `main.cpp` for the reason `CacheProtocol.cpp`,
/// `RootReconciler.cpp` and `AdminEndpoint.cpp` are: `main.cpp` is in no test target
/// (#370), and a sampling rule nothing can exercise is a sampling rule nobody knows
/// the rate of.

/// What a verified hit turned out to be.
enum class HitVerdict : std::uint8_t
{
    /// Not sampled, or verification is off. The overwhelmingly common answer, and a
    /// state of its own rather than a `bool` beside `Matched`: "not checked" and
    /// "checked and identical" are what an operator must never see collapsed, since
    /// one is evidence and the other is silence.
    NotChecked = 0,
    /// Compiled again, and the bytes are identical. What every hit should be.
    Matched,
    /// Compiled again, and the bytes differ. The cache served an object that this
    /// compiler does not produce from this source.
    Mismatched,
    /// Sampled, and the comparison could not be made -- the fresh compile failed, or
    /// one of the two files could not be read.
    ///
    /// Its own answer rather than folded into either neighbour, because both foldings
    /// are wrong in a way that matters: read as `Matched` it reports a verification
    /// that did not happen, and read as `Mismatched` it fails a build over a full
    /// disk. What it means is "ask again", and only a distinct value can say that.
    Inconclusive,
    /// Sampled, both objects were read, and this build cannot lay out the format
    /// its own compiler just produced -- so the clock every MSVC-family driver
    /// stamps into an object cannot be told apart from a real difference.
    ///
    /// Distinct from `Inconclusive` because the two say opposite things about
    /// asking again: `Inconclusive` means the attempt failed and the next one may
    /// not, while this is a property of the toolchain that will hold for every hit
    /// until this program learns the format. Answering `Mismatched` here is the
    /// defect [#493](https://github.com/LASTRADA-Software/fastcached/issues/493)
    /// records -- a precise wrong answer, which is worse than none, and exactly how
    /// a verifier gets switched off for good.
    Unsupported,
};

/// Verification is off; no hit is checked.
inline constexpr unsigned VerificationOff = 0;

/// Whether the hit for @p key is one of the sampled ones.
///
/// **Deterministic on the key rather than random**, and that is the whole design.
/// `fastcache-cc` is spawned once per translation unit -- thousands of times in one
/// build -- so a per-process RNG would be seeded thousands of times and give neither
/// a controllable rate nor a reproducible one. Hashing the key gives a rate that
/// holds over a build, spreads across translation units rather than clustering, and
/// makes a unit that verified verify again next time, which is what somebody
/// reproducing a report needs.
///
/// @param key The object key this hit was served under.
/// @param rate Verify one hit in this many; `VerificationOff` checks none.
/// @return True when this hit should be compiled again and compared.
[[nodiscard]] bool ShouldVerifyHit(std::string_view key, unsigned rate) noexcept;

/// Parse `FASTCACHE_VERIFY`.
///
/// @param text What the environment holds; empty or absent is off.
/// @return The rate, or `VerificationOff` when the value names none.
///
/// Junk reads as OFF rather than as a refusal, which is the opposite of how this
/// project treats a malformed flag and is deliberate: this variable is a diagnostic
/// somebody sets by hand while chasing a bug, and a launcher that refused to compile
/// over a typo in it would break the build it was brought in to investigate. The
/// value is echoed on the first hit so a typo is visible rather than silent.
[[nodiscard]] unsigned ParseVerificationRate(std::string_view text) noexcept;

/// A verdict, and enough about it to act on.
struct HitComparison
{
    HitVerdict verdict { HitVerdict::Inconclusive };
    /// What the image comparison found, or nothing when no images were compared --
    /// the hit was not sampled, or one of the two files could not be read.
    ///
    /// A disengaged optional rather than an extra enumerator, because "there was no
    /// comparison" is not a comparison outcome. Carried at all because `verdict`
    /// deliberately folds `Identical` and `EquivalentApartFromVolatile` into
    /// `Matched`, and one caller needs them apart again: recovering that from
    /// `detail` being non-empty would rebuild a state from prose, which is the exact
    /// collapse `HitVerdict` refuses two enumerators above.
    std::optional<ObjectComparison> comparison;
    /// Where the difference was, what was overlooked, or which format could not be
    /// read. Empty when there is nothing to add. Owned rather than viewed: the two
    /// images it describes are read into buffers this call drops on the way out.
    std::string detail;
};

/// Compare the object a hit produced against one a fresh compile produced.
///
/// A byte comparison rather than a hash: the two files are already on this disk, the
/// sizes are megabytes at most, and a hash would add a way for the comparison itself
/// to be the thing that is wrong.
///
/// **Not a bare `memcmp`, and that is #493.** Every MSVC-family driver stamps the
/// wall clock into the object header, and a cached object was compiled earlier than
/// the fresh one it is checked against by construction -- so a byte comparison
/// answered `Mismatched` on every Windows hit, on the platform where the defect it
/// exists to detect was observed. `ObjectEquivalence.hpp` carries the measurements
/// and the single field that is normalised; on ELF, where the bytes are reproducible
/// (measured, with and without `-g`), nothing is normalised and this stays the byte
/// comparison it was.
///
/// Both files are read whole. A streaming pass that answered "identical" without
/// allocating was tried and removed: on every MSVC driver the clock guarantees a
/// difference, so it fell through to reading both files anyway on the one platform
/// this exists for, while claiming in a comment to have saved the read.
///
/// @param served What the cache put on disk.
/// @param fresh What the compiler just produced, at the same path the served object
///        occupied -- which is what keeps the driver's path records out of the
///        comparison without anything having to overlook them.
/// @return The verdict, plus a sentence naming what it turned on.
[[nodiscard]] HitComparison CompareObjectFiles(std::filesystem::path const& served, std::filesystem::path const& fresh);

/// What to tell an operator about @p verdict, or empty when there is nothing to say.
///
/// One spelling, because the message is the entire product of this feature: a
/// mismatch that is counted and not described is a number somebody has to come back
/// and ask about.
/// Takes the whole `HitComparison` rather than a verdict and an optional detail:
/// the two always travel together, a defaulted detail existed only so tests could
/// omit it, and the pairing is load-bearing prose -- a mismatch naming `.text$mn`
/// and one naming `.debug$S` are a stale object and a foreign build path, acted on
/// differently. Passing them separately made a mismatched pairing spellable.
///
/// @param comparison What the comparison found, and what it turned on.
/// @param key The object key, so the entry can be looked at rather than only counted.
/// @return The line, or empty for `NotChecked` and `Matched`.
[[nodiscard]] std::string DescribeVerdict(HitComparison const& comparison, std::string_view key);

} // namespace FastCache::Cc
