// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Platform/ServiceControl.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace FastCache::Node
{

/// Everything this worker was told to be.
///
/// In a header rather than `main.cpp`'s anonymous namespace so that the things
/// derived FROM it can be tested. `main.cpp` is in no test target -- the lesson
/// `CacheProtocol.cpp`, `RootReconciler.cpp` and `AdminEndpoint.cpp` were each
/// extracted for -- and what is derived here is a *service registration*, where
/// a value that cannot survive its own parser produces a worker that registers
/// cleanly and then never starts again.
struct NodeConfig
{
    std::string scheduler; ///< host:port of the scheduler's dispatch endpoint.
    std::string advertise; ///< host:port clients should reach this worker on.
    std::string bindAddress { "0.0.0.0" };
    std::uint16_t port { 6676 };

    /// fingerprint=compilerPath, repeatable. A worker with none serves nothing,
    /// which is deliberate: there is no default compiler, because a default is how
    /// a job ends up running against something nobody chose.
    std::vector<std::string> toolchains;
    std::uint32_t slots { 0 }; ///< 0 means "one per hardware thread".

    /// Where the admin endpoint listens, or empty to leave it off.
    ///
    /// One string rather than the daemon's address/port/enabled triple, because a
    /// worker has one reason to want this and an empty value is already the "off"
    /// it would otherwise need a flag for. Defaults to off, and to loopback when a
    /// bare port is given: a scrape endpoint reachable from the network is the
    /// operator's decision, not this program's.
    std::string adminListen;

    std::string token;
    std::string user;
    LogLevel logLevel { LogLevel::Info };

    /// The name the platform's supervisor keys this worker's registration on.
    ///
    /// Distinct from the daemon's `FastCached` by default, because the two are
    /// separate services that a machine may well run both of -- sharing a name
    /// would make installing one silently displace the other.
    std::string serviceName { "FastCacheCompileNode" };

    /// Which supervisor domain `--install-service` registers into.
    ServiceScope serviceScope { ServiceScope::System };

    /// Where a POSIX daemonized run writes its pid, empty for none.
    std::string pidfile;

    bool daemon { false };           ///< Fork into the background / run under the SCM.
    bool installService { false };   ///< Register with the platform's supervisor and exit.
    bool uninstallService { false }; ///< Remove that registration and exit.
    bool help { false };
    bool version { false };
};

/// Every accepted option, one row each.
///
/// The same table idiom the daemon and the launcher use, so an accepted spelling
/// is necessarily a documented one and adding a flag is adding a row.
/// @return The table; stable for the life of the process.
[[nodiscard]] std::span<OptionSpec<NodeConfig> const> NodeOptions() noexcept;

/// Describe this worker as a service to register.
///
/// The worker's half of the `ServiceSpec` seam, mirroring the daemon's
/// `MakeDaemonServiceSpec`. Hand-written for the reason that one is: an
/// `OptionSpec` says how to PARSE a flag and carries no way to read a value back
/// out, so "emit every field that differs from its default" cannot be written
/// once generically. `NodeConfig_test` walks `NodeOptions()` and requires every
/// non-excluded row to be emitted, which is what keeps this from drifting.
///
/// `--requirepass` is never emitted, for the reason it is never emitted for the
/// daemon: a supervisor records its launch arguments where every local account
/// can read them.
/// @param exePath Absolute path to the fastcache-compile-node executable.
/// @param cfg Effective configuration to embed in the launch arguments.
/// @return The spec a supervisor is registered from.
[[nodiscard]] ServiceSpec MakeNodeServiceSpec(std::filesystem::path const& exePath, NodeConfig const& cfg);

/// Why @p cfg must not be registered as a service, if it must not.
///
/// Install-time rules that are the WORKER's rather than the platform's, checked
/// alongside `ServiceRegistrationRejection`. Every one of them describes a
/// registration that would succeed and then produce a service which cannot do
/// its job -- which is the worst shape this system has, because the failure is
/// silent from both ends: the operator is told the service was installed, and a
/// scheduler that leases the worker out sees clients fail to reach it with no
/// error anywhere.
///
/// `--advertise` is the one worth spelling out. Left empty it defaults to
/// `{--bind}:{--port}`, and `--bind` defaults to `0.0.0.0`, which is not an
/// address any client can dial. A worker registered that way registers happily,
/// heartbeats happily, is leased, and never answers.
/// @param cfg Configuration about to be baked into a registration.
/// @return An explanatory message when the install must be refused, else nullopt.
[[nodiscard]] std::optional<std::string> NodeServiceRejection(NodeConfig const& cfg);

/// Render the usage text from the same rows the parser matches.
/// @param color Whether to emit ANSI colour.
/// @return The complete help text.
[[nodiscard]] std::string HelpText(UsageColor color);

} // namespace FastCache::Node
