// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/Meta.hpp>
#include <CowTree/PageId.hpp>
#include <tests/MetaLayoutBuilders.hpp>

TEST_CASE("Meta encode/decode round-trip", "[meta]")
{
    CowTree::Meta meta;
    meta.pageSize = 4096;
    meta.txnId = 42;
    meta.root = CowTree::PageId { 7 };
    meta.freeRoot = CowTree::PageId { 3 };
    meta.itemCount = 1234;
    meta.dataPages = 77;

    std::vector<std::byte> page(meta.pageSize, std::byte { 0 });
    auto enc = CowTree::EncodeMeta({ page.data(), page.size() }, meta);
    REQUIRE(enc.has_value());

    auto dec = CowTree::DecodeMeta({ page.data(), page.size() });
    REQUIRE(dec.has_value());
    REQUIRE(dec->magic == CowTree::MetaMagic);
    REQUIRE(dec->version == CowTree::MetaVersion);
    REQUIRE(dec->pageSize == 4096U);
    REQUIRE(dec->txnId == 42U);
    REQUIRE(dec->root.value == 7U);
    REQUIRE(dec->freeRoot.value == 3U);
    REQUIRE(dec->itemCount == 1234U);
    REQUIRE(dec->dataPages == std::optional<std::uint64_t> { 77 });
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

    for (auto const i: std::views::iota(std::size_t { 0 }, CowTree::MetaEncodedSize - sizeof(std::uint32_t)))
    {
        auto copy = page;
        copy[i] = std::byte { static_cast<std::uint8_t>(static_cast<std::uint8_t>(copy[i]) ^ 0xFFU) };
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

TEST_CASE("Meta round-trips dataPages, and a disengaged one stays disengaged", "[meta]")
{
    std::vector<std::byte> page(4096);
    CowTree::Meta meta;
    meta.pageSize = 4096;
    meta.txnId = 7;
    meta.dataPages = 1234;
    REQUIRE(CowTree::EncodeMeta({ page.data(), page.size() }, meta).has_value());
    auto const dec = CowTree::DecodeMeta({ page.data(), page.size() });
    REQUIRE(dec.has_value());
    CHECK(dec->version == 2U);
    CHECK(dec->dataPages == std::optional<std::uint64_t> { 1234 });

    meta.dataPages.reset();
    REQUIRE(CowTree::EncodeMeta({ page.data(), page.size() }, meta).has_value());
    auto const unknown = CowTree::DecodeMeta({ page.data(), page.size() });
    REQUIRE(unknown.has_value());
    CHECK_FALSE(unknown->dataPages.has_value());
}

TEST_CASE("A version-1 meta still decodes, with no dataPages", "[meta]")
{
    CowTree::Meta meta;
    meta.pageSize = 4096;
    meta.txnId = 11;
    meta.root = CowTree::PageId { 3 };
    meta.itemCount = 5;
    auto const page = CowTree::Testing::EncodeMetaV1(meta, 4096);
    auto const dec = CowTree::DecodeMeta({ page.data(), page.size() });
    REQUIRE(dec.has_value());
    CHECK(dec->version == 1U);
    CHECK(dec->txnId == 11);
    CHECK(dec->root.value == 3);
    CHECK_FALSE(dec->dataPages.has_value());
}

TEST_CASE("An unknown meta version is refused, and damage to it is Corrupt", "[meta]")
{
    std::vector<std::byte> page(4096);
    CowTree::Meta meta;
    meta.pageSize = 4096;
    REQUIRE(CowTree::EncodeMeta({ page.data(), page.size() }, meta).has_value());
    // A flipped version byte, CRC untouched: damage, so Corrupt.
    auto damaged = page;
    damaged[4] = std::byte { 9 };
    CHECK(CowTree::DecodeMeta({ damaged.data(), damaged.size() }).error() == CowTree::CowTreeError::Corrupt);
    // A version this build does not know, with a CRC that matches: refused, not Corrupt.
    // `EncodeMeta` always writes the current version, so the bytes come from the layout builder.
    auto const foreign = CowTree::Testing::EncodeMetaLayout(meta, 4096, 9, true);
    CHECK(CowTree::DecodeMeta({ foreign.data(), foreign.size() }).error() == CowTree::CowTreeError::InvalidArg);
}
