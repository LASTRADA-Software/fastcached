// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Nonce.hpp>

#include <expected>
#include <optional>
#include <type_traits>
#include <utility>

/// @file ChallengeIssuer.hpp
/// A nonce this node drew for one question, as a type that only issuing produces and that one use
/// consumes.
///
/// An answer is worth something only against a nonce the asker chose and has not spent. A plain
/// `Nonce` says neither: any caller can build one with any bytes, and copy it to check a second
/// answer against. So what a fleet probe verifies an answer against is an `IssuedNonce` -- drawn by
/// `ChallengeIssuer` from the injected generator, never built by hand, never copied, and spent by
/// the verification that takes it, whatever that decides. Discovery's challenges are not issued
/// here: they are cookies, which no table holds (`ChallengeCookies`).
namespace FastCache::Cluster
{

class ChallengeIssuer;

/// A nonce this node drew for ONE question and has not yet checked an answer against: the
/// `FleetSummary` question's nonce.
///
/// **Move-only, and a move EMPTIES the source.** The nonce is held in an `optional` that a move
/// hands on and disengages, so the source holds nothing and is `Spent()`. A defaulted move would not
/// do that: an `optional`'s move leaves the source engaged, and the nonce is an array, whose move is
/// a copy -- so the source would keep a nonce able to verify a second answer. Emptiness rather than
/// a flag beside the nonce, so there is one state and not two to keep in step, and reading a spent
/// nonce is an access to an empty `optional`, which the analyser refuses unless the reader asked
/// first. **And only `ChallengeIssuer` makes one**, so an answer is verified only against a nonce
/// this process drew: a recorded nonce is bytes, and no bytes make an `IssuedNonce`, so a recorded
/// answer cannot be replayed against its recorded question. Consumed BY VALUE by
/// `ProvenFleetSummary::VerifyAnswer`, whatever it decides.
class IssuedNonce
{
  public:
    IssuedNonce(IssuedNonce const&) = delete;
    IssuedNonce& operator=(IssuedNonce const&) = delete;

    /// Take @p other's nonce, leaving it empty.
    /// @param other The nonce moved from.
    IssuedNonce(IssuedNonce&& other) noexcept:
        _nonce { std::exchange(other._nonce, std::nullopt) }
    {
    }

    /// Take @p other's nonce, leaving it empty.
    /// @param other The nonce moved from.
    /// @return This.
    IssuedNonce& operator=(IssuedNonce&& other) noexcept
    {
        _nonce = std::exchange(other._nonce, std::nullopt);
        return *this;
    }

    ~IssuedNonce() = default;

    /// The nonce, while it has not been spent.
    /// @return The nonce, or nullopt once spent.
    [[nodiscard]] std::optional<Nonce> const& Held() const noexcept
    {
        return _nonce;
    }

    /// Whether this nonce has been moved from, and so can verify nothing.
    /// @return True once spent.
    [[nodiscard]] bool Spent() const noexcept
    {
        return !_nonce.has_value();
    }

  private:
    friend class ChallengeIssuer;

    /// Hold a nonce `ChallengeIssuer` just drew.
    /// @param nonce The nonce.
    explicit IssuedNonce(Nonce const& nonce) noexcept:
        _nonce { nonce }
    {
    }

    std::optional<Nonce> _nonce;
};

// What makes a recorded answer worthless is that nothing but a draw makes one of these: no bytes
// convert, nothing default-constructs it, and nothing copies it.
static_assert(!std::is_constructible_v<IssuedNonce, Nonce>, "a recorded nonce must not make an IssuedNonce");
static_assert(!std::is_default_constructible_v<IssuedNonce>, "an IssuedNonce is drawn, never defaulted");
static_assert(!std::is_copy_constructible_v<IssuedNonce>, "an IssuedNonce verifies one answer");

/// What issuing a bare nonce produces: the bytes to send, and what the answer is verified against.
struct FreshNonce
{
    Nonce wire;       ///< The nonce as it goes on the wire, and as the answerer signs it.
    IssuedNonce held; ///< The one-use nonce the answer is verified against.
};

/// Where every nonce a fleet probe asks with comes from.
///
/// The one constructor of `IssuedNonce`: a fleet probe owns one and spends each nonce on the one
/// question it asked, so an answer is only ever checked against a nonce drawn here, from the
/// operating system's generator (#1527).
class ChallengeIssuer
{
  public:
    /// @param random Where nonces come from; must outlive this.
    explicit ChallengeIssuer(ISecureRandom& random) noexcept:
        _random { random }
    {
    }

    /// Draw a fresh nonce for one question.
    /// @return The nonce to send and the one to verify its answer against, or why none could be
    ///         drawn -- in which case none is issued, since a question with a weak nonce is one a
    ///         recorded answer could satisfy.
    [[nodiscard]] std::expected<FreshNonce, SecureRandomError> IssueNonce()
    {
        auto const nonce = DrawNonce(_random);
        if (!nonce.has_value())
            return std::unexpected { nonce.error() };
        return FreshNonce { .wire = *nonce, .held = IssuedNonce { *nonce } };
    }

  private:
    ISecureRandom& _random;
};

} // namespace FastCache::Cluster
