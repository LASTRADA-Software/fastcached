// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliEndpoint.hpp"
#include "MemcachedClient.hpp"
#include "RespClient.hpp"

#include <FastCache/Net/ISocket.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cli
{

/// A RESP connection over one blocking TCP socket.
///
/// **Blocking by type, driven by `SyncRun`.** That is sound only over a blocking
/// socket, which resolves every awaitable inline -- `Net/TcpClient`'s header says so,
/// and a reactor socket here would suspend with nothing to resume it. One connection
/// per invocation, held for the life of the command, which is what later lets the REPL
/// keep `MULTI`/`WATCH` state across commands.
class SocketExchange final: public IExchange
{
  public:
    /// Dial @p endpoint and present @p credential.
    ///
    /// **An AUTH refusal that is about the SERVER rather than the credential is an
    /// advisory, not a failure.** A daemon with no password answers `AUTH` with an
    /// error, and treating that as fatal is how a client with `FASTCACHE_TOKEN` set
    /// gets a permanent failure against a server that never needed it -- the shape
    /// that once cost a token-configured launcher a permanent 0% hit rate reading as
    /// a cold cache. A `WRONGPASS`, which *is* about the credential, is fatal. Either
    /// way the operator is told, because they asked for authentication.
    ///
    /// @param endpoint Where to dial.
    /// @param timeouts How long to wait.
    /// @param credential What to present; nothing is presented when unconfigured.
    /// @param limits Caps this side imposes on replies.
    /// @return The open connection, or why there is none.
    [[nodiscard]] static std::expected<std::unique_ptr<SocketExchange>, ExchangeError> Open(Endpoint const& endpoint,
                                                                                            DialTimeouts timeouts,
                                                                                            Credential const& credential,
                                                                                            ParseLimits const& limits = {});

    ~SocketExchange() override;
    SocketExchange(SocketExchange const&) = delete;
    SocketExchange(SocketExchange&&) = delete;
    SocketExchange& operator=(SocketExchange const&) = delete;
    SocketExchange& operator=(SocketExchange&&) = delete;

    [[nodiscard]] std::expected<RespValue, ExchangeError> Call(std::span<std::string const> argv) override;

    /// Remarks gathered while opening the connection, for stderr.
    ///
    /// Non-empty when the credential was configured and not honoured. Read once by
    /// `main` and folded into the command's own advisories, so the operator is told
    /// whichever verb they ran.
    /// @return The remarks, in the order they were made.
    [[nodiscard]] std::span<std::string const> Advisories() const noexcept;

  private:
    /// @param socket The connected socket.
    /// @param limits Caps this side imposes.
    SocketExchange(std::unique_ptr<ISocket> socket, ParseLimits limits) noexcept;

    /// Read until one whole reply is available, then consume it.
    /// @return The reply, or why there is none.
    [[nodiscard]] std::expected<RespValue, ExchangeError> ReadReply();

    std::unique_ptr<ISocket> _socket;
    ParseLimits _limits;
    /// Bytes read from the socket and not yet consumed by a reply.
    ///
    /// Held across calls because a reply can arrive in the same segment as its
    /// predecessor's tail; dropping the remainder would make the next command read
    /// this one's leftovers.
    std::string _pending;
    std::vector<std::string> _advisories;
};

/// A memcached-text connection over one blocking TCP socket.
///
/// The same shape as `SocketExchange`, over the other wire, and it shares that class's
/// read loop rather than carrying a second copy of the EOF rule.
///
/// **It takes no credential, and that is the design rather than an omission.** There is
/// no AUTH verb on this protocol: under `--requirepass` the server answers every verb
/// but `version` and `quit` with `CLIENT_ERROR authentication required` and then ends
/// the session (`MemcachedText.cpp`, deliberately -- a refused storage command leaves
/// its data block unread, so continuing would parse those bytes as the next command).
/// So there is nothing an exchange could do with a credential, and the refusal has to
/// happen ABOVE this class, by name and before dialling; `CliVerbs.hpp`'s
/// `credentialRefusal` column is where that is said.
///
/// A `Send` after the server has ended a session fails as a transport error, which is
/// the truth: the connection is gone. Nothing retries it, because the second attempt
/// would be refused for the same reason as the first.
class MemcachedExchange final: public IMemcachedExchange
{
  public:
    /// Dial @p endpoint.
    /// @param endpoint Where to dial.
    /// @param timeouts How long to wait.
    /// @param limits Caps this side imposes on replies.
    /// @return The open connection, or why there is none.
    [[nodiscard]] static std::expected<std::unique_ptr<MemcachedExchange>, ExchangeError> Open(
        Endpoint const& endpoint, DialTimeouts timeouts, McParseLimits const& limits = {});

    ~MemcachedExchange() override;
    MemcachedExchange(MemcachedExchange const&) = delete;
    MemcachedExchange(MemcachedExchange&&) = delete;
    MemcachedExchange& operator=(MemcachedExchange const&) = delete;
    MemcachedExchange& operator=(MemcachedExchange&&) = delete;

    [[nodiscard]] std::expected<McReply, ExchangeError> Send(std::string_view request) override;

  private:
    /// @param socket The connected socket.
    /// @param limits Caps this side imposes.
    MemcachedExchange(std::unique_ptr<ISocket> socket, McParseLimits limits) noexcept;

    std::unique_ptr<ISocket> _socket;
    McParseLimits _limits;
    /// Bytes read and not yet consumed by a reply; held across calls for the reason
    /// `SocketExchange::_pending` is.
    std::string _pending;
};

/// One HTTP response.
struct HttpResponse
{
    int status { 0 };    ///< The status line's code.
    std::string body {}; ///< Everything after the head.
};

/// Issue one `GET` and read the whole response.
///
/// Written here rather than reusing a client from the tree because there is none --
/// `AdminHttpServer` is a server, and this is the only place anything in this
/// repository is an HTTP *client*. Deliberately minimal: one request per connection,
/// `Connection: close`, read to EOF. That is exactly what the admin surface serves,
/// and it needs no chunked-transfer or keep-alive handling to be correct against it.
///
/// The request is **not** half-closed after sending. `ShutdownWrite` would be the
/// honest way to say *I have finished sending*, and the admin surface is documented to
/// serve a complete head that is half-closed after -- but it costs nothing to let the
/// server close first, and this way the client does not depend on that behaviour to
/// get its metrics.
///
/// @param endpoint Where to dial.
/// @param path The request target, e.g. `/metrics`.
/// @param timeouts How long to wait.
/// @param bearer A bearer token to present, or nullopt.
/// @param maxBodyBytes Cap on the response body; a larger one is a `Malformed` failure.
/// @return The response, or why there is none.
[[nodiscard]] std::expected<HttpResponse, ExchangeError> HttpGet(Endpoint const& endpoint,
                                                                 std::string_view path,
                                                                 DialTimeouts timeouts,
                                                                 std::optional<std::string> const& bearer,
                                                                 std::size_t maxBodyBytes = 8U * 1024U * 1024U);

} // namespace FastCache::Cli
