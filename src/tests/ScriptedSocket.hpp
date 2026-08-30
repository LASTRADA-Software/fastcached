// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/ISocket.hpp>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace FastCache::Testing
{

/// An `ISocket` that replays a fixed reply stream and records what was written.
///
/// ## Why this is one file rather than three
///
/// It existed three times — `ScriptedTcpClient` in `CacheProtocol_test.cpp`,
/// `ScriptedScheduler` in `WorkerProtocol_test.cpp` and `ScriptedSocket` in
/// `AuthRefusalContract_test.cpp` — and **the same defect was live in two of
/// them**: `WriteVectored` answering `0`, which is a short write and which
/// `SendAll` correctly treats as a failure. Nothing had ever tripped on it,
/// because `CacheProtocol::Exchange` is the first caller to send a vectored write
/// and no case reached it. The second copy was found and fixed while writing
/// #340; the first was still carrying the identical defect a day later and was
/// found only by reading the three side by side (#362).
///
/// That is the argument. A fake nothing exercises does not report its own bugs,
/// so the copies drift silently and the drift is discovered by whichever test
/// happens to walk into it.
///
/// ## Why `src/tests/` rather than beside one of its callers
///
/// `AuthRefusalContract_test.cpp` carried a comment saying its copy was
/// deliberate, because the point of that file is that the launcher and the node
/// share *nothing* but the wire, and a fixture reaching across would be the
/// coupling the test denies. That argument is about the two **binaries**, and it
/// still holds: this header is neither binary's — it is test infrastructure both
/// borrow, exactly as they both borrow `Unwrap.hpp` — and it includes nothing
/// from either app.
///
/// ## What it is not
///
/// A script, not a server. It answers with bytes decided before the call, so it
/// cannot model a peer whose reply depends on what it was sent. Where a *live*
/// responder is wanted, `Net/InMemoryTransport` is the tool.
class ScriptedSocket final: public ISocket
{
  public:
    /// @param replies The bytes the peer returns, in order. Running out is EOF.
    explicit ScriptedSocket(std::vector<std::byte> replies):
        _replies { std::move(replies) }
    {
    }

    /// Accept everything, and record it.
    /// @param bytes What the caller wrote.
    /// @return The full count; a scripted peer never applies back-pressure.
    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> bytes) override
    {
        ++_sendCalls;
        _trace.push_back('S');
        _sent.insert(_sent.end(), bytes.begin(), bytes.end());
        return IoAwaitable { IoResult { bytes.size() } };
    }

    /// Hand back the next slice of the script.
    /// @param buffer Where to put it.
    /// @return How much was copied; `0` once the script is spent, which is EOF.
    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        // Recorded before the short-read check: an attempted read is still a read
        // for the purpose of "did this client wait for a reply mid-conversation".
        if (_trace.empty() || _trace.back() != 'R')
            _trace.push_back('R');
        auto const take = std::min(_replies.size() - _cursor, buffer.size());
        // Zero is EOF, which is how `RecvExactly` learns the peer ran out.
        std::copy_n(_replies.begin() + static_cast<std::ptrdiff_t>(_cursor), take, buffer.begin());
        _cursor += take;
        return IoAwaitable { IoResult { take } };
    }

    /// Accept every segment, and report the total.
    ///
    /// **Reports what it accepted.** Two of the three copies this replaces answered
    /// `0` here — a short write, and therefore a failure — which was invisible until
    /// a test drove a vectored write through them. See the class comment.
    /// @param segments The buffers, in order.
    /// @return Their combined length.
    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> /*keepAlive*/ = {}) override
    {
        ++_sendCalls;
        _trace.push_back('S');
        std::size_t total = 0;
        for (auto const& segment: segments)
        {
            _sent.insert(_sent.end(), segment.begin(), segment.end());
            total += segment.size();
        }
        return IoAwaitable { IoResult { total } };
    }

    void Close() noexcept override
    {
        _closed = true;
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _closed;
    }

    [[nodiscard]] std::string PeerAddress() const override
    {
        return "scripted";
    }

    /// Everything the caller wrote, concatenated.
    /// @return The bytes, in write order.
    [[nodiscard]] std::vector<std::byte> const& Sent() const noexcept
    {
        return _sent;
    }

    /// How many reply bytes the caller consumed.
    /// @return The read cursor.
    [[nodiscard]] std::size_t Cursor() const noexcept
    {
        return _cursor;
    }

    /// How many separate write calls the caller made.
    /// @return The count.
    [[nodiscard]] std::size_t SendCalls() const noexcept
    {
        return _sendCalls;
    }

    /// The order in which the caller wrote and read, collapsed to one character
    /// per run: `"SSRR"` is two writes then two reads, `"SRSR"` a round trip
    /// between them.
    ///
    /// This, not the write count, is what pipelining actually means. Two `SendAll`
    /// calls back to back are exactly as pipelined as one concatenated buffer —
    /// neither waits for a reply — and demanding a single write would force the
    /// caller to COPY a frame carrying a whole object file just to satisfy a test.
    /// @return The trace.
    [[nodiscard]] std::string const& Trace() const noexcept
    {
        return _trace;
    }

  private:
    std::vector<std::byte> _replies;
    std::vector<std::byte> _sent;
    std::size_t _cursor { 0 };
    std::size_t _sendCalls { 0 };
    std::string _trace;
    bool _closed { false };
};

/// A socket on which every call fails.
///
/// A separate type rather than a mode of `ScriptedSocket`, because "every call
/// fails" is a different contract and not a script with no entries — and because
/// the version this replaces got that exactly wrong, returning *success* from the
/// one method whose whole purpose is to fail.
class FailingSocket final: public ISocket
{
  public:
    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> /*bytes*/) override
    {
        return IoAwaitable { std::unexpected(Reset("scripted write failure")) };
    }

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> /*buffer*/) override
    {
        return IoAwaitable { std::unexpected(Reset("scripted read failure")) };
    }

    /// Fails the way the other two do, rather than reporting a zero-byte success.
    ///
    /// `SendAll` reads both as failure, so today the two are indistinguishable — but
    /// this class's whole contract is "every call fails", and a caller that asks
    /// `has_value()` (which is how a transport failure is told from a short write)
    /// would have been handed a success by the one method that did not honour it.
    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> /*segments*/,
                                            std::shared_ptr<void const> /*keepAlive*/ = {}) override
    {
        return IoAwaitable { std::unexpected(Reset("scripted write failure")) };
    }

    void Close() noexcept override {}

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return false;
    }

    [[nodiscard]] std::string PeerAddress() const override
    {
        return "failing";
    }

  private:
    /// The one failure this fake reports, named once.
    /// @param context What the caller was attempting.
    /// @return A connection-reset error carrying that context.
    [[nodiscard]] static NetError Reset(std::string context)
    {
        return NetError { .code = NetErrorCode::ConnReset, .systemCode = 0, .context = std::move(context) };
    }
};

/// Concatenate reply frames into one scripted stream.
/// @param frames The frames, in the order the peer would send them.
/// @return Their concatenation.
[[nodiscard]] inline std::vector<std::byte> Replies(std::initializer_list<std::vector<std::byte>> frames)
{
    std::vector<std::byte> out;
    for (auto const& frame: frames)
        out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

} // namespace FastCache::Testing
