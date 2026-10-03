// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Distributed/LeaseSigner.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/RaftPeerKeyFakes.hpp>

/// @file LeaseRosterFakes.hpp
/// The one test signer and the one test roster for lease grants (#178).
///
/// Shared for `RaftPeerKeyFakes.hpp`'s reason: every case that reaches for these asserts that
/// one machine's grant verifies and another's does not, so the facts a copy must get right --
/// which key a signer has, what a revocation leaves, what an expired roster answers -- are
/// exactly what a wrong copy would get wrong while every case went on passing.
namespace FastCache::Testing
{

/// The grant signer a test's scheduler signs with: @p machine's test key, as that member.
/// @param machine Which machine signs, and the id its grants name.
/// @return The signer.
[[nodiscard]] inline Distributed::KeyPairLeaseSigner TestLeaseSigner(std::string const& machine = "scheduler")
{
    return Distributed::KeyPairLeaseSigner { machine, TestKeyPair(machine) };
}

/// A lease roster a case states outright: the voters it names sign with their test keys, the
/// machines it revokes are revoked, and its standing is whatever the case last set.
///
/// Thread-safe, because the production rosters are: a case that revokes while a compile reads
/// must not be testing a race of its own.
class FixedLeaseRoster final: public Distributed::ILeaseRoster
{
  public:
    /// @param voters The members whose grants verify, each under its own test key.
    /// @param revoked The machines whose test keys the roster has revoked.
    explicit FixedLeaseRoster(std::vector<std::string> voters = { "scheduler" }, std::vector<std::string> revoked = {}):
        _voters { std::move(voters) },
        _revoked { std::move(revoked) }
    {
    }

    /// Admit @p machine as a member that is not a voter: `MachineKeysOf` answers its test key, and
    /// `KeysOf` still does not, since a grant is a voter's alone.
    /// @param machine The machine.
    void AdmitMachine(std::string machine)
    {
        std::scoped_lock const lock { _lock };
        _machines.push_back(std::move(machine));
    }

    /// Revoke @p machine: it leaves the voters and the admitted machines, and its key joins the
    /// revoked, as `Forget` does (#1555).
    /// @param machine The machine.
    void Revoke(std::string const& machine)
    {
        std::scoped_lock const lock { _lock };
        std::erase(_voters, machine);
        std::erase(_machines, machine);
        _revoked.push_back(machine);
    }

    /// Revoke @p machine's key and leave the machine admitted, so `MachineKeysOf` answers that key
    /// live AND revoked at once. What a roster that does not filter its live answer against its
    /// revocations says -- `StateLeaseRoster` relies on the applied state keeping the two apart --
    /// so a verifier that trusts `live` alone is testable against it.
    /// @param machine The machine.
    void RevokeKeyOnly(std::string machine)
    {
        std::scoped_lock const lock { _lock };
        _revoked.push_back(std::move(machine));
    }

    /// Set what `Read` answers from now on.
    /// @param standing The standing.
    void SetStanding(Distributed::RosterStanding standing)
    {
        std::scoped_lock const lock { _lock };
        _standing = standing;
    }

    [[nodiscard]] Distributed::LeaseSignerKeys KeysOf(std::string_view signer) const override
    {
        std::scoped_lock const lock { _lock };
        auto keys = Distributed::LeaseSignerKeys {};
        for (auto const& machine: _revoked)
            keys.revoked.push_back(TestKeyPair(machine).PublicKey());
        if (std::ranges::contains(_voters, signer))
            keys.live = TestKeyPair(std::string { signer }).PublicKey();
        return keys;
    }

    [[nodiscard]] Distributed::RosterReading Read(std::chrono::system_clock::time_point /*now*/) const override
    {
        std::scoped_lock const lock { _lock };
        return Distributed::RosterReading { .standing = _standing };
    }

    [[nodiscard]] Distributed::LeaseSignerKeys MachineKeysOf(std::string_view machine) const override
    {
        std::scoped_lock const lock { _lock };
        auto keys = Distributed::LeaseSignerKeys {};
        for (auto const& revoked: _revoked)
            keys.revoked.push_back(TestKeyPair(revoked).PublicKey());
        if (std::ranges::contains(_voters, machine) || std::ranges::contains(_machines, machine))
            keys.live = TestKeyPair(std::string { machine }).PublicKey();
        return keys;
    }

  private:
    mutable std::mutex _lock;
    std::vector<std::string> _voters;
    std::vector<std::string> _machines; ///< Members admitted beside the voters, by `AdmitMachine`.
    std::vector<std::string> _revoked;
    Distributed::RosterStanding _standing { Distributed::RosterStanding::Current };
};

} // namespace FastCache::Testing
