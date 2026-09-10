// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "RespClient.hpp"

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

} // namespace FastCache::Cli::Testing
