// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
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
enum class LogLevel : unsigned
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
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?";
}

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

/// Null logger that drops every record. Useful as the default in tests so a
/// silent test fixture does not need a logger argument plumbed through.
class NullLogger final: public ILogger
{
  public:
    void Log(LogLevel /*level*/, std::string_view /*message*/) override {}
    [[nodiscard]] LogLevel MinLevel() const noexcept override { return LogLevel::Fatal; }
    void SetMinLevel(LogLevel /*level*/) noexcept override {}
};

/// Synchronous logger that writes to a std::ostream (typically std::cerr).
/// Thread-safe via a mutex around the stream write. Format:
///   "[LEVEL] message\n"
/// Timestamps and ANSI colours are intentionally omitted at this layer — the
/// daemon can wrap this logger to add timestamps without making the sink
/// itself more complex.
class ConsoleLogger final: public ILogger
{
  public:
    /// Construct over a stream reference whose lifetime exceeds this logger.
    /// @param sink Output stream (e.g., std::cerr).
    /// @param initialMinLevel Initial filter threshold.
    explicit ConsoleLogger(std::ostream& sink, LogLevel initialMinLevel = LogLevel::Info) noexcept;

    void Log(LogLevel level, std::string_view message) override;
    [[nodiscard]] LogLevel MinLevel() const noexcept override;
    void SetMinLevel(LogLevel level) noexcept override;

  private:
    std::ostream& _sink;
    std::atomic<LogLevel> _minLevel;
    std::mutex _writeMutex;
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
