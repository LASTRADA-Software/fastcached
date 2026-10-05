// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Cc
{

/// A part of a checkout an object can name, and which a bound key then folds.
///
/// Private: a marker persists the set BY NAME (`CheckoutPartName`), never by value, so no
/// enumerator carries an explicit value and the order is free to change.
enum class CheckoutPart : std::uint8_t
{
    /// `FASTCACHE_SOURCE_DIR`, made absolute.
    SourceRoot,
    /// `FASTCACHE_BINARY_DIR`, made absolute.
    BuildTree,
    /// The compile's working directory.
    WorkingDirectory,
    /// The count; not a part.
    Last,
};

/// One spelling of a checkout part that is neither how it was exported nor how the
/// filesystem resolves it: an 8.3 short form, a `subst` drive, a junction, a symlinked
/// prefix.
struct RootAlias
{
    CheckoutPart part;    ///< The part it spells.
    std::string spelling; ///< The alias, absolute, no trailing separator.
};

/// Every alias spelling of a checkout part that ONE compile accepts -- the list the key's
/// reconciliation writes and the root scan reads.
///
/// **One list, because "a root and the paths a driver emits are reconciled on both sides,
/// or neither".** The key maps an alias onto a root by RESOLVING it, which accepts any
/// spelling the filesystem agrees with, and the root scan looks for fixed spellings. Two
/// derivations of "which spellings are this root" drifted apart: `RootReconciler` resolved
/// an `-I` spelled wholly in 8.3 short names to `<BUILDTREE>`, so two build directories of
/// one checkout shared a key, while the scan knew only the long spellings, judged the
/// object portable, and the second build directory was served the first one's
/// `__builtin_FILE()` -- measured on `cl` and clang-cl, direct mode on and off.
///
/// So the reconciler RECORDS here, for every path it maps onto a root, an ancestor of that
/// path's spelling for EVERY root any ancestor resolves into -- the lowest resolving TO the
/// root, else the highest resolving INTO it -- walking up the whole spelling, so a second
/// link below the alias does not hide it and an alias running through one root into the
/// other is recorded as both; each ancestor in BOTH spellings, the prefix as spelled,
/// `.` and `..` kept, and its lexical normal form, since which one an object carries
/// depends on the compiler's flags (`cl` collapses under a debug flag or `/FC`);
/// and it maps the path ONLY once that is recorded; a spelling no ancestor of which
/// resolves to a root, or whose spelled prefix leads elsewhere than its normal form, is
/// left unmapped, which costs sharing and never serves a wrong object. `BindCheckout` seeds each part's 8.3
/// short form. The scan reads the list at STORE time, after every mapping the key made.
/// So no spelling reaches the key without reaching the list: `RootReconciler::Translate`
/// has no path to a mapped result that does not pass `RecordAlias`.
///
/// Not thread-safe, and it need not be: one launcher process is one compile, on one thread.
class RootAliasList
{
  public:
    /// Record @p spelling as an alias of @p part. Empty spellings and repeats are ignored.
    /// @param part The part it spells. @param spelling The alias.
    /// @return True when it was new.
    bool Add(CheckoutPart part, std::string spelling)
    {
        if (spelling.empty() || Contains(part, spelling))
            return false;
        _entries.push_back(RootAlias { .part = part, .spelling = std::move(spelling) });
        return true;
    }

    /// @param part A part. @param spelling A spelling.
    /// @return Whether that spelling is already recorded for that part.
    [[nodiscard]] bool Contains(CheckoutPart part, std::string_view spelling) const noexcept
    {
        return std::ranges::any_of(
            _entries, [part, spelling](RootAlias const& alias) { return alias.part == part && alias.spelling == spelling; });
    }

    /// @return Every alias, in the order recorded.
    [[nodiscard]] std::span<RootAlias const> Entries() const noexcept
    {
        return _entries;
    }

  private:
    std::vector<RootAlias> _entries;
};

} // namespace FastCache::Cc
