// SPDX-License-Identifier: Apache-2.0
#include "RootReconciler.hpp"

#include <algorithm>
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
    // Nothing under a bare root canonicalizes anyway -- IsSegmentPrefix wants a
    // separator AFTER the root, and a bare root IS its separator -- so exempting
    // one costs that configuration nothing it had. It is exempt because trimming
    // would be worse: "C:\" would become "C:", which flips the separator style
    // JoinLocalized derives from it, and "/" would become empty, which is a
    // prefix of nothing at all.
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

RootReconciler::RootReconciler(std::string_view sourceRoot, std::string_view buildTree, IPathResolver& resolver):
    _resolver { resolver },
    _asGiven { .sourceRoot = WithoutTrailingSeparator(std::string { sourceRoot }),
               .buildTree = WithoutTrailingSeparator(std::string { buildTree }) },
    _resolved { .sourceRoot = resolver.ResolveDirectory(_asGiven.sourceRoot),
                .buildTree = resolver.ResolveDirectory(_asGiven.buildTree) }
{
}

std::string RootReconciler::Path(std::string_view path)
{
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
