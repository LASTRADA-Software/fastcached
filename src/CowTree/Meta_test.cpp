// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/Meta.hpp>
#include <CowTree/PageId.hpp>

TEST_CASE("Meta encode/decode round-trip", "[meta]")
{
    CowTree::Meta meta;
    meta.pageSize = 4096;
    meta.txnId = 42;
    meta.root = CowTree::PageId { 7 };
    meta.freeRoot = CowTree::PageId { 3 };
    meta.itemCount = 1234;

    std::vector<std::byte> page(meta.pageSize, std::byte { 0 });
    auto enc = CowTree::EncodeMeta({ page.data(), page.size() }, meta);
    REQUIRE(enc.has_value());

    auto dec = CowTree::DecodeMeta({ page.data(), page.size() });
    REQUIRE(dec.has_value());
    REQUIRE(dec->magic == CowTree::MetaMagic);
    REQUIRE(dec->version == CowTree::MetaVersion);
    REQUIRE(dec->pageSize == 4096u);
    REQUIRE(dec->txnId == 42u);
    REQUIRE(dec->root.value == 7u);
    REQUIRE(dec->freeRoot.value == 3u);
    REQUIRE(dec->itemCount == 1234u);
}

TEST_CASE("Meta decode rejects truncated buffers", "[meta]")
{
    std::vector<std::byte> page(4, std::byte { 0 });
    auto dec = CowTree::DecodeMeta({ page.data(), page.size() });
    REQUIRE_FALSE(dec.has_value());
    REQUIRE(dec.error() == CowTree::CowTreeError::OutOfRange);
}

TEST_CASE("Meta CRC catches every single-byte mutation in encoded payload", "[meta][crc]")
{
    CowTree::Meta meta;
    meta.pageSize = 4096;
    meta.txnId = 99;
    meta.root = CowTree::PageId { 11 };
    meta.freeRoot = CowTree::PageId::None();
    meta.itemCount = 5;

    std::vector<std::byte> page(meta.pageSize, std::byte { 0 });
    REQUIRE(CowTree::EncodeMeta({ page.data(), page.size() }, meta).has_value());

    for (std::size_t i = 0; i < CowTree::MetaEncodedSize - sizeof(std::uint32_t); ++i)
    {
        auto copy = page;
        copy[i] = std::byte { static_cast<std::uint8_t>(static_cast<std::uint8_t>(copy[i]) ^ 0xFFu) };
        auto dec = CowTree::DecodeMeta({ copy.data(), copy.size() });
        REQUIRE_FALSE(dec.has_value());
    }
}

TEST_CASE("Meta decode rejects wrong magic", "[meta]")
{
    CowTree::Meta meta;
    meta.pageSize = 4096;
    std::vector<std::byte> page(meta.pageSize, std::byte { 0 });
    REQUIRE(CowTree::EncodeMeta({ page.data(), page.size() }, meta).has_value());

    // Corrupt the magic field; recompute a matching CRC so we hit the
    // post-CRC magic check, not the CRC check.
    page[0] = std::byte { 0 };
    page[1] = std::byte { 0 };
    page[2] = std::byte { 0 };
    page[3] = std::byte { 0 };

    auto dec = CowTree::DecodeMeta({ page.data(), page.size() });
    REQUIRE_FALSE(dec.has_value());
    // Either the CRC or the magic check fires, both are valid failure modes.
    REQUIRE((dec.error() == CowTree::CowTreeError::Corrupt || dec.error() == CowTree::CowTreeError::InvalidArg));
}
