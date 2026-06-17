// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Severity level for log records. Numerically ordered low → high.
/// Used both as a per-record tag and as a per-logger threshold filter.
enum class LogLevel : std::uint8_t
{
    Trace = 0, ///< Per-byte / per-step traces. Very loud.
    Debug = 1, ///< Useful during development.
    Info = 2,  ///< Default production level.
    Warn = 3,  ///< Recoverable anomaly.
    Error = 4, ///< Operation failed.
    Fatal = 5, ///< Process is about to exit.
};

/// Stable string name for a LogLevel, suitable for log formatting.
/// @param level Level to translate.
/// @return Static string view; never empty.
[[nodiscard]] constexpr std::string_view ToStringView(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Fatal:
            return "FATAL";
    }
    return "?";
}

namespace Detail
{
    /// Thread-local "source" tag (typically a bracketed client IP like
    /// "[203.0.113.7]") consulted by trace logging that runs *below* the
    /// connection layer — chiefly TracingStorage — so a storage trace line can
    /// name the client that triggered it. The connection layer cannot pass the
    /// source down the synchronous IStorage call chain without threading it
    /// through every method, so it is published here instead.
    ///
    /// Contract: a protocol handler assigns this immediately before a
    /// *synchronous* storage call (the engine calls run in co_await-free blocks),
    /// so the value is current when TracingStorage reads it. It is a view: the
    /// referenced string lives in the connection's coroutine frame and outlives
    /// the storage call. Empty means "no source" — the line is logged unprefixed.
    /// Each command assigns it afresh, so a connection never inherits another's.
    inline thread_local std::string_view StorageSourceTag {};
} // namespace Detail

/// Logger abstraction. Implementations are expected to be O(memcpy) on the
/// caller's thread — protocol/reactor code logs inline. Asynchronous flushing
/// is a post-MVP add-on; the contract is "writes return promptly."
///
/// Thread safety: implementations must allow concurrent calls to Log() from
/// any thread.
class ILogger
{
  public:
    ILogger() = default;
    ILogger(ILogger const&) = delete;
    ILogger(ILogger&&) = delete;
    ILogger& operator=(ILogger const&) = delete;
    ILogger& operator=(ILogger&&) = delete;
    virtual ~ILogger() = default;

    /// Emit a single pre-formatted log record at the given level.
    /// @param level Severity of this record.
    /// @param message Formatted message body. Newlines are appended by the sink.
    virtual void Log(LogLevel level, std::string_view message) = 0;

    /// Filter threshold. Records below this level are dropped before formatting.
    /// @return Current minimum level that will be emitted.
    [[nodiscard]] virtual LogLevel MinLevel() const noexcept = 0;

    /// Update the filter threshold. May be invoked from any thread (e.g., by
    /// the config reloader after SIGHUP).
    /// @param level New minimum level.
    virtual void SetMinLevel(LogLevel level) noexcept = 0;

    /// Convenience: format-and-log only if the level passes the filter.
    /// Variadic so callers can write `logger.Logf(LogLevel::Info, "n={}", 42);`
    /// without paying for std::format() when the filter rejects the level.
    /// @tparam Args Argument types for std::format.
    /// @param level Severity.
    /// @param fmt std::format-compatible format string.
    /// @param args Values to substitute.
    template <typename... Args>
    void Logf(LogLevel level, std::format_string<Args...> fmt, Args&&... args)
    {
        if (level < MinLevel())
            return;
        Log(level, std::format(fmt, std::forward<Args>(args)...));
    }
};

/// Whether connection-scoped log lines carry the client source (its IP).
/// Modelled as a scoped enum rather than a bare bool so call sites read
/// `LogSource::Yes` instead of an unlabelled `true`.
enum class LogSource : std::uint8_t
{
    No,  ///< Do not prefix the source; log lines are unchanged.
    Yes, ///< Prefix each connection log line with the client IP.
};

/// Logging decorator that prefixes a fixed source tag onto every record before
/// delegating to an inner ILogger, then forwards the level-filter calls
/// unchanged. Used per connection so each client's log lines carry its IP
/// (`[INFO] [203.0.113.7] ...`) without every call site repeating the IP. An
/// empty source forwards the message untouched, so a connection with no known
/// peer address (the in-memory transport) costs nothing extra.
///
/// Thread safety: forwards to the inner logger, which owns synchronisation; the
/// decorator holds only immutable state after construction.
class SourceLogger final: public ILogger
{
  public:
    /// Wrap an inner logger with a source tag.
    /// @param inner Logger every record is forwarded to; must outlive this.
    /// @param source Already-bracketed source tag (e.g. "[203.0.113.7]"), or ""
    ///        to forward records unchanged.
    SourceLogger(ILogger& inner, std::string source) noexcept;

    void Log(LogLevel level, std::string_view message) override;
    [[nodiscard]] LogLevel MinLevel() const noexcept override;
    void SetMinLevel(LogLevel level) noexcept override;

  private:
    ILogger& _inner;
    std::string _source;
};

/// Null logger that drops every record. Useful as the default in tests so a
/// silent test fixture does not need a logger argument plumbed through.
class NullLogger final: public ILogger
{
  public:
    void Log(LogLevel /*level*/, std::string_view /*message*/) override {}
    [[nodiscard]] LogLevel MinLevel() const noexcept override
    {
        return LogLevel::Fatal;
    }
    void SetMinLevel(LogLevel /*level*/) noexcept override {}
};

/// Synchronous logger that writes to a std::ostream (typically std::cerr).
/// Thread-safe via a mutex around the stream write. Format without timestamps:
///   "[LEVEL] message\n"
/// With timestamps enabled:
///   "2026-06-05T14:23:01.123456Z [LEVEL] message\n"
class ConsoleLogger final: public ILogger
{
  public:
    /// Construct over a stream reference whose lifetime exceeds this logger.
    /// @param sink Output stream (e.g., std::cerr).
    /// @param initialMinLevel Initial filter threshold.
    /// @param timestamps When true, prefix each line with an ISO 8601 UTC timestamp.
    explicit ConsoleLogger(std::ostream& sink, LogLevel initialMinLevel = LogLevel::Info, bool timestamps = false) noexcept;

    void Log(LogLevel level, std::string_view message) override;
    [[nodiscard]] LogLevel MinLevel() const noexcept override;
    void SetMinLevel(LogLevel level) noexcept override;

  private:
    std::ostream& _sink;
    std::atomic<LogLevel> _minLevel;
    std::mutex _writeMutex;
    bool _timestamps;
};

/// Capturing logger that stores every emitted record in memory. Designed for
/// tests that need to assert on log output.
class CapturingLogger final: public ILogger
{
  public:
    struct Record
    {
        LogLevel level;
        std::string message;
    };

    /// Construct with the given initial threshold.
    /// @param initialMinLevel Initial filter threshold.
    explicit CapturingLogger(LogLevel initialMinLevel = LogLevel::Trace) noexcept;

    void Log(LogLevel level, std::string_view message) override;
    [[nodiscard]] LogLevel MinLevel() const noexcept override;
    void SetMinLevel(LogLevel level) noexcept override;

    /// Snapshot the records captured so far.
    /// @return Copy of the in-memory record vector.
    [[nodiscard]] std::vector<Record> Snapshot() const;

    /// Clear the captured records. Used between test cases sharing a fixture.
    void Clear();

  private:
    std::atomic<LogLevel> _minLevel;
    mutable std::mutex _mutex;
    std::vector<Record> _records;
};

} // namespace FastCache
