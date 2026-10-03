// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Protocol/SealedFrameSocket.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <core/async/Task.hpp>

namespace FastCache::Node
{

/// @file NodeProofClient.hpp
/// The proving half of the node handshake: what a machine does on every connection it opens to a
/// scheduler or to the fleet's shared cache, before the first verb it came to send (#178).
///
/// ## Why before anything else
///
/// The verbs a joining machine sends -- `Register`, `NodeAnnounce`, `Heartbeat`, `Withdraw` --
/// require a proven identity (`CompileCacheWire::IdentityRequirement`), and what a proof
/// establishes is connection state. So a round that dials proves once, and every verb on that
/// connection is sent sealed, as the machine the cluster admitted.
///
/// ## It checks the SERVER too
///
/// The server signs first. A machine that holds a roster -- a consensus member, or a worker that
/// has adopted a certified one -- refuses to prove itself to a server whose key is not a live
/// voter's: a revoked ex-scheduler still named in its `--scheduler` gets nothing, not even a proof
/// it could relay. A machine with no roster yet takes the server's word for its key, and the seal
/// is what still stops a relay from injecting verbs on the connection the proof admitted.
///
/// The shared cache asks a different question of the same signature: not *is this a voter* but
/// *is this exactly the machine the fleet named, under exactly the key the roster holds for it*.
/// That is a second `IServerTrust`, `NamedMachineTrust`, and which standings a machine proves
/// itself to is one table, `ServerStandingTable`, whichever trust produced the standing.

/// What a caller knows about the server that signed a handshake.
///
/// **PRIVATE: persisted and transmitted nowhere**, so its enumerators carry no explicit values.
/// Seven answers, because the refusals are different diagnoses with different remedies, the
/// acceptances are not equally strong, and the two trusts ask different questions: a scheduler's
/// trust answers the first four, the shared cache's the last three.
enum class ServerStanding : std::uint8_t
{
    Voter,     ///< Its key is a live voter's in the roster this machine holds.
    Unchecked, ///< This machine holds no roster yet, so the key could be checked against nothing.
    NotVoter,  ///< The roster holds no such voter key: a learner, a stranger, or a stale address.
    Revoked,   ///< The roster REVOKED this key: the machine the cluster forgot.
    /// Exactly the machine this connection was opened to reach, under the key the roster holds
    /// for it: the shared-cache leg's only acceptance.
    Named,
    /// A valid signature by any OTHER identity at the endpoint the named machine advertised in the
    /// replicated state: that endpoint now reaches another machine, or the named machine's
    /// `--cluster-dir` was wiped and it came back under a new id, which the fleet's shared-cache
    /// setting must then name. Nothing in THIS node's configuration is the remedy.
    NotNamed,
    /// The named id under a key that is not the one the roster records for it: an impostor that
    /// typed the right name, or the named machine re-keyed and its new key not recorded yet. The
    /// roster keeps the recorded key until an operator records another, so waiting fixes nothing.
    NamedUnderOtherKey,
    Last, ///< The count, not a standing.
};

/// Whether this machine proves itself to a server of each standing, and what it says when it will not.
struct ServerStandingRow
{
    ServerStanding standing; ///< The standing this row describes.
    bool provesTo;           ///< Whether a proof may follow.
    /// Why not: `{0}` is the id the server claimed, `{1}` what the trust expected
    /// (`IServerTrust::Expected`). Empty exactly when `provesTo`.
    std::string_view refusal;
};

/// Which standings a proof may follow, whichever trust answered.
inline constexpr EnumTable<ServerStanding, ServerStandingRow> ServerStandingTable { {
    { .standing = ServerStanding::Voter, .provesTo = true, .refusal = {} },
    { .standing = ServerStanding::Unchecked, .provesTo = true, .refusal = {} },
    { .standing = ServerStanding::NotVoter,
      .provesTo = false,
      .refusal = "{0} answered where this node expected {1}, and signed with a key that is no voter's in the roster "
                 "this node holds, so this node proves nothing to it: a --scheduler that names a machine which is "
                 "not, or no longer, a voter" },
    { .standing = ServerStanding::Revoked,
      .provesTo = false,
      .refusal = "{0} answered where this node expected {1}, and signed with a key this cluster REVOKED: it is a "
                 "machine the cluster forgot, and this node proves nothing to it. Drop it from --scheduler" },
    { .standing = ServerStanding::Named, .provesTo = true, .refusal = {} },
    { .standing = ServerStanding::NotNamed,
      .provesTo = false,
      .refusal = "{0} answered at the endpoint {1} advertised, where this node expected {1}, and this node sends it "
                 "nothing: either that endpoint now reaches another machine, or {1}'s --cluster-dir was wiped and it "
                 "came back as {0}, in which case the fleet's shared-cache setting must name {0}" },
    { .standing = ServerStanding::NamedUnderOtherKey,
      .provesTo = false,
      .refusal = "{0} answered at the endpoint {1} advertised, as {1}, but signed with a key that is not the one the "
                 "roster records for {1}, and this node sends it nothing: an impostor, or {1} was re-keyed and its new "
                 "key is not recorded yet -- record it with --cluster-admit={1}=<endpoint>@<key>, or "
                 "--cluster-admit-worker={1}@<key> for a worker" },
} };
static_assert(RowsInEnumeratorOrder(ServerStandingTable, &ServerStandingRow::standing),
              "ServerStandingTable must hold one row per ServerStanding, in enumerator order");
static_assert(std::ranges::all_of(ServerStandingTable,
                                  [](ServerStandingRow const& row) { return row.provesTo == row.refusal.empty(); }),
              "a standing is proved to, or says why not -- never both, never neither");
static_assert(std::ranges::all_of(ServerStandingTable,
                                  [](ServerStandingRow const& row) {
                                      return row.provesTo || (row.refusal.contains("{0}") && row.refusal.contains("{1}"));
                                  }),
              "every refusal names both the server that answered, {0}, and what this node expected, {1}");

/// Whom this machine may prove itself to (#178).
///
/// A seam, because the answer is the roster this node holds -- the applied state on a consensus
/// member, a certified roster on a worker, nothing yet on a fresh one -- and that is read per
/// handshake, so a revocation reaches the next round rather than the next restart.
class IServerTrust
{
  public:
    IServerTrust() = default;
    IServerTrust(IServerTrust const&) = delete;
    IServerTrust(IServerTrust&&) = delete;
    IServerTrust& operator=(IServerTrust const&) = delete;
    IServerTrust& operator=(IServerTrust&&) = delete;
    virtual ~IServerTrust() = default;

    /// @param serverId The id the server claimed.
    /// @param serverKey The key its signature verified under.
    /// @return What this machine's roster says about it.
    [[nodiscard]] virtual ServerStanding StandingOf(std::string_view serverId, Ed25519PublicKey const& serverKey) const = 0;

    /// What this trust would accept, in the words a refusal puts beside the server that answered:
    /// a machine's id, or a kind of machine such as `a voter`. A refusal that named only the server
    /// would leave the operator to guess which of two opposite mistakes it was.
    /// @return The expectation; valid for as long as this trust.
    [[nodiscard]] virtual std::string_view Expected() const = 0;
};

/// Trust in exactly one machine: the shared cache the fleet named, under the key its roster holds.
///
/// Answers `Named`, `NotNamed` or `NamedUnderOtherKey` and nothing else -- never `Unchecked`, since a
/// named trust always has something to check against.
class NamedMachineTrust final: public IServerTrust
{
  public:
    /// @param machineId The node id of the machine a connection is opened to reach.
    /// @param key The identity key the roster holds for it.
    NamedMachineTrust(std::string machineId, Ed25519PublicKey key) noexcept:
        _machineId { std::move(machineId) },
        _key { key }
    {
    }

    /// @copydoc IServerTrust::StandingOf
    [[nodiscard]] ServerStanding StandingOf(std::string_view serverId, Ed25519PublicKey const& serverKey) const override
    {
        // BOTH halves: the id alone is a claim anybody can type, the key alone would accept another
        // fleet machine that happens to answer at a stale address. Which half failed is kept apart,
        // because a wrong machine and a wrong key are fixed in different places.
        if (serverId != _machineId)
            return ServerStanding::NotNamed;
        return serverKey == _key ? ServerStanding::Named : ServerStanding::NamedUnderOtherKey;
    }

    /// @copydoc IServerTrust::Expected
    [[nodiscard]] std::string_view Expected() const override
    {
        return _machineId;
    }

  private:
    std::string _machineId;
    Ed25519PublicKey _key;
};

/// What one attempt to prove this machine's identity over one connection learned.
///
/// **Four outcomes, and the reason is the rulebook's**: the four call for four different actions,
/// and a `bool` would fold the one an operator acts on into the others.
enum class NodeProofResult : std::uint8_t
{
    /// The server accepted the proof; the connection is sealed both ways from here on.
    Proved,
    /// The server serves no proof: it runs no consensus, so it is not a scheduler of this fleet.
    NotOffered,
    /// This machine refused to prove itself to the server: its key is not one it may trust.
    Untrusted,
    /// The server refused the proof, or the exchange did not complete.
    Refused,
};

/// What the attempt learned, and what to say about it.
struct NodeProofAttempt
{
    NodeProofResult result { NodeProofResult::Refused }; ///< What happened.

    /// The server's own words when it refused, or what this machine concluded about the server.
    /// Empty only for `Proved`.
    std::string reason {};

    /// What the trust said about the server: engaged whenever the server's signature verified and
    /// the trust was asked, so a caller can tell "proved ANOTHER key" apart from "did not complete".
    std::optional<ServerStanding> standing {};
};

/// This machine's identity, as it proves it on every connection to a scheduler.
///
/// Holds references: the key pair is read once at startup and lives for the process, and the
/// trust and the randomness are the node's own seams.
class NodeProofClient
{
  public:
    /// @param nodeId The id this machine minted into its `--cluster-dir`, as the cluster admitted it.
    /// @param key Its identity key pair; must outlive this.
    /// @param trust Whom it may prove itself to; must outlive this.
    /// @param random Where each handshake's nonce and ephemeral key come from; must outlive this.
    NodeProofClient(std::string nodeId, Ed25519KeyPair const& key, IServerTrust const& trust, ISecureRandom& random) noexcept
        :
        _nodeId { std::move(nodeId) },
        _key { key },
        _trust { trust },
        _random { random }
    {
    }

    /// Prove this machine's identity over @p peer, and seal it: the handshake itself.
    ///
    /// On `Proved` the socket is sealed both ways and every later exchange over it is too. On any
    /// other outcome nothing verb-worthy may follow on this connection: a receiving seal may be
    /// engaged already, and a server that refused the proof refuses every joining verb anyway.
    /// Runs on a reactor, over a reactor socket; `Prove` is this same coroutine run to completion.
    /// Every parameter is a pointer, never a reference, as for every coroutine here: a reference
    /// parameter is a borrow the signature does not show surviving a suspension.
    ///
    /// Presents no password: the proof IS this machine's credential, with a scheduler and with the
    /// shared cache alike (`Cc::ExchangeWithScheduler`'s rule).
    /// @param peer A fresh connection, wrapped so it can be sealed; not owned, and must outlive the
    ///        coroutine.
    /// @param trust Whom this machine may prove itself to on this connection; not owned, and must
    ///        outlive the coroutine.
    /// @return What was learned.
    [[nodiscard]] core::async::Task<NodeProofAttempt> ProveAsync(SealedFrameSocket* peer, IServerTrust const* trust) const;

    /// `ProveAsync` under the trust this client was built with, run to completion on this thread.
    ///
    /// For a thread that may block, over a blocking socket -- the presence round. Never from a
    /// reactor: over a reactor socket it would wait on the loop it is blocking.
    /// @param peer A fresh connection to a scheduler, wrapped so it can be sealed.
    /// @return What was learned.
    [[nodiscard]] NodeProofAttempt Prove(SealedFrameSocket& peer) const;

    /// @return The id this machine proves.
    [[nodiscard]] std::string_view NodeId() const noexcept
    {
        return _nodeId;
    }

  private:
    std::string _nodeId;
    Ed25519KeyPair const& _key;
    IServerTrust const& _trust;
    ISecureRandom& _random;
};

} // namespace FastCache::Node
