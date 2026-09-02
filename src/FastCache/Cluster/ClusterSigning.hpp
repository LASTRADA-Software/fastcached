// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

/// Everything the cluster's pre-shared key signs, and the one construction that
/// signs it.
///
/// ## Why this exists
///
/// One key MACs two different things -- a discovery proof and a lease token --
/// and each used to build its own message inline, from `HmacSha256` and
/// `WireFields::Encode`. Those are primitives, not a construction: what a message
/// is made of was written twice, and the rulebook's requirement that *every*
/// message carry a domain label was true of exactly one of them. The lease
/// carried `"fastcache-lease-v1"`; the discovery proof carried nothing.
///
/// The safety that held anyway was a property of the **pair**, not of either
/// construction. A discovery proof was a four-field encoding beginning with a
/// cluster id and a lease tag a two-field encoding beginning with a literal, so
/// no byte string was a valid message under both -- a coincidence that survives
/// exactly as long as nobody adds a field to discovery or adds a third signer
/// whose shape happens to collide. **Arity is not domain separation.**
///
/// So the label is not a constant each signer remembers to fold in. It is a
/// required parameter of the only function here that MACs anything, taken as a
/// `SigningDomain` rather than as a string: there is no argument to pass a bare
/// label to, no default to omit, and adding a signer is adding a row to
/// `SigningDomainTable` (#402).
///
/// ## What a caller still owns
///
/// Three things deliberately stay outside this seam, because each is per
/// protocol and getting them wrong here would be getting them wrong everywhere:
///
/// - **Which fields go into the message, and in what order.** A signer states its
///   own claim list; nothing here knows what a claim is.
/// - **Whether a weak or empty key may sign at all.** An empty HMAC key is a
///   perfectly valid HMAC key, so two nodes that both failed to load a key file
///   would happily authenticate each other's tags -- two machines agreeing on "no
///   secret" and calling it authentication. Both wires already refuse that, at
///   different layers and for different reasons: `ReadClusterKey` refuses a file
///   holding fewer than `MinimumKeyBytes` before a `DiscoveryConfig` is built, and
///   `AuthenticateLeaseToken` refuses an empty key at verify because a verifier
///   that legitimately runs without one must decide so in the open rather than by
///   omission. Neither belongs here: it is a policy about what a *deployment*
///   means, and a MAC that refused its own key would give a caller no way to say
///   what it decided.
/// - **When the MAC is checked relative to everything else.** It is checked
///   before any other claim is reported on, or a named refusal becomes an oracle
///   -- and that is an ordering property of a verifier, which this cannot enforce
///   for it.
///
/// This module performs no I/O, holds no state and reads no clock, so there is
/// nothing to inject: the same deliberate exception to the project's
/// dependency-injection rule that `WireFields` and `CompileCacheWire` document.
namespace FastCache::Cluster
{

/// Every construction the cluster's pre-shared key signs.
///
/// A closed set on purpose. The point of naming them is that a signer cannot be
/// added without appearing here, and cannot appear here without a label -- which
/// is the half of the rule that was previously written in prose and enforced
/// nowhere.
enum class SigningDomain : std::uint8_t
{
    /// The LAN handshake's proof of key possession. See `Cluster/DiscoveryWire`.
    DiscoveryProof = 0,

    /// The scheduler's signed grant. See `Distributed/LeaseToken`.
    LeaseToken,

    Last, ///< Not a domain, and has no row: the length of a table keyed by one.
};

/// What a domain is on the wire.
///
/// No member carries a default: a row that does not state its label is the
/// omission this whole header exists to make impossible.
struct SigningDomainDescriptor
{
    SigningDomain domain;   ///< The construction this row describes.
    std::string_view label; ///< The bytes folded in ahead of every field.
};

/// One row per `SigningDomain`, in enumerator order.
///
/// The labels are versioned because they are covered by the MAC, so changing one
/// retires every outstanding tag in that domain -- which is a deliberate, stated
/// act rather than something to discover.
///
/// `fastcache-lease-v1` is the label the lease token already carried, spelled
/// identically, so moving that call site onto this seam changes no byte a lease
/// authenticates. `fastcache-discovery-v1` is new: the discovery proof had no
/// label at all, so its message -- and therefore every proof tag -- changes.
inline constexpr EnumTable<SigningDomain, SigningDomainDescriptor> SigningDomainTable { {
    { .domain = SigningDomain::DiscoveryProof, .label = "fastcache-discovery-v1" },
    { .domain = SigningDomain::LeaseToken, .label = "fastcache-lease-v1" },
} };

static_assert(RowsInEnumeratorOrder(SigningDomainTable, &SigningDomainDescriptor::domain),
              "SigningDomainTable must hold one row per SigningDomain, in enumerator order");

/// Whether every label is present and no two are the same.
///
/// Both halves are load-bearing and neither implies the other. An empty label
/// encodes as a zero-length field, which separates one domain from another
/// exactly as well as no label did -- that is the state this header was written
/// to leave. Two domains sharing a label is the same hole reached by a copied
/// row, and is the likeliest way to reintroduce it: a new signer added by
/// duplicating the line above it and changing only the enumerator.
/// @return True when the table can actually separate its domains.
[[nodiscard]] consteval bool SigningLabelsSeparateDomains() noexcept
{
    if (std::ranges::any_of(SigningDomainTable, [](SigningDomainDescriptor const& row) { return row.label.empty(); }))
        return false;

    std::array<std::string_view, EnumeratorCount<SigningDomain>> labels {};
    std::ranges::transform(SigningDomainTable, labels.begin(), &SigningDomainDescriptor::label);
    std::ranges::sort(labels);
    return std::ranges::adjacent_find(labels) == labels.end();
}

static_assert(SigningLabelsSeparateDomains(),
              "every SigningDomain needs a label of its own: an empty or duplicated one separates nothing");

/// The row describing @p domain.
/// @param domain Which construction; never `SigningDomain::Last`, which has no row.
/// @return Its descriptor.
[[nodiscard]] constexpr SigningDomainDescriptor const& DescribeSigningDomain(SigningDomain domain) noexcept
{
    return SigningDomainTable[static_cast<std::size_t>(domain)];
}

/// The tag a holder of @p key must produce for @p fields in @p domain.
///
/// The message is `[domain label][field]...` in this project's length-prefixed
/// field grammar, so the label is a field like any other rather than a prefix
/// glued onto the first one. That distinction is the same one the claim fields
/// already rest on: a separator that can occur inside a value is not a framing,
/// and an endpoint is `host:port`, so joining would put the separator *inside* a
/// value every time. With the label length-prefixed, no choice of first field can
/// shift bytes across the boundary and impersonate a different domain.
///
/// @param key The cluster's pre-shared key. An empty key signs perfectly well and
///        authenticates nothing meaningful; refusing one is the caller's, and is
///        documented at the top of this file.
/// @param domain Which construction this message belongs to.
/// @param fields The message's fields, in wire order, without the label.
/// @return The expected tag.
[[nodiscard]] inline Sha256::Digest SignFields(std::span<std::byte const> key,
                                               SigningDomain domain,
                                               WireFields::FieldList fields)
{
    std::vector<std::span<std::byte const>> labelled;
    labelled.reserve(fields.size() + 1);
    labelled.push_back(WireFields::AsBytes(DescribeSigningDomain(domain).label));
    labelled.insert(labelled.end(), fields.begin(), fields.end());

    return HmacSha256(key, WireFields::Encode(WireFields::FieldList { labelled }));
}

/// The tag a holder of @p key must produce for a field list written out inline.
/// @param key The cluster's pre-shared key.
/// @param domain Which construction this message belongs to.
/// @param fields The message's fields, in wire order, without the label.
/// @return The expected tag.
[[nodiscard]] inline Sha256::Digest SignFields(std::span<std::byte const> key,
                                               SigningDomain domain,
                                               std::initializer_list<std::span<std::byte const>> fields)
{
    return SignFields(key, domain, WireFields::AsFields(fields));
}

/// Whether @p presented is the tag @p fields carry in @p domain.
///
/// A function rather than leaving the comparison to each verifier, and the reason
/// is `ConstantTimeEquals`: `==` on two digests stops at the first difference, so
/// its timing tells a caller who can retry how many leading bytes a guess got
/// right and lets them recover a tag one byte at a time instead of guessing all
/// 32.
///
/// **What that does and does not guarantee.** This module exposes no other
/// comparison, and each wire's verifier goes through it -- `VerifyLeaseToken` via
/// `AuthenticateLeaseToken`, and `DiscoveryService` via
/// `DiscoveryWire::VerifyProofTag`. It does NOT make a hand-rolled comparison
/// impossible: signing entry points are public because minting is a separate act,
/// so a future caller could take a tag from one and compare it itself. That is
/// the residual, and it is smaller than it was rather than gone. `ctest -R
/// psk-signing-seam` covers the other half -- a second construction -- and does
/// not cover this one, because a legitimately obtained tag compared with the
/// wrong operator is not a call to the primitive.
///
/// @param key The cluster's pre-shared key.
/// @param domain Which construction this message belongs to.
/// @param fields The message's fields, in wire order, without the label.
/// @param presented The tag the peer sent.
/// @return True when it authenticates.
[[nodiscard]] inline bool VerifyFields(std::span<std::byte const> key,
                                       SigningDomain domain,
                                       WireFields::FieldList fields,
                                       Sha256::Digest const& presented)
{
    return ConstantTimeEquals(SignFields(key, domain, fields), presented);
}

/// Whether @p presented is the tag a field list written out inline carries.
/// @param key The cluster's pre-shared key.
/// @param domain Which construction this message belongs to.
/// @param fields The message's fields, in wire order, without the label.
/// @param presented The tag the peer sent.
/// @return True when it authenticates.
[[nodiscard]] inline bool VerifyFields(std::span<std::byte const> key,
                                       SigningDomain domain,
                                       std::initializer_list<std::span<std::byte const>> fields,
                                       Sha256::Digest const& presented)
{
    return VerifyFields(key, domain, WireFields::AsFields(fields), presented);
}

} // namespace FastCache::Cluster
