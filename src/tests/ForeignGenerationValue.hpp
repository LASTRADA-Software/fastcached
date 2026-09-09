// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace FastCache::Testing
{

/// A generation inside the reserved leading-byte range that this build does not implement.
///
/// Derived rather than spelled `CompileValueVersion + 1`, which is what all four call
/// sites this header replaced were spelling. That arithmetic is correct today and stops
/// being correct at the top of the range: `MaxCompileValueGeneration` is 15, a leading
/// byte outside `[1, 15]` is **not a compile value of any generation** (#552), and a
/// value built that way would classify as `NotACompileValue` -- a different outcome,
/// under a different policy, in a case whose name still says foreign generation.
///
/// The direction is therefore not fixed, and it does not need to be: the refusal is
/// direction-neutral (`DecodeCompileValue` refuses any `version != CompileValueVersion`),
/// and since #547 took the byte to 2 the direction actually in the field is the
/// backward one -- older launchers against newer servers. Forward is merely the
/// direction a test can synthesise honestly while there is room for it.
inline constexpr std::uint8_t ForeignGeneration = CompileValueVersion < MaxCompileValueGeneration
                                                      ? static_cast<std::uint8_t>(CompileValueVersion + 1)
                                                      : static_cast<std::uint8_t>(CompileValueVersion - 1);

static_assert(ForeignGeneration != CompileValueVersion,
              "a foreign generation this build also implements is not a foreign generation");
static_assert(ForeignGeneration >= 1 && ForeignGeneration <= MaxCompileValueGeneration,
              "a leading byte outside the reserved range is not a compile value at all (#552), so a value "
              "built with one is `NotACompileValue` rather than another generation -- a different outcome "
              "under a different policy, in a case still named for the one it no longer builds");

/// Restamp an encoded compile value with another generation.
///
/// Where the generation sits is `CompileValueGenerationOffset` and `DeclaredGeneration`,
/// both in `CompileValue.hpp` beside the encoder that guarantees it -- not a fourth
/// spelling here. That was hand-rolled as `front()` in four places across two test
/// binaries (#649), which is three more than can be corrected together: a
/// `CompileValueVersion` bump, or anything moving the generation off byte 0, has to find
/// every one of them. Miss one and it stops constructing what its name says while its
/// case goes on passing, because the assertion at every site is about a REFUSAL and a
/// value damaged in some other way is refused too.
///
/// A violated pre-condition throws rather than returning something: a caller has no
/// answer it could act on, and this is contract misuse rather than a runtime failure.
/// @param encoded    A value straight out of `EncodeCompileValue`.
/// @param generation The generation byte to stamp in.
/// @return The same bytes, restamped.
[[nodiscard]] inline std::vector<std::byte> StampGeneration(std::vector<std::byte> encoded, std::uint8_t generation)
{
    auto const declared = DeclaredGeneration(encoded);
    if (!declared.has_value())
        throw std::logic_error { "StampGeneration: EncodeCompileValue produced nothing to stamp" };

    auto const leading = *declared;
    if (leading != CompileValueVersion)
        throw std::logic_error { "StampGeneration: the byte at CompileValueGenerationOffset is " + std::to_string(leading)
                                 + ", not this build's generation " + std::to_string(CompileValueVersion)
                                 + " -- these bytes did not come from this build's encoder, so stamping them would "
                                   "leave the caller asserting a refusal it gets for the wrong reason (#649)" };

    encoded[CompileValueGenerationOffset] = std::byte { generation };
    return encoded;
}

/// A well-formed value of THIS build, stamped with a generation this build does not implement.
///
/// Encoded through the real encoder, so the framing is honest and what separates it from
/// junk is the LAYOUT rather than the leading byte -- which is the whole of #483's
/// distinction and the reason every call site says so in its own words.
///
/// The content is what three of the four call sites built by hand, and its shape is
/// load-bearing: an object blob and one `/showIncludes` region, the region being what a
/// canonicalizer would have had to rewrite. A server refusing this value is refusing one it
/// can SEE holds the producer's absolute paths -- #229 reached by nothing worse than a
/// rolling upgrade. An empty value would be refused too and would demonstrate less.
///
/// A case wanting its own content spells the two steps itself
/// (`StampGeneration(EncodeCompileValue(mine), ForeignGeneration)`), which is why there is
/// no overload taking one: an entry point nothing calls is a shape the next author copies.
/// @return The encoded bytes, carrying `ForeignGeneration`.
[[nodiscard]] inline std::vector<std::byte> ForeignGenerationValue()
{
    CompileValue produced;
    produced.objectBlob = { std::byte { 0x01 } };
    produced.textRegions.push_back(
        TextRegion { .grammar = PathCanon::Grammar::ShowIncludes, .bytes = "Note: including file: /src/inc/a.hpp\n" });
    return StampGeneration(EncodeCompileValue(produced), ForeignGeneration);
}

} // namespace FastCache::Testing
