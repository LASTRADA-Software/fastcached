// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Platform/HostNaming.hpp>
#include <FastCache/Platform/SrvResolver.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file HostNamingFakes.hpp
/// The one scripted `IHostNaming`, the one scripted `ISrvResolver` and the one scripted
/// `IConfigPathProbe` a node's defaults are derived through.
///
/// Shared rather than written per file: every case that reaches for these asserts what a node
/// DOES with a name or a lookup's answer, and a second copy that answered an unknown name
/// differently would make those cases pass for the wrong reason.
namespace FastCache::Testing
{

/// An `IHostNaming` that answers the names it was built with.
class ScriptedHostNaming final: public IHostNaming
{
  public:
    /// @param fqdn     What `FullyQualifiedName` answers.
    /// @param suffix   What `PrimaryDnsSuffix` answers.
    /// @param declined What `DeclinedName` answers; nothing by default.
    ScriptedHostNaming(std::string fqdn, std::string suffix, std::string declined = {}):
        _fqdn { std::move(fqdn) },
        _suffix { std::move(suffix) },
        _declined { std::move(declined) }
    {
    }

    [[nodiscard]] std::string FullyQualifiedName() const override
    {
        return _fqdn;
    }
    [[nodiscard]] std::string PrimaryDnsSuffix() const override
    {
        return _suffix;
    }
    [[nodiscard]] std::string DeclinedName() const override
    {
        return _declined;
    }

  private:
    std::string _fqdn;
    std::string _suffix;
    std::string _declined;
};

/// An `IConfigPathProbe` that answers the environment it was built with.
///
/// Nothing is a readable file and nothing is a trusted location: a node's defaults are derived
/// from the environment and the privilege alone, and a case that needs the disk asks for it by
/// writing its own probe rather than by this one guessing.
class ScriptedConfigPathProbe final: public IConfigPathProbe
{
  public:
    /// Whether the process the probe stands for is privileged -- named, so a call site says which
    /// half of `DefaultNodeClusterDirectory` it drives rather than passing a bare `bool`.
    enum class Privilege : std::uint8_t
    {
        Unprivileged, ///< A process somebody started by hand.
        Privileged,   ///< Root, an elevated administrator, or a service.
    };

    /// @param environment The variables `GetEnv` answers; any other name is unset.
    /// @param privilege What `IsPrivilegedProcess` answers.
    ScriptedConfigPathProbe(std::map<std::string, std::string, std::less<>> environment, Privilege privilege):
        _environment { std::move(environment) },
        _privilege { privilege }
    {
    }

    [[nodiscard]] std::optional<std::string> GetEnv(std::string_view name) const override
    {
        auto const found = _environment.find(name);
        if (found == _environment.end())
            return std::nullopt;
        return found->second;
    }
    [[nodiscard]] bool IsReadableFile(std::filesystem::path const& /*path*/) const override
    {
        return false;
    }
    [[nodiscard]] bool IsTrustedSystemLocation(std::filesystem::path const& /*path*/) const override
    {
        return false;
    }
    [[nodiscard]] bool IsPrivilegedProcess() const override
    {
        return _privilege == Privilege::Privileged;
    }

  private:
    std::map<std::string, std::string, std::less<>> _environment;
    Privilege _privilege;
};

/// The probe a service registration is built with in a case: an elevated installer whose
/// environment names the Windows machine-wide base, so the machine-wide state directory resolves
/// alike on every platform -- `C:\ProgramData\fastcache-node` or the POSIX row's own path --
/// and never from the real environment of the machine running the test.
/// @return The probe.
[[nodiscard]] inline ScriptedConfigPathProbe InstallerPathProbe()
{
    return ScriptedConfigPathProbe { { { "ProgramData", R"(C:\ProgramData)" } },
                                     ScriptedConfigPathProbe::Privilege::Privileged };
}

/// An `ISrvResolver` that answers what it was scripted to, and counts how often it was asked.
///
/// A name nothing scripted is `NoSuchName`, as a real resolver answers a name that does not exist.
/// Counting is how a case asserts that a machine with no DNS domain asked NOTHING, which a
/// scripted fault cannot show.
class ScriptedSrvResolver final: public ISrvResolver
{
  public:
    /// Script (or re-script) the answer for one name.
    /// @param name   The name.
    /// @param answer What a lookup of it answers.
    void Answer(std::string name, std::expected<std::vector<SrvTarget>, SrvLookupFailure> answer)
    {
        std::scoped_lock const lock { _mutex };
        _answers.insert_or_assign(std::move(name), std::move(answer));
    }

    [[nodiscard]] std::expected<std::vector<SrvTarget>, SrvLookupFailure> Lookup(std::string_view name) const override
    {
        std::scoped_lock const lock { _mutex };
        ++_asked;
        auto const found = _answers.find(std::string { name });
        return found != _answers.end() ? found->second
                                       : std::expected<std::vector<SrvTarget>, SrvLookupFailure> { std::unexpected {
                                             SrvFailureOf(SrvLookupFault::NoSuchName) } };
    }

    /// @return How many lookups were asked, of any name.
    [[nodiscard]] std::size_t Asked() const
    {
        std::scoped_lock const lock { _mutex };
        return _asked;
    }

  private:
    mutable std::mutex _mutex;
    mutable std::size_t _asked { 0 };
    std::map<std::string, std::expected<std::vector<SrvTarget>, SrvLookupFailure>> _answers;
};

} // namespace FastCache::Testing
