// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "PathResolve.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Platform/NarrowText.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// Drop a trailing separator from a layout root.
///
/// `PathCanon::Layout` takes roots WITHOUT one — `IsSegmentPrefix` requires the
/// byte after the root to be a separator, so `/x/build/` matches nothing under
/// `/x/build` — and a build system that exports one is doing nothing wrong. Left
/// in place it is worse than a no-op in both directions: nothing under that root
/// canonicalizes, so the stored value keeps this machine's absolute paths; and
/// once reconciliation gives the path a second chance through the resolved root
/// (which the resolver returns without the separator), `JoinLocalized` puts the
/// root back with a separator of its own and emits `/x/build//a.o` — a depfile
/// rule target the build system did not ask for, which Ninja rejects outright and
/// make matches against no rule at all.
///
/// A bare root is left alone: `/` and `C:\` ARE their trailing separator, and
/// trimming `C:\` to `C:` would also flip the separator style `JoinLocalized`
/// derives from it.
/// @param root A layout root as the environment supplied it.
/// @return The root with any trailing separators removed.
[[nodiscard]] std::string WithoutTrailingSeparator(std::string root);

/// Rewrites a path a compiler emitted into the spelling THIS BUILD uses.
///
/// There are two spellings of every root and the launcher needs both, for exactly
/// opposite reasons:
///
/// - **Matching** has to use the spelling the FILESYSTEM reports, because that is
///   what a driver reports. `cl` resolves an include through the filesystem and
///   prints the long name while clang-cl echoes the short one it was handed; a
///   `subst` drive, a junction and a symlink all split the two the same way. When
///   the root does not share a spelling with what the driver emits, the keyed
///   dependency set is empty, the replay guard probes nothing, and the stored
///   value keeps this machine's absolute paths — silently (issue #66).
/// - **Emitting** has to use the spelling the BUILD SYSTEM uses. A replayed
///   depfile's rule target must be byte-identical to the `-o` path the build
///   passed, or Ninja fails outright ("expected depfile ... to mention ...") and
///   make matches no rule at all and quietly loses every header dependency.
///
/// Translating INTO the as-given spelling satisfies both at once, and leaves one
/// layout for everything downstream: the roots on the wire, the key, the manifest,
/// the replay guard and the localized regions all keep speaking the spelling the
/// build system exported, exactly as they did before any of this existed. The only
/// thing that changes is that a path the driver spelled some other way now arrives
/// already translated.
///
/// The translation is PathCanon's own two operations — canonicalize against the
/// resolved roots, localize into the as-given ones — rather than a prefix test
/// written out again here, so it cannot come to disagree with the rule everything
/// else applies.
class RootReconciler
{
  public:
    /// @param sourceRoot The source root as the build system spelled it.
    /// @param buildTree  The build tree as the build system spelled it.
    /// @param resolver   The path-identity seam.
    /// @param policy     How this host reads narrow text a TOOL wrote. Defaulted to
    ///                   "bytes are bytes", which is POSIX and is what every caller
    ///                   that is not handling a compiler's own output wants.
    RootReconciler(std::string_view sourceRoot,
                   std::string_view buildTree,
                   IPathResolver& resolver,
                   NarrowTextPolicy policy = {});

    /// Reconcile one path naming a FILE — an include note, a depfile entry.
    ///
    /// Also where a path a tool emitted becomes TEXT, because this is the funnel
    /// every one of them passes through (`All` and `Region` both come here) and
    /// because nothing downstream can do it: the launcher's own paths arrive as
    /// UTF-8 through `argv` while a compiler writes its output in a code page of
    /// its own, and a `std::filesystem::path` built from the second on a host that
    /// reads narrow bytes as UTF-8 does not mis-name a file, it THROWS. A path this
    /// process cannot read as text is returned verbatim and untranslated, and
    /// counted -- see `UnreadablePaths()`.
    /// @param path The path as the compiler spelled it.
    /// @return The same location in this build's spelling, or `path` unchanged.
    [[nodiscard]] std::string Path(std::string_view path);

    /// Reconcile one path naming a DIRECTORY, or any path few enough in number to
    /// resolve completely.
    ///
    /// `IPathResolver::Resolve` leaves the final component exactly as spelled —
    /// the memo that makes hundreds of include notes affordable works per PARENT
    /// directory — so an `-I` whose own last component is the aliased one has to
    /// come through here instead, and does: an include directory must resolve the
    /// same way the headers reported from under it do, or the two disagree and the
    /// argument keeps the checkout location in the key.
    /// @param path The path as the build system spelled it.
    /// @return The same location in this build's spelling, or `path` unchanged.
    [[nodiscard]] std::string Directory(std::string_view path);

    /// Reconcile every path in a list, in place.
    /// @param paths The dependency paths to reconcile.
    void All(std::vector<std::string>& paths);

    /// Reconcile every path span a captured region names, per its grammar, except
    /// one the caller names as the build system's own.
    ///
    /// Applied to what is STORED, never to what the compiler already printed: the
    /// build's own output stays the compiler's own bytes.
    ///
    /// `preserve` is the general rule rather than a depfile special case:
    /// reconciliation exists to translate a spelling the DRIVER chose, and a path
    /// the BUILD SYSTEM chose is already the spelling this build wants. A depfile
    /// is the one grammar carrying both, so the launcher names the object path
    /// there — respelling it hands the build system back an output it never asked
    /// for, and a build whose `-o` does not share a spelling with
    /// `FASTCACHE_BINARY_DIR` then gets a depfile Ninja rejects outright and make
    /// matches against no rule at all.
    ///
    /// Named by VALUE rather than by position in the grammar, because position
    /// does not say what a path is: `-MP` emits a phony rule per header, whose
    /// target is a path the COMPILER reported and which must be reconciled like
    /// any other, or a consumer's replayed depfile names files that do not exist
    /// there and `-MP`'s deleted-header protection is lost with them. The caller
    /// derives the set from the depfile itself (`ParseDepFileTargets`) rather than
    /// from the command line, since a compile may name a target more than once and
    /// `-MQ` escapes it on the way out.
    /// @param text     The captured region.
    /// @param grammar  The grammar identifying path spans within it.
    /// @param preserve Paths to return verbatim wherever they appear; empty for
    ///                 none.
    /// @return The region with every other span reconciled; the input on failure.
    [[nodiscard]] std::string Region(std::string_view text,
                                     PathCanon::Grammar grammar,
                                     std::span<std::string const> preserve = {});

    /// Whether `path` has a portable form under one of this build's roots.
    ///
    /// Asked of the RECONCILED path against the as-given layout, which is the same
    /// question — the same two values — that decides whether the path reaches the
    /// key. Asking the resolved layout instead answers a question nothing else
    /// asks, and then a compile can key nothing while this reports it in-tree.
    ///
    /// A RELATIVE path counts, and the order of the tests is what makes that true:
    /// it resolves against the compile's working directory, so it is already
    /// machine-independent, and the resolver hands it back verbatim by contract —
    /// after which `Canonicalize` leaves it alone and the token test would call it
    /// out-of-tree. `KeyDependencySet` and `ReplayGuard` both answer this the same
    /// way and for the same reason (DependencyProbe's PortableForm puts the
    /// relative branch first, in as many words), so a third answer here would make
    /// the launcher contradict itself about one path.
    /// @param path A path as it was emitted.
    /// @return True when it has a portable form.
    [[nodiscard]] bool IsInTree(std::string_view path);

    /// The as-given layout, which is the one everything downstream speaks.
    /// @return The roots as the build system exported them, trailing separator
    ///         trimmed.
    [[nodiscard]] PathCanon::Layout const& Layout() const noexcept
    {
        return _asGiven;
    }

    /// How many paths this reconciler could not read as text.
    ///
    /// Not a drop and not a diagnostic: a path whose bytes this process cannot read
    /// is keyed by nothing, resolved by nothing and stat'ed by nothing, so a header
    /// moved inside it replays a stored object under a zero exit code -- the same
    /// hazard `PathDisposition::DriveRelative` exists for, reached by a different
    /// road. The caller declines to cache the compile; see main.cpp.
    ///
    /// Counted per reconciled OCCURRENCE rather than per distinct path, because the
    /// only question asked of it is whether it is zero.
    /// @return The count since construction.
    [[nodiscard]] std::size_t UnreadablePaths() const noexcept
    {
        return _unreadablePaths;
    }

  private:
    /// How much of a path the resolver should be asked about.
    enum class Depth : std::uint8_t
    {
        LeafAsSpelled, ///< Resolve the parent chain only (IPathResolver::Resolve).
        Whole,         ///< Resolve the final component too (ResolveDirectory).
    };

    /// @param original The path as emitted; returned unchanged when it lies under
    ///                 neither root, so a toolchain path keeps its exact bytes.
    /// @param depth    Which resolver entry point the caller's path kind calls for.
    /// @return The as-given spelling of the same location.
    [[nodiscard]] std::string Translate(std::string_view original, Depth depth);

    IPathResolver& _resolver;
    PathCanon::Layout _asGiven;
    PathCanon::Layout _resolved;
    NarrowTextPolicy _policy;
    std::size_t _unreadablePaths { 0 };
};

} // namespace FastCache::Cc
