// SPDX-License-Identifier: Apache-2.0
#include "LiveSubscriber.hpp"
#include "SocketExchange.hpp"

#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>
#include <FastCache/Protocol/LiveStream.hpp>

#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// @param subject A subject.
    /// @return Its key, as a refusal names it.
    [[nodiscard]] std::string_view KeyOf(Wire::LiveSubject subject) noexcept
    {
        auto const* const row = Wire::FindLiveSubject(static_cast<std::uint8_t>(subject));
        return row != nullptr ? row->key : std::string_view { "unknown" };
    }

    /// A frame nothing here can read, saying why.
    /// @param note Why, for a person.
    /// @return The frame.
    [[nodiscard]] LiveFrame Unreadable(std::string note)
    {
        return LiveFrame { .kind = LiveFrameKind::Unreadable, .note = std::move(note) };
    }

    /// Why a reading did not decode, as the tail of a sentence that names the subject.
    /// @param fault What the codec said.
    /// @return The words.
    [[nodiscard]] std::string_view DescribeFault(StatsReadingFault fault) noexcept
    {
        switch (fault)
        {
            case StatsReadingFault::ForeignLayout:
                return "is laid out by a build other than this client's; upgrade the older of the two";
            case StatsReadingFault::Truncated:
                return "ends before its layout does";
            case StatsReadingFault::Malformed:
                return "carries a value its layout has no meaning for";
            case StatsReadingFault::TrailingBytes:
                return "carries bytes past the end of its layout";
        }
        return "cannot be read";
    }

    /// A reading's figures, or the frame saying why there are none.
    /// @param subject The subject, for the words.
    /// @param bytes One `EncodeStatsReading` field.
    /// @return The reading, or the unreadable frame.
    [[nodiscard]] std::expected<StatsReading, LiveFrame> DecodeReading(Wire::LiveSubject subject,
                                                                       std::span<std::byte const> bytes)
    {
        auto reading = DecodeStatsReading(bytes);
        if (!reading.has_value())
            return std::unexpected(Unreadable(std::format("a {} reading {}",
                                                          KeyOf(subject),
                                                          DescribeFault(reading.error()))));
        return *std::move(reading);
    }

    /// A snapshot's body, read in its subject's grammar (`LiveSnapshotView`).
    /// @param subject What the stream was asked for.
    /// @param snapshot The decoded fields.
    /// @return The reading frame, or the unreadable one.
    [[nodiscard]] LiveFrame ReadSnapshot(Wire::LiveSubject subject, Wire::LiveSnapshotView snapshot)
    {
        auto frame = LiveFrame { .kind = LiveFrameKind::Reading, .tick = snapshot.tick };
        switch (subject)
        {
            case Wire::LiveSubject::Cache: {
                auto reading = DecodeReading(subject, snapshot.body);
                if (!reading.has_value())
                    return std::move(reading).error();
                frame.reading = *std::move(reading);
                return frame;
            }
            case Wire::LiveSubject::Node: {
                auto const fields = WireFields::SplitExactly(snapshot.body, 2);
                if (!fields.has_value())
                    return Unreadable("a node reading is not the two fields a node snapshot carries");
                auto reading = DecodeReading(subject, (*fields)[0]);
                if (!reading.has_value())
                    return std::move(reading).error();
                auto status = Wire::DecodeNodeStatus((*fields)[1]);
                if (!status.has_value())
                    return Unreadable("a node reading carries a status this client cannot read");
                frame.reading = *std::move(reading);
                frame.nodeStatus = *std::move(status);
                return frame;
            }
            case Wire::LiveSubject::Fleet:
                frame.document = std::string { Wire::AsStringView(snapshot.body) };
                return frame;
        }
        return Unreadable("a reading for a subject this client does not know");
    }

    /// The grant, refused when it is not what was asked or not in this build's layout.
    /// @param subject What the stream was asked for.
    /// @param granted The decoded fields.
    /// @return The granted frame, or the unreadable one.
    [[nodiscard]] LiveFrame ReadGrant(Wire::LiveSubject subject, Wire::LiveSubscribedFields granted)
    {
        auto const asked = KeyOf(subject);
        if (granted.subject != subject)
            return Unreadable(std::format("a {} subscription was granted a {} stream",
                                          asked,
                                          KeyOf(granted.subject)));
        auto const expected = CarriesStatsReading(subject) ? StatsReadingLayout : std::uint64_t { 0 };
        if (granted.statsLayout != expected)
            return Unreadable(std::format("{} streams {} readings in layout {:#018x} and this client reads {:#018x}; "
                                          "upgrade the older of the two",
                                          granted.endpoint,
                                          asked,
                                          granted.statsLayout,
                                          expected));
        return LiveFrame { .kind = LiveFrameKind::Granted, .granted = std::move(granted) };
    }

    /// A push, read by its kind.
    /// @param subject What the stream was asked for.
    /// @param payload The push frame's payload.
    /// @return The frame.
    [[nodiscard]] LiveFrame ReadPush(Wire::LiveSubject subject, std::span<std::byte const> payload)
    {
        auto const push = Wire::DecodePush(payload);
        if (!push.has_value())
            return Unreadable("a push of a kind this client does not know");
        switch (push->kind)
        {
            case Wire::PushKind::Subscribed: {
                auto granted = Wire::DecodeLiveSubscribed(push->fields);
                return granted.has_value() ? ReadGrant(subject, *std::move(granted))
                                           : Unreadable("a grant whose fields do not decode");
            }
            case Wire::PushKind::Snapshot: {
                auto const snapshot = Wire::DecodeLiveSnapshot(push->fields);
                return snapshot.has_value() ? ReadSnapshot(subject, *snapshot)
                                            : Unreadable("a snapshot whose fields do not decode");
            }
            case Wire::PushKind::Event: {
                auto event = Wire::DecodeLiveEvent(push->fields);
                return event.has_value() ? LiveFrame { .kind = LiveFrameKind::Event, .event = *std::move(event) }
                                         : Unreadable("an event of a kind this client does not know");
            }
            case Wire::PushKind::Gap: {
                auto const gap = Wire::DecodeLiveGap(push->fields);
                return gap.has_value() ? LiveFrame { .kind = LiveFrameKind::Gap, .dropped = gap->dropped }
                                       : Unreadable("a gap whose fields do not decode");
            }
        }
        return Unreadable("a push of a kind this client does not know");
    }
} // namespace

LiveFrame ReadLiveFrame(CompileCacheWire::LiveSubject subject, NodeReply const& reply)
{
    switch (reply.status)
    {
        case Wire::Status::Push:
            return ReadPush(subject, reply.payload);
        case Wire::Status::Ok:
            return LiveFrame { .kind = LiveFrameKind::Ended, .note = "the server ended the stream" };
        case Wire::Status::Error:
            return LiveFrame { .kind = LiveFrameKind::Refused, .code = reply.code, .note = reply.detail };
        case Wire::Status::Miss:
        case Wire::Status::Progress:
            break;
    }
    return Unreadable(
        std::format("a reply with status {:#04x}, which no live-stats stream sends", static_cast<unsigned>(reply.status)));
}

NodeSubscription::NodeSubscription(DialTimeouts timeouts, Credential credential):
    _timeouts { timeouts },
    _credential { std::move(credential) }
{
}

NodeSubscription::~NodeSubscription() = default;

std::expected<void, ExchangeError> NodeSubscription::Open(Endpoint const& where,
                                                          CompileCacheWire::SubscribeRequest const& request)
{
    {
        // The stream being replaced goes first, under the lock `Leave` takes, so a leave never meets
        // an exchange being destroyed.
        auto const lock = std::scoped_lock { _mutex };
        _exchange.reset();
        if (_left)
            return std::unexpected(
                ExchangeError { .kind = ExchangeFailure::Transport, .detail = "the session left its stream" });
    }

    auto opened = NodeExchange::Open(where, _timeouts, _credential);
    if (!opened.has_value())
        return std::unexpected(std::move(opened).error());
    if (auto posted = (*opened)->Post(CompileCacheWire::EncodeSubscribeRequest(request)); !posted.has_value())
        return std::unexpected(std::move(posted).error());

    auto const lock = std::scoped_lock { _mutex };
    _exchange = *std::move(opened);
    // A leave that arrived while this dialled had nothing to half-close; it is owed now.
    if (_left)
        _exchange->ShutdownWrite();
    return {};
}

std::expected<NodeReply, ExchangeError> NodeSubscription::Read()
{
    // Unlocked: this thread is the only one that replaces the exchange (see `_mutex`).
    if (_exchange == nullptr)
        return std::unexpected(ExchangeError { .kind = ExchangeFailure::Transport, .detail = "no stream is open" });
    return _exchange->ReadFrame();
}

void NodeSubscription::ExpectEvery(std::chrono::milliseconds cadence)
{
    if (_exchange != nullptr)
        _exchange->SetReceiveDeadline(CompileCacheWire::LiveIdleBound(cadence));
}

void NodeSubscription::Leave() noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    _left = true;
    if (_exchange != nullptr)
        _exchange->ShutdownWrite();
}

} // namespace FastCache::Cli
