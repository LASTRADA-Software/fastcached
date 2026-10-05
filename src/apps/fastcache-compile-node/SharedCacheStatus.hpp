// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "SharedCacheDirectory.hpp"

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <string_view>

// Declared, not included: the status reads one string from it per call, and its header carries
// the whole worker protocol.
namespace FastCache::Cc
{
class IAdvertisedEndpointSource;
} // namespace FastCache::Cc

namespace FastCache::Node
{

class SharedCacheHost;

/// What `--node-status` reads about the fleet's shared cache from where this node stands.
class ISharedCacheStatusSource
{
  public:
    ISharedCacheStatusSource() = default;
    ISharedCacheStatusSource(ISharedCacheStatusSource const&) = delete;
    ISharedCacheStatusSource(ISharedCacheStatusSource&&) = delete;
    ISharedCacheStatusSource& operator=(ISharedCacheStatusSource const&) = delete;
    ISharedCacheStatusSource& operator=(ISharedCacheStatusSource&&) = delete;
    virtual ~ISharedCacheStatusSource() = default;

    /// Thread-safe. Reads where the shared cache is itself, ONCE.
    /// @return Which machine, reached where, and how the last attempt went.
    [[nodiscard]] virtual CompileCacheWire::SharedCacheStatusFields Report() const = 0;

    /// The same answer, judged against @p target rather than a target read here.
    ///
    /// **So a report composed of several parts reads the directory once.** Two reads a moment apart
    /// can straddle an apply, and a record assembled from both -- one part's source, another's verdict
    /// -- describes a state the cluster was never in. The composer reads, and hands the part its read.
    /// Thread-safe.
    /// @param target Where the directory said the shared cache is, read once by the caller.
    /// @return Which machine, reached where, and how the last attempt went.
    [[nodiscard]] virtual CompileCacheWire::SharedCacheStatusFields ReportFor(SharedCacheTarget const& target) const = 0;
};

/// What this node says about the fleet's shared cache, by where the answer comes from.
///
/// **Always an answer, and `None` says so**: an absent record is reserved for a sender too old to
/// carry one, so a node with no shared cache reports `none` rather than nothing.
///
/// - `None` -- nothing named and nothing overriding: not tried.
/// - `Override` -- `--upstream` is read through to instead, and the detail names the machine the
///   setting names, when it names one. Not tried: that leg proves nothing.
/// - `Setting` -- the fleet half's `ReportFor(target)`, which knows how its last attempt went, judged
///   against THIS target and never one it reads again. Without
///   one -- a node keeping no private tier, or holding no identity to prove itself with -- nothing
///   reads through to it, and that is said; a setting it could not use anyway is `Unresolved`.
/// - `ThisMachine` -- this node serves the tier at the endpoint it advertises: `Serving`, or
///   `Unavailable` with the host's reason, or with *not open yet* while the host has still to
///   reconcile the state that named this machine.
///
/// A `switch` over the source rather than a row table, deliberately: the enum is transmitted and
/// carries explicit values with no `Last`, so no `EnumTable` can hold it. A fifth source unhandled
/// here fails the GCC and Clang legs (`-Wall` implies `-Wswitch`, fatal under
/// `PEDANTIC_COMPILER_WERROR`) and the clang-tidy sweep (`clang-diagnostic-switch`, every warning an
/// error). The MSVC and clang-cl legs do NOT fail: neither gets `/WX` or `-Werror`
/// (`PedanticCompiler.cmake`), and cl's C4062 is off even at `/W4`. So a row table would guard no leg
/// more than this does, and one guard fewer than the switch.
/// @param target Where the directory says the shared cache is.
/// @param fleet The fleet half's status, or null where this node builds none.
/// @param host This machine's shared tier, or null where nothing hosts one.
/// @param selfEndpoint The `0xFC` endpoint this node advertises.
/// @return The record `--node-status` carries.
[[nodiscard]] CompileCacheWire::SharedCacheStatusFields SharedCacheStatusOf(SharedCacheTarget const& target,
                                                                            ISharedCacheStatusSource const* fleet,
                                                                            SharedCacheHost const* host,
                                                                            std::string_view selfEndpoint);

/// The production `ISharedCacheStatusSource` `--node-status` reads: `SharedCacheStatusOf` over the
/// directory, the fleet half, the host and the advertised endpoint, each asked per call so the
/// report follows an apply and a re-advertise as they happen.
class NodeSharedCacheStatus final: public ISharedCacheStatusSource
{
  public:
    /// @param targets Where the shared cache is; must outlive this.
    /// @param fleet The fleet half's status (`CacheTier::SharedCacheStatus()`), or null; must
    ///        outlive this.
    /// @param host This machine's shared tier, or null; must outlive this.
    /// @param advertised What this node advertises; must outlive this.
    NodeSharedCacheStatus(ISharedCacheTargetSource const& targets,
                          ISharedCacheStatusSource const* fleet,
                          SharedCacheHost const* host,
                          Cc::IAdvertisedEndpointSource const& advertised) noexcept;

    /// @copydoc ISharedCacheStatusSource::Report
    [[nodiscard]] CompileCacheWire::SharedCacheStatusFields Report() const override;

    /// @copydoc ISharedCacheStatusSource::ReportFor
    [[nodiscard]] CompileCacheWire::SharedCacheStatusFields ReportFor(SharedCacheTarget const& target) const override;

  private:
    ISharedCacheTargetSource const& _targets;
    ISharedCacheStatusSource const* _fleet;
    SharedCacheHost const* _host;
    Cc::IAdvertisedEndpointSource const& _advertised;
};

} // namespace FastCache::Node
