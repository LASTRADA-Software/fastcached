// SPDX-License-Identifier: Apache-2.0
//
// Sealing a session's frames under its own key, driven directly: which key, which position,
// which bytes. Where a key comes from is the wire's, and is tested there.
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

using namespace FastCache;

namespace
{

/// Bytes of @p text, for a secret, a salt or an info that only has to be DISTINCT.
/// @param text The text.
/// @return Its bytes, borrowed.
[[nodiscard]] std::span<std::byte const> Bytes(std::string_view text) noexcept
{
    return WireFields::AsBytes(text);
}

/// The session key every case here seals under unless it says otherwise.
/// @param secret The shared secret.
/// @param info The wire's info.
/// @return The key.
[[nodiscard]] SessionKey Key(std::string_view secret = "a shared secret", std::string_view info = "a wire")
{
    auto key = DeriveSessionKey(Bytes(secret), Bytes("both nonces"), Bytes(info));
    REQUIRE(key.has_value());
    return *std::move(key);
}

/// A frame's two halves, distinct per @p n so two frames never tag alike by accident.
struct Frame
{
    std::array<std::byte, 7> header {}; ///< Stands in for a wire header.
    std::vector<std::byte> payload;     ///< Stands in for its payload.
};

/// Frame number @p n.
/// @param n Which frame.
/// @return Its halves.
[[nodiscard]] Frame FrameNumber(unsigned n)
{
    auto frame = Frame { .header = {}, .payload = std::vector<std::byte>(12, static_cast<std::byte>(0x40 + n)) };
    frame.header.fill(static_cast<std::byte>(n));
    return frame;
}

} // namespace

TEST_CASE("Sealed frames open in order, once", "[core][session-seal]")
{
    auto const first = FrameNumber(1);
    auto const second = FrameNumber(2);

    FrameSealer sealer { Key() };
    auto const firstTag = sealer.Seal(first.header, first.payload);
    auto const secondTag = sealer.Seal(second.header, second.payload);

    SECTION("in order, every frame opens")
    {
        FrameOpener opener { Key() };
        CHECK(opener.Open(first.header, first.payload, firstTag));
        CHECK(opener.Open(second.header, second.payload, secondTag));
    }

    SECTION("a replayed frame is refused")
    {
        FrameOpener opener { Key() };
        REQUIRE(opener.Open(first.header, first.payload, firstTag));
        CHECK_FALSE(opener.Open(first.header, first.payload, firstTag));
    }

    SECTION("a reordered frame is refused, and a refusal does not move the position")
    {
        FrameOpener opener { Key() };
        CHECK_FALSE(opener.Open(second.header, second.payload, secondTag));
        // The position stayed at zero, so the frame that belongs there still opens: a
        // refusal that advanced the count would refuse it too.
        CHECK(opener.Open(first.header, first.payload, firstTag));
    }

    SECTION("a dropped frame is refused at the next one")
    {
        FrameOpener opener { Key() };
        CHECK_FALSE(opener.Open(second.header, second.payload, secondTag));
    }
}

TEST_CASE("A sealed frame changed anywhere is refused", "[core][session-seal]")
{
    auto const frame = FrameNumber(3);
    FrameSealer sealer { Key() };
    auto const tag = sealer.Seal(frame.header, frame.payload);

    SECTION("the control: unchanged, it opens")
    {
        FrameOpener opener { Key() };
        CHECK(opener.Open(frame.header, frame.payload, tag));
    }

    SECTION("a header byte")
    {
        auto changed = frame;
        changed.header[2] ^= std::byte { 0x01 };
        FrameOpener opener { Key() };
        CHECK_FALSE(opener.Open(changed.header, changed.payload, tag));
    }

    SECTION("a payload byte")
    {
        auto changed = frame;
        changed.payload.back() ^= std::byte { 0x80 };
        FrameOpener opener { Key() };
        CHECK_FALSE(opener.Open(changed.header, changed.payload, tag));
    }

    SECTION("a byte moved from the header into the payload")
    {
        // The two halves are two FIELDS of the MAC, so where the boundary falls is covered:
        // a MAC over their concatenation would accept this.
        auto whole = std::vector<std::byte>(frame.header.begin(), frame.header.end());
        whole.insert(whole.end(), frame.payload.begin(), frame.payload.end());
        auto const bytes = std::span<std::byte const> { whole };
        FrameOpener opener { Key() };
        CHECK_FALSE(opener.Open(bytes.first(frame.header.size() - 1), bytes.subspan(frame.header.size() - 1), tag));
    }

    SECTION("the tag")
    {
        auto changed = tag;
        changed[0] ^= std::byte { 0x01 };
        FrameOpener opener { Key() };
        CHECK_FALSE(opener.Open(frame.header, frame.payload, changed));
    }
}

TEST_CASE("A frame sealed under one session's key opens under no other", "[core][session-seal]")
{
    auto const frame = FrameNumber(4);
    FrameSealer sealer { Key() };
    auto const tag = sealer.Seal(frame.header, frame.payload);

    // Each input to the derivation, changed alone: a key that ignored one of them would be the
    // same key for two exchanges, and a frame recorded in one would open in the other.
    SECTION("another shared secret")
    {
        FrameOpener opener { Key("another shared secret") };
        CHECK_FALSE(opener.Open(frame.header, frame.payload, tag));
    }

    SECTION("another salt")
    {
        auto other = DeriveSessionKey(Bytes("a shared secret"), Bytes("other nonces"), Bytes("a wire"));
        REQUIRE(other.has_value());
        FrameOpener opener { *std::move(other) };
        CHECK_FALSE(opener.Open(frame.header, frame.payload, tag));
    }

    SECTION("another wire's info")
    {
        FrameOpener opener { Key("a shared secret", "another wire") };
        CHECK_FALSE(opener.Open(frame.header, frame.payload, tag));
    }

    SECTION("the control: the same inputs derive the same key")
    {
        FrameOpener opener { Key() };
        CHECK(opener.Open(frame.header, frame.payload, tag));
    }
}

TEST_CASE("A session key is one HKDF-SHA256 block, and a tag one HMAC-SHA256", "[core][session-seal]")
{
    CHECK(Key().Bytes().size() == SessionKeyBytes);
    CHECK(SessionKeyBytes == 32);
    CHECK(SessionTagBytes == 32);
}
