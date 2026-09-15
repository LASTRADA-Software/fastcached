// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterSigning.hpp>
#include <FastCache/Consensus/IRaftPeerCredential.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace FastCache::Cluster
{

/// Which signing domain one Raft peer MAC is made in.
struct RaftPeerMacDomain
{
    Consensus::RaftPeerMac purpose; ///< The MAC a Raft connection asks for.
    SigningDomain domain;           ///< The domain whose label it carries.
};

/// One row per `Consensus::RaftPeerMac`, in enumerator order.
inline constexpr EnumTable<Consensus::RaftPeerMac, RaftPeerMacDomain> RaftPeerMacDomains { {
    { .purpose = Consensus::RaftPeerMac::DiallerProof, .domain = SigningDomain::RaftPeerDialler },
    { .purpose = Consensus::RaftPeerMac::AcceptorVerdict, .domain = SigningDomain::RaftPeerVerdict },
    { .purpose = Consensus::RaftPeerMac::Frame, .domain = SigningDomain::RaftPeerFrame },
} };

static_assert(RowsInEnumeratorOrder(RaftPeerMacDomains, &RaftPeerMacDomain::purpose),
              "RaftPeerMacDomains must hold one row per RaftPeerMac, in enumerator order");

/// Whether no two Raft peer MACs share a signing domain.
///
/// The property `IRaftPeerCredential` promises, checked where it is decided. Two rows
/// naming one domain is the copied-row mistake `SigningLabelsSeparateDomains` exists
/// for, one level up: the labels would all still be distinct, and a dialler's proof
/// would verify as an acceptor's verdict.
/// @return True when every purpose has a domain of its own.
[[nodiscard]] consteval bool RaftPeerMacsSeparateDomains() noexcept
{
    std::array<SigningDomain, EnumeratorCount<Consensus::RaftPeerMac>> domains {};
    std::ranges::transform(RaftPeerMacDomains, domains.begin(), &RaftPeerMacDomain::domain);
    std::ranges::sort(domains);
    return std::ranges::adjacent_find(domains) == domains.end();
}

static_assert(RaftPeerMacsSeparateDomains(), "each RaftPeerMac needs a signing domain of its own");

/// The Raft peer wire's credential: the cluster's pre-shared key, through the one
/// construction that key has.
///
/// Holds the key for the life of the consensus tier. It is read once at startup, as
/// discovery and the lease validator read theirs: `--cluster-key-file` is not
/// reloadable, so rotating the key is a restart everywhere (#1308).
class PskRaftPeerCredential final: public Consensus::IRaftPeerCredential
{
  public:
    /// @param presharedKey The cluster key. Whether a short or empty one may sign is
    ///        refused before this is built (`ReadClusterKey`), for the reason
    ///        `ClusterSigning.hpp` gives: a MAC that refused its own key would give the
    ///        caller no way to say what it decided.
    explicit PskRaftPeerCredential(SecureByteBuffer presharedKey) noexcept:
        _presharedKey { std::move(presharedKey) }
    {
    }

    /// @copydoc Consensus::IRaftPeerCredential::Sign
    [[nodiscard]] Sha256::Digest Sign(Consensus::RaftPeerMac purpose, WireFields::FieldList fields) const override
    {
        return SignFields(_presharedKey, DomainOf(purpose), fields);
    }

    /// @copydoc Consensus::IRaftPeerCredential::Verify
    [[nodiscard]] bool Verify(Consensus::RaftPeerMac purpose,
                              WireFields::FieldList fields,
                              Sha256::Digest const& presented) const override
    {
        return VerifyFields(_presharedKey, DomainOf(purpose), fields, presented);
    }

  private:
    /// @param purpose A MAC a Raft connection asks for; never `Last`.
    /// @return The domain it is made in.
    [[nodiscard]] static constexpr SigningDomain DomainOf(Consensus::RaftPeerMac purpose) noexcept
    {
        return RaftPeerMacDomains[static_cast<std::size_t>(purpose)].domain;
    }

    SecureByteBuffer _presharedKey;
};

} // namespace FastCache::Cluster
