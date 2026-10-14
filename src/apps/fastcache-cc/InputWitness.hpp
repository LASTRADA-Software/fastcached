// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DirectManifest.hpp"
#include "InputFiles.hpp"
#include "Stats.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// What the input witness concluded about one invocation's inputs (#1650).
///
/// Private: never transmitted or persisted.
enum class InputStability : std::uint8_t
{
    Stable,             ///< Every input was the same at the key and after the compile.
    MovedDuringCompile, ///< The key is sound, but an input changed after it: the object may not be.
    MovedBeforeKey,     ///< An input changed while the key was being taken: neither is sound.
    Unwitnessed,        ///< An input could not be observed, so nothing about it is known.
    Last,               ///< The enumerator count; not a verdict.
};

/// Whether an invocation's object may be stored in the cache.
///
/// Private: never transmitted or persisted.
enum class StoreObject : std::uint8_t
{
    Yes, ///< Store it.
    No,  ///< Do not store it.
};

/// Whether an invocation's direct-mode manifest may be recorded.
///
/// Private: never transmitted or persisted.
enum class KeepManifest : std::uint8_t
{
    Yes, ///< Record it.
    No,  ///< Do not record it.
};

/// Whether the witness hashes contents as well as identifying files. Off when direct mode is off:
/// there is then no manifest, and the witness guards the store by identity alone.
///
/// Private: never transmitted or persisted.
enum class ContentHashing : std::uint8_t
{
    Yes, ///< Hash every input.
    No,  ///< Identify only.
};

/// What one verdict decides.
struct StabilityRow
{
    InputStability stability;  ///< The verdict this row describes.
    StoreObject store;         ///< Whether the object may be stored.
    KeepManifest manifest;     ///< Whether the manifest may be recorded.
    std::string_view sentence; ///< What happened, for the operator; empty for `Stable`.
    /// What the statistics record for this verdict when the object is not stored.
    ///
    /// Unused for `Stable`, whose compile is stored and traced by the ordinary path; `Miss`
    /// there names that path rather than a decision this row makes.
    Outcome outcome;
};

/// One row per verdict, in enumerator order.
inline constexpr EnumTable<InputStability, StabilityRow> StabilityTable { {
    { .stability = InputStability::Stable,
      .store = StoreObject::Yes,
      .manifest = KeepManifest::Yes,
      .sentence = "",
      .outcome = Outcome::Miss },
    { .stability = InputStability::MovedDuringCompile,
      .store = StoreObject::No,
      .manifest = KeepManifest::Yes,
      .sentence = "an input changed during the compile",
      .outcome = Outcome::InputsMoved },
    { .stability = InputStability::MovedBeforeKey,
      .store = StoreObject::No,
      .manifest = KeepManifest::No,
      .sentence = "an input changed while the key was taken",
      .outcome = Outcome::InputsMoved },
    { .stability = InputStability::Unwitnessed,
      .store = StoreObject::No,
      .manifest = KeepManifest::No,
      .sentence = "an input could not be observed",
      .outcome = Outcome::Uncacheable },
} };

static_assert(RowsInEnumeratorOrder(StabilityTable, &StabilityRow::stability),
              "StabilityTable must hold one row per InputStability, in enumerator order");

/// The row `RowOf` answers for a value outside the table -- `InputStability::Last`, or a byte no
/// enumerator names. It is not any verdict's row, so it says so rather than borrowing the nearest
/// one, and it stores nothing and records nothing.
inline constexpr StabilityRow UnknownStabilityRow { .stability = InputStability::Last,
                                                    .store = StoreObject::No,
                                                    .manifest = KeepManifest::No,
                                                    .sentence = "an unknown input verdict",
                                                    .outcome = Outcome::Uncacheable };

/// @param stability A verdict.
/// @return Its row; `UnknownStabilityRow` for a value that is not a verdict.
[[nodiscard]] StabilityRow const& RowOf(InputStability stability) noexcept;

class StorePermit;

/// @param row The verdict's row.
/// @return A permit when the row allows the object STORE (`StoreObject::Yes`); nullopt otherwise.
[[nodiscard]] std::optional<StorePermit> PermitStore(StabilityRow const& row) noexcept;

/// What the object STORE (`StoreCompiledObject`) takes, by value, so it cannot run without a
/// verdict that allows it (#1650). Only `PermitStore` makes one, and only for an allowing row: the
/// guard is folded INTO that operation rather than called beside it, so an object STORE reached on
/// a path that skipped the verdict does not build.
///
/// What it does NOT enforce: it is move-only, which stops a permit being COPIED, not used twice --
/// a moved-from permit is still a permit, and nothing marks it spent. Nor does it guard every
/// write to the cache: `StoreRaw`, the raw write underneath, takes none. The root-bound marker
/// reaches it only from inside `StoreCompiledObject`; the manifest reaches it gated by the row's
/// `KeepManifest` column instead, a check called beside the write rather than folded into it.
class StorePermit
{
  public:
    StorePermit(StorePermit const&) = delete;
    StorePermit& operator=(StorePermit const&) = delete;
    StorePermit(StorePermit&&) noexcept = default;
    StorePermit& operator=(StorePermit&&) noexcept = default;
    ~StorePermit() = default;

  private:
    StorePermit() = default;

    friend std::optional<StorePermit> PermitStore(StabilityRow const& row) noexcept;
};

/// A verdict and the path it is about.
struct StabilityFinding
{
    InputStability stability; ///< The verdict.
    std::string path;         ///< The first input that produced it; empty for `Stable`.

    [[nodiscard]] friend bool operator==(StabilityFinding const&, StabilityFinding const&) = default;
};

/// @param finding A verdict and its path.
/// @return "<sentence>: <path>", or empty for `Stable`.
[[nodiscard]] std::string DescribeInstability(StabilityFinding const& finding);

/// Snapshots of every input of one invocation, at three moments, and what they say about the
/// soundness of the key and of the object (#1650).
///
/// Invariant 1 of the design: a witness is never encoded, never transmitted and compared only
/// within one process. Every identity it holds is local to this machine and this checkout, and a
/// manifest or key carrying one would stop being shared.
///
/// Why THREE moments (`.agent/plans/2026-10-11-input-witness-design.md`): T0, before the key's
/// preprocess, is the instant every later change time is judged against; T1', right after that
/// preprocess, is what the key was derived from and what the manifest records; T2, after the
/// compile, says whether the object was built from that same content. Two moments cannot tell a
/// change that reached the KEY (both unsound) from one that reached only the OBJECT (the key is
/// sound, so the manifest can still be kept).
class InputWitness
{
  public:
    /// Take T0, through `IInputFiles::Fence`, so a change saved just before the invocation reads as
    /// before it rather than sharing its tick (#1650).
    /// @param resolvedSource The translation unit, resolved.
    /// @param files The input files.
    /// @param hashing Whether contents are hashed as well.
    /// @return The witness, holding T0 and the TU's identity (and hash) at it.
    [[nodiscard]] static InputWitness Begin(std::string resolvedSource, IInputFiles const& files, ContentHashing hashing);

    /// Take T1', right after the key's preprocess, and decide `AtKey`.
    ///
    /// Re-entrant: each call replaces the entries AND resets `AtKey` to `Stable` before deciding
    /// it again, so a second snapshot never inherits the first one's finding.
    /// @param resolvedProjectPaths The project dependencies the preprocess reported, resolved.
    /// @param files The input files.
    void Snapshot(std::span<std::string const> resolvedProjectPaths, IInputFiles const& files);

    /// @return The verdict at T1': the first finding in entry order (TU first), or `Stable`.
    [[nodiscard]] StabilityFinding AtKey() const;

    /// Take T2, after the compile.
    /// @param lateResolvedPaths Dependencies only the compile reported, resolved.
    /// @param files The input files.
    /// @return `AtKey()` when that is not `Stable`; otherwise the verdict at T2.
    [[nodiscard]] StabilityFinding AfterCompile(std::span<std::string const> lateResolvedPaths,
                                                IInputFiles const& files) const;

    /// @param resolvedPath A path, resolved.
    /// @return Its content hash as taken at T1', or nullopt when it is not an entry or was not hashed.
    [[nodiscard]] std::optional<std::string> KeyedHash(std::string_view resolvedPath) const;

    /// @return T0.
    [[nodiscard]] FileInstant Started() const noexcept
    {
        return _started;
    }

  private:
    /// Private, so `Begin` is the only way to obtain a witness: one without T0 would judge every
    /// change time against the epoch.
    InputWitness() = default;

    struct Entry
    {
        std::string path;
        std::optional<FileIdentity> identity;
        std::string hash;
    };
    FileInstant _started {};
    std::string _source;
    std::optional<FileIdentity> _sourceAtStart;
    std::string _sourceHashAtStart;
    ContentHashing _hashing { ContentHashing::Yes };
    std::vector<Entry> _entries;
    StabilityFinding _atKey { .stability = InputStability::Stable, .path = {} };
};

/// The production `IManifestContent`: an entry's hash as the witness took it at T1', and a path
/// the witness did not take (a late dependency) hashed now, only when its change time and identity
/// show it unchanged since T0.
class WitnessedManifestContent final: public IManifestContent
{
  public:
    /// @param witness The invocation's witness, after `Snapshot`. Must outlive this.
    /// @param files The input files. Must outlive this.
    WitnessedManifestContent(InputWitness const& witness, IInputFiles const& files):
        _witness { &witness },
        _files { &files }
    {
    }

    /// @param resolvedPath A path as `ResolveAgainst` resolved it.
    /// @return Its content hash as of the key, `Unreadable`, or `ChangedDuringInvocation`.
    [[nodiscard]] std::expected<std::string, ManifestFault> HashAsKeyed(std::string_view resolvedPath) const override;

  private:
    InputWitness const* _witness;
    IInputFiles const* _files;
};

} // namespace FastCache::Cc
