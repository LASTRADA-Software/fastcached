// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Logger.hpp>

#include <format>
#include <ostream>
#include <string>

namespace FastCache
{

// -- ConsoleLogger ---------------------------------------------------------

ConsoleLogger::ConsoleLogger(std::ostream& sink, LogLevel initialMinLevel) noexcept:
    _sink { sink },
    _minLevel { initialMinLevel }
{
}

void ConsoleLogger::Log(LogLevel level, std::string_view message)
{
    if (level < _minLevel.load(std::memory_order_relaxed))
        return;

    auto const line = std::format("[{}] {}\n", ToStringView(level), message);
    std::lock_guard const lock { _writeMutex };
    _sink.write(line.data(), static_cast<std::streamsize>(line.size()));
}

LogLevel ConsoleLogger::MinLevel() const noexcept
{
    return _minLevel.load(std::memory_order_relaxed);
}

void ConsoleLogger::SetMinLevel(LogLevel level) noexcept
{
    _minLevel.store(level, std::memory_order_relaxed);
}

// -- CapturingLogger -------------------------------------------------------

CapturingLogger::CapturingLogger(LogLevel initialMinLevel) noexcept: _minLevel { initialMinLevel } {}

void CapturingLogger::Log(LogLevel level, std::string_view message)
{
    if (level < _minLevel.load(std::memory_order_relaxed))
        return;

    std::lock_guard const lock { _mutex };
    _records.push_back(Record { level, std::string { message } });
}

LogLevel CapturingLogger::MinLevel() const noexcept
{
    return _minLevel.load(std::memory_order_relaxed);
}

void CapturingLogger::SetMinLevel(LogLevel level) noexcept
{
    _minLevel.store(level, std::memory_order_relaxed);
}

std::vector<CapturingLogger::Record> CapturingLogger::Snapshot() const
{
    std::lock_guard const lock { _mutex };
    return _records;
}

void CapturingLogger::Clear()
{
    std::lock_guard const lock { _mutex };
    _records.clear();
}

} // namespace FastCache
