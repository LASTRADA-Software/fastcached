// SPDX-License-Identifier: Apache-2.0
#include "NodeClient.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/LeaderRedirect.hpp>

#include <array>
#include <format>
#include <ranges>
#include <utility>
#include <vector>

namespace FastCache::Cli
{

namespace Wire = FastCache::CompileCacheWire;

namespace
{
    /// One refusal code a client treats specially.
    ///
    /// A table rather than a `switch`, and the DEFAULT is the interesting part: a code
    /// with no row is `Reported`, which is the safe direction. Stepping over a refusal
    /// nobody enumerated is how a client comes to ignore something it should have
    /// stopped on, and the two codes below are the only two that are not ordinary.
    struct SpecialRefusal
    {
        Wire::ErrorCode code; ///< The code.
        NodeRefusalKind kind; ///< What a client does about it.
    };

    /// The codes a client does something other than report.
    ///
    /// Spelled against the two named constants rather than against bytes, because the
    /// rulebook's rule is that `UnimplementedVerb` is ONE constant every surface and
    /// every client spells -- three surfaces naming the enumerator separately is exactly
    /// how they drifted (#283, #340). The BYTE is pinned in `CompileCacheWire.hpp`'s own
    /// `static_assert`, which is the anchor a symbol cannot be.
    constexpr std::array<SpecialRefusal, 2> SpecialRefusals { {
        { .code = Wire::UnimplementedVerb, .kind = NodeRefusalKind::Unimplemented },
        { .code = Wire::ErrorCode::DispatchNotPermitted, .kind = NodeRefusalKind::ServedElsewhere },
    } };

    // The two must not collapse onto one code, which is the defect this whole
    // classification exists to prevent -- and a table keyed on the code cannot hold two
    // rows sharing one, so a collision would be silent rather than a build failure.
    static_assert(SpecialRefusals[0].code != SpecialRefusals[1].code,
                  "unimplemented and served-elsewhere must stay distinguishable on the wire");
} // namespace

NodeRefusalKind ClassifyRefusal(Wire::ErrorCode code) noexcept
{
    // A loop rather than `std::ranges::find`, and not by preference: `std::array`'s
    // iterator is a RAW POINTER on libstdc++ and libc++ and a CLASS on MSVC, so the
    // `auto const* const` spelling `readability-qualified-auto` asks for builds on two
    // of this project's three platforms and fails on the third. Nothing here needs an
    // iterator -- the answer is a value.
    for (auto const& row: SpecialRefusals)
        if (row.code == code)
            return row.kind;

    // **`Reported` is the DEFAULT, and it is the safe direction.** Stepping over a
    // refusal nobody enumerated is how a client comes to ignore something it was told
    // to stop for; reporting one it could have stepped over costs an exit code.
    return NodeRefusalKind::Reported;
}

std::string ExplainRefusal(std::string_view verb, std::string_view endpoint, NodeReply const& reply)
{
    if (!reply.code.has_value())
        return {};

    // The server's own words, when it sent any. A refusal whose detail is empty is
    // ordinary -- most are -- so this is a suffix rather than the whole sentence.
    auto const said = reply.detail.empty() ? std::string {} : std::format(" ({})", reply.detail);

    // `NotLeader` is an INSTRUCTION, not an answer about the fleet, and it carries two
    // opposite facts under one code. Asked through `LeaderRedirectTarget` rather than
    // decided here: `ClusterAdminCli` and `fastcache-cc` each wrote this rule once and
    // came to disagree (#237), and this would have been the third author. It sits above
    // the classification because it is about WHICH refusal rather than which of the
    // three kinds -- `NotLeader` classifies as `Reported`, correctly, and relaying it
    // bare tells an operator their command failed when it was merely sent to the wrong
    // machine.
    if (*reply.code == Wire::ErrorCode::NotLeader)
    {
        if (auto const leader = LeaderRedirectTarget(*reply.code, reply.detail); leader.has_value())
            return std::format("{} does not lead the cluster; ask {} instead", endpoint, *leader);
        // An election in progress. A different fact from *somebody else leads*, with no
        // address to offer -- and saying so beats relaying a sentence that reads as a
        // permanent refusal of a command that will work in a moment.
        return std::format("{} does not lead the cluster, and no leader is known right now; try again shortly", endpoint);
    }

    switch (ClassifyRefusal(*reply.code))
    {
        case NodeRefusalKind::Unimplemented:
            // Names the ENDPOINT, because the remedy is to ask a different one or to
            // give this one the component. "not supported" with no address sends an
            // operator to read this tool's documentation, which is the wrong file.
            return std::format("{} does not implement `{}`{}", endpoint, verb, said);
        case NodeRefusalKind::ServedElsewhere:
            // Deliberately NOT worded as a version problem. `UnknownOpcode` there would
            // tell a client this endpoint is too OLD when it is in fact too NEW, and
            // sending somebody to upgrade a node that is working correctly is worse than
            // saying nothing.
            return std::format(
                "{} does not serve `{}` -- that verb is answered by another endpoint{}", endpoint, verb, said);
        case NodeRefusalKind::Reported:
        case NodeRefusalKind::Last:
            break;
    }
    // The code's own NAME, from the wire table, so a refusal this client has never
    // heard of is still reported as something an operator can search for rather than as
    // a number. A code with no row at all is one this build does not carry, which is
    // reported as the byte -- the only honest thing left to say about it.
    auto const* const descriptor = Wire::Describe(*reply.code);
    auto const named = descriptor != nullptr ? std::string { descriptor->name }
                                             : std::format("code 0x{:02x}", static_cast<unsigned>(*reply.code));
    return std::format("{} refused `{}`: {}{}", endpoint, verb, named, said);
}

std::optional<Outcome> EstablishedBy(RemoteKind kind) noexcept
{
    auto const index = static_cast<std::size_t>(kind);
    if (index >= RemoteKindTable.size())
        return std::nullopt;
    return RemoteKindTable[index].established;
}

RemoteKind ProbeRemote(INodeExchange& node)
{
    auto const reply = node.Send(Wire::EncodeNodeStatusRequest());
    if (!reply.has_value())
        // A `0xFC` request that produced no frame at all. That covers a peer that closed
        // having sent nothing and one that sent something unreadable, and both mean the
        // same thing to the caller: whatever is on that port, it is not this protocol.
        return RemoteKind::NotFastcacheWire;

    if (reply->status == Wire::Status::Ok)
        return RemoteKind::CompileNode;

    // It framed a refusal, so it speaks this wire. Which refusal does NOT narrow it
    // further and must not be read as though it did -- `UnimplementedVerb` from a daemon
    // and from a pre-#431 node are the same bytes -- so every refusal lands here.
    return RemoteKind::FastcacheWireOnly;
}

std::string ExplainRemoteKind(std::string_view verb, std::string_view endpoint, RemoteKind kind)
{
    switch (kind)
    {
        case RemoteKind::CompileNode:
            // Names what the endpoint IS and what to do next. "not supported" alone
            // sends an operator to read this tool's documentation, which is the wrong
            // file: the endpoint is fine and the address is wrong.
            return std::format("{} is a fastcache-compile-node: it speaks the 0xFC compile-cache wire only, holds no "
                               "user keyspace, and cannot answer `{}` -- try `node` and `node-metrics` here, or point "
                               "--addr at a fastcached",
                               endpoint,
                               verb);
        case RemoteKind::FastcacheWireOnly:
            // Deliberately names BOTH possibilities rather than guessing. A confident
            // wrong signal is worse than a vague right one, and a client that announced
            // *this is your cache daemon* would send somebody debugging a daemon that
            // is in fact a node one release behind.
            return std::format("{} speaks the 0xFC compile-cache wire but answered no RESP: it is a fastcached whose "
                               "data plane is elsewhere, or a compile node older than these verbs",
                               endpoint);
        case RemoteKind::NotFastcacheWire:
        case RemoteKind::Last:
            break;
    }
    // Nothing to add. The caller's own transport diagnostic already said the port did
    // not answer, and repeating it in other words would make one fault read as two.
    return {};
}

std::optional<Value> DecodeNodeCounters(std::span<std::byte const> payload)
{
    auto const rows = WireFields::SplitAll(payload);
    if (!rows.has_value())
        return std::nullopt;

    std::vector<Field> fields;
    fields.reserve(rows->size());
    for (auto const& row: *rows)
    {
        auto const pair = WireFields::SplitExactly(row, 2);
        if (!pair.has_value())
            return std::nullopt;
        auto const value = Wire::DecodeU64Field((*pair)[1]);
        if (!value.has_value())
            return std::nullopt;

        // The name is text the NODE chose, so it goes through the one UTF-8 gate rather
        // than being trusted: a counter name that is not text would make `--format=json`
        // unparseable for the whole record, which is the fleet page's own rule arriving
        // on a different wire.
        auto const name = Wire::AsStringView((*pair)[0]);
        auto cell = TextCell(std::string { name });
        if (cell.kind != CellKind::Text)
            return std::nullopt;

        fields.push_back({ .name = std::string { name }, .value = NumberCell(*value) });
    }

    return RecordValue(std::move(fields));
}

std::expected<NodeReply, ExchangeError> DecodeNodeReply(std::span<std::byte const> bytes)
{
    auto const header = Wire::DecodeReplyHeader(bytes);
    if (!header.has_value())
        return std::unexpected(
            ExchangeError { .kind = ExchangeFailure::Malformed, .detail = "the reply is not a 0xFC frame" });

    // **A peer-declared length sizes nothing until it is checked against what arrived.**
    // The caller reads exactly `payloadLength` bytes before handing them here, so a
    // short buffer is this client's own bug rather than a hostile peer -- and it is
    // still refused rather than indexed past.
    if (bytes.size() < Wire::ReplyHeaderSize + header->payloadLength)
        return std::unexpected(ExchangeError { .kind = ExchangeFailure::Malformed,
                                               .detail = std::format("the reply declared {} payload bytes and {} arrived",
                                                                     header->payloadLength,
                                                                     bytes.size() - Wire::ReplyHeaderSize) });

    auto const payload = bytes.subspan(Wire::ReplyHeaderSize, header->payloadLength);

    NodeReply reply;
    reply.status = header->status;
    if (header->status == Wire::Status::Error)
    {
        auto const refusal = Wire::DecodeErrorPayload(payload);
        if (!refusal.has_value())
            return std::unexpected(
                ExchangeError { .kind = ExchangeFailure::Malformed, .detail = "the reply said error and carried no code" });
        reply.code = refusal->first;
        reply.detail = std::string { refusal->second };
        return reply;
    }

    reply.payload.assign(payload.begin(), payload.end());
    return reply;
}

} // namespace FastCache::Cli
