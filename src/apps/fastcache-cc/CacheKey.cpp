// SPDX-License-Identifier: Apache-2.0
#include "CacheKey.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Crc32c.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>

namespace FastCache::Cc
{
namespace
{

    /// Include-dir flag prefixes whose trailing value is a path we relativize.
    /// Order matters: the longer `/external:I` must be tried before `/I`.
    constexpr std::array<std::string_view, 4> IncludePrefixes { "/external:I", "-external:I", "/I", "-I" };

    /// True if `c` introduces a command-line option in the MSVC style.
    ///
    /// Only meaningful on Windows: on POSIX a leading `/` starts an absolute
    /// path, never an option, so treating it as one would leave absolute paths
    /// unrelativized and bake the checkout location into the cache key.
    /// @param c The argument's first character.
    /// @return True when `c` introduces an option on this platform.
    [[nodiscard]] constexpr bool IsWindowsOptionPrefix(char c) noexcept
    {
#if defined(_WIN32)
        return c == '/';
#else
        static_cast<void>(c);
        return false;
#endif
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

        // Include-dir forms: <prefix><path>.
        for (std::string_view const prefix: IncludePrefixes)
        {
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
        // `-` introduces an option everywhere. `/` does so only on Windows: on
        // POSIX a leading slash is an ABSOLUTE PATH, and skipping those would
        // leave the checkout path in the key — which is exactly what breaks
        // cross-machine sharing, since two checkouts at different paths would
        // then key differently despite identical content.
        if (!arg.empty() && arg.front() != '-' && !(IsWindowsOptionPrefix(arg.front())))
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
    std::string blob;
    blob += inputs.compilerId;
    blob.push_back('\x00');
    blob += inputs.preprocessed;
    blob.push_back('\x00');
    for (auto const& arg: inputs.relativizedArgs)
    {
        blob += arg;
        blob.push_back('\x01');
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
