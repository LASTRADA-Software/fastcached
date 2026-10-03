// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ProvenFleetSummary.hpp>
#include <FastCache/Cluster/SeedSources.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

/// @file ProvenFleet.hpp
/// Another fleet as this node came to know it, and the two seams discovery reaches formation
/// through: what this node says about itself, and where a proven fleet of another cluster goes.
namespace FastCache::Cluster
{

/// How a proven fleet reached this node, in PREFERENCE order: a seed typed at install beats
/// anything a beacon shouts.
///
/// **Private**: never transmitted or persisted.
enum class FleetOrigin : std::uint8_t
{
    FleetSeedFlag, ///< A `--fleet-seed` the operator typed, asked with `FleetSummary`.
    Remembered,    ///< An endpoint this node remembered from a fleet it was in.
    DnsSrv,        ///< A seed a DNS SRV record named.
    Beacon,        ///< A discovery beacon, proven by the challenge after it.
    Last,          ///< Not an origin: the length of a table keyed by one.
};

/// Which origin a seed's answer carries, by the source that named the seed.
struct SeedOriginRow
{
    SeedSource source;  ///< The source this row describes.
    FleetOrigin origin; ///< The origin an answer from such a seed carries.
};

/// Every seed source's origin: one row per `SeedSource`, and no row can name `FleetOrigin::Beacon`,
/// which only a discovery proof carries (`ProvenFleet::FromBeaconProof`).
inline constexpr EnumTable<SeedSource, SeedOriginRow> SeedOriginTable { {
    { .source = SeedSource::Remembered, .origin = FleetOrigin::Remembered },
    { .source = SeedSource::FleetSeedFlag, .origin = FleetOrigin::FleetSeedFlag },
    { .source = SeedSource::DnsSrv, .origin = FleetOrigin::DnsSrv },
} };
static_assert(RowsInEnumeratorOrder(SeedOriginTable, &SeedOriginRow::source),
              "SeedOriginTable must hold one row per SeedSource, in enumerator order");
static_assert(std::ranges::none_of(SeedOriginTable,
                                   [](SeedOriginRow const& row) { return row.origin == FleetOrigin::Beacon; }),
              "a seed's answer never carries the origin only a discovery proof may");

/// A fleet summary whose signature verified under the key it carries, over a nonce this node
/// chose, and how it arrived.
///
/// Holds a `ProvenFleetSummary` rather than a bare `FleetSummary`, so the one way to hold one is
/// still a verified signature: a beacon's raw summary cannot be dressed up as a proven fleet.
/// Proven means the signer holds its key, not that what it says is true (`ProvenFleetSummary`).
///
/// **And the origin is not the holder's to choose**, because `FleetOrigin` is a preference order a
/// yield reads: code holding a beacon's proof must not be able to label it a seed the operator
/// typed. So there is no public constructor, only one factory per way of proving -- each beside the
/// verification it names -- and a seed's origin comes from `SeedOriginTable`, which cannot name
/// `Beacon`. What remains open is DEMOTION: `FromBeaconProof` takes any verified summary, so a
/// seed's answer can be handed on as a beacon's, the least preferred origin, which buys nothing.
class ProvenFleet
{
  public:
    /// A discovery proof's summary, already verified against the challenge this node issued.
    ///
    /// Takes the verified summary rather than verifying, because a proof is verified BEFORE its
    /// cluster is asked about -- a signature before any claim -- and only then is it known to be
    /// another fleet's.
    /// @param proven What `ProvenFleetSummary::Verify` returned.
    /// @return The proven fleet, carrying `FleetOrigin::Beacon`.
    [[nodiscard]] static ProvenFleet FromBeaconProof(ProvenFleetSummary proven)
    {
        return ProvenFleet { std::move(proven), FleetOrigin::Beacon };
    }

    /// A seed's `FleetSummary` answer, when its signature verifies under the key it carries over
    /// THIS nonce, which it spends.
    /// @param nonce The nonce this node drew and sent with the question (`ChallengeIssuer::IssueNonce`).
    /// @param reply What the seed answered.
    /// @param source Which source named that seed; its origin is `SeedOriginTable`'s.
    /// @return The proven fleet, or nullopt when the answer does not verify over @p nonce.
    [[nodiscard]] static std::optional<ProvenFleet> FromSeedAnswer(IssuedNonce nonce,
                                                                   CompileCacheWire::FleetSummaryReply const& reply,
                                                                   SeedSource source)
    {
        auto const origin = SeedOriginTable[static_cast<std::size_t>(source)].origin;
        return ProvenFleetSummary::VerifyAnswer(std::move(nonce), reply).transform([origin](ProvenFleetSummary proven) {
            return ProvenFleet { std::move(proven), origin };
        });
    }

    /// What the signer said, and under which key.
    /// @return The verified summary.
    [[nodiscard]] constexpr ProvenFleetSummary const& Proven() const noexcept
    {
        return _proven;
    }

    /// What the signer said.
    /// @return The summary, every field of it covered by the signature.
    [[nodiscard]] constexpr CompileCacheWire::FleetSummary const& Summary() const noexcept
    {
        return _proven.Summary();
    }

    /// Who signed it.
    /// @return The identity key the signature verified under.
    [[nodiscard]] constexpr Ed25519PublicKey const& Key() const noexcept
    {
        return _proven.Key();
    }

    /// How it reached this node.
    /// @return The origin its factory gave it.
    [[nodiscard]] constexpr FleetOrigin Origin() const noexcept
    {
        return _origin;
    }

    /// Field-wise equality, so a case compares what was handed on whole.
    [[nodiscard]] friend bool operator==(ProvenFleet const&, ProvenFleet const&) = default;

  private:
    /// Hold what a factory verified, with the origin that factory names.
    /// @param proven The verified summary.
    /// @param origin How it reached this node.
    ProvenFleet(ProvenFleetSummary proven, FleetOrigin origin):
        _proven { std::move(proven) },
        _origin { origin }
    {
    }

    ProvenFleetSummary _proven;
    FleetOrigin _origin;
};

// The factories are the only doors: a default member initializer added one day must not reopen the
// type, and an aggregate would let any holder choose the origin.
static_assert(!std::is_default_constructible_v<ProvenFleet>, "a ProvenFleet is made only by its factories");
static_assert(!std::is_aggregate_v<ProvenFleet>, "a ProvenFleet's origin is its factory's, never its holder's");
static_assert(!std::is_constructible_v<ProvenFleet, ProvenFleetSummary, FleetOrigin>,
              "a ProvenFleet's origin is its factory's, never its holder's");

/// What this node says about itself right now: the summary its beacon shouts, its proofs sign and
/// its `FleetSummary` answer carries.
///
/// ONE source for all three, so a beacon and the proof after it never describe this node
/// differently -- the endpoint a peer challenged must be the endpoint the answer signs. The
/// formation controller implements it; until it exists, the discovery tier answers from the
/// configuration.
class IFleetSummarySource
{
  public:
    IFleetSummarySource() = default;
    IFleetSummarySource(IFleetSummarySource const&) = delete;
    IFleetSummarySource& operator=(IFleetSummarySource const&) = delete;
    IFleetSummarySource(IFleetSummarySource&&) = delete;
    IFleetSummarySource& operator=(IFleetSummarySource&&) = delete;
    virtual ~IFleetSummarySource() = default;

    /// This node's summary, as it stands now.
    /// @return The summary.
    [[nodiscard]] virtual CompileCacheWire::FleetSummary Current() const = 0;
};

/// Where every authenticated discovery reply is handed: a fleet of ANOTHER cluster, proven by its
/// signature alone, and this node's OWN fleet, once the key the roster holds for the speaker proved it.
///
/// Another cluster's machine is never asked of the roster and never desired: nobody this node's
/// cluster admits. What it IS good for is formation -- a solitary node yields to it, an established
/// one says two fleets can see each other. This node's own fleet is told too, because hearing ANY
/// reply is the evidence that "no other fleet is visible" is a finding rather than deafness
/// (`ForeignFleetWatch`). **Every observer acting on other fleets tells the two apart by cluster id
/// ITSELF**, the guard folded into the operation -- never trusting a decorator in front of it to have
/// filtered, since nothing makes one stand there.
class IFleetObserver
{
  public:
    IFleetObserver() = default;
    IFleetObserver(IFleetObserver const&) = delete;
    IFleetObserver& operator=(IFleetObserver const&) = delete;
    IFleetObserver(IFleetObserver&&) = delete;
    IFleetObserver& operator=(IFleetObserver&&) = delete;
    virtual ~IFleetObserver() = default;

    /// Told a fleet proved its summary: another cluster's, or this node's own -- see the class comment.
    /// @param fleet What it proved, and how it arrived.
    virtual void OnFleetProven(ProvenFleet const& fleet) = 0;
};

/// The endpoints in @p summary a peer would dial and reach only ITSELF, in
/// `CompileCacheWire::DialledEndpointTexts`' order.
///
/// `localhost`, a name under `.localhost` or a loopback address resolve to the PEER itself on every
/// machine (`NamesOnlyThisMachine`), so a peer acting on one dials itself, confidently, with no error
/// at either end. Every endpoint a peer dials is asked, from the one list the decoder's dial rule
/// walks too. An EMPTY endpoint names nothing to dial -- a learner has no consensus endpoint -- and
/// is not this.
/// @param summary What this node would announce.
/// @return The offending endpoints; empty when there are none.
[[nodiscard]] inline std::vector<std::string_view> EndpointsOnlyThisMachine(CompileCacheWire::FleetSummary const& summary)
{
    auto found = std::vector<std::string_view> {};
    for (auto const text: CompileCacheWire::DialledEndpointTexts(summary))
        if (!text.empty() && NamesOnlyThisMachine(HostOfEndpoint(text)))
            found.emplace_back(text);
    return found;
}

/// Whether @p summary tells a peer to dial an address that reaches only the machine dialling it:
/// **what a node must never announce** -- in a beacon, in a proof, or in a `FleetSummary` answer.
/// @param summary What this node would announce.
/// @return True when `EndpointsOnlyThisMachine` names any.
[[nodiscard]] inline bool AnnouncesOnlyThisMachine(CompileCacheWire::FleetSummary const& summary)
{
    return !EndpointsOnlyThisMachine(summary).empty();
}

} // namespace FastCache::Cluster
