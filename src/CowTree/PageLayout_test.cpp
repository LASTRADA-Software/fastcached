// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

#include <CowTree/Bytes.hpp>
#include <CowTree/Errors.hpp>
#include <CowTree/PageId.hpp>
#include <CowTree/PageLayout.hpp>

namespace
{

CowTree::BytesView Bytes(std::string_view s) noexcept
{
    return CowTree::AsBytes(s);
}

} // namespace

TEST_CASE("Encode/decode round-trip for an empty leaf page", "[pagelayout]")
{
    std::vector<std::byte> page(4096, std::byte { 0 });
    REQUIRE(CowTree::EncodeLeafPage({ page.data(), page.size() }, {}).has_value());

    auto header = CowTree::DecodePageHeader({ page.data(), page.size() });
    REQUIRE(header.has_value());
    REQUIRE(header->type == CowTree::PageType::Leaf);
    REQUIRE(header->entryCount == 0);
}

TEST_CASE("Leaf page round-trip preserves entries in order", "[pagelayout]")
{
    std::vector<CowTree::LeafEntry> entries {
        { Bytes("apple"), Bytes("red") },
        { Bytes("banana"), Bytes("yellow") },
        { Bytes("cherry"), Bytes("dark-red") },
    };
    std::vector<std::byte> page(4096, std::byte { 0 });
    REQUIRE(CowTree::EncodeLeafPage({ page.data(), page.size() }, { entries.data(), entries.size() }).has_value());

    auto header = CowTree::DecodePageHeader({ page.data(), page.size() });
    REQUIRE(header.has_value());
    REQUIRE(header->type == CowTree::PageType::Leaf);
    REQUIRE(header->entryCount == 3);

    auto decoded = CowTree::DecodeLeafEntries({ page.data(), page.size() }, *header);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == 3);
    REQUIRE(CowTree::AsStringView((*decoded)[0].key) == "apple");
    REQUIRE(CowTree::AsStringView((*decoded)[0].value) == "red");
    REQUIRE(CowTree::AsStringView((*decoded)[1].key) == "banana");
    REQUIRE(CowTree::AsStringView((*decoded)[2].value) == "dark-red");
}

TEST_CASE("Leaf page rejects oversized payload", "[pagelayout]")
{
    std::vector<std::byte> big(2000, std::byte { 0x42 });
    std::vector<CowTree::LeafEntry> entries {
        { Bytes("k1"), CowTree::BytesView { big.data(), big.size() } },
        { Bytes("k2"), CowTree::BytesView { big.data(), big.size() } },
        { Bytes("k3"), CowTree::BytesView { big.data(), big.size() } },
    };
    std::vector<std::byte> page(4096, std::byte { 0 });
    auto r = CowTree::EncodeLeafPage({ page.data(), page.size() }, { entries.data(), entries.size() });
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error() == CowTree::CowTreeError::ValueTooLarge);
}

TEST_CASE("Internal page encodes firstChild + entries", "[pagelayout]")
{
    std::vector<CowTree::InternalEntry> entries {
        { Bytes("d"), CowTree::PageId { 2 } },
        { Bytes("m"), CowTree::PageId { 3 } },
    };
    std::vector<std::byte> page(4096, std::byte { 0 });
    REQUIRE(
        CowTree::EncodeInternalPage({ page.data(), page.size() }, CowTree::PageId { 1 }, { entries.data(), entries.size() })
            .has_value());

    auto header = CowTree::DecodePageHeader({ page.data(), page.size() });
    REQUIRE(header.has_value());
    REQUIRE(header->type == CowTree::PageType::Internal);
    REQUIRE(header->entryCount == 2);
    REQUIRE(header->firstChild.value == 1u);

    auto decoded = CowTree::DecodeInternalEntries({ page.data(), page.size() }, *header);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == 2);
    REQUIRE((*decoded)[0].child.value == 2u);
    REQUIRE((*decoded)[1].child.value == 3u);
    REQUIRE(CowTree::AsStringView((*decoded)[0].key) == "d");
}

TEST_CASE("DecodePageHeader rejects flipped CRC byte", "[pagelayout][crc]")
{
    std::vector<std::byte> page(4096, std::byte { 0 });
    REQUIRE(CowTree::EncodeLeafPage({ page.data(), page.size() }, {}).has_value());

    page[0] = std::byte { static_cast<std::uint8_t>(static_cast<std::uint8_t>(page[0]) ^ 0xFFu) };
    auto header = CowTree::DecodePageHeader({ page.data(), page.size() });
    REQUIRE_FALSE(header.has_value());
    REQUIRE(header.error() == CowTree::CowTreeError::Corrupt);
}
