// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/IdentityKeyLabel.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/MachineTicket.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;
using FastCache::Testing::TestKeyPair;
using FastCache::Testing::Unwrap;

namespace
{

/// Claims whose every field is distinct, so a field read from the wrong slot shows.
[[nodiscard]] MachineTicketClaims Claims()
{
    auto nonce = MachineTicketNonce {};
    for (auto const index: std::views::iota(std::size_t { 0 }, nonce.size()))
        nonce[index] = static_cast<std::byte>(index + 1);
    return MachineTicketClaims {
        .machineId = "pc-07", .audience = "office.corp:6674", .expiresAtUnixSeconds = 1'790'000'000, .nonce = nonce
    };
}

/// A ticket's bytes, as AUTH carries them.
[[nodiscard]] std::vector<std::byte> BytesOf(SecureString const& ticket)
{
    auto const view = WireFields::AsBytes(ticket.View());
    return { view.begin(), view.end() };
}

/// `Claims()` packed field by field, so a test can give the label, the expiry or the nonce a
/// value or a WIDTH the codec itself would never write.
/// @param label The first field.
/// @param expiry The fourth field's bytes.
/// @param nonce The fifth field's bytes.
/// @return The packed fields.
[[nodiscard]] std::vector<std::byte> PackFields(std::string_view label,
                                                std::span<std::byte const> expiry,
                                                std::span<std::byte const> nonce)
{
    auto const claims = Claims();
    return WireFields::Encode({ WireFields::AsBytes(label),
                                WireFields::AsBytes(claims.machineId),
                                WireFields::AsBytes(claims.audience),
                                expiry,
                                nonce });
}

/// A ticket around @p packed, validly signed by pc-07's key, its signature then cut or padded to
/// @p signatureBytes -- so the signature's WIDTH can be wrong while everything else is right.
/// @param packed The packed claims.
/// @param signatureBytes The signature field's width.
/// @return The encoded ticket.
[[nodiscard]] std::vector<std::byte> Envelope(std::span<std::byte const> packed,
                                              std::size_t signatureBytes = Ed25519SignatureBytes)
{
    auto const signature = TestKeyPair("pc-07").Sign(packed);
    auto field = std::vector<std::byte>(signature.begin(), signature.end());
    field.resize(signatureBytes, std::byte { 0xA5 });
    return WireFields::Encode({ packed, std::span<std::byte const> { field } });
}

/// @p bytes with one more byte on the end.
/// @param bytes The bytes to extend.
/// @return A copy, a byte longer.
[[nodiscard]] std::vector<std::byte> OneLonger(std::span<std::byte const> bytes)
{
    auto longer = std::vector<std::byte>(bytes.begin(), bytes.end());
    longer.push_back(std::byte { 0xA5 });
    return longer;
}

} // namespace

TEST_CASE("A machine ticket round-trips its claims, and its signature covers the bytes as sent", "[distributed][ticket]")
{
    auto const key = TestKeyPair("pc-07");
    auto const bytes = BytesOf(MintMachineTicket(key, Claims()));

    auto const decoded = DecodeMachineTicket(bytes);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).claims == Claims());
    CHECK(Unwrap(decoded).signedBytes == PackMachineTicketClaims(Claims()));
    CHECK(Ed25519Verify(key.PublicKey(), Unwrap(decoded).signedBytes, Unwrap(decoded).signature));
    // Another machine's key does not verify it: the signature is the machine's, not the label's.
    CHECK_FALSE(Ed25519Verify(TestKeyPair("pc-08").PublicKey(), Unwrap(decoded).signedBytes, Unwrap(decoded).signature));
}

// The MINT-TICKET reply a client reads holds the largest ticket the decoder accepts.
static_assert(MaxMachineTicketBytes < CompileCacheWire::MaxMintTicketReply,
              "a ticket the decoder accepts must fit the MINT-TICKET reply ceiling");

TEST_CASE("A machine ticket is one labelled construction, and what it carries is what the verifier rebuilds",
          "[distributed][ticket]")
{
    // The verifier checks the signature over the message it REBUILDS from the decoded claims, through
    // the seam every identity-key construction verifies through. That is sound only while the bytes
    // a ticket carries are exactly that message, so both halves are asserted here.
    auto const key = TestKeyPair("pc-07");
    auto const decoded = DecodeMachineTicket(BytesOf(MintMachineTicket(key, Claims())));
    REQUIRE(decoded.has_value());
    auto const message = MachineTicketMessage(Unwrap(decoded).claims);
    CHECK(message.Purpose() == IdentityKeyPurpose::MachineTicket);
    CHECK(std::ranges::equal(message.Bytes(), Unwrap(decoded).signedBytes));
    CHECK(VerifyLabelled(key.PublicKey(), message, Unwrap(decoded).signature));
    // The label the table spells is the one tickets were first signed under: routing them through
    // `IdentityKeyLabels` moved no byte on the wire.
    CHECK(LabelOf(IdentityKeyPurpose::MachineTicket) == "fastcache-ticket-v1");
    CHECK(MachineTicketLabel == LabelOf(IdentityKeyPurpose::MachineTicket));
}

TEST_CASE("A machine ticket's signed bytes open with its versioned label, every field length-prefixed",
          "[distributed][ticket][wire]")
{
    auto const packed = PackMachineTicketClaims(Claims());
    // label (4+19), machine id (4+5), audience (4+16), expiry (4+8), nonce (4+16)
    REQUIRE(packed.size() == 23 + 9 + 20 + 12 + 20);
    CHECK(packed[0] == std::byte { 0x00 });
    CHECK(packed[1] == std::byte { 0x00 });
    CHECK(packed[2] == std::byte { 0x00 });
    CHECK(packed[3] == std::byte { 0x13 }); // 19: the label's length
    CHECK(WireFields::AsStringView(std::span { packed }.subspan(4, 19)) == "fastcache-ticket-v1");
    // The machine id is the second field and the audience the third. Pinned by content, because
    // the two lengths sum alike in either order, so the expiry's offset cannot tell them apart.
    CHECK(WireFields::FromBigEndian<std::uint32_t>(std::span { packed }.subspan(23, 4))
          == std::optional<std::uint32_t> { 5 });
    CHECK(WireFields::AsStringView(std::span { packed }.subspan(23 + 4, 5)) == "pc-07");
    CHECK(WireFields::AsStringView(std::span { packed }.subspan(23 + 9 + 4, 16)) == "office.corp:6674");
    // The expiry is a big-endian u64 of Unix SECONDS, the fourth field.
    auto const expiryField = std::span { packed }.subspan(23 + 9 + 20 + 4, 8);
    CHECK(WireFields::FromBigEndian<std::uint64_t>(expiryField) == std::optional<std::uint64_t> { 1'790'000'000 });
}

TEST_CASE("Field boundaries are part of what a machine ticket signs", "[distributed][ticket]")
{
    // `DiscoveryWire`'s scar: joined fields let {"a", "b:1"} and {"a:b", "1"} sign alike.
    auto left = Claims();
    left.machineId = "a";
    left.audience = "b:1";
    auto right = Claims();
    right.machineId = "a:b";
    right.audience = "1";
    CHECK(PackMachineTicketClaims(left) != PackMachineTicketClaims(right));
}

TEST_CASE("Bytes that are not exactly one machine ticket are refused, never partly read", "[distributed][ticket]")
{
    auto const good = BytesOf(MintMachineTicket(TestKeyPair("pc-07"), Claims()));

    struct Row
    {
        char const* what;
        std::vector<std::byte> bytes;
    };
    auto const claims = Claims();
    auto const expiry = WireFields::ToBigEndian<std::uint64_t>(claims.expiresAtUnixSeconds);
    auto const nonce = std::span<std::byte const> { claims.nonce };
    auto withLabel = [&](std::string_view label) {
        return Envelope(PackFields(label, expiry, nonce));
    };
    auto withExpiry = [&](std::span<std::byte const> field) {
        return Envelope(PackFields(MachineTicketLabel, field, nonce));
    };
    auto withNonce = [&](std::span<std::byte const> field) {
        return Envelope(PackFields(MachineTicketLabel, expiry, field));
    };
    auto withSignatureBytes = [&](std::size_t width) {
        return Envelope(PackFields(MachineTicketLabel, expiry, nonce), width);
    };
    auto withClaims = [](MachineTicketClaims const& ticketClaims) {
        return BytesOf(MintMachineTicket(TestKeyPair("pc-07"), ticketClaims));
    };
    // The builders make a ticket the codec accepts, so every row below is refused for its ONE
    // defect rather than for something the builder got wrong.
    REQUIRE(DecodeMachineTicket(withLabel(MachineTicketLabel)).has_value());

    auto emptyId = claims;
    emptyId.machineId.clear();
    auto emptyAudience = claims;
    emptyAudience.audience.clear();
    auto farFuture = claims;
    farFuture.expiresAtUnixSeconds = MaxMachineTicketExpirySeconds + 1;

    // The size bound, at the byte: a well-formed, validly signed ticket exactly AT
    // `MaxMachineTicketBytes` decodes and one a byte longer does not, so nothing but the bound can
    // be what refuses the longer one.
    auto const bytesBesideAudience = good.size() - claims.audience.size();
    auto atBound = claims;
    atBound.audience = std::string(MaxMachineTicketBytes - bytesBesideAudience, 'a');
    auto overBound = claims;
    overBound.audience = std::string(MaxMachineTicketBytes - bytesBesideAudience + 1, 'a');
    REQUIRE(withClaims(atBound).size() == MaxMachineTicketBytes);
    REQUIRE(withClaims(overBound).size() == MaxMachineTicketBytes + 1);

    auto const rows = std::to_array<Row>({
        { .what = "empty", .bytes = {} },
        { .what = "truncated by one byte", .bytes = std::vector<std::byte>(good.begin(), good.end() - 1) },
        { .what = "a trailing byte",
          .bytes =
              [&] {
                  auto b = good;
                  b.push_back(std::byte { 0 });
                  return b;
              }() },
        { .what = "another label", .bytes = withLabel("fastcache-ticket-v0") },
        { .what = "a lease's label", .bytes = withLabel(LabelOf(IdentityKeyPurpose::Lease)) },
        { .what = "no machine id", .bytes = withClaims(emptyId) },
        { .what = "no audience", .bytes = withClaims(emptyAudience) },
        { .what = "an expiry the clock cannot hold", .bytes = withClaims(farFuture) },
        { .what = "a 7-byte expiry", .bytes = withExpiry(std::span { expiry }.first(7)) },
        { .what = "a 9-byte expiry", .bytes = withExpiry(OneLonger(expiry)) },
        // Each short row before its long one: without the width check the long one copies past a
        // fixed array, and the short one's verdict is then already on record.
        { .what = "a 15-byte nonce", .bytes = withNonce(nonce.first(MachineTicketNonceBytes - 1)) },
        { .what = "a 17-byte nonce", .bytes = withNonce(OneLonger(nonce)) },
        { .what = "a 63-byte signature", .bytes = withSignatureBytes(Ed25519SignatureBytes - 1) },
        { .what = "a 65-byte signature", .bytes = withSignatureBytes(Ed25519SignatureBytes + 1) },
        { .what = "one byte over the size bound, well formed and signed", .bytes = withClaims(overBound) },
    });
    for (auto const& row: rows)
    {
        INFO(row.what);
        CHECK_FALSE(DecodeMachineTicket(row.bytes).has_value());
    }
    CHECK(DecodeMachineTicket(good).has_value());                // the control
    CHECK(DecodeMachineTicket(withClaims(atBound)).has_value()); // and the bound's own
}
