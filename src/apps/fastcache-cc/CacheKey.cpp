// SPDX-License-Identifier: Apache-2.0
#include "CacheKey.hpp"
#include "KeyDigest.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <vector>

namespace FastCache::Cc
{
namespace
{

    /// Include-dir flag prefixes whose trailing value is a path we relativize,
    /// paired with the leading character that introduces them.
    ///
    /// Order matters: the longer `/external:I` must be tried before `/I`. The
    /// `/`-led spellings are MSVC's and are only recognised under a Windows
    /// layout — on POSIX a leading slash begins an absolute path, and matching
    /// `/I` there would split a checkout rooted at `/Infra` into the prefix
    /// `/I` plus the fragment `nfra/...`, which lies under neither root and so
    /// comes back verbatim with the absolute path still embedded in the key.
    constexpr std::array<std::string_view, 4> IncludePrefixes { "/external:I", "-external:I", "/I", "-I" };

    /// True if `c` introduces a command-line option in the MSVC style, for the
    /// layout being relativized against.
    ///
    /// The answer comes from the LAYOUT, not from the compiling host. A leading
    /// `/` introduces an option under a Windows layout, but on POSIX it starts
    /// an absolute path; treating it as an option there would leave absolute
    /// paths unrelativized and bake the checkout location into the cache key —
    /// the exact failure that breaks cross-checkout sharing.
    ///
    /// Keying off `_WIN32` instead would make a Windows-hosted launcher
    /// mis-handle POSIX paths, and it made the behaviour untestable from the
    /// other platform. `PathCanon::IsWindowsLayout` is the one definition of
    /// "is this a Windows layout", shared with the canonicalizer, so the two
    /// cannot drift apart on a root such as `C:/src/proj`.
    ///
    /// @param c      The argument's first character.
    /// @param layout The layout whose path conventions apply.
    /// @return True when `c` introduces an option under that layout.
    [[nodiscard]] bool IsWindowsOptionPrefix(char c, PathCanon::Layout const& layout) noexcept
    {
        return c == '/' && PathCanon::IsWindowsLayout(layout);
    }

    /// Relativize one argument against both roots: if it is a bare path or an
    /// include-dir flag whose path lies under the source root or the build tree,
    /// replace the path portion with its canonical token; otherwise return it
    /// unchanged. PathCanon prefers the longer-matching root, so a build tree
    /// nested under the source root tokenizes to `<BUILDTREE>`.
    /// @param arg    The raw argument.
    /// @param layout The source-root / build-tree layout to relativize against.
    /// @return The (possibly) relativized argument.
    [[nodiscard]] std::string RelativizeOne(std::string_view arg, PathCanon::Layout const& layout)
    {

        // Include-dir forms: <prefix><path>. A `/`-led spelling is only an
        // option under a Windows layout; under POSIX it is the head of an
        // absolute path and must fall through to the bare-path branch below,
        // or a checkout rooted at `/Infra` would be mis-split at `/I`.
        for (std::string_view const prefix: IncludePrefixes)
        {
            if (prefix.starts_with('/') && !PathCanon::IsWindowsLayout(layout))
                continue;

            if (arg.starts_with(prefix) && arg.size() > prefix.size())
            {
                std::string_view const path = arg.substr(prefix.size());
                auto const canon = PathCanon::Canonicalize(path, layout);
                if (canon.has_value() && *canon != path)
                    return std::string { prefix } + *canon;
                return std::string { arg };
            }
        }

        // Bare path argument (a source file or a response path). Only rewrite when
        // it actually lies under the source root (Canonicalize returns it verbatim
        // otherwise, so a no-op change is left as-is).
        //
        // `-` introduces an option everywhere. `/` does so only under a Windows
        // layout: on POSIX a leading slash is an ABSOLUTE PATH, and skipping
        // those would leave the checkout path in the key — which is exactly
        // what breaks cross-machine sharing, since two checkouts at different
        // paths would then key differently despite identical content.
        if (!arg.empty() && arg.front() != '-' && !IsWindowsOptionPrefix(arg.front(), layout))
        {
            auto const canon = PathCanon::Canonicalize(arg, layout);
            if (canon.has_value())
                return *canon;
        }
        return std::string { arg };
    }

} // namespace

std::vector<std::string> RelativizeArgs(std::span<std::string const> args,
                                        std::string_view sourceRoot,
                                        std::string_view buildTree)
{
    PathCanon::Layout const layout { .sourceRoot = std::string { sourceRoot }, .buildTree = std::string { buildTree } };
    std::vector<std::string> out;
    out.reserve(args.size());
    for (auto const& arg: args)
        out.push_back(RelativizeOne(arg, layout));
    return out;
}

std::string ComputeKey(KeyInputs const& inputs)
{
    // The schema tag comes first, and bumping it is what makes a format change
    // safe. Nothing else in this key describes how the stored value is framed or
    // canonicalized, so without it a change to either would leave old entries
    // matching new keys and being served under rules they were not written by --
    // a silent mis-serve, which presents as a mysterious hit-rate collapse rather
    // than as a miss. Bumping re-keys the cache instead, so stale entries simply
    // miss and are rewritten. ComputeManifestKey's tag moves in lock-step; see
    // the reason recorded there.
    //
    // v2 added the dependency path set, which is what makes a moved header a
    // different key rather than a hit whose depfile has to be re-checked (see
    // KeyInputs::dependencyPaths).
    //
    // v3 is the digest itself (issue #63). Until it, this was four CRC32C runs
    // over one blob distinguished only by a leading salt byte. CRC is affine over
    // GF(2): with `A` the per-byte state-update operator and `S_i` the state
    // after salt `i`, quarter_i XOR quarter_j is `A^len(blob) * (S_i XOR S_j)` --
    // a function of the blob's LENGTH and of nothing else about it. So matching
    // one quarter forced all four, and a key that was 128 bits wide carried 32
    // bits of strength. Equal length is not an exotic condition, it is the
    // ordinary shape of a source edit (`return 1;` -> `return 2;`), and the
    // consequence was not a miss but an unrelated translation unit's object file
    // served under a zero exit code. Measured before the fix: one distinct XOR
    // value across 2000 random equal-length blobs, and a full 32-hex-char
    // collision after 86,125 of them -- the birthday bound for the 32 bits it
    // really had, against ~10^5 entries a shared team cache reaches.
    KeyDigest digest { "objkey-v3" };
    digest.Field(inputs.compilerId);
    digest.Field(inputs.preprocessed);
    for (auto const& arg: inputs.relativizedArgs)
        digest.Item(arg);
    for (auto const& path: inputs.dependencyPaths)
        digest.Path(path);
    return digest.ToHex();
}

} // namespace FastCache::Cc
