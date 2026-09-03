// SPDX-License-Identifier: Apache-2.0
#include "RootReconciler.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

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

void RootReconciler::All(std::vector<std::string>& paths)
{
    for (auto& path: paths)
        path = Path(path);
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
    return PathCanon::Localize(token, _asGiven);
}

} // namespace FastCache::Cc
