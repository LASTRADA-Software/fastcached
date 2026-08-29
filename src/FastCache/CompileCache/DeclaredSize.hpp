// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace FastCache
{

/// Whether a peer's declared element count is achievable in the bytes that remain.
///
/// **A declared count is a claim about bytes the frame must already contain.** It is
/// not a request, and it is not a size to allocate from — it is an assertion that can
/// be checked, because every element costs at least some fixed number of bytes on the
/// wire, and the frame either carries them or it does not.
///
/// This is the rule behind issue #267 and the same one `Protocol/CompileCacheWire.hpp`
/// states for a codec envelope's expansion field: the number exists so a decoder can
/// refuse an impossible claim *before* it reserves anything. `DecodeCompileValue`
/// reserved from a `u32` region count with no such check, and a region costs five wire
/// bytes minimum, so a nine-byte frame declared four billion of them — an allocation
/// on the order of 170 GB, reachable from the daemon's STORE path and from a rogue
/// worker's reply to the launcher.
///
/// The division is deliberate and is not a rearrangement of `count * minBytesEach`,
/// which is the overflowing spelling: at `u32` counts and any element size above one
/// byte the product wraps a 32-bit type and can wrap a 64-bit one, so the check would
/// pass on exactly the values it exists to refuse.
///
/// **Necessary, and on its own not sufficient.** It bounds a declared count by the
/// bytes present, which still permits an amplification when an in-memory element is
/// much larger than its wire minimum — a `TextRegion` is forty bytes against five on
/// the wire, so a capped frame could still reserve eight times its own size. So a
/// decoder validates the claim with this AND grows its container from the elements it
/// actually decodes, rather than reserving what a peer said was coming. Together those
/// make the allocation proportional to bytes genuinely received.
///
/// @param count The count the peer declared.
/// @param minBytesEach The fewest wire bytes one element can occupy; must be non-zero,
///        since a zero-cost element would make every count "achievable" and defeat the
///        check.
/// @param remaining Bytes left in the frame at the point the count was read.
/// @return True when the frame could actually carry that many elements.
[[nodiscard]] constexpr bool DeclaredCountFits(std::uint64_t count, std::size_t minBytesEach, std::size_t remaining) noexcept
{
    if (minBytesEach == 0)
        return false;
    return count <= remaining / minBytesEach;
}

} // namespace FastCache
