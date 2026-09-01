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
/// **Why this exists.** `FASTCACHE_VERIFY` compared object files with a `memcmp`,
/// which is exactly right on ELF and can never succeed on Windows: every
/// MSVC-family driver stamps the wall clock into the COFF header, and a cached
/// object was produced earlier than the fresh compile it is checked against **by
/// construction**. So the one instrument that turns a wrong object from invisible
/// into loud reported a wrong object on every hit, on the platform where the defect
/// it exists to detect was observed
/// ([#493](https://github.com/LASTRADA-Software/fastcached/issues/493)).
///
/// Measured on this tree -- MSVC 14.51 and the VS-shipped clang-cl, clang 20.1.2
/// and GCC 14.2 -- and the numbers decide the design rather than illustrate it:
///
/// | two compiles of one TU differing in           | cl                      | clang-cl | clang / gcc |
/// |-----------------------------------------------|-------------------------|----------|-------------|
/// | nothing (same object path, seconds apart)     | the 4-byte `TimeDateStamp`, and nothing else | the same | identical, with and without `-g` |
/// | the object's directory                        | + `.debug$S`, `.chks64` | nothing more | identical |
/// | the source's directory                        | + `.debug$S`, `.chks64` | nothing more | identical |
///
/// Two conclusions, and the second is the whole shape of this file:
///
/// **On ELF the byte comparison is correct and is kept.** This is a platform gap,
/// not a total failure, and a correct answer must not be replaced by a parser.
///
/// **`.debug$S` and `.chks64` are volatile with respect to the PATH, not to time,**
/// and the verifier holds the path fixed -- it copies the served object aside and
/// compiles to the *same* output path. So a hit produced by this machine in this
/// checkout differs in the clock **alone**, and excusing those two sections would
/// buy nothing while blinding the verifier to
/// [#489](https://github.com/LASTRADA-Software/fastcached/issues/489): a hit served
/// from another checkout's object, whose entire difference lives in `.debug$S`.
/// That is the case an operator runs this to catch, so it is reported, and the
/// message says which of the two it is rather than leaving "wrong object" to mean
/// either.
///
/// **`/Brepro` is not the fix**, for the reason `scripts/dist-compile-e2e.ps1`
/// already gives about itself: it would make the comparison true about a command
/// line no build uses. What is compared here is what real builds produce.
///
/// The one normalised region is therefore a single header field, whose offset is
/// the only thing the format table below is consulted for. **Parsing never grants
/// an excuse beyond it** -- the section walk exists solely to say where a
/// difference was, so a parser defect can make a message vague and cannot make a
/// wrong object pass.

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
