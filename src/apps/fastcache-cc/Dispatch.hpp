// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"

#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// Opens connections to endpoints named at runtime.
///
/// The library's `ISocket` is one *connected* peer, which was enough
/// while there was only ever one address to reach. Distribution talks to two —
/// the scheduler, and then whichever worker the scheduler names — so the act of
/// connecting becomes the thing that has to be injected. Tests hand out scripted
/// clients per endpoint and never open a socket.
class IEndpointDialer
{
  public:
    IEndpointDialer() = default;
    virtual ~IEndpointDialer() = default;
    IEndpointDialer(IEndpointDialer const&) = delete;
    IEndpointDialer& operator=(IEndpointDialer const&) = delete;
    IEndpointDialer(IEndpointDialer&&) = delete;
    IEndpointDialer& operator=(IEndpointDialer&&) = delete;

    /// Connect to `hostPort`.
    /// @param hostPort The endpoint, e.g. "10.0.0.7:6676".
    /// @return A connected client, or nullptr when it could not be reached.
    [[nodiscard]] virtual std::unique_ptr<ISocket> Dial(std::string_view hostPort) = 0;
};

/// How a dispatch attempt ended.
///
/// There is deliberately no "failed" outcome. Every way this can go wrong ends
/// with the caller compiling locally, because the client is holding the source and
/// has a working fallback — distribution must be incapable of breaking a build.
enum class DispatchStatus : std::uint8_t
{
    /// A worker ran the compiler. `exitCode` says what it thought of the code, and
    /// may be non-zero: that is a *successful dispatch* of a failing compile.
    Compiled,
    /// The scheduler declined, or this command line was not dispatchable. Ordinary,
    /// and the reason is in `detail`.
    Declined,
    /// The scheduler or the worker could not be reached, or the exchange broke.
    Unavailable,
};

/// The result of one dispatch attempt.
struct DispatchResult
{
    DispatchStatus status { DispatchStatus::Unavailable };
    int exitCode { 0 };            ///< The remote compiler's exit code (Compiled only).
    std::vector<std::byte> object; ///< The compiled object, already decoded.
    std::string stdoutText;        ///< The remote compiler's stdout.
    std::string stderrText;        ///< The remote compiler's stderr.
    std::string detail;            ///< Why it was declined or unavailable; empty on success.
    std::string workerEndpoint;    ///< Which worker ran it, for diagnostics.

    /// @return True when a worker actually ran the compiler.
    [[nodiscard]] bool Ran() const noexcept
    {
        return status == DispatchStatus::Compiled;
    }
};

/// Everything one dispatch needs.
struct DispatchRequest
{
    std::string_view schedulerEndpoint; ///< Where to ask for a worker.
    std::string_view fingerprint;       ///< This client's toolchain identity.
    std::string_view objectKey;         ///< The cache key, for duplicate suppression.
    std::span<std::string const> args;  ///< Already filtered by `RemoteCompileArgs`.
    std::string_view preprocessed;      ///< The translation unit, preprocessed.
    /// The translation unit's path, as the build system spelled it. Only its base
    /// name travels -- see `Dispatch`, which takes it -- because that is what a
    /// compiler records in the object and the worker has no use for the rest.
    std::string_view sourceName;
};

/// Ask the scheduler for a worker and have it compile this translation unit.
///
/// Three exchanges, each a short request/reply on a fresh connection: a `Lease` to
/// the scheduler, a `Compile` to whichever worker it named, and a `Release` back to
/// the scheduler saying the job is over. The client never waits in a queue — a
/// scheduler with nothing free refuses immediately, and the caller compiles
/// locally. That is not a fallback bolted on afterwards; it is why the scheduler is
/// allowed to refuse at all.
///
/// **The release is not optional and not the caller's to remember.** A lease
/// suppresses every other client's attempt at the same key, so one that is never
/// resolved pins that key for the scheduler's whole lease timeout — ten minutes by
/// default (#212). It therefore happens here, on every path out of the compile,
/// rather than being handed back for the caller to do: expiry exists for a client
/// that *died*, not for one that forgot. It costs a second connection to the
/// scheduler per dispatched translation unit; the one the grant arrived on cannot
/// carry it, because that port sweeps a connection idle for five seconds and a
/// compile is longer than that.
///
/// The residual, deliberately: the lease is resolved when the compile ends, and the
/// caller stores the object afterwards, so for the length of that store the key is
/// neither in flight nor in the cache and a client arriving inside that window can
/// be granted a lease for work already done. It costs one duplicate compile of one
/// object, and the alternative — holding the lease across work this function does
/// not control, on paths where the caller may legitimately never store at all — is
/// how a lease comes to be resolved by expiry again.
///
/// **The object comes back to the client, and the client stores it.** A worker is
/// never given cache credentials. Today a `STORE` is trusted because whoever stores
/// compiled the thing themselves, so the worst they can do is poison their own key
/// space with something they would have gotten anyway. If workers stored, one rogue
/// worker would poison keys every other machine fetches. Routing the result back
/// through the client keeps that trust model exactly as it is, and needs no new
/// authorization anywhere.
///
/// @param dialer How to reach the scheduler and the worker.
/// @param request The job.
/// @param credential Presented to both peers; default-constructed sends none.
/// @param acceptedCodecs What this client can decode, most-preferred first.
/// @return What happened. Never throws; every failure is a status.
[[nodiscard]] DispatchResult Dispatch(IEndpointDialer& dialer,
                                      DispatchRequest const& request,
                                      Credential const& credential = {},
                                      CompileCacheWire::CodecList const& acceptedCodecs = {});

/// Split a COMPILE request's argument field back into arguments.
///
/// The inverse of what `Dispatch` encodes, and the worker's half of it. One
/// length-prefixed field per argument rather than a joined string, because an
/// argument may contain a space and a receiver splitting on whitespace would turn
/// `-DMSG=hello world` into two flags.
///
/// A truncated field yields **nothing**, never a prefix: a partial argument list is
/// a different compile from the one that was authorized, and running it would
/// produce an object nobody asked for.
/// @param field The encoded argument field.
/// @return The arguments, or an empty list when the field is malformed.
[[nodiscard]] std::vector<std::string> DecodeArgs(std::span<std::byte const> field);

/// Create the dialer that opens real TCP connections.
/// @param connectTimeout Deadline for opening a connection, resolution included.
/// @param ioTimeout Per-call send/recv deadline, armed on the connector.
/// @return A dialer; never null.
[[nodiscard]] std::unique_ptr<IEndpointDialer> MakeTcpDialer(std::chrono::milliseconds connectTimeout,
                                                             std::chrono::milliseconds ioTimeout);

} // namespace FastCache::Cc
