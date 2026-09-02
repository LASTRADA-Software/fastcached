// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterSigning.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cluster;

namespace
{
/// The cluster's key, as a file holding thirty-two bytes would supply it.
[[nodiscard]] std::vector<std::byte> Key(unsigned char fill = 0x5A)
{
    return std::vector<std::byte>(32, static_cast<std::byte>(fill));
}

/// The bytes of @p text.
[[nodiscard]] std::span<std::byte const> Bytes(std::string_view text)
{
    return WireFields::AsBytes(text);
}
} // namespace

TEST_CASE("Each domain's label is the byte string on the wire", "[cluster][signing]")
{
    // The labels themselves, spelled out, because they are covered by the MAC:
    // changing one retires every outstanding tag in that domain, and that should
    // be a deliberate act with a test to update rather than a silent edit.
    // `fastcache-lease-v1` in particular is what the lease carried BEFORE the seam
    // existed, which is what made moving it a no-op on the wire.
    //
    // That the two are non-empty and distinct is `SigningLabelsSeparateDomains`,
    // `static_assert`ed three lines below its own definition -- a runtime `CHECK`
    // of a `consteval` predicate cannot fail in a translation unit that compiled,
    // so it would read as a guarantee while asserting nothing.
    CHECK(DescribeSigningDomain(SigningDomain::DiscoveryProof).label == "fastcache-discovery-v1");
    CHECK(DescribeSigningDomain(SigningDomain::LeaseToken).label == "fastcache-lease-v1");
}

TEST_CASE("One field list signs differently in each domain", "[cluster][signing]")
{
    // The property the seam exists for, and the one neither construction had on its
    // own. Safety used to be a fact about the PAIR: a discovery proof was four
    // fields beginning with a cluster id, a lease tag two beginning with a literal,
    // so no byte string was a valid message under both. That is a coincidence, and
    // it survives exactly as long as nobody adds a field to discovery. Here the
    // SAME fields are signed in both domains and must not produce the same tag.
    auto const key = Key();
    auto const fields = std::array<std::span<std::byte const>, 2> { Bytes("node-a"), Bytes("10.0.0.7:6675") };

    auto const asProof = SignFields(key, SigningDomain::DiscoveryProof, fields);
    auto const asLease = SignFields(key, SigningDomain::LeaseToken, fields);

    CHECK_FALSE(ConstantTimeEquals(asProof, asLease));
    CHECK_FALSE(VerifyFields(key, SigningDomain::LeaseToken, fields, asProof));
    CHECK_FALSE(VerifyFields(key, SigningDomain::DiscoveryProof, fields, asLease));

    // And each still authenticates in its own domain, or the separation would be
    // indistinguishable from nothing working at all.
    CHECK(VerifyFields(key, SigningDomain::DiscoveryProof, fields, asProof));
    CHECK(VerifyFields(key, SigningDomain::LeaseToken, fields, asLease));
}

TEST_CASE("An empty field list is still a signed statement of its domain", "[cluster][signing]")
{
    // Not a degenerate case to skip. A message with no fields still carries the
    // label, so the two domains disagree even with nothing to say -- which is the
    // whole failure mode in miniature: under an unlabelled construction, signing
    // nothing yields one tag that is valid in every domain.
    auto const key = Key();
    auto const none = WireFields::FieldList {};

    CHECK_FALSE(ConstantTimeEquals(SignFields(key, SigningDomain::DiscoveryProof, none),
                                   SignFields(key, SigningDomain::LeaseToken, none)));
}

TEST_CASE("The signed message is the label, then the fields, all length-prefixed", "[cluster][signing]")
{
    // The construction pinned as BYTES rather than described in prose, because a
    // seam that two protocols share is exactly the place where "it still verifies
    // against itself" passes while every deployed peer stops agreeing. Written out
    // here the long way -- `HmacSha256` over `WireFields::Encode` -- so the message
    // shape is asserted against something other than the code that produces it.
    auto const key = Key();
    auto const first = Bytes("node-a");
    auto const second = Bytes("10.0.0.7:6675");

    for (auto const& row: SigningDomainTable)
    {
        auto const expected = HmacSha256(key, WireFields::Encode({ WireFields::AsBytes(row.label), first, second }));
        CHECK(ConstantTimeEquals(SignFields(key, row.domain, { first, second }), expected));
    }
}

TEST_CASE("A tampered tag is refused", "[cluster][signing]")
{
    auto const key = Key();
    auto const fields = std::array<std::span<std::byte const>, 1> { Bytes("payload") };
    auto const honest = SignFields(key, SigningDomain::LeaseToken, fields);

    REQUIRE(VerifyFields(key, SigningDomain::LeaseToken, fields, honest));

    // Every byte, not just the first. A comparison that stopped early would accept
    // a suffix edit, and stopping early is precisely what `ConstantTimeEquals` --
    // which `VerifyFields` exists so that nobody has to remember to reach for --
    // is there to prevent.
    for (auto const index: std::views::iota(std::size_t { 0 }, honest.size()))
    {
        auto tampered = honest;
        tampered[index] ^= std::byte { 0x01 };
        CHECK_FALSE(VerifyFields(key, SigningDomain::LeaseToken, fields, tampered));
    }
}

TEST_CASE("A tag covers these fields, this key, and this arity", "[cluster][signing]")
{
    auto const key = Key();
    auto const granted = std::array<std::span<std::byte const>, 2> { Bytes("node-a"), Bytes("10.0.0.7:6675") };
    auto const honest = SignFields(key, SigningDomain::DiscoveryProof, granted);

    // A different endpoint. This is the load-bearing field on both wires: a MAC
    // over "somebody may join" or "somebody may compile" is a tag captured on the
    // way to one machine and replayed against every machine that trusts the key.
    auto const elsewhere = std::array<std::span<std::byte const>, 2> { Bytes("node-a"), Bytes("10.0.0.9:6675") };
    CHECK_FALSE(VerifyFields(key, SigningDomain::DiscoveryProof, elsewhere, honest));

    // A different key, which is what it all rests on.
    CHECK_FALSE(VerifyFields(Key(0x5B), SigningDomain::DiscoveryProof, granted, honest));

    // A different arity. An appended EMPTY field is the cheapest way to ask whether
    // the encoding says how many fields there were: it adds no content, only a
    // length prefix, so a construction that merely concatenated would not notice.
    auto const padded = std::array<std::span<std::byte const>, 3> { Bytes("node-a"), Bytes("10.0.0.7:6675"), Bytes("") };
    CHECK_FALSE(VerifyFields(key, SigningDomain::DiscoveryProof, padded, honest));
}

TEST_CASE("The label boundary cannot be shifted into", "[cluster][signing]")
{
    // That fields are framed rather than joined is `WireFields`' property, and
    // `DiscoveryWire_test` and `LeaseToken_test` each already assert it for the
    // claim lists they own. What is this seam's alone is the boundary between the
    // label and the first field: the label is the one part of the message a caller
    // does not supply, so if it were a prefix glued onto the first field rather
    // than a field of its own, a caller choosing that field could spell the tail of
    // another domain's label into it and produce that domain's message.
    auto const key = Key();

    auto const label = std::string { DescribeSigningDomain(SigningDomain::LeaseToken).label };
    REQUIRE_FALSE(label.empty());

    auto const truncated = label.substr(0, label.size() - 1);
    auto const restored = label.substr(label.size() - 1) + "payload";
    CHECK_FALSE(ConstantTimeEquals(SignFields(key, SigningDomain::LeaseToken, { Bytes("payload") }),
                                   HmacSha256(key, WireFields::Encode({ Bytes(truncated), Bytes(restored) }))));
}

TEST_CASE("Both spellings of a field list sign the same", "[cluster][signing]")
{
    // The `initializer_list` overload is a convenience over the span one and must
    // not become a second construction: a minter reaching for one and a verifier
    // for the other presents as every credential in the fleet failing to
    // authenticate, after a change nobody connected to it.
    auto const key = Key();
    auto const fields = std::array<std::span<std::byte const>, 2> { Bytes("node-a"), Bytes("10.0.0.7:6675") };

    CHECK(ConstantTimeEquals(SignFields(key, SigningDomain::LeaseToken, fields),
                             SignFields(key, SigningDomain::LeaseToken, { Bytes("node-a"), Bytes("10.0.0.7:6675") })));
}
