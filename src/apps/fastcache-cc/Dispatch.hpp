// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"

#include <FastCache/Core/Compression.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// Ceiling on what a codec envelope may declare it expands to.
///
/// 256 MiB, the same figure every framed surface in this project caps a request
/// at, because that is exactly what it has to be: an envelope's declared length
/// decides an allocation on the receiving side, so the number that bounds it is
/// the one the surface already promised to bound. A preprocessed C++ translation
/// unit runs to a few megabytes, so this is orders of magnitude above any honest
/// payload.
///
/// It is a **default**, not the rule: `Unenvelope` takes the ceiling as an
/// argument so each surface passes its own, and a surface with a smaller request
/// cap must pass that instead of inheriting this.
// `ULL`, not `UL`: `unsigned long` is 32 bits on Win64 (LLP64), so a future edit
// raising this past 4 GiB would silently wrap there and nowhere else.
inline constexpr std::size_t DefaultMaxDecompressedBytes = 256ULL * 1024ULL * 1024ULL;

/// Why an enveloped payload could not be opened.
///
/// Distinct reasons rather than one "no", because they are different facts about
/// the peer and call for different answers: a codec this build lacks is a
/// configuration difference between two honest processes, while a declared
/// expansion above the cap is a peer trying to make this process allocate.
enum class EnvelopeError : std::uint8_t
{
    /// The envelope framing is not decodable, or an Identity payload's declared
    /// length disagrees with the bytes beside it.
    Malformed,
    /// The codec id is one this build cannot decode.
    UnsupportedCodec,
    /// The declared decompressed length exceeds the caller's ceiling. Answered
    /// **before** a byte is decompressed, which is the whole point of the field.
    DeclaredTooLarge,
    /// The payload did not expand to exactly the declared length.
    Corrupt,
};

/// A human-readable reason, for a refusal a person has to act on.
/// @param error The reason.
/// @return Its description.
[[nodiscard]] std::string_view DescribeEnvelopeError(EnvelopeError error) noexcept;

/// Undo a codec envelope, refusing an oversized declared expansion first.
///
/// **The one implementation of this, deliberately.** It was two — the worker
/// opening a request and the launcher opening a worker's reply — and both trusted
/// the declared length, so a guard added to either would have been half a fix and
/// the two would have had to agree forever afterwards.
///
/// `Core/Compression.hpp` states the precondition that makes the guard necessary:
/// `originalLen` is "the trusted expected size taken from the record header", and
/// it bounds the output allocation. Off a socket it is neither trusted nor a record
/// header — and `Decompress` **value-initializes** a buffer of that size, so the
/// pages are touched rather than lazily reserved. A `u32` field therefore turns a
/// thirty-byte frame into a 4 GiB allocation the surface's byte budget never
/// charged anyone for.
///
/// `Protocol/CompileCacheWire.hpp` already says this is what the field is for:
/// carrying `rawLen` "is what lets a decoder reject a payload whose declared
/// expansion exceeds its cap before decompressing a byte". This implements the
/// guard that comment promised; no decoder in the tree performed it.
///
/// An **Identity** envelope is checked too, and for a reason worth stating: it
/// takes no decompression path at all, so it never reached `Decompress`'s own
/// length check and could declare any length it liked beside any payload. That is
/// not an allocation, but it is a field describing bytes it does not describe, and
/// a receiver that believes it later is the next defect.
///
/// @tparam Bytes The owning container to produce — `std::string` for text a
///         compiler will read, `std::vector<std::byte>` for an object file. A
///         template rather than one spelling plus a conversion, so neither caller
///         pays a copy of a multi-megabyte payload to share one implementation.
/// @param field The enveloped field, exactly as it arrived.
/// @param maxRawBytes The caller's own ceiling on the decompressed size.
/// @return The original bytes, or why they could not be produced.
template <typename Bytes>
[[nodiscard]] std::expected<Bytes, EnvelopeError> Unenvelope(std::span<std::byte const> field, std::size_t maxRawBytes)
{
    auto const envelope = CompileCacheWire::DecodeCodecEnvelope(field);
    if (!envelope.has_value())
        return std::unexpected(EnvelopeError::Malformed);

    // FIRST, before the codec is even looked up, and certainly before any byte is
    // decompressed. Everything below this line can allocate what the field says.
    if (envelope->rawLength > maxRawBytes)
        return std::unexpected(EnvelopeError::DeclaredTooLarge);

    auto const* const begin = reinterpret_cast<Bytes::value_type const*>(envelope->bytes.data());
    if (envelope->codec == CompileCacheWire::IdentityCodec)
    {
        if (envelope->rawLength != envelope->bytes.size())
            return std::unexpected(EnvelopeError::Malformed);
        return Bytes { begin, begin + envelope->bytes.size() };
    }

    auto const codec = static_cast<CompressionCodec>(envelope->codec);
    if (!Compression::IsAvailable(codec))
        return std::unexpected(EnvelopeError::UnsupportedCodec);

    auto const decoded = Compression::Decompress(codec, envelope->bytes, envelope->rawLength);
    if (!decoded.has_value())
        return std::unexpected(EnvelopeError::Corrupt);

    auto const* const plain = reinterpret_cast<Bytes::value_type const*>(decoded->data());
    return Bytes { plain, plain + decoded->size() };
}

/// Runs one framed request/reply against an endpoint named at runtime.
///
/// The library's `ISocket` is one *connected* peer, which was enough
/// while there was only ever one address to reach. Distribution talks to two —
/// the scheduler, and then whichever worker the scheduler names — so reaching an
/// endpoint becomes the thing that has to be injected. Tests answer with scripted
/// bytes per endpoint and never open a socket.
///
/// **The seam is the whole exchange rather than the dial, and that is what makes a
/// deadline expressible.** A dialer can only hand back a socket, so the only
/// ceiling it can arm is `SO_RCVTIMEO`, which bounds a single `recv`; a worker
/// dribbling one byte before each expiry holds the build forever without ever
/// exceeding it. Bounding the *exchange* is `DeadlineTimer`'s job and needs the
/// reactor the exchange runs on, which is `ReactorExchange` — so the caller asks
/// for an exchange and hands over the budget it must finish inside.
class IEndpointExchange
{
  public:
    IEndpointExchange() = default;
    virtual ~IEndpointExchange() = default;
    IEndpointExchange(IEndpointExchange const&) = delete;
    IEndpointExchange& operator=(IEndpointExchange const&) = delete;
    IEndpointExchange(IEndpointExchange&&) = delete;
    IEndpointExchange& operator=(IEndpointExchange&&) = delete;

    /// Send `frame` to `hostPort` and read its reply.
    /// @param hostPort The endpoint, e.g. "10.0.0.7:6676".
    /// @param frame A complete framed request.
    /// @param credential Presented with the request; default-constructed sends none.
    /// @param budget The deadlines this exchange must finish inside.
    /// @return The outcome. `Transport` covers every way it did not complete — an
    ///         endpoint that could not be reached, a peer that broke mid-reply, and
    ///         a budget that ran out — because the caller answers all three by
    ///         compiling locally.
    [[nodiscard]] virtual CacheOutcome Exchange(std::string_view hostPort,
                                                std::vector<std::byte> frame,
                                                Credential const& credential,
                                                ExchangeBudget budget) = 0;
};

/// How long a remote compile may take, once the connection is open.
///
/// Ten minutes, and NOT the cache's ten seconds. A cache exchange is answered from
/// memory, so ten seconds is generous there and being impatient is what makes a
/// miss cheap. A compile exchange is bounded by how long a compiler runs, and the
/// worker writes nothing until it has finished — so the client sits in one read for
/// the whole compile. Sharing the cache's number meant every translation unit
/// taking more than ten seconds was abandoned mid-compile and rebuilt locally,
/// which is precisely the set of translation units distribution exists for: the
/// work was done twice, the object crossed the network to nobody, and the fleet's
/// counters went up (#223).
///
/// **Ten minutes because that is `LeaseTable::DefaultLeaseTimeout`, not because it
/// is a comfortable round number.** A client waiting past its lease is waiting on
/// one the scheduler has already reclaimed and may have re-granted for the same
/// key, so nothing above that value can be honoured and everything below it throws
/// away a compile the fleet is still holding capacity for. The two are coupled and
/// cannot be `static_assert`ed together — this launcher deliberately does not link
/// the library the scheduler lives in — so moving either means moving both.
///
/// Deliberately generous rather than tight, because a *flat* deadline has to cover
/// the slowest translation unit anybody legitimately compiles: real ones here run
/// well past a minute, and a ceiling sized for the average reintroduces exactly the
/// defect above, further out.
///
/// **The cost is stated rather than absorbed.** This is also how long a genuinely
/// dead worker goes unnoticed — a machine powered off, unplugged or suspended
/// mid-compile — and against the ten seconds a dispatch used to get that is sixty
/// times slower, which on a parallel build is a handful of stalled slots. A flat
/// ceiling cannot separate that from a slow compile; splitting them needs the worker
/// to say it is still there, so the idle bound can be seconds while the total stays
/// long. That is a wire change tracked as #245.
///
/// It is `FASTCACHE_DISPATCH_TIMEOUT_MS` at run time, and `fastcache-cc` is one
/// process per translation unit, so the variable IS the runtime knob: the next
/// compile reads it. Nothing has to be reloaded, and nothing has to be restarted.
constexpr std::chrono::milliseconds DefaultDispatchTotal { 600'000 };

/// The deadlines a dispatch's exchanges run under.
///
/// Two, because a dispatch is two shapes of conversation and one number cannot
/// serve both — the defect above, stated as a type so a caller cannot pass the
/// cache's budget where the compile's belongs.
struct DispatchBudgets
{
    /// The scheduler's LEASE and RELEASE: short request/reply, answered from the
    /// scheduler's own tables. The launcher's ordinary exchange budget.
    ExchangeBudget control {};

    /// The worker's COMPILE: as long as a compiler runs.
    ExchangeBudget compile { .total = DefaultDispatchTotal };

    /// Ceiling on the object a worker may declare its reply expands to.
    ///
    /// A byte budget beside the two time budgets, because it bounds the same thing
    /// they do — what one exchange may cost this process — and a peer's declared
    /// decompressed length is the one figure in a reply that decides an allocation
    /// before any of it is validated.
    std::size_t maxDecompressedBytes { DefaultMaxDecompressedBytes };
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
/// Three exchanges, each a request/reply on a fresh connection: a `Lease` to the
/// scheduler, a `Compile` to whichever worker it named, and a `Release` back to the
/// scheduler saying the job is over. Two of the three are short and one is as long
/// as a compile, which is why `DispatchBudgets` carries two deadlines rather than
/// one. The client never waits in a queue — a scheduler with nothing free refuses
/// immediately, and the caller compiles locally. That is not a fallback bolted on
/// afterwards; it is why the scheduler is allowed to refuse at all.
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
/// @param exchange How to reach the scheduler and the worker.
/// @param request The job.
/// @param budgets The deadlines each leg runs under.
/// @param credential Presented to both peers; default-constructed sends none.
/// @param acceptedCodecs What this client can decode, most-preferred first.
/// @return What happened. Never throws; every failure is a status.
[[nodiscard]] DispatchResult Dispatch(IEndpointExchange& exchange,
                                      DispatchRequest const& request,
                                      DispatchBudgets const& budgets = {},
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

// The exchange that talks over real TCP connections is `MakeTcpExchange()` in
// `ReactorExchange.hpp`, deliberately not here. It needs a reactor, and this header
// is compiled into the compile NODE as well -- which wants `DecodeArgs` and the
// request types and has no use for a client's dialling machinery. Declaring the
// factory beside the reactor keeps that link honest instead of costing the node a
// translation unit it never calls.

} // namespace FastCache::Cc
