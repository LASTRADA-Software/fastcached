// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "MemcachedClient.hpp"
#include "NodeClient.hpp"
#include "RespClient.hpp"
#include "StatsSource.hpp"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace FastCache::Cli::Testing
{

/// @file ScriptedExchange.hpp
/// A programmed `IExchange`, shared by every verb test.
///
/// One helper rather than a fake per test file, for the reason the tree records about
/// `ScriptedSocket.hpp`: three private copies of one scripted socket carried the same
/// defect in two of them, found a day apart, because **a fake nothing exercises does
/// not report its own bugs**.
///
/// It is deliberately no more permissive than the thing it stands for. It records what
/// was sent so a test can assert the *request*, and it runs out of answers rather than
/// repeating the last one -- a fake that answers forever makes an extra round trip
/// invisible.

/// Build a bulk-string reply.
/// @param text The payload.
/// @return The reply.
[[nodiscard]] inline RespValue Bulk(std::string text)
{
    return RespValue { .type = RespType::BulkString, .text = std::move(text) };
}

/// Build a simple-string reply.
/// @param text The payload, without the leading `+`.
/// @return The reply.
[[nodiscard]] inline RespValue Simple(std::string text)
{
    return RespValue { .type = RespType::SimpleString, .text = std::move(text) };
}

/// Build an error reply.
/// @param text The payload, without the leading `-`.
/// @return The reply.
[[nodiscard]] inline RespValue Error(std::string text)
{
    return RespValue { .type = RespType::Error, .text = std::move(text) };
}

/// Build an integer reply.
/// @param value The value.
/// @return The reply.
[[nodiscard]] inline RespValue Integer(std::int64_t value)
{
    return RespValue { .type = RespType::Integer, .integer = value };
}

/// Build a null reply.
/// @return The reply.
[[nodiscard]] inline RespValue Nil()
{
    return RespValue { .type = RespType::Null };
}

/// Build an array reply.
/// @param items The elements.
/// @return The reply.
[[nodiscard]] inline RespValue Array(std::initializer_list<RespValue> items)
{
    return RespValue { .type = RespType::Array, .items = std::vector<RespValue> { items } };
}

/// Build a verbatim-string reply, which is what `INFO` answers under RESP3.
/// @param text The payload.
/// @return The reply.
[[nodiscard]] inline RespValue Verbatim(std::string text)
{
    return RespValue { .type = RespType::Verbatim, .text = std::move(text) };
}

/// The script for a `ScriptedExchange`, from a list of successful replies.
///
/// A free builder rather than a static factory on the class: `IExchange` deletes its
/// move constructor -- correctly, since an interface is held by reference -- so a
/// factory could not return one by value.
/// @param replies The replies, in call order.
/// @return The script.
[[nodiscard]] inline std::vector<std::expected<RespValue, ExchangeError>> Answers(std::initializer_list<RespValue> replies)
{
    std::vector<std::expected<RespValue, ExchangeError>> script;
    script.reserve(replies.size());
    for (auto const& reply: replies)
        script.emplace_back(reply);
    return script;
}

/// A one-entry script that fails instead of answering.
/// @param failure What kind of failure.
/// @param detail The specifics.
/// @return The script.
[[nodiscard]] inline std::vector<std::expected<RespValue, ExchangeError>> Failure(ExchangeFailure failure,
                                                                                  std::string detail)
{
    std::vector<std::expected<RespValue, ExchangeError>> script;
    script.emplace_back(std::unexpected(ExchangeError { .kind = failure, .detail = std::move(detail) }));
    return script;
}

/// An `IExchange` that answers from a script.
class ScriptedExchange final: public IExchange
{
  public:
    /// Answer each call from @p answers, in order.
    /// @param answers What to return, one per expected call.
    explicit ScriptedExchange(std::vector<std::expected<RespValue, ExchangeError>> answers):
        _answers { std::move(answers) }
    {
    }

    [[nodiscard]] std::expected<RespValue, ExchangeError> Call(std::span<std::string const> argv) override
    {
        _sent.emplace_back(argv.begin(), argv.end());
        if (_at >= _answers.size())
            // Running out is a failure of the TEST, not of the subject, and it must not
            // look like a transport problem -- so it is reported as a distinctive
            // malformed answer no production path produces.
            return std::unexpected(ExchangeError { .kind = ExchangeFailure::Malformed,
                                                   .detail = "ScriptedExchange: the script ran out of answers" });
        return _answers[_at++];
    }

    /// What was sent, in order.
    /// @return One entry per call.
    [[nodiscard]] std::vector<std::vector<std::string>> const& Sent() const noexcept
    {
        return _sent;
    }

    /// How many answers are left unused.
    ///
    /// A test that programmed two answers and used one has proved something different
    /// from what it meant to.
    /// @return The count.
    [[nodiscard]] std::size_t Unused() const noexcept
    {
        return _answers.size() - _at;
    }

  private:
    std::vector<std::expected<RespValue, ExchangeError>> _answers;
    std::size_t _at { 0 };
    std::vector<std::vector<std::string>> _sent;
};

/// An `IMemcachedExchange` that answers from a script of raw reply BYTES.
///
/// **Scripted in bytes rather than in parsed `McReply`s, deliberately.** A script of
/// pre-parsed replies would let a handler case pass over a codec that reads those bytes
/// differently, and the bytes are what a server sends -- so a case here exercises the
/// real parser and tests the pair. It also means every script in a verb test is written
/// in the server's own vocabulary and can be checked against `MemcachedText.cpp`.
///
/// Same two properties as `ScriptedExchange`: it records what was sent, and it runs out
/// rather than repeating, so an extra round trip cannot go unnoticed.
class ScriptedMemcachedExchange final: public IMemcachedExchange
{
  public:
    /// One scripted outcome: reply bytes, or the exchange failing instead.
    ///
    /// A failure is scriptable because *nothing answered* and *the server declined* are
    /// different exit codes and are fixed in different places, and a fake that can only
    /// answer leaves that distinction untested -- which is how a case comes to assert
    /// the running-out-of-script path under a name claiming to test a broken connection.
    using Outcome = std::expected<std::string, ExchangeError>;

    /// Answer each call with the next of @p replies, in order.
    /// @param replies Raw reply bytes, one per expected call.
    explicit ScriptedMemcachedExchange(std::vector<Outcome> replies):
        _replies { std::move(replies) }
    {
    }

    [[nodiscard]] std::expected<McReply, ExchangeError> Send(std::string_view request) override
    {
        _sent.emplace_back(request);
        if (_at >= _replies.size())
            return std::unexpected(ExchangeError { .kind = ExchangeFailure::Malformed,
                                                   .detail = "ScriptedMemcachedExchange: the script ran out of replies" });
        auto const& scripted = _replies[_at++];
        if (!scripted.has_value())
            return std::unexpected(scripted.error());
        auto parsed = ParseMemcachedReply(*scripted);
        if (parsed.state != McParseState::Complete)
            // A script this parser cannot read whole is a broken TEST. Reported as
            // malformed with the script's own diagnostic, so a mistyped fixture names
            // itself instead of presenting as the handler mishandling a good reply.
            return std::unexpected(ExchangeError { .kind = ExchangeFailure::Malformed,
                                                   .detail = "ScriptedMemcachedExchange: " + parsed.diagnostic });
        return std::move(parsed.reply);
    }

    /// What was sent, in order.
    /// @return One entry per call, verbatim.
    [[nodiscard]] std::vector<std::string> const& Sent() const noexcept
    {
        return _sent;
    }

    /// How many replies are left unused.
    /// @return The count.
    [[nodiscard]] std::size_t Unused() const noexcept
    {
        return _replies.size() - _at;
    }

  private:
    std::vector<Outcome> _replies;
    std::size_t _at { 0 };
    std::vector<std::string> _sent;
};

/// A scripted memcached outcome that fails instead of replying.
/// @param failure What kind of failure.
/// @param detail The specifics.
/// @return The outcome, for a `ScriptedMemcachedExchange` script.
[[nodiscard]] inline ScriptedMemcachedExchange::Outcome McFailure(ExchangeFailure failure, std::string detail)
{
    return std::unexpected(ExchangeError { .kind = failure, .detail = std::move(detail) });
}

/// An `INodeExchange` that answers from a script of raw reply FRAMES.
///
/// Scripted in bytes rather than in decoded `NodeReply`s, for the reason
/// `ScriptedMemcachedExchange` is: a script of pre-decoded replies lets a handler case
/// pass over a codec that reads those bytes differently, and the bytes are what a node
/// sends -- so a case here exercises the real decoder and tests the pair.
///
/// Same two properties as its siblings: it records what was sent, and it runs out
/// rather than repeating, so an extra round trip cannot go unnoticed.
class ScriptedNodeExchange final: public INodeExchange
{
  public:
    /// One scripted outcome: reply frame bytes, or the exchange failing instead.
    using Outcome = std::expected<std::vector<std::byte>, ExchangeError>;

    /// Answer each call with the next of @p replies, in order.
    /// @param replies Framed replies, one per expected call.
    /// @param endpoint What to call this connection in a diagnostic.
    explicit ScriptedNodeExchange(std::vector<Outcome> replies, std::string endpoint = "10.0.0.7:6674"):
        _replies { std::move(replies) },
        _endpoint { std::move(endpoint) }
    {
    }

    [[nodiscard]] std::expected<NodeReply, ExchangeError> Send(std::span<std::byte const> request) override
    {
        _sent.emplace_back(request.begin(), request.end());
        if (_at >= _replies.size())
            return std::unexpected(ExchangeError { .kind = ExchangeFailure::Malformed,
                                                   .detail = "ScriptedNodeExchange: the script ran out of replies" });
        auto const& scripted = _replies[_at++];
        if (!scripted.has_value())
            return std::unexpected(scripted.error());
        return DecodeNodeReply(*scripted);
    }

    [[nodiscard]] std::string_view Address() const override
    {
        return _endpoint;
    }

    /// What was sent, in order.
    /// @return One framed request per call, verbatim.
    [[nodiscard]] std::vector<std::vector<std::byte>> const& Sent() const noexcept
    {
        return _sent;
    }

    /// How many replies are left unused.
    /// @return The count.
    [[nodiscard]] std::size_t Unused() const noexcept
    {
        return _replies.size() - _at;
    }

  private:
    std::vector<Outcome> _replies;
    std::size_t _at { 0 };
    std::vector<std::vector<std::byte>> _sent;
    std::string _endpoint;
};

/// A scripted node outcome that fails instead of replying.
/// @param failure What kind of failure.
/// @param detail The specifics.
/// @return The outcome, for a `ScriptedNodeExchange` script.
[[nodiscard]] inline ScriptedNodeExchange::Outcome NodeFailure(ExchangeFailure failure, std::string detail)
{
    return std::unexpected(ExchangeError { .kind = failure, .detail = std::move(detail) });
}

/// A gatherer that answers from a fixed list.
class ScriptedGatherer final: public IStatsGatherer
{
  public:
    /// @param attempts What to report.
    explicit ScriptedGatherer(std::vector<StatsAttempt> attempts):
        _attempts { std::move(attempts) }
    {
    }

    [[nodiscard]] std::vector<StatsAttempt> Gather() override
    {
        ++_calls;
        return _attempts;
    }

    /// How many times it was asked.
    /// @return The count.
    [[nodiscard]] int Calls() const noexcept
    {
        return _calls;
    }

  private:
    std::vector<StatsAttempt> _attempts;
    int _calls { 0 };
};

} // namespace FastCache::Cli::Testing
