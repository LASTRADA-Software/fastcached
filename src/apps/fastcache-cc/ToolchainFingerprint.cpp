// SPDX-License-Identifier: Apache-2.0
#include "KeyDigest.hpp"
#include "ToolchainFingerprint.hpp"

#include <algorithm>

namespace FastCache::Cc
{

std::string ComputeToolchainFingerprint(std::string_view compilerBanner, std::vector<ToolchainFile> files)
{
    // Sorted here rather than trusted from the caller. The caller's order comes
    // from a directory traversal, which is a property of the filesystem — two
    // machines with byte-identical toolchains can enumerate them differently, and
    // an order-sensitive digest would report that as two different toolchains.
    std::ranges::sort(files, [](ToolchainFile const& a, ToolchainFile const& b) {
        // By path, then by hash, so the order is total even if a caller somehow
        // reports one path twice. Leaving it partial would make the result depend
        // on the sort's stability, which is exactly the kind of coupling this sort
        // exists to remove.
        return a.relativePath != b.relativePath ? a.relativePath < b.relativePath : a.contentHash < b.contentHash;
    });

    KeyDigest digest { FingerprintSchema };
    digest.Field(compilerBanner);
    for (auto const& file: files)
    {
        // Path and hash are folded as separate length-prefixed pieces, not
        // concatenated. Concatenation is not a framing: `{"ab", "c"}` and
        // `{"a", "bc"}` would digest identically, so a header renamed in a way that
        // shifted a character into its hash would fingerprint the same as the
        // original — a false MATCH, which is the one error direction that
        // dispatches to the wrong toolchain.
        digest.Path(file.relativePath);
        digest.Item(file.contentHash);
    }
    return digest.ToHex();
}

} // namespace FastCache::Cc
