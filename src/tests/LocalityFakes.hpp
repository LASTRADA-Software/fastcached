// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Platform/LocalAddresses.hpp>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Testing
{

/// @file LocalityFakes.hpp
/// The one stated `ILocalityOracle`.
///
/// Shared rather than written per file, for `NodeProofFakes.hpp`'s reason: every case that reaches
/// for this asserts WHICH question a component puts to the locality seam -- is this caller on this
/// machine, is this audience one of mine -- and a copy that answered another host, or matched by
/// prefix, would make those cases pass for the wrong reason.

/// Answers `IsThisMachine` for exactly the hosts a case names.
///
/// A stand-in rather than `CachedLocalityOracle`, because what these cases ask is which question
/// is put, not how this machine's addresses are found. Matched WHOLE, never by prefix.
class ThisMachineIs final: public ILocalityOracle
{
  public:
    /// @param hosts The hosts that are this machine, spelled as a caller's address would be.
    ThisMachineIs(std::initializer_list<std::string_view> hosts):
        _hosts { hosts.begin(), hosts.end() }
    {
    }

    [[nodiscard]] bool IsThisMachine(std::string_view host) const override
    {
        return std::ranges::contains(_hosts, host);
    }

  private:
    std::vector<std::string> _hosts;
};

} // namespace FastCache::Testing
