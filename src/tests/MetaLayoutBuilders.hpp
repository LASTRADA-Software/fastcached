// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/Crc32c.hpp>
#include <CowTree/Meta.hpp>

namespace CowTree::Testing
{

/// Encode @p meta by hand in an on-disk meta layout, stamping @p version into the version field.
///
/// Spelled out here, never through `EncodeMeta`: the encoder only writes the current layout, so a
/// fixture asking it for an old or foreign one would test nothing old. This is the ONE copy, shared
/// by `Meta_test` and `FilePageStore_test`, because a hand-rolled builder that drifts from the
/// layout fails silently -- every case built on it still asserts a refusal other damage also produces.
///
/// @param meta           Source record; `dataPages` is written only when @p withDataPages is set,
///                       and a disengaged one is written as `MetaDataPagesUnknown`.
/// @param pageSize       Size of the returned zero-padded page.
/// @param version        Value written into the version field.
/// @param withDataPages  True for the version-2 layout (CRC at offset 64), false for version 1 (offset 56).
/// @return The encoded page, CRC included.
[[nodiscard]] inline std::vector<std::byte> EncodeMetaLayout(Meta const& meta,
                                                             std::size_t pageSize,
                                                             std::uint32_t version,
                                                             bool withDataPages)
{
    std::vector<std::byte> page(pageSize, std::byte { 0 });
    std::size_t at = 0;
    auto const put = [&](auto value) {
        for (auto const i: std::views::iota(std::size_t { 0 }, sizeof(value)))
            page[at + i] = static_cast<std::byte>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xFFU);
        at += sizeof(value);
    };
    put(meta.magic);
    put(version);
    put(meta.pageSize);
    put(meta.keyBytes);
    put(meta.txnId);
    put(meta.root.value);
    put(meta.freeRoot.value);
    put(meta.itemCount);
    put(meta.valueBytes);
    if (withDataPages)
        put(meta.dataPages.value_or(MetaDataPagesUnknown));
    put(Crc32c::Compute(BytesView { page.data(), at }));
    return page;
}

/// Encode @p meta in the VERSION-1 layout, the one every store written before #1624 carries.
/// @param meta     Source record; its `dataPages` is not written.
/// @param pageSize Size of the returned page.
/// @return The page; its version field says 1 and its CRC sits at `MetaEncodedSizeV1 - 4`.
[[nodiscard]] inline std::vector<std::byte> EncodeMetaV1(Meta const& meta, std::size_t pageSize)
{
    return EncodeMetaLayout(meta, pageSize, 1U, false);
}

} // namespace CowTree::Testing
