// SPDX-License-Identifier: Apache-2.0
#include "ReachabilityMemo.hpp"
#include "Stats.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <ranges>
#include <span>
#include <system_error>
#include <utility>

namespace FastCache::Cc
{

namespace
{
    /// The first line of every memo this build writes; anything else is read as empty.
    constexpr std::string_view Header = "fastcache-cc reachability 1";

    [[nodiscard]] std::int64_t UnixMs(std::chrono::system_clock::time_point at) noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
    }

    [[nodiscard]] MemoKindRow const& RowOf(MemoKind kind) noexcept
    {
        return MemoKinds[static_cast<std::size_t>(kind)];
    }

    [[nodiscard]] std::optional<MemoKind> KindOf(std::string_view token) noexcept
    {
        for (auto const& row: MemoKinds)
            if (row.token == token)
                return row.kind;
        return std::nullopt;
    }

    /// One `<token> <unix-ms> <endpoint>` line, or nothing when it is not one this build reads.
    [[nodiscard]] std::optional<MemoEntry> ParseLine(std::string_view line)
    {
        auto const firstSpace = line.find(' ');
        if (firstSpace == std::string_view::npos)
            return std::nullopt;
        auto const kind = KindOf(line.substr(0, firstSpace));
        auto const rest = line.substr(firstSpace + 1);
        auto const secondSpace = rest.find(' ');
        if (!kind.has_value() || secondSpace == std::string_view::npos)
            return std::nullopt;
        std::int64_t stamp = 0;
        auto const digits = rest.substr(0, secondSpace);
        auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), stamp);
        auto const endpoint = rest.substr(secondSpace + 1);
        // A negative stamp is not one this build writes. Dropping the line reads it as
        // nothing remembered, which is the direction that dials.
        if (error != std::errc {} || end != digits.data() + digits.size() || stamp < 0 || endpoint.empty()
            || endpoint.contains(' '))
            return std::nullopt;
        return MemoEntry { .kind = *kind, .stampedUnixMs = stamp, .endpoint = std::string { endpoint } };
    }
} // namespace

FileMemoStore::FileMemoStore(std::filesystem::path file, IAtomicWriteFiles& files):
    _file { std::move(file) },
    _files { files }
{
}

std::optional<std::string> FileMemoStore::Read() const
{
    if (_file.empty())
        return std::nullopt;
    return ReadFileShared(_file);
}

bool FileMemoStore::Write(std::string_view text)
{
    if (_file.empty())
        return false;
    // Many launchers write at once, and a reader must see a whole memo or the previous
    // one, never half of either -- the fingerprint cache's reason, and its writer.
    return WriteFileAtomically(_file, std::as_bytes(std::span { text }), CurrentProcessId(), _files).has_value();
}

ReachabilityMemo ReachabilityMemo::Parse(std::string_view text)
{
    ReachabilityMemo memo;
    auto const asText = [](auto const& piece) {
        return std::string_view { piece.begin(), piece.end() };
    };
    auto lines = text | std::views::split('\n');
    if (lines.begin() == lines.end() || asText(*lines.begin()) != Header)
        return memo;
    for (auto const& piece: lines | std::views::drop(1))
        if (auto entry = ParseLine(asText(piece)); entry.has_value())
            memo._entries.push_back(*std::move(entry));
    return memo;
}

ReachabilityMemo ReachabilityMemo::Load(IMemoStore const& store)
{
    return Parse(store.Read().value_or(""));
}

std::string ReachabilityMemo::Serialize() const
{
    auto text = std::format("{}\n", Header);
    for (auto const& entry: _entries)
        text += std::format("{} {} {}\n", RowOf(entry.kind).token, entry.stampedUnixMs, entry.endpoint);
    return text;
}

void ReachabilityMemo::SaveIfChanged(IMemoStore& store)
{
    if (!_changed)
        return;
    // A failed write is not retried or reported: the next launcher dials once more,
    // which is what it would have done had this memo never existed.
    (void) store.Write(Serialize());
    _changed = false;
}

MemoAge ReachabilityMemo::AgeOf(MemoEntry const& entry, std::chrono::system_clock::time_point now) noexcept
{
    // The one subtraction, and the comparison comes FIRST. A wall clock steps both
    // ways, so a stamp after now is an outcome of its own rather than a small negative
    // age that would read as fresh. And the stamp can be anything a file spells, so
    // subtracting before comparing overflows for one near the minimum. Once the stamp
    // is known not to be after now, the difference is non-negative and below 2^64, so
    // it is taken in unsigned arithmetic, which is defined for every pair.
    auto const nowMs = UnixMs(now);
    if (entry.stampedUnixMs > nowMs)
        return MemoAge::FromTheFuture;
    auto const age = static_cast<std::uint64_t>(nowMs) - static_cast<std::uint64_t>(entry.stampedUnixMs);
    if (std::cmp_greater_equal(age, RowOf(entry.kind).ttl.count()))
        return MemoAge::Expired;
    return MemoAge::Fresh;
}

bool ReachabilityMemo::Remembers(MemoKind kind, std::string_view endpoint, std::chrono::system_clock::time_point now) const
{
    return std::ranges::any_of(_entries, [&](MemoEntry const& entry) {
        return entry.kind == kind && entry.endpoint == endpoint && AgeOf(entry, now) == MemoAge::Fresh;
    });
}

std::vector<std::string> ReachabilityMemo::Fresh(MemoKind kind, std::chrono::system_clock::time_point now) const
{
    // Collected by hand rather than with `std::ranges::to`, which clang does not
    // compile against libstdc++ 14 -- one of the standard libraries CI builds with.
    std::vector<MemoEntry const*> fresh;
    for (auto const& entry: _entries)
        if (entry.kind == kind && AgeOf(entry, now) == MemoAge::Fresh)
            fresh.push_back(&entry);
    std::ranges::stable_sort(fresh, std::ranges::greater {}, [](MemoEntry const* entry) { return entry->stampedUnixMs; });
    std::vector<std::string> endpoints;
    endpoints.reserve(fresh.size());
    for (auto const* entry: fresh)
        endpoints.push_back(entry->endpoint);
    return endpoints;
}

void ReachabilityMemo::Note(MemoKind kind, std::string_view endpoint, std::chrono::system_clock::time_point now)
{
    std::erase_if(_entries, [&](MemoEntry const& entry) {
        return entry.kind == kind && (AgeOf(entry, now) != MemoAge::Fresh || entry.endpoint == endpoint);
    });
    _entries.push_back(MemoEntry { .kind = kind, .stampedUnixMs = UnixMs(now), .endpoint = std::string { endpoint } });

    auto const ofKind = [kind](MemoEntry const& entry) {
        return entry.kind == kind;
    };
    while (std::cmp_greater(std::ranges::count_if(_entries, ofKind), RowOf(kind).capacity))
    {
        auto kept = _entries | std::views::filter(ofKind);
        auto const oldest = std::ranges::min_element(kept, std::ranges::less {}, &MemoEntry::stampedUnixMs);
        _entries.erase(oldest.base());
    }
    _changed = true;
}

void ReachabilityMemo::Forget(MemoKind kind, std::string_view endpoint)
{
    auto const erased =
        std::erase_if(_entries, [&](MemoEntry const& entry) { return entry.kind == kind && entry.endpoint == endpoint; });
    if (erased > 0)
        _changed = true;
}

void ReachabilityMemo::Absorb(DispatchResult const& result,
                              std::string_view configuredScheduler,
                              std::chrono::system_clock::time_point now)
{
    // Only a dial that made NO connection (`MarksUnreachable`, the one verdict the
    // compile leg's worker is judged by too), and only to the scheduler this launcher
    // is configured with: a leader it was redirected to is a machine the configured one
    // vouched for by answering, and a peer that was reached and then failed is up.
    if (MarksUnreachable(result.leaseTransport) && result.leaseEndpoint == configuredScheduler)
        Note(MemoKind::SchedulerUnreached, configuredScheduler, now);
    else if (result.leaseTransport == TransportFailure::None && !result.leaseEndpoint.empty())
        Forget(MemoKind::SchedulerUnreached, configuredScheduler);

    if (!result.unreachedWorker.empty())
        Note(MemoKind::WorkerUnreached, result.unreachedWorker, now);
    if (result.Ran() && !result.workerEndpoint.empty())
        Forget(MemoKind::WorkerUnreached, result.workerEndpoint);
}

bool ReachabilityMemo::Changed() const noexcept
{
    return _changed;
}

std::span<MemoEntry const> ReachabilityMemo::Entries() const noexcept
{
    return _entries;
}

std::filesystem::path ReachabilityMemoPath()
{
    auto const dir = StateDirectory();
    return dir.empty() ? std::filesystem::path {} : dir / "reachability.memo";
}

} // namespace FastCache::Cc
