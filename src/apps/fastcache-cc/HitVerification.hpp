// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
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

/// Compare the object a hit produced against one a fresh compile produced.
///
/// A byte comparison rather than a hash: the two files are already on this disk, the
/// sizes are megabytes at most, and a hash would add a way for the comparison itself
/// to be the thing that is wrong.
///
/// @param served What the cache put on disk.
/// @param fresh What the compiler just produced.
/// @return `Matched`, `Mismatched`, or `Inconclusive` when either could not be read.
[[nodiscard]] HitVerdict CompareObjectFiles(std::filesystem::path const& served, std::filesystem::path const& fresh);

/// What to tell an operator about @p verdict, or empty when there is nothing to say.
///
/// One spelling, because the message is the entire product of this feature: a
/// mismatch that is counted and not described is a number somebody has to come back
/// and ask about.
/// @param verdict What the comparison found.
/// @param key The object key, so the entry can be looked at rather than only counted.
/// @return The line, or empty for `NotChecked` and `Matched`.
[[nodiscard]] std::string DescribeVerdict(HitVerdict verdict, std::string_view key);

} // namespace FastCache::Cc
