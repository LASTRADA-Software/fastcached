// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ChallengeCookies.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

namespace FastCache::Cluster
{

namespace
{
    /// How many bytes of the nonce carry the MAC: the rest of it after the serial.
    constexpr std::size_t MacBytes = NonceBytes - ChallengeCookies::SerialBytes;

    // A truncated HMAC is a forgery bound of its width, so the width is a claim this pins: below
    // 128 bits a cookie would be the weakest thing a proof rests on.
    static_assert(MacBytes >= 16, "a cookie's MAC must stay at least 128 bits wide");
    static_assert(MacBytes <= std::tuple_size_v<Sha256::Digest>, "a cookie's MAC is a prefix of one HMAC");

    /// The cookie for @p serial under @p cookieKey, sent to @p target at @p host by a node of
    /// @p challengerCluster.
    ///
    /// Length-prefixed fields, label first, for the reason every construction here is: a separator
    /// that can occur inside a value is not a framing, and a label is what keeps this MAC from
    /// ever meaning anything another construction under the same key would.
    /// @param cookieKey The epoch's key.
    /// @param serial The cookie's serial.
    /// @param challengerCluster This node's cluster.
    /// @param target Who it was sent to.
    /// @param host The host it was sent to: where the beacon came from at issue, where the proof
    ///        came from at verification.
    /// @return The nonce: the serial, big-endian, then the MAC's prefix.
    [[nodiscard]] Nonce CookieFor(std::span<std::byte const> cookieKey,
                                  std::uint64_t serial,
                                  std::string_view challengerCluster,
                                  CompileCacheWire::FleetSummary const& target,
                                  std::string_view host)
    {
        auto const serialBytes = WireFields::ToBigEndian(serial);
        auto const message = WireFields::Encode({ WireFields::AsBytes(ChallengeCookies::CookieLabel),
                                                  std::span<std::byte const> { serialBytes },
                                                  WireFields::AsBytes(challengerCluster),
                                                  WireFields::AsBytes(target.clusterId),
                                                  WireFields::AsBytes(target.nodeId),
                                                  WireFields::AsBytes(target.raftEndpoint),
                                                  WireFields::AsBytes(host) });
        auto const mac = HmacSha256(cookieKey, message);
        Nonce cookie {};
        std::ranges::copy(serialBytes, cookie.begin());
        std::ranges::copy(std::span { mac }.first(MacBytes), cookie.begin() + ChallengeCookies::SerialBytes);
        return cookie;
    }

    /// Whether two cookies are equal, without leaking where they first differ.
    ///
    /// Every byte is folded in whatever the outcome, for `ConstantTimeEquals`' reason: the cookie
    /// arrives from anybody, and an early exit would let a sender who can retry learn the MAC a
    /// byte at a time. Not `ConstantTimeEquals` itself, which takes a whole digest.
    /// @param lhs One cookie.
    /// @param rhs The other.
    /// @return True when equal.
    [[nodiscard]] bool CookiesEqual(Nonce const& lhs, Nonce const& rhs) noexcept
    {
        auto difference = std::uint8_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, lhs.size()))
            difference |= std::to_integer<std::uint8_t>(lhs[index] ^ rhs[index]);
        return difference == 0;
    }
} // namespace

ChallengeCookies::ChallengeCookies(ISecureRandom& random, std::chrono::seconds lifetime) noexcept:
    _random { random },
    _lifetime { lifetime }
{
}

std::expected<ChallengeCookies::Epoch*, SecureRandomError> ChallengeCookies::IssuingEpoch(
    core::platform::SteadyTimePoint now)
{
    // A new key every half lifetime, so a cookie issued now is answerable for at least that long,
    // and early once the current epoch has issued `MaxEpochChallenges`, so every cookie it issued
    // has its place in its record.
    if (_current.has_value() && now - _current->startedAt < _lifetime / 2
        && _nextSerial - _current->firstSerial < MaxEpochChallenges)
        return &*_current;

    auto cookieKey = SecureByteBuffer(CookieKeyBytes);
    if (auto drawn = _random.Fill(cookieKey); !drawn.has_value())
        return std::unexpected { drawn.error() };
    _previous = std::move(_current);
    return &_current.emplace(Epoch { .cookieKey = std::move(cookieKey),
                                     .firstSerial = _nextSerial,
                                     .startedAt = now,
                                     .states = std::vector<std::uint64_t>(StateWords) });
}

std::expected<DiscoveryWire::Challenge, SecureRandomError> ChallengeCookies::Issue(
    core::platform::SteadyTimePoint now,
    std::string_view challengerCluster,
    CompileCacheWire::FleetSummary const& target,
    std::string_view destinationHost)
{
    auto const epoch = IssuingEpoch(now);
    if (!epoch.has_value())
        return std::unexpected { epoch.error() };

    auto const serial = _nextSerial++;
    return DiscoveryWire::Challenge { .clusterId = std::string { challengerCluster },
                                      .nonce = CookieFor(
                                          (*epoch)->cookieKey, serial, challengerCluster, target, destinationHost) };
}

std::expected<ChallengeCookies::Epoch*, CookieRefusal> ChallengeCookies::EpochOf(std::uint64_t serial) noexcept
{
    // A serial at or past the next one was never issued; nothing was issued before the first key.
    if (!_current.has_value() || serial >= _nextSerial)
        return std::unexpected { CookieRefusal::NotIssued };
    if (serial >= _current->firstSerial)
        return &*_current;
    if (_previous.has_value() && serial >= _previous->firstSerial)
        return &*_previous;
    return std::unexpected { CookieRefusal::Expired };
}

std::expected<ProvenFleetSummary, CookieRefusal> ChallengeCookies::Verify(core::platform::SteadyTimePoint now,
                                                                          std::string_view challengerCluster,
                                                                          DiscoveryWire::Proof const& proof,
                                                                          std::string_view sourceHost,
                                                                          std::function<bool()> const& mayCheckSignature)
{
    auto const serial = WireFields::FromBigEndian<std::uint64_t>(std::span { proof.answers }.first(SerialBytes));
    if (!serial.has_value())
        return std::unexpected { CookieRefusal::NotIssued };
    auto const epoch = EpochOf(*serial);
    if (!epoch.has_value())
        return std::unexpected { epoch.error() };
    auto& held = **epoch;

    // The cookie before anything else: one HMAC, and a proof naming another cluster, node or
    // endpoint than was challenged -- or arriving from another host than the challenge went to --
    // ends here, before the budget is asked and before a signature is spent on it.
    if (!CookiesEqual(CookieFor(held.cookieKey, *serial, challengerCluster, proof.summary, sourceHost), proof.answers))
        return std::unexpected { CookieRefusal::NotIssued };

    // Its window, then its state. Every cookie the epoch issued has a place in its record, so
    // there is no "full" to refuse on: what a cookie has been through is all that is asked.
    if (now - held.startedAt >= _lifetime)
        return std::unexpected { CookieRefusal::Expired };
    auto const offset = *serial - held.firstSerial;
    auto const state = StateOf(held, offset);
    if (state == CookieState::Spent)
        return std::unexpected { CookieRefusal::Replayed };
    if (state == CookieState::Exhausted)
        return std::unexpected { CookieRefusal::Exhausted };

    // Only now is a signature check worth asking for, and only now can one be refused without
    // harm: this cookie is live and unspent, so the answer to it can still arrive and be checked.
    if (!mayCheckSignature())
        return std::unexpected { CookieRefusal::Unchecked };

    // Spent only once the signature holds: a forgery proves nothing about the cookie, and is
    // counted instead -- one step toward exhausted, since below `Spent` the state IS that count.
    auto proven = ProvenFleetSummary::FromProof(
        DiscoveryWire::Challenge { .clusterId = std::string { challengerCluster }, .nonce = proof.answers }, proof);
    if (!proven.has_value())
    {
        SetState(held, offset, static_cast<CookieState>(std::to_underlying(state) + 1));
        return std::unexpected { CookieRefusal::Forged };
    }
    SetState(held, offset, CookieState::Spent);
    return *std::move(proven);
}

ChallengeCookies::CookieState ChallengeCookies::StateOf(Epoch const& epoch, std::uint64_t offset) noexcept
{
    auto const bit = offset * StateBits;
    auto const word = epoch.states[bit / WordBits];
    return static_cast<CookieState>((word >> (bit % WordBits)) & StateMask);
}

void ChallengeCookies::SetState(Epoch& epoch, std::uint64_t offset, CookieState state) noexcept
{
    auto const bit = offset * StateBits;
    auto& word = epoch.states[bit / WordBits];
    word = (word & ~(StateMask << (bit % WordBits)))
           | (static_cast<std::uint64_t>(std::to_underlying(state)) << (bit % WordBits));
}

std::size_t ChallengeCookies::SpentHeld() const noexcept
{
    auto const spentIn = [](std::optional<Epoch> const& epoch) -> std::size_t {
        if (!epoch.has_value())
            return 0;
        return static_cast<std::size_t>(
            std::ranges::count_if(std::views::iota(std::uint64_t { 0 }, std::uint64_t { MaxEpochChallenges }),
                                  [&epoch](std::uint64_t offset) { return StateOf(*epoch, offset) == CookieState::Spent; }));
    };
    return spentIn(_current) + spentIn(_previous);
}

} // namespace FastCache::Cluster
