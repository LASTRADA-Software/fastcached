// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Protocol/ProvenIdentity.hpp>

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>

namespace FastCache::Testing
{

/// Refuse a route label a host-answering fake could not honestly carry.
///
/// **A fake more permissive than the real thing makes its cases pass while describing a route that
/// does not exist**, and nothing ever fails to find it. Two labels are such a fake by
/// construction: `Reserved`, which no decision can name, and `Loopback` on any host that is not
/// this machine -- loopback admits this machine and nobody else. A remote host a case must admit
/// by address is `OpenPolicy`'s, the one route that admits a remote host without a key.
/// @param participant The route the fake stands for.
/// @param admitted The hosts it answers `Member` for.
/// @throws std::invalid_argument naming the label and the host.
inline void RequireHonestLabel(Distributed::MembershipParticipant participant, std::span<std::string const> admitted)
{
    if (participant == Distributed::MembershipParticipant::Reserved)
        throw std::invalid_argument("a membership fake may not stand for the reserved participant");
    if (participant != Distributed::MembershipParticipant::Loopback)
        return;
    for (auto const& host: admitted)
        if (!IsLoopbackHost(host))
            throw std::invalid_argument(
                std::format("a membership fake labels {} Loopback, which admits only this machine; a remote host "
                            "admitted by address is OpenPolicy's",
                            host));
}

/// An oracle that admits whatever is in a host list.
///
/// **Shared because a fake is a helper too** (#1497). Four node test files carried a
/// byte-identical copy of this class and two more carried near-identical copies of
/// `FixedMembership` below, which is four and two places for one fake to be wrong -- and a
/// fake's bug presents as a GREEN test, so an assertion review never finds it. What made the
/// cost concrete is that #1471 gave each copy a FACT to get right: every copy now has to name
/// which admission route it stands for, and each is a separate chance to name the wrong one.
///
/// `src/tests/` rather than beside one of the six: this implements a *library* interface, so
/// `MembershipOracle_test.cpp` is a caller too and `src/FastCache/` must not include an app
/// header. Same argument as `src/tests/FleetHarness.hpp`.
class ListedMembership final: public Distributed::IMembershipOracle
{
  public:
    /// @param members Who may ask.
    /// @param participant Which route this fake stands for. **Required, and deliberately
    ///        undefaulted** -- the reason `MembershipOracle_test`'s own fake already gives: a
    ///        defaulted route is a fact the call site did not state, so a case asserting
    ///        *which participant decided* can compare an inherited answer against an
    ///        inherited answer and pass while attributing nothing. Consolidating these fakes
    ///        must not consolidate the fact each case states, which is why this is a
    ///        parameter rather than the route the four copies hardcoded.
    ///        Checked by `RequireHonestLabel`: `Loopback` lists this machine's spellings only.
    /// @throws std::invalid_argument for a label no production route could carry.
    ListedMembership(std::vector<std::string> members, Distributed::MembershipParticipant participant):
        _members { std::move(members) },
        _participant { participant }
    {
        RequireHonestLabel(_participant, _members);
    }

    /// @copydoc Distributed::IMembershipOracle::Explain
    ///
    /// Through `DecidedBy`, so a miss stays UNATTRIBUTED rather than claiming the named route
    /// refused a host it never mentioned (#1471).
    [[nodiscard]] Distributed::MembershipDecision Explain(std::string_view peerAddress) const override
    {
        return Distributed::DecidedBy(std::ranges::find(_members, peerAddress) != _members.end()
                                          ? Distributed::Membership::Member
                                          : Distributed::Membership::Outsider,
                                      _participant);
    }

    /// No opinion: a host list knows no keys. A case about a key a connection established
    /// composes this with a `Distributed::KeyRosterMembership`.
    [[nodiscard]] Distributed::MembershipDecision ExplainKey(ProvenIdentity const& /*identity*/,
                                                             Distributed::KeyEvidence /*evidence*/) const override
    {
        return {};
    }

    /// Stop admitting @p peer.
    ///
    /// Part of the union rather than of one caller: `LiveStatsResponder_test` needs a member to
    /// stop being one mid-case, because its subject is that a subscription is re-gated on
    /// EVERY tick rather than once at subscribe time.
    /// @param peer Who to remove.
    void Remove(std::string_view peer)
    {
        std::erase(_members, peer);
    }

  private:
    std::vector<std::string> _members;
    Distributed::MembershipParticipant _participant;
};

/// An oracle that answers one fixed verdict, whatever it is asked about.
///
/// A fake that could only answer `Member` or `Outsider` could not exercise the rules these
/// cases are for: every oracle in the tree derives its answer from a host list, and a list
/// cannot spell `Forgotten`. So a composite's FOLD, and a gate's choice of refusal row, are
/// both driven from here.
class FixedMembership final: public Distributed::IMembershipOracle
{
  public:
    /// @param verdict What every peer gets.
    /// @param participant Which route this fake stands for. **Required** for the reason above:
    ///        left defaulted, a case asserting *which participant decided* compares an
    ///        inherited answer against an inherited answer and passes while the fold
    ///        attributes nothing -- #1471's acceptance clause rendered vacuous by the fake
    ///        rather than by the assertion, which is where reading does not find it. Ask for a
    ///        route explicitly even where the case does not care which; that is a statement,
    ///        not a default.
    ///        Checked by `RequireHonestLabel`: a fixed verdict answers EVERY host, so `Loopback` may
    ///        carry only `Outsider` -- loopback's honest opinion of a host that is not this machine.
    /// @throws std::invalid_argument for a label no production route could carry.
    FixedMembership(Distributed::Membership verdict, Distributed::MembershipParticipant participant):
        _verdict { verdict },
        _participant { participant }
    {
        // Every host, so any host that is not this machine stands for the lot.
        if (_verdict != Distributed::Membership::Outsider)
            RequireHonestLabel(_participant, std::vector<std::string> { "10.0.0.1" });
        else if (_participant == Distributed::MembershipParticipant::Reserved)
            RequireHonestLabel(_participant, {});
    }

    /// @param peerAddress Ignored.
    /// @return The fixed verdict, attributed to the named route -- except `Outsider`, which
    ///         `DecidedBy` leaves unattributed even here, so no case can be the thing that
    ///         establishes the opposite convention.
    [[nodiscard]] Distributed::MembershipDecision Explain(std::string_view /*peerAddress*/) const override
    {
        return Distributed::DecidedBy(_verdict, _participant);
    }

    /// No opinion: this answers about ADDRESSES, one verdict for all of them. A case about a key a
    /// connection established composes a `Distributed::KeyRosterMembership` beside it.
    [[nodiscard]] Distributed::MembershipDecision ExplainKey(ProvenIdentity const& /*identity*/,
                                                             Distributed::KeyEvidence /*evidence*/) const override
    {
        return {};
    }

  private:
    Distributed::Membership _verdict;
    Distributed::MembershipParticipant _participant;
};

/// The identity @p id establishes -- by proof or by ticket -- under its shared test key.
/// @param id The machine.
/// @return Its id and key.
[[nodiscard]] inline ProvenIdentity IdentityOf(std::string const& id)
{
    return ProvenIdentity { .id = id, .key = TestKeyPair(id).PublicKey() };
}

/// The fold a node composes when it is not open -- this machine, and a key roster -- over REAL
/// oracles, holding the test keys of `live` and revoking those of `revoked`.
///
/// Not a fake: a case about which MACHINE is admitted by a key it proved or presented needs the
/// production participants, since a fake that answered `Member` for a key would prove only that
/// the fake does. Shared because several surfaces ask the same question -- a ticketed machine
/// admitted to compile, refused this machine's cache -- and each copy is a place to compose the
/// fold differently from `Node::NodeMembership`.
struct RosterFold
{
    Distributed::LoopbackMembership loopback;
    Distributed::KeyRosterMembership keys;
    /// Declared after the participants it borrows.
    Distributed::AnyOfMembership admitted { { &loopback, &keys } };

    /// @param live The ids whose test keys are live.
    /// @param revoked The ids whose test keys are revoked.
    explicit RosterFold(std::vector<std::string> const& live, std::vector<std::string> const& revoked = {})
    {
        std::map<std::string, Ed25519PublicKey, std::less<>> liveKeys;
        for (auto const& id: live)
            liveKeys.emplace(id, TestKeyPair(id).PublicKey());
        std::vector<Ed25519PublicKey> revokedKeys;
        revokedKeys.reserve(revoked.size());
        for (auto const& id: revoked)
            revokedKeys.push_back(TestKeyPair(id).PublicKey());
        keys.Publish(std::move(liveKeys), std::move(revokedKeys));
    }

    // The composite points into this object, so it is neither copied nor moved.
    RosterFold(RosterFold const&) = delete;
    RosterFold& operator=(RosterFold const&) = delete;
    RosterFold(RosterFold&&) = delete;
    RosterFold& operator=(RosterFold&&) = delete;
    ~RosterFold() = default;
};

/// A `--fleet-open` node's admission, composed as `Node::NodeMembership` composes it under the flag:
/// loopback, the open policy, and a key roster in which pc-07's test key is live.
///
/// Not a fake, for `RosterFold`'s reason: which ROUTE admitted a caller is exactly what an
/// operator's control verb asks (`IdentityRequirement::IdentifiedCaller`), and only the production
/// participants name their routes as production does. The four callers such a case needs are
/// spelled once here, so each file asks about the same connections.
struct OpenFleetFold
{
    Distributed::LoopbackMembership loopback;
    Distributed::OpenMembership open;
    Distributed::KeyRosterMembership keys;
    /// Declared after the participants it borrows.
    Distributed::AnyOfMembership admitted { { &loopback, &open, &keys } };

    /// The machine the roster holds.
    static constexpr std::string_view Machine = "pc-07";

    OpenFleetFold()
    {
        keys.Publish({ { std::string { Machine }, TestKeyPair(std::string { Machine }).PublicKey() } }, {});
    }

    /// @return A caller on another machine that showed nothing: admitted by `--fleet-open` alone.
    [[nodiscard]] static ConnectionFacts Anonymous()
    {
        return ConnectionFacts { .host = "203.0.113.9" };
    }

    /// @return The same address, presenting a verified ticket for the machine the roster holds.
    [[nodiscard]] static ConnectionFacts Ticketed()
    {
        return ConnectionFacts { .host = "203.0.113.9",
                                 .authenticatedMachine =
                                     ProvenIdentity { .id = std::string { Machine },
                                                      .key = TestKeyPair(std::string { Machine }).PublicKey() } };
    }

    /// @return The same address, having proved the key the roster holds.
    [[nodiscard]] static ConnectionFacts Proven()
    {
        return ConnectionFacts { .host = "203.0.113.9",
                                 .proven = ProvenIdentity { .id = std::string { Machine },
                                                            .key = TestKeyPair(std::string { Machine }).PublicKey() } };
    }

    /// @return A caller on this machine that showed nothing.
    [[nodiscard]] static ConnectionFacts Local()
    {
        return ConnectionFacts { .host = "127.0.0.1" };
    }

    // The composite points into this object, so it is neither copied nor moved.
    OpenFleetFold(OpenFleetFold const&) = delete;
    OpenFleetFold& operator=(OpenFleetFold const&) = delete;
    OpenFleetFold(OpenFleetFold&&) = delete;
    OpenFleetFold& operator=(OpenFleetFold&&) = delete;
    ~OpenFleetFold() = default;
};

} // namespace FastCache::Testing
