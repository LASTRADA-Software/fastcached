// SPDX-License-Identifier: Apache-2.0
#include "RootReconciler.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Cc
{

std::string WithoutTrailingSeparator(std::string root)
{
    auto const isBareRoot = [&root]() {
        // "/" on POSIX; "C:\" or "C:/" on Windows.
        return root.size() <= 1 || (root.size() == 3 && root[1] == ':');
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

std::string RootReconciler::Region(std::string_view text, PathCanon::Grammar grammar, std::string_view preserve)
{
    auto rewritten = PathCanon::RewritePaths(text, grammar, [this, preserve](std::string_view span) {
        return span == preserve ? std::string { span } : Path(span);
    });
    return rewritten.has_value() ? *std::move(rewritten) : std::string { text };
}

bool RootReconciler::IsInTree(std::string_view path)
{
    if (!PathCanon::IsAbsoluteForLayout(path, _asGiven))
        return !path.empty();
    auto const reconciled = Directory(path);
    auto const token = PathCanon::Canonicalize(reconciled, _asGiven);
    return token.has_value() && *token != reconciled;
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
    // Inequality, not a sentinel spelling PathCanon keeps private, is what says a
    // root matched — the same test DependencyProbe's PortableForm uses.
    if (auto const asIs = PathCanon::Canonicalize(original, _asGiven); asIs.has_value() && *asIs != original)
        return std::string { original };

    // Only now is the filesystem worth asking.
    auto const resolved = depth == Depth::Whole ? _resolver.ResolveDirectory(original) : _resolver.Resolve(original);
    auto const token = PathCanon::Canonicalize(resolved, _resolved);
    if (!token.has_value() || *token == resolved)
        return std::string { original };
    auto localized = PathCanon::Localize(*token, _asGiven);
    return localized.has_value() ? *std::move(localized) : std::string { original };
}

} // namespace FastCache::Cc
