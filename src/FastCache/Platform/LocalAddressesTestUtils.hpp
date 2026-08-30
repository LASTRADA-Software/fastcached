// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Platform/LocalAddresses.hpp>

#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/// Shared helper for tests that need a machine with addresses of their own. Header-
/// only so each test translation unit gets its own `inline` copy without an extra
/// link target, the same arrangement as `Platform/EnvironmentTestUtils.hpp`.
namespace FastCache::Testing
{

/// An `IHostAddressSource` answering whatever a test says this machine has.
///
/// The whole reason `IHostAddressSource` is a seam: the rules worth being wrong
/// about — when the set is refreshed, what an empty answer means, whether an
/// IPv4-mapped spelling folds — are unassertable against a real kernel, because a
/// test asserting them would be asserting about whichever machine the suite ran on.
///
/// It also **counts its calls**, and that is not incidental. "Refreshed on an
/// interval, never on a miss" is a claim about how often the platform is asked, so a
/// test that could not see the probe count could not tell the amplifier bug from the
/// fix — both answer every question correctly.
class ScriptedHostAddresses final: public IHostAddressSource
{
  public:
    /// @param addresses What this machine answers on, to begin with.
    explicit ScriptedHostAddresses(std::vector<std::string> addresses = {}):
        _addresses { std::move(addresses) }
    {
    }

    /// @copydoc IHostAddressSource::Addresses
    [[nodiscard]] std::vector<std::string> Addresses() const override
    {
        std::scoped_lock const guard { _mutex };
        ++_calls;
        return _addresses;
    }

    /// Give the machine a different set, as an interface being reconfigured would.
    /// @param addresses The new set.
    void Publish(std::vector<std::string> addresses)
    {
        std::scoped_lock const guard { _mutex };
        _addresses = std::move(addresses);
    }

    /// How many times the platform has been asked since construction.
    [[nodiscard]] std::size_t Calls() const
    {
        std::scoped_lock const guard { _mutex };
        return _calls;
    }

  private:
    /// Mutable so `Addresses()` can stay `const` while counting; the oracle holds
    /// this by const reference, which is the shape production code has.
    mutable std::mutex _mutex;
    mutable std::size_t _calls { 0 };
    std::vector<std::string> _addresses;
};

} // namespace FastCache::Testing
