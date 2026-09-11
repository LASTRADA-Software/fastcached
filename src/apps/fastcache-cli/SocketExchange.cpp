// SPDX-License-Identifier: Apache-2.0
#include "SocketExchange.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/TcpClient.hpp>

#include <array>
#include <charconv>
#include <format>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    /// How much to ask the socket for at a time.
    constexpr std::size_t ReadChunkBytes = 16U * 1024U;

    /// One read from a socket, as a task `SyncRun` can drive.
    ///
    /// @param socket The socket. A pointer, never a reference: a coroutine parameter
    ///        that is a reference dangles, because the frame outlives the call
    ///        expression that made it.
    /// @param buffer Destination. Must be non-empty and must outlive the awaitable.
    /// @return Bytes read, `0` for EOF, or the failure.
    [[nodiscard]] Task<IoResult> ReadSome(ISocket* socket, std::span<std::byte> buffer)
    {
        co_return co_await socket->Read(buffer);
    }

    /// Convert bytes to a string without reinterpreting them as text.
    /// @param bytes The bytes.
    /// @return The same bytes in a `std::string`.
    [[nodiscard]] std::string AsChars(std::span<std::byte const> bytes)
    {
        std::string out;
        out.reserve(bytes.size());
        for (auto const byte: bytes)
            out.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
        return out;
    }

    /// Encode a string as bytes for the wire.
    /// @param text The text.
    /// @return The same characters as bytes.
    [[nodiscard]] std::vector<std::byte> AsBytes(std::string_view text)
    {
        std::vector<std::byte> out;
        out.reserve(text.size());
        for (auto const ch: text)
            out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
        return out;
    }

    /// Read one chunk from @p socket and append it to @p pending.
    ///
    /// **The EOF rule lives here and nowhere else.** Both wires' read loops need it and
    /// it is the subtle part: EOF means *this peer has finished sending*, which is a
    /// complete statement when a reply was already parsed and a truncation when one was
    /// not. The two sentences are separate because they are separate problems -- a
    /// server that closes without answering is usually an auth gate or a wrong port,
    /// one that closes mid-reply is a broken connection -- and a second copy of that
    /// distinction is a second thing to get wrong.
    ///
    /// @param socket The connected socket.
    /// @param pending The buffer to append to; its emptiness is what the EOF arms read.
    /// @return Nothing on success, or why the read did not happen.
    [[nodiscard]] std::expected<void, ExchangeError> FillMore(ISocket* socket, std::string& pending)
    {
        std::array<std::byte, ReadChunkBytes> chunk {};
        auto const got = SyncRun(ReadSome(socket, chunk));
        if (!got.has_value())
            return std::unexpected(ExchangeError {
                .kind = ExchangeFailure::Transport,
                .detail = std::format("the connection failed while reading the reply ({})", got.error().context) });
        if (*got == 0)
            return std::unexpected(
                ExchangeError { .kind = ExchangeFailure::Transport,
                                .detail = pending.empty() ? "the server closed the connection without answering"
                                                          : "the server closed the connection part-way through a reply" });
        pending += AsChars(std::span<std::byte const> { chunk.data(), *got });
        return {};
    }

    /// Dial and hand back the socket.
    /// @param endpoint Where.
    /// @param timeouts How long.
    /// @return The socket, or why there is none.
    [[nodiscard]] std::expected<std::unique_ptr<ISocket>, ExchangeError> Dial(Endpoint const& endpoint,
                                                                              DialTimeouts timeouts)
    {
        auto socket = SyncRun(ConnectTcp(endpoint.host, endpoint.port, timeouts.connect, timeouts.io));
        if (!socket.has_value())
            return std::unexpected(ExchangeError {
                .kind = ExchangeFailure::Unreachable,
                .detail = std::format("cannot reach {}:{} ({})", endpoint.host, endpoint.port, socket.error().context) });
        return std::move(*socket);
    }
} // namespace

SocketExchange::SocketExchange(std::unique_ptr<ISocket> socket, ParseLimits limits) noexcept:
    _socket { std::move(socket) },
    _limits { limits }
{
}

SocketExchange::~SocketExchange()
{
    if (_socket == nullptr)
        return;
    // A courtesy, and only that: the server frees the connection on EOF regardless.
    // Its reply is deliberately not read -- there is nothing to do with it, and
    // waiting for it would make closing able to block.
    auto const quit = std::vector<std::string> { "QUIT" };
    (void) SyncRun(SendAll(_socket.get(), EncodeCommand(quit)));
    _socket->Close();
}

std::expected<std::unique_ptr<SocketExchange>, ExchangeError> SocketExchange::Open(Endpoint const& endpoint,
                                                                                   DialTimeouts timeouts,
                                                                                   Credential const& credential,
                                                                                   ParseLimits const& limits)
{
    auto socket = Dial(endpoint, timeouts);
    if (!socket.has_value())
        return std::unexpected(socket.error());

    auto exchange = std::unique_ptr<SocketExchange> { new SocketExchange { std::move(*socket), limits } };
    if (!credential.Configured())
        return exchange;

    auto argv = std::vector<std::string> { "AUTH" };
    if (!credential.username.empty())
        argv.push_back(credential.username);
    argv.push_back(credential.secret);

    auto const reply = exchange->Call(argv);
    if (!reply.has_value())
        return std::unexpected(reply.error());

    if (IsError(*reply))
    {
        auto const code = ErrorCodeWord(reply->text);
        if (code == ErrorCode::WrongPass)
            return std::unexpected(
                ExchangeError { .kind = ExchangeFailure::Transport,
                                .detail = std::format("the server rejected the credential ({})", reply->text) });
        // Not about the credential: this server has no password set. Proceed
        // unauthenticated and say so, rather than refusing a command the server
        // would have answered.
        exchange->_advisories.push_back(
            std::format("a credential was configured and this server does not require one ({})", reply->text));
    }
    return exchange;
}

std::span<std::string const> SocketExchange::Advisories() const noexcept
{
    return _advisories;
}

std::expected<RespValue, ExchangeError> SocketExchange::Call(std::span<std::string const> argv)
{
    if (!SyncRun(SendAll(_socket.get(), EncodeCommand(argv))))
        return std::unexpected(ExchangeError { .kind = ExchangeFailure::Transport,
                                               .detail = "the connection failed while sending the command" });
    return ReadReply();
}

std::expected<RespValue, ExchangeError> SocketExchange::ReadReply()
{
    for (;;)
    {
        auto parsed = ParseReply(_pending, _limits);
        switch (parsed.state)
        {
            case ParseState::Complete:
                _pending.erase(0, parsed.consumed);
                return std::move(parsed.value);
            case ParseState::Malformed:
                return std::unexpected(
                    ExchangeError { .kind = ExchangeFailure::Malformed, .detail = std::move(parsed.diagnostic) });
            case ParseState::Incomplete:
            case ParseState::Last:
                break;
        }

        if (auto const filled = FillMore(_socket.get(), _pending); !filled.has_value())
            return std::unexpected(filled.error());
    }
}

MemcachedExchange::MemcachedExchange(std::unique_ptr<ISocket> socket, McParseLimits limits) noexcept:
    _socket { std::move(socket) },
    _limits { limits }
{
}

MemcachedExchange::~MemcachedExchange()
{
    if (_socket == nullptr)
        return;
    // `quit` is one of the two verbs this surface answers before authentication, so
    // this courtesy works against a password-protected daemon as well. Its reply is
    // not read, for the reason `~SocketExchange` gives: there is nothing to do with
    // one, and waiting would make closing able to block.
    (void) SyncRun(SendAll(_socket.get(), AsBytes(EncodeMemcachedCommand("quit", {}))));
    _socket->Close();
}

std::expected<std::unique_ptr<MemcachedExchange>, ExchangeError> MemcachedExchange::Open(Endpoint const& endpoint,
                                                                                         DialTimeouts timeouts,
                                                                                         McParseLimits const& limits)
{
    auto socket = Dial(endpoint, timeouts);
    if (!socket.has_value())
        return std::unexpected(socket.error());
    return std::unique_ptr<MemcachedExchange> { new MemcachedExchange { std::move(*socket), limits } };
}

std::expected<McReply, ExchangeError> MemcachedExchange::Send(std::string_view request)
{
    if (!SyncRun(SendAll(_socket.get(), AsBytes(request))))
        return std::unexpected(ExchangeError { .kind = ExchangeFailure::Transport,
                                               .detail = "the connection failed while sending the command" });

    for (;;)
    {
        auto parsed = ParseMemcachedReply(_pending, _limits);
        switch (parsed.state)
        {
            case McParseState::Complete:
                _pending.erase(0, parsed.consumed);
                return std::move(parsed.reply);
            case McParseState::Malformed:
                return std::unexpected(
                    ExchangeError { .kind = ExchangeFailure::Malformed, .detail = std::move(parsed.diagnostic) });
            case McParseState::Incomplete:
            case McParseState::Last:
                break;
        }

        if (auto const filled = FillMore(_socket.get(), _pending); !filled.has_value())
            return std::unexpected(filled.error());
    }
}

NodeExchange::NodeExchange(std::unique_ptr<ISocket> socket, std::string endpoint) noexcept:
    _socket { std::move(socket) },
    _endpoint { std::move(endpoint) }
{
}

NodeExchange::~NodeExchange()
{
    if (_socket != nullptr)
        _socket->Close();
}

std::expected<std::unique_ptr<NodeExchange>, ExchangeError> NodeExchange::Open(Endpoint const& endpoint,
                                                                               DialTimeouts timeouts,
                                                                               Credential const& credential)
{
    auto socket = Dial(endpoint, timeouts);
    if (!socket.has_value())
        return std::unexpected(socket.error());

    std::unique_ptr<NodeExchange> exchange { new NodeExchange { std::move(*socket),
                                                                std::format("{}:{}", endpoint.host, endpoint.port) } };
    if (!credential.Configured())
        return exchange;

    auto const reply =
        exchange->Send(CompileCacheWire::EncodeAuth({ .username = credential.username, .secret = credential.secret }));
    if (!reply.has_value())
        return std::unexpected(reply.error());

    if (reply->status == CompileCacheWire::Status::Ok)
        return exchange;

    // **`UnimplementedVerb` on AUTH alone falls through; every other refusal is about
    // the credential and is reported.** This is #283/#340's rule from the client side:
    // a surface that holds no policy is one a credential cannot help and must not
    // break, and a build with no `AUTH` verb at all is one an operator can still use.
    // Anything else -- a rejected secret, a malformed one -- is about what was
    // presented, and stopping there is what keeps a wrong token from looking like an
    // empty node.
    if (reply->code.has_value() && ClassifyRefusal(*reply->code) == NodeRefusalKind::Unimplemented)
    {
        exchange->_advisories.push_back(
            std::format("{} does not implement AUTH; continuing without presenting the credential", exchange->_endpoint));
        return exchange;
    }

    return std::unexpected(
        ExchangeError { .kind = ExchangeFailure::Transport, .detail = ExplainRefusal("auth", exchange->_endpoint, *reply) });
}

std::span<std::string const> NodeExchange::Advisories() const noexcept
{
    return _advisories;
}

std::expected<NodeReply, ExchangeError> NodeExchange::Send(std::span<std::byte const> request)
{
    if (!SyncRun(SendAll(_socket.get(), request)))
        return std::unexpected(ExchangeError { .kind = ExchangeFailure::Transport,
                                               .detail = "the connection failed while sending the request" });

    // **Loop to a TERMINAL status**, never to the first frame. A reply carries a status
    // byte and no kind, so the step-over-what-you-do-not-know property is REQUEST-side
    // only -- a reader that stops at frame one reads a liveness pulse as the answer.
    // What bounds how many arrive is the exchange's own I/O deadline, which every read
    // below is subject to.
    for (;;)
    {
        auto frame = ReadFrame();
        if (!frame.has_value())
            return frame;
        if (CompileCacheWire::IsTerminalStatus(frame->status))
            return frame;
    }
}

std::expected<NodeReply, ExchangeError> NodeExchange::ReadFrame()
{
    for (;;)
    {
        // The header first, then exactly what it declared. A peer-declared length sizes
        // nothing on its own: this waits for the bytes rather than reserving for them,
        // so a header claiming four gigabytes costs this process nothing at all.
        if (_pending.size() >= CompileCacheWire::ReplyHeaderSize)
        {
            auto const bytes =
                std::span<std::byte const> { reinterpret_cast<std::byte const*>(_pending.data()), _pending.size() };
            auto const header = CompileCacheWire::DecodeReplyHeader(bytes);
            if (!header.has_value())
                return std::unexpected(ExchangeError { .kind = ExchangeFailure::Malformed,
                                                       .detail = std::format("{} answered something that is not a 0xFC "
                                                                             "frame -- is that really a fastcache port?",
                                                                             _endpoint) });

            auto const whole = CompileCacheWire::ReplyHeaderSize + header->payloadLength;
            if (_pending.size() >= whole)
            {
                auto reply = DecodeNodeReply(bytes.subspan(0, whole));
                _pending.erase(0, whole);
                return reply;
            }
        }

        if (auto const filled = FillMore(_socket.get(), _pending); !filled.has_value())
            return std::unexpected(filled.error());
    }
}

std::expected<HttpResponse, ExchangeError> HttpGet(Endpoint const& endpoint,
                                                   std::string_view path,
                                                   DialTimeouts timeouts,
                                                   std::optional<std::string> const& bearer,
                                                   std::size_t maxBodyBytes)
{
    auto socket = Dial(endpoint, timeouts);
    if (!socket.has_value())
        return std::unexpected(socket.error());

    auto request = std::format(
        "GET {} HTTP/1.1\r\nHost: {}:{}\r\nAccept: */*\r\nConnection: close\r\n", path, endpoint.host, endpoint.port);
    if (bearer.has_value())
        request += std::format("Authorization: Bearer {}\r\n", *bearer);
    request += "\r\n";

    if (!SyncRun(SendAll(socket->get(), AsBytes(request))))
        return std::unexpected(ExchangeError { .kind = ExchangeFailure::Transport,
                                               .detail = "the connection failed while sending the request" });

    std::string raw;
    for (;;)
    {
        std::array<std::byte, ReadChunkBytes> chunk {};
        auto const got = SyncRun(ReadSome(socket->get(), chunk));
        if (!got.has_value())
            return std::unexpected(ExchangeError {
                .kind = ExchangeFailure::Transport,
                .detail = std::format("the connection failed while reading the response ({})", got.error().context) });
        if (*got == 0)
            break;
        raw += AsChars(std::span<std::byte const> { chunk.data(), *got });
        if (raw.size() > maxBodyBytes)
            return std::unexpected(
                ExchangeError { .kind = ExchangeFailure::Malformed,
                                .detail = std::format("the response exceeds the {}-byte cap", maxBodyBytes) });
    }

    auto const headEnd = raw.find("\r\n\r\n");
    if (headEnd == std::string::npos)
        return std::unexpected(
            ExchangeError { .kind = ExchangeFailure::Malformed, .detail = "the response has no header terminator" });

    auto const statusLineEnd = raw.find("\r\n");
    auto const statusLine = std::string_view { raw }.substr(0, statusLineEnd);
    auto const firstSpace = statusLine.find(' ');
    if (firstSpace == std::string_view::npos)
        return std::unexpected(
            ExchangeError { .kind = ExchangeFailure::Malformed, .detail = "the response has no status line" });

    auto const codeText = statusLine.substr(firstSpace + 1, 3);
    int status = 0;
    auto const [ptr, ec] = std::from_chars(codeText.data(), codeText.data() + codeText.size(), status);
    if (ec != std::errc {} || ptr != codeText.data() + codeText.size())
        return std::unexpected(ExchangeError { .kind = ExchangeFailure::Malformed,
                                               .detail = std::format("'{}' is not a status code", codeText) });

    return HttpResponse { .status = status, .body = raw.substr(headEnd + 4) };
}

} // namespace FastCache::Cli
