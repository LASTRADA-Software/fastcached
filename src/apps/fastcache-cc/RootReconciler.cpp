// SPDX-License-Identifier: Apache-2.0
#include "RootReconciler.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

namespace
{

    /// The roots a reconciler maps paths onto, and the part each one is.
    struct ReconciledRoot
    {
        CheckoutPart part;
        std::string PathCanon::Layout::* root;
    };

    constexpr std::array<ReconciledRoot, 2> ReconciledRoots { {
        { .part = CheckoutPart::SourceRoot, .root = &PathCanon::Layout::sourceRoot },
        { .part = CheckoutPart::BuildTree, .root = &PathCanon::Layout::buildTree },
    } };

    /// @param path A path. @param root A root, or empty.
    /// @return Whether @p path spells @p root itself, as `PathCanon` compares roots.
    [[nodiscard]] bool SpellsRoot(std::string_view path, std::string const& root)
    {
        if (root.empty())
            return false;
        PathCanon::Layout const alone { .sourceRoot = root, .buildTree = {} };
        return PathCanon::Canonicalize(path, alone) == PathCanon::Canonicalize(root, alone);
    }

    /// @param path A path. @param root A root, or empty.
    /// @return Whether @p path is @p root or lies below it, as `PathCanon` compares roots.
    [[nodiscard]] bool LiesIn(std::string_view path, std::string const& root)
    {
        if (root.empty())
            return false;
        PathCanon::Layout const alone { .sourceRoot = root, .buildTree = {} };
        return PathCanon::Canonicalize(path, alone) != path;
    }

    /// @param path A path, either separator. @param count How many components to drop.
    /// @return @p path without its last @p count components and without a trailing
    ///         separator -- except a drive's own root, `S:\`, which keeps it -- or nothing
    ///         when too few components are left to name a directory.
    [[nodiscard]] std::optional<std::string> WithoutTrailingComponents(std::string_view path, std::size_t count)
    {
        auto const isSeparator = [](char c) {
            return c == '/' || c == '\\';
        };
        // `S:\` is a directory and `S:` is not: the second is the drive's CURRENT directory.
        auto const isDriveRoot = [](std::string const& text) {
            return text.size() == 3 && PathCanon::IsDriveLetter(text[0]) && text[1] == ':';
        };
        std::string prefix { path };
        auto const trim = [&] {
            while (prefix.size() > 1 && isSeparator(prefix.back()) && !isDriveRoot(prefix))
                prefix.pop_back();
        };
        trim();
        for ([[maybe_unused]] auto const step: std::views::iota(std::size_t { 0 }, count))
        {
            if (isDriveRoot(prefix))
                return std::nullopt;
            auto const separator = prefix.find_last_of("/\\");
            if (separator == std::string::npos || separator == 0)
                return std::nullopt;
            auto const keepsDriveRoot = separator == 2 && prefix[1] == ':';
            prefix.resize(keepsDriveRoot ? separator + 1 : separator);
            trim();
        }
        return prefix;
    }

    /// @param a A spelling. @param b Another.
    /// @return Whether they differ at most in separator style -- which `LexicallyNormal`'s
    ///         own output does between a path and a prefix of it that has no `\`.
    [[nodiscard]] bool SameSeparatorsAside(std::string_view a, std::string_view b)
    {
        auto const unified = [](char c) {
            return c == '\\' ? '/' : c;
        };
        return std::ranges::equal(a, b, [&unified](char x, char y) { return unified(x) == unified(y); });
    }

    /// One prefix of a path as spelled, and its lexical normal form.
    struct SpelledPrefix
    {
        std::string spelled;
        std::string normal;
    };

    /// @param spelling A path as spelled. @return It and every ancestor, nearest first, each
    ///         beside its lexical normal form.
    [[nodiscard]] std::vector<SpelledPrefix> SpelledPrefixes(std::optional<std::string> spelling)
    {
        std::vector<SpelledPrefix> prefixes;
        while (spelling.has_value())
        {
            auto normal = PathCanon::LexicallyNormal(*spelling);
            auto next = WithoutTrailingComponents(*spelling, 1);
            prefixes.push_back(SpelledPrefix { .spelled = std::move(*spelling), .normal = std::move(normal) });
            spelling = std::move(next);
        }
        return prefixes;
    }

    /// The prefix of the spelling that IS @p ancestor: the LONGEST one whose normal form it
    /// is, since only after the last such prefix does the rest of the spelling stay below
    /// it -- `a\L\..\L\x` passes `a\L` twice, and the route continues from the second.
    /// @param ancestor An ancestor of the spelling's normal form.
    /// @param prefixes The spelling's prefixes, nearest first.
    /// @return That prefix as spelled, or nothing when none normalizes to @p ancestor.
    [[nodiscard]] std::optional<std::string> SpelledPrefixOf(std::string_view ancestor,
                                                             std::span<SpelledPrefix const> prefixes)
    {
        for (auto const& prefix: prefixes)
            if (SameSeparatorsAside(prefix.normal, ancestor))
                return prefix.spelled;
        return std::nullopt;
    }

} // namespace

std::string WithoutTrailingSeparator(std::string root)
{
    // "/" on POSIX; "C:\" or "C:/" on Windows. The drive test is as narrow as
    // IsWindowsRoot's, and for the same reason: without the letter check, any
    // three-byte string whose middle byte is a colon reads as a drive root.
    //
    // A bare root is exempt because trimming would be worse: "C:\" would become
    // "C:", which flips the separator style JoinLocalized derives from it, and "/"
    // would become empty, which is a prefix of nothing at all.
    //
    // This carried a second justification until #547 -- "nothing under a bare root
    // canonicalizes anyway, so exempting one costs that configuration nothing it
    // had" -- which was an accurate description of a DEFECT rather than a reason to
    // be comfortable. It was true: IsSegmentPrefix demanded a separator AFTER the
    // root and a bare root is its own, so every path under one was judged to lie
    // outside both roots and the stored value kept that machine's absolute paths.
    // The clause is gone with the defect; the exemption above stands on its own and
    // always did.
    auto const isBareRoot = [&root]() {
        if (root.size() <= 1)
            return true;
        return root.size() == 3 && PathCanon::IsDriveLetter(root[0]) && root[1] == ':'
               && (root[2] == '/' || root[2] == '\\');
    };
    while (!isBareRoot() && (root.back() == '/' || root.back() == '\\'))
        root.pop_back();
    return root;
}

RootReconciler::RootReconciler(std::string_view sourceRoot,
                               std::string_view buildTree,
                               IPathResolver& resolver,
                               NarrowTextPolicy policy):
    _resolver { resolver },
    _asGiven { .sourceRoot = WithoutTrailingSeparator(std::string { sourceRoot }),
               .buildTree = WithoutTrailingSeparator(std::string { buildTree }) },
    _resolved { .sourceRoot = resolver.ResolveDirectory(_asGiven.sourceRoot),
                .buildTree = resolver.ResolveDirectory(_asGiven.buildTree) },
    _policy { policy }
{
}

std::string RootReconciler::Path(std::string_view path)
{
    // The roots this translates against came from `argv`, which on a host that
    // declares the UTF-8 code page is UTF-8; `path` came from a compiler, which
    // does not. Reading it as text first is what keeps the two comparable -- and
    // what keeps `Translate` from handing bytes no decoder accepts to
    // `std::filesystem::path`, which on such a host throws rather than mis-names.
    //
    // Skipped entirely where narrow bytes are not decoded at all: on POSIX a
    // legacy filename is a perfectly good filename, and refusing it here would
    // break a build that works.
    std::optional<std::string> decoded;
    if (_policy.pathsAreUtf8)
    {
        decoded = Utf8FromNarrowText(path, _policy.toolCodePage);
        if (!decoded.has_value())
        {
            // Verbatim and UNTRANSLATED: resolving it would be the throw this exists
            // to avoid, and inventing a spelling for a path this process cannot read
            // would put a guess into a cache key. The count is what the caller acts
            // on.
            ++_unreadablePaths;
            return std::string { path };
        }
        // The DECODED form is what comes back, not merely what the decision is made
        // on, and returning the raw bytes for a path this translates no further was
        // tried and is wrong. Both consumers need the text:
        //
        // - the key and the manifest prefix-match it against roots that came from
        //   `argv` and are UTF-8, so legacy bytes match neither and a project header
        //   silently keys as toolchain content -- the very defect this decode is
        //   here to close;
        // - `Region` hands its result to a value SHARED between machines, and the
        //   daemon canonicalizes it against those same roots. A span left in the
        //   producer's code page is one no consumer can canonicalize, and a
        //   consumer whose legacy page differs reads it as different characters
        //   entirely. One encoding in a stored value is the same rule #141 settled
        //   for the wire.
        //
        // The class's "a path under neither root keeps its exact bytes" property is
        // about its SPELLING -- that this does not rewrite where a path points --
        // and it is unchanged. Only the encoding of a non-ASCII one moves, and only
        // on a host that transcodes at all.
        //
        // Outlives the call below, so the view is safe -- and rebinding rather than
        // branching keeps one translation path for both hosts.
        path = *decoded;
    }
    return Translate(path, Depth::LeafAsSpelled);
}

std::string RootReconciler::Directory(std::string_view path)
{
    return Translate(path, Depth::Whole);
}

void RootReconciler::DependencyList(std::vector<std::string>& paths)
{
    bool const windows = PathCanon::IsWindowsLayout(Layout());
    for (auto& path: paths)
    {
        path = PathCanon::CollapseRelativeSegments(Path(path));
        if (windows)
            std::ranges::replace(path, '/', '\\');
    }
}

std::string RootReconciler::Region(std::string_view text, PathCanon::Grammar grammar, std::span<std::string const> preserve)
{
    return PathCanon::RewritePaths(text, grammar, [this, preserve](std::string_view span) {
        auto const named = std::ranges::find(preserve, span) != preserve.end();
        return named ? std::string { span } : Path(span);
    });
}

bool RootReconciler::IsInTree(std::string_view path)
{
    // The same three-way classification PortableForm applies, and the same
    // disposition: a working-directory-relative path names the same file on any
    // machine running the same build, so it is keyed and counts as in-tree, while
    // a drive-relative one resolves against per-process state no cache entry
    // records and has to face the root tests like an absolute one (issue #65).
    switch (PathCanon::AnchorForLayout(path, _asGiven))
    {
        case PathCanon::Anchor::WorkingDirectory:
            return !path.empty();
        case PathCanon::Anchor::DriveRelative:
        case PathCanon::Anchor::Absolute:
            break;
    }
    auto const reconciled = Directory(path);
    return PathCanon::Canonicalize(reconciled, _asGiven) != reconciled;
}

std::string RootReconciler::Translate(std::string_view original, Depth depth)
{
    // Already spelled the way this build spells things: return it untouched, and
    // DO NOT round-trip it through resolution.
    //
    // This is not an optimization, it is the correctness case. Resolution rewrites
    // a symlink ANYWHERE in the path, not only in the root prefix, so a header
    // reached through an in-tree symlink (`src/inc -> src/real-inc`) would key
    // under this machine's real subpath while a machine holding the same content
    // without that symlink keys under the plain one. Two byte-identical checkouts
    // would stop sharing every entry — the exact property the launcher exists to
    // provide, traded away to fix a spelling that was never wrong here.
    // Reconciliation is therefore a no-op in every configuration that already
    // worked, which is also why it re-keys nothing and needs no schema bump.
    //
    // Inequality is what says a root matched — the same test DependencyProbe's
    // PortableForm uses, and the only one there is: PathCanon cannot fail.
    if (PathCanon::Canonicalize(original, _asGiven) != original)
        return std::string { original };

    // Only now is the filesystem worth asking.
    auto const resolved = depth == Depth::Whole ? _resolver.ResolveDirectory(original) : _resolver.Resolve(original);
    auto const token = PathCanon::Canonicalize(resolved, _resolved);
    if (token == resolved)
        return std::string { original };

    // Mapped only through a spelling the root scan will know, and that is ONE operation:
    // the alias is recorded here or the path is not mapped at all. A mapping the list
    // does not cover is how two build directories came to share a key while the scan
    // judged their objects portable -- a WRONG hit -- so the fallback is to key the
    // spelling as it stands, which costs sharing and nothing else.
    if (!RecordAlias(original, depth))
        return std::string { original };
    return PathCanon::Localize(token, _asGiven);
}

bool RootReconciler::RecordAlias(std::string_view original, Depth depth)
{
    // Walked UP the spelling, ancestor by ancestor, asking the filesystem about each: a
    // count of components below the root cannot find the alias once the tail crosses a
    // second link (`jx\L1\genlink` with `genlink` a junction inside the build tree), and
    // that shape was mapped and never recorded. A file's own leaf is not asked about.
    //
    // And walked to the TOP, one root at a time: the nearest ancestor naming a root is not
    // the only one. `jx\L1\srclink\src\gen`, with `jx\L1` a junction to build directory 1
    // and `srclink` one inside it back to the source root, names the source root through
    // `jx\L1\srclink` and build directory 1 through `jx\L1` above it. Stopping at the first
    // bound the object to the source root alone, and build directory 2 was served a path
    // spelled through build directory 1's link.
    // So each root is looked for until it is found, and the walk ends when all are.
    //
    // And a root no ancestor resolves TO, but one resolves INTO, takes the highest such
    // ancestor. `jx\L1` a junction to `b1\x` -- a subdirectory of the build tree -- with
    // `srclink` inside it back to the source root: no ancestor is build directory 1, yet
    // `jx\L1` is inside it, and the object is spelled through it. The FOURTH shape of one
    // class, so the rule is stated as the class: for every root ANY ancestor resolves into,
    // an ancestor is recorded for that root -- the lowest that resolves TO it, else the
    // highest that resolves INTO it, which is the broadest needle and one entry rather than
    // one per directory below it. `RootReconciler_test` checks it GENERATIVELY, over every
    // composition of links it enumerates, rather than shape by shape.
    //
    // And each ancestor is recorded in BOTH spellings, because which one the object carries
    // depends on the compiler's flags (measured): `cl` writes the `-I` spelling verbatim, `.`
    // and `..` included, with no debug flag, and collapses it under `/Z7`, `/Zi`, `/ZI` or
    // `/FC`; clang-cl writes it verbatim, and under `-Z7` both. Recorded only as `jx\L1`,
    // `jx\zz\..\L1` was a needle a verbatim object never contains -- judged portable, and
    // build directory 2 was served build directory 1's spelling (measured on clang-cl) -- and
    // recorded only as spelled it is one a collapsed object never contains (`cl /FC`). The
    // spelled prefix is DERIVED for each ancestor walked, and where it cannot be, or leads
    // elsewhere than the normal form (a `..` after a link ascends from the TARGET wherever the
    // filesystem resolves physically), the mapping is refused: the walk would have recorded a
    // directory the path does not pass through.
    auto const leaf = depth == Depth::Whole ? std::size_t { 0 } : std::size_t { 1 };
    auto const prefixes = SpelledPrefixes(WithoutTrailingComponents(original, leaf));
    auto spelling = WithoutTrailingComponents(PathCanon::LexicallyNormal(original), leaf);
    std::array<bool, ReconciledRoots.size()> found {};
    // The highest ancestor seen so far that resolves INTO each root, for a root nothing
    // resolves TO -- in both spellings.
    std::array<std::optional<SpelledPrefix>, ReconciledRoots.size()> into {};
    // A root the layout does not name cannot be found, and is not waited for.
    for (auto const index: std::views::iota(std::size_t { 0 }, ReconciledRoots.size()))
        found[index] = (_resolved.*ReconciledRoots[index].root).empty();
    auto const anyFound = [&found, this] {
        return std::ranges::any_of(std::views::iota(std::size_t { 0 }, ReconciledRoots.size()), [&](std::size_t index) {
            return found[index] && !(_resolved.*ReconciledRoots[index].root).empty();
        });
    };
    // A root's own spelling, or an alias already on the list, needs no entry.
    auto const covered = [this](SpelledPrefix const& ancestor, ReconciledRoot const& root) {
        auto const listed = [&](std::string const& text) {
            return SpellsRoot(text, _resolved.*root.root) || SpellsRoot(text, _asGiven.*root.root)
                   || _aliases.Contains(root.part, text);
        };
        return listed(ancestor.normal) && listed(ancestor.spelled);
    };
    auto const record = [this](SpelledPrefix const& ancestor, ReconciledRoot const& root) {
        for (auto const* text: std::array { &ancestor.normal, &ancestor.spelled })
            if (!SpellsRoot(*text, _resolved.*root.root) && !SpellsRoot(*text, _asGiven.*root.root))
                _aliases.Add(root.part, *text);
    };
    while (spelling.has_value() && !std::ranges::all_of(found, std::identity {}))
    {
        auto spelled = SpelledPrefixOf(*spelling, prefixes);
        // No PRODUCTION input reaches this refusal, and no test reddens without it -- but not
        // because it is unreachable in itself. A RELATIVE spelling reaches it: `a\..\..\L\x`
        // keeps its surviving `..`, so `LexicallyNormal` hands it back as spelled, the walk
        // comes to the ancestor `a\..`, and the only prefix of that name normalizes to `.`, so
        // none matches (measured by rev-filemacro, with a resolver that maps `a\..\..\L`). The
        // real resolver never gets it here: it hands a relative spelling back verbatim
        // (`IsResolvable`), so `Translate` never maps one and this walk never runs for it. That
        // premise belongs to the resolver, so this stays a refusal: under a resolver that maps
        // relative paths an assertion would be a reachable abort, where this costs one miss.
        if (!spelled.has_value())
            return false;
        SpelledPrefix const ancestor { .spelled = std::move(*spelled), .normal = *spelling };
        std::optional<std::string> resolved;
        auto const resolve = [&]() -> std::string const& {
            if (!resolved.has_value())
                resolved = WithoutTrailingSeparator(_resolver.ResolveDirectory(ancestor.normal));
            return *resolved;
        };
        // Compared by where each LEADS, so an answer a resolver spells unnormalized -- one it
        // could not resolve comes back as asked -- is not a disagreement by spelling alone.
        auto const leadsTo = [](std::string const& answer) {
            return WithoutTrailingSeparator(PathCanon::LexicallyNormal(answer));
        };
        if (!SameSeparatorsAside(ancestor.spelled, ancestor.normal)
            && leadsTo(_resolver.ResolveDirectory(ancestor.spelled)) != leadsTo(resolve()))
            return false;
        for (auto const index: std::views::iota(std::size_t { 0 }, ReconciledRoots.size()))
        {
            if (found[index])
                continue;
            auto const& root = ReconciledRoots[index];
            if (covered(ancestor, root))
                found[index] = true;
            else if (SpellsRoot(resolve(), _resolved.*root.root))
            {
                record(ancestor, root);
                found[index] = true;
            }
            else if (LiesIn(resolve(), _resolved.*root.root))
                into[index] = ancestor;
        }
        spelling = WithoutTrailingComponents(*spelling, 1);
    }
    for (auto const index: std::views::iota(std::size_t { 0 }, ReconciledRoots.size()))
    {
        auto const& candidate = into[index];
        if (found[index] || !candidate.has_value())
            continue;
        record(*candidate, ReconciledRoots[index]);
        found[index] = true;
    }
    return anyFound();
}

} // namespace FastCache::Cc
