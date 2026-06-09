// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/KeyspaceNotifier.hpp>

#include <array>
#include <format>
#include <string>
#include <utility>

namespace FastCache
{

namespace
{

    /// Data-driven flag→bit lookup. One row per recognised letter; unknown
    /// letters fall through to a hard ConfigError so a typo in a YAML
    /// `notify-keyspace-events: AKE` doesn't silently disable notifications.
    ///
    /// `x` (Expired) is intentionally absent: the daemon has no expiry
    /// callback at the storage layer yet, so accepting `x` would advertise
    /// a capability we don't deliver. The bit itself stays defined in
    /// KeyspaceEvents so the follow-up `NotifyingStorage` decorator (see
    /// TODO.md) can add it back in one edit.
    struct FlagBit
    {
        char letter;
        std::uint32_t mask;
    };

    constexpr std::array<FlagBit, 5> FlagTable { {
        { .letter = 'K', .mask = KeyspaceEvents::Keyspace },
        { .letter = 'E', .mask = KeyspaceEvents::Keyevent },
        { .letter = 'g', .mask = KeyspaceEvents::Generic },
        { .letter = '$', .mask = KeyspaceEvents::String },
        { .letter = 'A', .mask = KeyspaceEvents::All },
    } };

} // namespace

std::expected<std::uint32_t, ConfigError> ParseKeyspaceEvents(std::string_view flags)
{
    std::uint32_t result = KeyspaceEvents::None;
    for (auto const c: flags)
    {
        bool matched = false;
        for (auto const& row: FlagTable)
        {
            if (row.letter == c)
            {
                result |= row.mask;
                matched = true;
                break;
            }
        }
        if (!matched)
        {
            return std::unexpected(ConfigError {
                .code = ConfigErrorCode::TypeMismatch,
                .source = "notify-keyspace-events",
                .line = 0,
                .field = "notify-keyspace-events",
                .context = std::format("unknown flag character '{}'", c),
            });
        }
    }
    return result;
}

KeyspaceNotifier::KeyspaceNotifier(IPubSubRegistry* pubsub, std::uint32_t classes) noexcept:
    _pubsub { pubsub },
    _classes { classes }
{
}

bool KeyspaceNotifier::IsEnabled() const noexcept
{
    // At least one class AND at least one publishing channel must be set; if
    // K and E are both off, the class flags are unreachable.
    auto constexpr ChannelMask = KeyspaceEvents::Keyspace | KeyspaceEvents::Keyevent;
    auto constexpr ClassMask = KeyspaceEvents::Generic | KeyspaceEvents::String | KeyspaceEvents::Expired;
    return (_classes & ChannelMask) != 0 && (_classes & ClassMask) != 0;
}

std::uint32_t KeyspaceNotifier::Classes() const noexcept
{
    return _classes;
}

bool KeyspaceNotifier::WouldPublish(std::uint32_t classFlag) const noexcept
{
    if (_pubsub == nullptr)
        return false;
    if ((_classes & classFlag) == 0)
        return false;
    return _pubsub->HasAnySubscribers();
}

void KeyspaceNotifier::OnEvent(std::uint32_t classFlag, std::string_view event, std::string_view key) const
{
    if (_pubsub == nullptr)
        return;
    if ((_classes & classFlag) == 0)
        return;
    // Subscriberless fast path: when nothing is subscribed to anything, skip
    // the std::format of the channel names entirely. This matters on hot-
    // write workloads where the operator enabled `notify-keyspace-events`
    // (e.g. "AKE") for the option to subscribe later but has no subscriber
    // attached right now. Without this probe every SET/DEL/EXPIRE would
    // allocate two std::strings just to look them up and find no subscriber.
    if (!_pubsub->HasAnySubscribers())
        return;
    if ((_classes & KeyspaceEvents::Keyspace) != 0)
    {
        auto const channel = std::format("__keyspace@0__:{}", key);
        (void) _pubsub->Publish(channel, event);
    }
    if ((_classes & KeyspaceEvents::Keyevent) != 0)
    {
        auto const channel = std::format("__keyevent@0__:{}", event);
        (void) _pubsub->Publish(channel, key);
    }
}

} // namespace FastCache
