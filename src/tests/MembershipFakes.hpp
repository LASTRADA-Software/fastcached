// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Distributed/MembershipOracle.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Testing
{

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
    ///        parameter rather than the `FleetMemberList` the four copies hardcoded.
    ListedMembership(std::vector<std::string> members, Distributed::MembershipParticipant participant) noexcept:
        _members { std::move(members) },
        _participant { participant }
    {
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

    /// Stop admitting @p peer.
    ///
    /// Part of the union rather than of one caller: `LiveStatsResponder_test` needs a member to
    /// stop being one mid-case, because its subject is that a subscription is re-gated on
    /// EVERY tick rather than once at subscribe time.
    /// @param peer Who to remove.
    /// No opinion: a host list knows no keys, exactly as `Distributed::HostSetMembership` answers.
    /// A case about a PROVED identity composes this with a `Distributed::KeyRosterMembership`.
    [[nodiscard]] Distributed::MembershipDecision ExplainKey(ProvenIdentity const& /*proven*/) const override
    {
        return {};
    }

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
    FixedMembership(Distributed::Membership verdict, Distributed::MembershipParticipant participant) noexcept:
        _verdict { verdict },
        _participant { participant }
    {
    }

    /// @param peerAddress Ignored.
    /// @return The fixed verdict, attributed to the named route -- except `Outsider`, which
    ///         `DecidedBy` leaves unattributed even here, so no case can be the thing that
    ///         establishes the opposite convention.
    [[nodiscard]] Distributed::MembershipDecision Explain(std::string_view /*peerAddress*/) const override
    {
        return Distributed::DecidedBy(_verdict, _participant);
    }

    /// No opinion: this answers about ADDRESSES, one verdict for all of them. A case about a PROVED
    /// identity composes a `Distributed::KeyRosterMembership` beside it.
    [[nodiscard]] Distributed::MembershipDecision ExplainKey(ProvenIdentity const& /*proven*/) const override
    {
        return {};
    }

  private:
    Distributed::Membership _verdict;
    Distributed::MembershipParticipant _participant;
};

} // namespace FastCache::Testing
