// SPDX-License-Identifier: Apache-2.0
#include "CacheKey.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Crc32c.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
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

    /// One CRC32C "lane": digest `data` prefixed with a distinct salt byte so the
    /// four lanes are independent, widening the effective key to 128 bits.
    /// @param salt Lane discriminator.
    /// @param data Bytes to digest.
    /// @return The finalised 32-bit lane value.
    [[nodiscard]] std::uint32_t Lane(std::uint8_t salt, std::string_view data)
    {
        std::uint32_t state = Crc32c::Seed;
        std::array<std::byte, 1> const saltByte { static_cast<std::byte>(salt) };
        Crc32c::Update(state, saltByte);
        Crc32c::Update(state, std::span<std::byte const> { reinterpret_cast<std::byte const*>(data.data()), data.size() });
        return Crc32c::Finalise(state);
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
    // Serialise the inputs into one blob with unambiguous separators, then take
    // four independent CRC32C lanes over it for a 128-bit key.
    //
    // The leading schema tag is what makes a future format change safe. Nothing
    // else in this key describes how the stored value is framed or canonicalized,
    // so without it a change to either would leave old entries matching new keys
    // and being served under rules they were not written by — a silent
    // mis-serve, which presents as a mysterious hit-rate collapse rather than as
    // a miss. Bumping the tag re-keys the cache instead, so stale entries simply
    // miss and are rewritten. Mirrors ComputeManifestKey's "manifest-v1".
    //
    // v2 adds the dependency path set, which is what makes a moved header a
    // different key rather than a hit whose depfile has to be re-checked (see
    // KeyInputs::dependencyPaths). Every entry written by a v1 launcher therefore
    // misses once and is rewritten — the one-time cost the tag exists to make safe.
    std::string blob;
    // Sized up front. The preprocessed text alone runs to megabytes, and without
    // this the single `push_back` that follows it reallocates a buffer that had
    // just been grown to fit it exactly — one extra full-length copy per
    // translation unit, on the launcher's hot path, to append one byte.
    auto const argBytes = [](std::vector<std::string> const& list) {
        return std::ranges::fold_left(
            list, std::size_t { 0 }, [](std::size_t n, std::string const& s) { return n + s.size() + 1; });
    };
    blob.reserve(inputs.compilerId.size() + inputs.preprocessed.size() + argBytes(inputs.relativizedArgs)
                 + argBytes(inputs.dependencyPaths) + 32);
    blob += "objkey-v2";
    blob.push_back('\x00');
    blob += inputs.compilerId;
    blob.push_back('\x00');
    blob += inputs.preprocessed;
    blob.push_back('\x00');
    for (auto const& arg: inputs.relativizedArgs)
    {
        blob += arg;
        blob.push_back('\x01');
    }
    // A separator of its own, so a dependency path can never be read as a trailing
    // argument: the two lists are adjacent and both hold path-shaped strings.
    for (auto const& path: inputs.dependencyPaths)
    {
        blob += path;
        blob.push_back('\x02');
    }

    std::array<std::uint32_t, 4> const lanes {
        Lane(0xA1, blob),
        Lane(0xB2, blob),
        Lane(0xC3, blob),
        Lane(0xD4, blob),
    };
    std::string key;
    key.reserve(32);
    for (auto const lane: lanes)
        key += std::format("{:08x}", lane);
    return key;
}

} // namespace FastCache::Cc
