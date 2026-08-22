// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Endian.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

/// The `0xFC` compile-cache wire format — the single source of truth shared by
/// the daemon's handler, the `fastcache-cc` launcher, and the protocol test
/// client.
///
/// **This header must stay dependency-free** (the standard library plus the
/// header-only `Core/Endian.hpp`, and nothing else). `fastcache-cc` deliberately
/// does not link the `FastCache` library — it compiles a handful of leaf sources
/// in directly so it pulls no vcpkg dependency — so an include of anything from
/// `Net/`, `Cache/`, `Async/` or `Config/` here breaks the launcher's **link**,
/// not merely its build. This is the same constraint `Cli/UsageDoc` carries, and
/// for the same reason. Being header-only is also what keeps it free: it costs no
/// row in `_fc_cc_core`.
///
/// **The module performs no I/O and holds no state.** Every function here is a
/// pure transform between bytes and structs. That is a deliberate exception to
/// the project's inject-every-dependency rule rather than an oversight: there is
/// no clock, socket, filesystem or environment behind any of it, so there is
/// nothing to inject. Reading and writing stay at the two call sites, which have
/// very different concurrency models — a coroutine reader on the server, a
/// blocking socket on the client — and share these builders and parsers verbatim.
///
/// ## Frame layout
///
/// A request is a fixed 7-byte header followed by exactly `payloadLength` bytes:
///
/// ```
/// [u8 magic=0xFC][u8 version][u8 op][u32 payloadLength]
/// payload := field*        where field := [u32 len][len bytes]
/// ```
///
/// A reply is a fixed 5-byte header followed by exactly `payloadLength` bytes,
/// **uniformly for every status** — including a miss, which carries a zero-length
/// payload rather than no payload at all:
///
/// ```
/// [u8 status][u32 payloadLength][payload]
/// ```
///
/// The declared lengths are what make the protocol extensible. A receiver that
/// does not recognise an opcode can skip exactly `payloadLength` bytes, answer
/// with a typed error, and stay in sync — so adding a verb is not a breaking
/// change, and a rejection can be a *reply* instead of a dropped connection.
/// Without them (as in the pre-1 format) the only possible response to anything
/// unrecognised was to close, which is indistinguishable from a dead peer.
///
/// All multi-byte integers are big-endian.
///
/// ## Authentication, and why it costs no round trip
///
/// When the server requires a credential, every verb whose `OpDescriptor::preAuth`
/// is false is refused with `Unauthenticated` until an `Auth` frame has been
/// accepted on that connection. That is per-connection state, and the launcher
/// opens a fresh connection *per operation* — so the obvious spelling (send AUTH,
/// await its reply, then send the real command) would double the round trips on
/// exactly the path the "no handshake" decision above exists to protect.
///
/// It does not have to be spelled that way. Replies are strictly ordered and
/// one-per-request, so a client may **pipeline**: write `AUTH` and the real
/// command in a single write, then read the two replies in order. The credential
/// costs a few dozen bytes in a segment that was going to be sent anyway, and the
/// round-trip count is unchanged. `Cc::CacheFetch`/`Cc::CacheStore` do exactly
/// this. A server must therefore never coalesce, reorder, or skip a reply — which
/// the framing already guarantees, since every request is answered exactly once.
namespace FastCache::CompileCacheWire
{

/// First byte of every compile-cache frame. Distinct from the memcached binary
/// magic (0x80) and from every RESP first byte, so `ProtocolAutodetect` can route
/// a connection on it. Lives here rather than in `ProtocolAutodetect.hpp` because
/// the clients need it and cannot include that header.
inline constexpr std::byte Magic { 0xFC };

/// Protocol version, carried in every request header.
///
/// A plain integer alias rather than an `enum class`: this is an ordered quantity
/// compared against a supported *range*, and an enumeration would need a cast at
/// every comparison. The named constants below are the vocabulary.
using WireVersion = std::uint8_t;

/// The version this build speaks and emits.
///
/// Bump this whenever the framing changes shape. It is deliberately **not**
/// derived from the release version: the wire format changes far more rarely than
/// the product does, and tying them would force a flag day on every release.
inline constexpr WireVersion CurrentVersion = 1;

/// The oldest version this build still accepts. Equal to `CurrentVersion` while
/// only one version exists; widen the range when a second one ships and this
/// build can still decode the older shape.
inline constexpr WireVersion MinSupportedVersion = 1;

/// Size of the fixed request header: magic, version, op, payload length.
inline constexpr std::size_t RequestHeaderSize = 7;

/// Size of the fixed reply header: status, payload length.
inline constexpr std::size_t ReplyHeaderSize = 5;

/// Largest payload a frame can describe, since the length field is a u32.
///
/// Enforced by the encoders rather than assumed. Casting an over-large size down
/// to `std::uint32_t` would quietly emit a frame whose declared length disagrees
/// with its contents — the exact desynchronisation the declared length exists to
/// prevent, and undetectable by the peer.
///
/// A caller that exceeds it has misused the contract, so the encoders throw
/// `std::length_error` rather than returning a sentinel. That keeps the
/// post-condition simple — an encoder either returns a well-formed frame or does
/// not return — which is what lets every caller index the result without first
/// proving it non-empty. It is also unreachable in practice: the daemon caps
/// values at `--storage-max-value` (256 MiB by default), far below this.
inline constexpr std::uint64_t MaxFramePayload = 0xFFFFFFFFULL;

/// Wire opcodes. One byte, third in the request header.
enum class Op : std::uint8_t
{
    Store = 0x01, ///< Canonicalize and store a compile result.
    Fetch = 0x02, ///< Retrieve a compile result in canonical form.
    Auth = 0x03,  ///< Present a credential; gates every other verb when auth is on.
};

/// Reply status, the first byte of every reply.
///
/// `Miss` and `Ok` keep the byte values the pre-version format used. A miss is a
/// legitimate negative answer, not a failure, and is therefore distinct from
/// `Error` — conflating the two (as the pre-1 format did, where both were `0x00`)
/// makes a rejected client see an endlessly cold cache with no diagnostic.
enum class Status : std::uint8_t
{
    Miss = 0x00,  ///< FETCH found nothing. Payload is empty.
    Ok = 0x01,    ///< Command succeeded. Payload is the result, if any.
    Error = 0x02, ///< Command refused. Payload is `[u8 ErrorCode][message]`.
};

/// Why a command was refused. Travels as the first payload byte of an `Error`
/// reply, ahead of a human-readable message.
///
/// A numeric code rather than the bare string the pre-1 format sent: a client has
/// to *act* differently on a version rejection (stop trying, tell the user the
/// install is mismatched) than on a value the server could not store (carry on,
/// this build is simply uncached), and parsing English to decide is not a
/// contract.
enum class ErrorCode : std::uint8_t
{
    UnsupportedVersion = 0x01,     ///< Request version outside this build's range.
    UnknownOpcode = 0x02,          ///< Opcode not in `OpTable`.
    MalformedFrame = 0x03,         ///< Fields do not exactly fill the declared payload.
    PayloadTooLarge = 0x04,        ///< Declared payload exceeds the session's cap.
    MalformedValue = 0x05,         ///< STORE payload is not a decodable compile-value.
    CanonicalizationFailed = 0x06, ///< A text region's paths could not be canonicalized.
    StorageWriteFailed = 0x07,     ///< The cache engine refused the write.
    Unauthenticated = 0x08,        ///< A credential is required and has not been accepted.
};

/// Bit for `status` within an `OpDescriptor::legalStatuses` mask.
/// @param status The status to encode.
/// @return A single-bit mask.
[[nodiscard]] constexpr std::uint8_t StatusBit(Status status) noexcept
{
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(status));
}

/// One row of the opcode table: everything the framing layer knows about a verb.
struct OpDescriptor
{
    Op code;                    ///< The wire byte.
    std::string_view name;      ///< Stable lower-case name, for logs and diagnostics.
    std::size_t fieldCount;     ///< Exact number of length-prefixed request fields.
    std::uint8_t legalStatuses; ///< Mask of the statuses this op may be answered with.
    /// Whether this verb may be served before a credential has been accepted.
    ///
    /// A column rather than a predicate with its own `switch`: "which verbs are
    /// reachable by an unauthenticated peer" is the security-relevant property of
    /// the whole table, and a reviewer must be able to read it off the table
    /// itself. A verb added without thinking about it defaults to `false` —
    /// closed, which is the direction a mistake here has to fail in.
    bool preAuth;
    /// Largest payload this verb may declare, or 0 for "the session cap".
    ///
    /// A verb reachable *before* authentication MUST declare a real bound, and
    /// `PreAuthVerbsAreBounded` asserts that it does. The pre-auth gate exists so
    /// an unauthenticated peer cannot make the server allocate `maxPayloadBytes`
    /// (256 MiB by default) per frame — and a verb waved through that gate is read
    /// with exactly that allocation unless it says otherwise, which would defeat
    /// the gate through the one door it deliberately holds open.
    ///
    /// A gated verb may legitimately leave this 0: STORE carries a whole object
    /// file, and by the time it is read the peer has authenticated, so the
    /// operator's own cap is the right bound.
    std::size_t maxPayload;
};

/// Payload ceiling for AUTH: a username and a shared secret, and nothing that
/// grows with a build artefact. Generous by orders of magnitude against any real
/// credential, and small enough that a peer which has proved nothing cannot use
/// it to make the server take memory.
inline constexpr std::size_t MaxAuthPayload = 4096;

/// Every opcode this build understands.
///
/// `fieldCount` is the single source of the request arity — parsers take it from
/// here rather than spelling a literal, so an op's shape lives in exactly one
/// place. `legalStatuses` makes "a FETCH may miss but a STORE may not" a property
/// of the data instead of a convention, and is assertable in tests.
///
/// The array size is deduced rather than spelled: a count that has to be edited
/// alongside the rows is a second place to change when a verb is added, which is
/// exactly what this table exists to avoid.
inline constexpr std::array OpTable {
    OpDescriptor { .code = Op::Store,
                   .name = "store",
                   .fieldCount = 5, // key, prefetchGroup, srcRoot, buildTree, value
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = false,
                   .maxPayload = 0 }, // an object file; bounded by the operator's cap
    OpDescriptor { .code = Op::Fetch,
                   .name = "fetch",
                   .fieldCount = 1, // key
                   .legalStatuses =
                       static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Miss) | StatusBit(Status::Error)),
                   .preAuth = false,
                   .maxPayload = 0 },
    OpDescriptor { .code = Op::Auth,
                   .name = "auth",
                   .fieldCount = 2, // username, secret
                   .legalStatuses = static_cast<std::uint8_t>(StatusBit(Status::Ok) | StatusBit(Status::Error)),
                   .preAuth = true,
                   .maxPayload = MaxAuthPayload },
};

/// Whether every verb reachable before authentication declares a payload bound.
///
/// A `static_assert`ed invariant rather than a comment, because getting it wrong
/// is not a bug in the new verb — it is the pre-auth allocation gate silently
/// ceasing to hold for the whole protocol.
/// @return True when no `preAuth` row leaves `maxPayload` at 0.
[[nodiscard]] constexpr bool PreAuthVerbsAreBounded() noexcept
{
    return std::ranges::all_of(OpTable, [](OpDescriptor const& row) { return !row.preAuth || row.maxPayload != 0; });
}

static_assert(PreAuthVerbsAreBounded(), "a verb reachable before AUTH must declare its own payload ceiling");

/// One row of the error table: the code, its stable name, and the message sent
/// when the caller has nothing more specific to say.
struct ErrorDescriptor
{
    ErrorCode code;                  ///< The wire byte.
    std::string_view name;           ///< Stable lower-case name, for logs and tests.
    std::string_view defaultMessage; ///< Human-readable text sent when no detail is supplied.
};

/// Every error code this build emits. Size deduced, for the reason `OpTable`'s is.
inline constexpr std::array ErrorTable {
    ErrorDescriptor {
        .code = ErrorCode::UnsupportedVersion, .name = "unsupported-version", .defaultMessage = "unsupported wire version" },
    ErrorDescriptor { .code = ErrorCode::UnknownOpcode, .name = "unknown-opcode", .defaultMessage = "unknown opcode" },
    ErrorDescriptor { .code = ErrorCode::MalformedFrame, .name = "malformed-frame", .defaultMessage = "malformed frame" },
    ErrorDescriptor {
        .code = ErrorCode::PayloadTooLarge, .name = "payload-too-large", .defaultMessage = "payload too large" },
    ErrorDescriptor {
        .code = ErrorCode::MalformedValue, .name = "malformed-value", .defaultMessage = "malformed compile-value frame" },
    ErrorDescriptor { .code = ErrorCode::CanonicalizationFailed,
                      .name = "canonicalization-failed",
                      .defaultMessage = "path canonicalization failed" },
    ErrorDescriptor {
        .code = ErrorCode::StorageWriteFailed, .name = "storage-write-failed", .defaultMessage = "storage write failed" },
    ErrorDescriptor {
        .code = ErrorCode::Unauthenticated, .name = "unauthenticated", .defaultMessage = "authentication required" },
};

/// Look up the descriptor for a raw opcode byte.
/// @param opRaw The third header byte, as received.
/// @return The descriptor, or nullptr when this build does not know the opcode.
[[nodiscard]] constexpr OpDescriptor const* FindOp(std::uint8_t opRaw) noexcept
{
    for (auto const& row: OpTable)
        if (static_cast<std::uint8_t>(row.code) == opRaw)
            return &row;
    return nullptr;
}

/// Look up the descriptor for an error code.
/// @param code The code to describe.
/// @return The descriptor, or nullptr when the code is not in the table.
[[nodiscard]] constexpr ErrorDescriptor const* Describe(ErrorCode code) noexcept
{
    for (auto const& row: ErrorTable)
        if (row.code == code)
            return &row;
    return nullptr;
}

/// Request field count for a known opcode, from the table.
/// @param op The opcode.
/// @return The field count, or 0 when the op is not in the table.
[[nodiscard]] constexpr std::size_t OpFieldCount(Op op) noexcept
{
    auto const* row = FindOp(static_cast<std::uint8_t>(op));
    return row != nullptr ? row->fieldCount : 0;
}

/// The payload ceiling a raw opcode declares, bounded by the session's own cap.
///
/// Takes the raw byte and the session cap together so a caller cannot apply one
/// without the other: the per-op bound is an *additional* restriction, never a
/// licence to exceed what the operator configured.
/// @param opRaw The third header byte, as received.
/// @param sessionCap The session's configured maximum payload.
/// @return The effective ceiling for this frame.
[[nodiscard]] constexpr std::size_t OpPayloadCap(std::uint8_t opRaw, std::size_t sessionCap) noexcept
{
    auto const* row = FindOp(opRaw);
    if (row == nullptr || row->maxPayload == 0)
        return sessionCap;
    return row->maxPayload < sessionCap ? row->maxPayload : sessionCap;
}

/// Whether `op` may legally be answered with `status`, per the table.
/// @param op The opcode.
/// @param status The status under consideration.
/// @return True when the pairing is allowed.
[[nodiscard]] constexpr bool IsLegalStatus(Op op, Status status) noexcept
{
    auto const* row = FindOp(static_cast<std::uint8_t>(op));
    return row != nullptr && (row->legalStatuses & StatusBit(status)) != 0;
}

/// Whether `op` may be served to a peer that has not authenticated, per the table.
///
/// Takes the raw opcode byte, not an `Op`: the gate has to answer for whatever
/// arrived on the wire, and an unknown opcode must read as "not allowed" rather
/// than force the caller to resolve the descriptor first and decide what a null
/// row means. An unknown verb is refused on its own grounds anyway, but a gate
/// that fails open for anything is the wrong shape regardless of who calls it.
/// @param opRaw The third header byte, as received.
/// @return True only for a known verb whose row permits it.
[[nodiscard]] constexpr bool IsPreAuthAllowed(std::uint8_t opRaw) noexcept
{
    auto const* row = FindOp(opRaw);
    return row != nullptr && row->preAuth;
}

/// Whether this build can decode a request at `version`.
/// @param version The version byte from a request header.
/// @return True when within [MinSupportedVersion, CurrentVersion].
[[nodiscard]] constexpr bool IsSupported(WireVersion version) noexcept
{
    return version >= MinSupportedVersion && version <= CurrentVersion;
}

/// Reinterpret a string view as wire bytes. No copy.
/// @param text The text to view.
/// @return A span over the same storage.
[[nodiscard]] inline std::span<std::byte const> AsBytes(std::string_view text) noexcept
{
    return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
}

/// Reinterpret wire bytes as a string view. No copy.
/// @param bytes The bytes to view.
/// @return A view over the same storage.
[[nodiscard]] inline std::string_view AsStringView(std::span<std::byte const> bytes) noexcept
{
    return { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
}

/// The decoded fixed part of a request. The opcode is kept **raw** because an
/// unrecognised one is a recoverable condition the caller answers with a typed
/// error, not a decode failure.
struct RequestHeader
{
    WireVersion version {};         ///< Protocol version the sender used.
    std::uint8_t opRaw {};          ///< Opcode byte, not yet validated against OpTable.
    std::uint32_t payloadLength {}; ///< Exact byte count following the header.
};

/// The decoded fixed part of a reply.
struct ReplyHeader
{
    Status status {};               ///< Outcome of the command.
    std::uint32_t payloadLength {}; ///< Exact byte count following the header.
};

/// The five fields of a STORE request, as spans into the caller's payload buffer.
///
/// Views, not copies: the value is routinely a multi-megabyte object file and
/// must not be duplicated on the way in. The referenced buffer must outlive the
/// view — on the server that buffer is a coroutine local, which lives in the
/// coroutine frame and so survives suspension.
struct StoreView
{
    std::span<std::byte const> key;           ///< Cache key.
    std::span<std::byte const> prefetchGroup; ///< Prefetch group id, may be empty.
    std::span<std::byte const> srcRoot;       ///< Producer's source root.
    std::span<std::byte const> buildTree;     ///< Producer's build tree.
    std::span<std::byte const> value;         ///< Encoded compile-value.
};

/// The two fields of an AUTH request, as spans into the caller's payload buffer.
struct AuthView
{
    std::span<std::byte const> username; ///< Username; empty selects the default user.
    std::span<std::byte const> secret;   ///< Shared secret.
};

/// The fields of an AUTH request, for encoding.
///
/// The username may be empty, which asks to be verified against the secret alone
/// — the redis one-argument `AUTH <pass>` / `requirepass` form. Carrying it as an
/// always-present (possibly empty) field rather than a separate one-field opcode
/// keeps the arity fixed, so the frame shape does not depend on which credential
/// style the client happens to use.
struct AuthRequest
{
    std::string_view username; ///< Username, or empty for the default user.
    std::string_view secret;   ///< Shared secret.
};

/// The fields of a STORE request, as owning views for encoding.
struct StoreRequest
{
    std::string_view key;             ///< Cache key.
    std::string_view prefetchGroup;   ///< Prefetch group id, may be empty.
    std::string_view srcRoot;         ///< This machine's source root.
    std::string_view buildTree;       ///< This machine's build tree.
    std::span<std::byte const> value; ///< Encoded compile-value.
};

namespace Detail
{

    /// Byte offset `n` into a buffer, as an iterator difference.
    [[nodiscard]] constexpr std::ptrdiff_t Offset(std::size_t n) noexcept
    {
        return static_cast<std::ptrdiff_t>(n);
    }

    /// Write a big-endian u32 at `offset` within `out`.
    /// @param out Destination buffer, already sized to hold it.
    /// @param offset Byte offset to write at.
    /// @param n Value to write.
    inline void PutU32(std::span<std::byte> out, std::size_t offset, std::uint32_t n)
    {
        WriteBigEndian<std::uint32_t>(out.subspan(offset, sizeof(std::uint32_t)), n);
    }

    /// Build a complete request frame. **The only place a request header is
    /// written** — every encoder funnels through here, so the layout has exactly
    /// one author.
    ///
    /// The frame is sized exactly once and then filled in place. Growing it with
    /// `reserve` + `push_back` + `insert` would be the obvious spelling, but the
    /// total is known up front, so one allocation and no reallocation is both
    /// simpler and faster — and it keeps GCC's `-Wfree-nonheap-object` analysis
    /// out of a false positive it reaches when a `reserve`d vector is fed from a
    /// stack buffer at -O3.
    /// @param version Version to advertise.
    /// @param op The opcode.
    /// @param fields The length-prefixed fields, in wire order.
    /// @return The framed request.
    [[nodiscard]] inline std::vector<std::byte> EncodeRequest(WireVersion version,
                                                              Op op,
                                                              std::initializer_list<std::span<std::byte const>> fields)
    {
        std::uint64_t const payloadSize =
            std::ranges::fold_left(fields, std::uint64_t { 0 }, [](std::uint64_t acc, std::span<std::byte const> field) {
                return acc + sizeof(std::uint32_t) + std::uint64_t { field.size() };
            });
        if (payloadSize > MaxFramePayload)
            throw std::length_error("compile-cache request payload exceeds the u32 wire length");

        std::vector<std::byte> frame(RequestHeaderSize + static_cast<std::size_t>(payloadSize));
        std::span<std::byte> const out { frame };
        out[0] = Magic;
        out[1] = static_cast<std::byte>(version);
        out[2] = static_cast<std::byte>(op);
        PutU32(out, 3, static_cast<std::uint32_t>(payloadSize));

        std::size_t offset = RequestHeaderSize;
        for (auto const& field: fields)
        {
            PutU32(out, offset, static_cast<std::uint32_t>(field.size()));
            offset += sizeof(std::uint32_t);
            std::ranges::copy(field, std::next(frame.begin(), Offset(offset)));
            offset += field.size();
        }
        return frame;
    }

} // namespace Detail

/// Frame a STORE request.
/// @param request The fields to send.
/// @param version Version to advertise; overridable so tests can offer a version
///                the peer does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeStore(StoreRequest const& request, WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version,
                                 Op::Store,
                                 { AsBytes(request.key),
                                   AsBytes(request.prefetchGroup),
                                   AsBytes(request.srcRoot),
                                   AsBytes(request.buildTree),
                                   request.value });
}

/// Frame a FETCH request.
/// @param key The key to look up.
/// @param version Version to advertise; overridable so tests can offer a version
///                the peer does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeFetch(std::string_view key, WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::Fetch, { AsBytes(key) });
}

/// Frame an AUTH request.
/// @param request The credential to present.
/// @param version Version to advertise; overridable so tests can offer a version
///                the peer does not support.
/// @return The framed request.
[[nodiscard]] inline std::vector<std::byte> EncodeAuth(AuthRequest const& request, WireVersion version = CurrentVersion)
{
    return Detail::EncodeRequest(version, Op::Auth, { AsBytes(request.username), AsBytes(request.secret) });
}

/// Frame a reply. **The only place a reply header is written.**
///
/// A `Miss` is this function with an empty payload — there is no separate
/// miss encoder, because a second spelling of one thing is how the two drift.
/// @param status The outcome.
/// @param payload The reply body; empty for a miss.
/// @return The framed reply.
[[nodiscard]] inline std::vector<std::byte> EncodeReply(Status status, std::span<std::byte const> payload)
{
    if (payload.size() > MaxFramePayload)
        throw std::length_error("compile-cache reply payload exceeds the u32 wire length");

    std::vector<std::byte> frame(ReplyHeaderSize + payload.size());
    std::span<std::byte> const out { frame };
    out[0] = static_cast<std::byte>(status);
    Detail::PutU32(out, 1, static_cast<std::uint32_t>(payload.size()));
    std::ranges::copy(payload, std::next(frame.begin(), Detail::Offset(ReplyHeaderSize)));
    return frame;
}

/// Frame an `Error` reply carrying a code and a message.
/// @param code The refusal reason.
/// @param message Detail for a human; falls back to the table's default when empty.
/// @return The framed reply.
[[nodiscard]] inline std::vector<std::byte> EncodeErrorReply(ErrorCode code, std::string_view message = {})
{
    if (message.empty())
        if (auto const* row = Describe(code); row != nullptr)
            message = row->defaultMessage;
    if (message.size() >= MaxFramePayload)
        throw std::length_error("compile-cache error message exceeds the u32 wire length");

    std::vector<std::byte> payload(1 + message.size());
    payload[0] = static_cast<std::byte>(code);
    std::ranges::copy(AsBytes(message), std::next(payload.begin()));
    return EncodeReply(Status::Error, payload);
}

/// Decode the fixed request header.
///
/// Validates **only the magic** — a wrong magic means the peer is not speaking
/// this protocol at all, the one condition that still has to close the
/// connection. The version and the opcode are checked separately by `IsSupported`
/// and `FindOp`, because each maps to a different error code and a different
/// recovery, and the *order* of those checks belongs to the handler.
/// @param bytes Exactly `RequestHeaderSize` bytes.
/// @return The header, or nullopt when short or not this protocol.
[[nodiscard]] inline std::optional<RequestHeader> DecodeRequestHeader(std::span<std::byte const> bytes)
{
    if (bytes.size() < RequestHeaderSize || bytes[0] != Magic)
        return std::nullopt;
    return RequestHeader { .version = static_cast<WireVersion>(bytes[1]),
                           .opRaw = static_cast<std::uint8_t>(bytes[2]),
                           .payloadLength = ReadBigEndian<std::uint32_t>(bytes.subspan(3, sizeof(std::uint32_t))) };
}

/// Decode the fixed reply header.
/// @param bytes Exactly `ReplyHeaderSize` bytes.
/// @return The header, or nullopt when short or the status byte is unknown.
[[nodiscard]] inline std::optional<ReplyHeader> DecodeReplyHeader(std::span<std::byte const> bytes)
{
    if (bytes.size() < ReplyHeaderSize)
        return std::nullopt;
    auto const raw = static_cast<std::uint8_t>(bytes[0]);
    if (raw != static_cast<std::uint8_t>(Status::Miss) && raw != static_cast<std::uint8_t>(Status::Ok)
        && raw != static_cast<std::uint8_t>(Status::Error))
        return std::nullopt;
    return ReplyHeader { .status = static_cast<Status>(raw),
                         .payloadLength = ReadBigEndian<std::uint32_t>(bytes.subspan(1, sizeof(std::uint32_t))) };
}

/// Split a request payload into exactly `expectedCount` length-prefixed fields.
///
/// Strict in both directions: a field length that overruns the payload and any
/// trailing byte after the last field are both rejected. The declared payload
/// length and the per-field lengths are redundant by design, and disagreement
/// between them is a typed, recoverable `MalformedFrame` rather than the silent
/// desynchronisation it would have been without a declared total.
///
/// Takes the count as a runtime argument rather than a template parameter so the
/// arity has one home — `OpDescriptor::fieldCount` — instead of being spelled
/// again at every call site.
///
/// @param payload The bytes following the request header.
/// @param expectedCount Field count, from the op's descriptor.
/// @return The fields as spans into `payload`, or nullopt when malformed.
[[nodiscard]] inline std::optional<std::vector<std::span<std::byte const>>> SplitFields(std::span<std::byte const> payload,
                                                                                        std::size_t expectedCount)
{
    std::vector<std::span<std::byte const>> fields;
    fields.reserve(expectedCount);

    std::size_t offset = 0;
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, expectedCount))
    {
        if (payload.size() - offset < sizeof(std::uint32_t))
            return std::nullopt;
        auto const length = ReadBigEndian<std::uint32_t>(payload.subspan(offset, sizeof(std::uint32_t)));
        offset += sizeof(std::uint32_t);
        if (payload.size() - offset < length)
            return std::nullopt;
        fields.push_back(payload.subspan(offset, length));
        offset += length;
    }
    if (offset != payload.size())
        return std::nullopt; // trailing bytes
    return fields;
}

/// Split a STORE payload into its five named fields.
/// @param payload The bytes following the request header.
/// @return The field views, or nullopt when malformed.
[[nodiscard]] inline std::optional<StoreView> DecodeStorePayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Store));
    if (!fields.has_value())
        return std::nullopt;
    return StoreView { .key = (*fields)[0],
                       .prefetchGroup = (*fields)[1],
                       .srcRoot = (*fields)[2],
                       .buildTree = (*fields)[3],
                       .value = (*fields)[4] };
}

/// Split a FETCH payload into its single key field.
/// @param payload The bytes following the request header.
/// @return The key, or nullopt when malformed.
[[nodiscard]] inline std::optional<std::span<std::byte const>> DecodeFetchPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Fetch));
    if (!fields.has_value())
        return std::nullopt;
    return (*fields)[0];
}

/// Split an AUTH payload into its two named fields.
/// @param payload The bytes following the request header.
/// @return The field views, or nullopt when malformed.
[[nodiscard]] inline std::optional<AuthView> DecodeAuthPayload(std::span<std::byte const> payload)
{
    auto const fields = SplitFields(payload, OpFieldCount(Op::Auth));
    if (!fields.has_value())
        return std::nullopt;
    return AuthView { .username = (*fields)[0], .secret = (*fields)[1] };
}

/// Split an `Error` reply payload into its code and message.
/// @param payload The bytes following the reply header.
/// @return The code and message, or nullopt when the payload is empty.
[[nodiscard]] inline std::optional<std::pair<ErrorCode, std::string_view>> DecodeErrorPayload(
    std::span<std::byte const> payload)
{
    if (payload.empty())
        return std::nullopt;
    return std::pair { static_cast<ErrorCode>(payload[0]), AsStringView(payload.subspan(1)) };
}

} // namespace FastCache::CompileCacheWire
