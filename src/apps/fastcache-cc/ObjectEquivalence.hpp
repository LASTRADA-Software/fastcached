// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace FastCache::Cc
{

/// Deciding whether two object files are the same compile, when the bytes differ.
///
/// `FASTCACHE_VERIFY` compared object files with a `memcmp`, which is exactly right
/// on ELF and can never succeed on Windows: every MSVC-family driver stamps the wall
/// clock into the COFF header, and a cached object was produced earlier than the
/// fresh compile it is checked against **by construction**. So the one instrument
/// that turns a wrong object from invisible into loud reported a wrong object on
/// every hit, on the platform where the defect it exists to detect was observed
/// ([#493](https://github.com/LASTRADA-Software/fastcached/issues/493)).
///
/// **The measurements this is built on, the reasoning, and the two conclusions that
/// are easiest to get backwards live in ONE place**:
/// `.agent/rules/compile-cache.md`, "An object file is not a byte string". They are
/// not restated here -- a figure others refer to belongs where they point at it,
/// never copied. The two worth knowing before reading this file:
///
/// - ELF keeps the byte comparison. This is a platform gap, not a total failure.
/// - `.debug$S` and `.chks64` are volatile in the PATH, not in time, and the
///   verifier holds the path fixed -- so they are **not** excused, or a hit served
///   from another checkout goes unreported.
///
/// The one normalised region is therefore a single header field, whose offset is the
/// only thing the layout table is consulted for. **Parsing never grants an excuse
/// beyond it** -- the section walk exists solely to say where a difference was, so a
/// parser defect can make a message vague and cannot make a wrong object pass.

/// What comparing two object images found.
///
/// Four answers and not three, because `Unsupported` is the one that must not be
/// folded into `Different`: "this is not what the compiler produces" and "this
/// program cannot read this format" are opposite claims, and reporting the second
/// as the first is the defect this file was written to remove.
enum class ObjectComparison : std::uint8_t
{
    /// Byte for byte. Every ELF hit, and every Windows hit compiled inside the same
    /// second as the object it is checked against.
    Identical = 0,
    /// Differs only in the compiler's timestamp -- the ordinary Windows hit.
    EquivalentApartFromVolatile,
    /// Differs in code, data, structure, or a record of where the compile happened.
    /// `ObjectComparisonResult::detail` says which, because those need different
    /// things done about them.
    Different,
    /// The format could not be read, so nothing is claimed either way. An operator
    /// who cannot tell "the instrument does not apply here" from "your cache is
    /// broken" switches the instrument off, which is how the guarded class becomes
    /// invisible again.
    Unsupported,
};

/// What a comparison found, and enough about it to act on.
///
/// `detail` OWNS its text rather than viewing the images it was computed from:
/// this is returned by value and its inputs are spans over buffers the caller is
/// free to drop, which is the use-after-free `.agent/rules/wire-and-protocol.md`
/// records having happened twice already.
/// Every field is named at every designated-initializer site, including the ones a
/// member's own default initializer would supply. That looks like redundancy and is
/// not: clang's `-Wmissing-designated-field-initializers` is an ERROR in this tree's
/// pedantic presets, while MSVC accepts the short form -- so trimming them builds
/// clean on Windows and fails the Linux gate. A cleanup pass removed them here for
/// exactly that reason and the gate put them back.
struct ObjectComparisonResult
{
    ObjectComparison outcome { ObjectComparison::Different };
    /// Where the difference was, or which format could not be read. Empty only for
    /// `Identical`, where there is nothing to say.
    std::string detail;
};

/// Compare the object a cache hit produced against one a fresh compile produced.
///
/// @param served The bytes the cache put on disk.
/// @param fresh The bytes the compiler has just produced, at the SAME path. Its
///        readability is what decides `Unsupported`: it is this toolchain's own
///        output, so an image this code cannot lay out is this code's blind spot
///        and never the cache's fault. A `served` image that will not parse while
///        `fresh` does is `Different` -- that is what a truncated transfer looks
///        like, and it is a real finding.
/// @return The outcome and a sentence naming what it turned on.
[[nodiscard]] ObjectComparisonResult CompareObjectImages(std::span<std::byte const> served,
                                                         std::span<std::byte const> fresh);

} // namespace FastCache::Cc
