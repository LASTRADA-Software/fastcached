// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliEndpoint.hpp"
#include "MemcachedClient.hpp"
#include "NodeClient.hpp"
#include "RespClient.hpp"

#include <FastCache/Core/SecureBytes.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <core/net/ISocket.hpp>

namespace FastCache::Cli
{

/// A RESP connection over one blocking TCP socket.
///
/// **Blocking by type, driven by `core::async::syncRun`.** That is sound only over a blocking
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
    SocketExchange(std::unique_ptr<core::net::ISocket> socket, ParseLimits limits) noexcept;

    /// Read until one whole reply is available, then consume it.
    /// @return The reply, or why there is none.
    [[nodiscard]] std::expected<RespValue, ExchangeError> ReadReply();

    std::unique_ptr<core::net::ISocket> _socket;
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
    MemcachedExchange(std::unique_ptr<core::net::ISocket> socket, McParseLimits limits) noexcept;

    std::unique_ptr<core::net::ISocket> _socket;
    McParseLimits _limits;
    /// Bytes read and not yet consumed by a reply; held across calls for the reason
    /// `SocketExchange::_pending` is.
    std::string _pending;
};

/// A `0xFC` connection over one blocking TCP socket.
///
/// The third wire, and the one `fastcache-compile-node` is reachable on AT ALL: that
/// binary speaks `0xFC` and nothing else, so the other two classes here close having
/// sent nothing when pointed at it.
///
/// **It presents the credential, and unlike the RESP one it does so by VERB.** `AUTH`
/// is a `0xFC` verb, and a surface that holds no policy answers it `Ok` while marking
/// nothing -- so a credential offered to a node with no scheduler is accepted, changes
/// nothing, and must not be reported as a failure. That is the same false inference
/// `SocketExchange::Open`'s AUTH advisory exists to avoid, arriving on a different wire.
///
/// **Reads loop to a TERMINAL status.** `Status::Progress` is the compile pulse, and a
/// reader that does not ask treats the first one as the answer. Nothing here sends a
/// compile today, so the loop is unreachable in this binary -- and it is written anyway,
/// because `IsTerminalStatus` exists in the wire header precisely so every reader asks,
/// and a client that would misread a pulse is one that cannot later grow a verb that
/// produces them.
/// How a connection is opened: a TCP dial in production, a scripted socket in a test.
using SocketDial = std::function<std::expected<std::unique_ptr<core::net::ISocket>, ExchangeError>(Endpoint const& endpoint,
                                                                                                   DialTimeouts timeouts)>;

/// Dial @p endpoint over TCP.
/// @param endpoint Where.
/// @param timeouts How long.
/// @return The connected socket, or why there is none.
[[nodiscard]] std::expected<std::unique_ptr<core::net::ISocket>, ExchangeError> DialTcp(Endpoint const& endpoint,
                                                                                        DialTimeouts timeouts);

class NodeExchange final: public INodeExchange
{
  public:
    /// Dial @p endpoint and present what @p credentials answers for it.
    ///
    /// **Asked per endpoint** (`Cc::ChooseCredential`): the token to the endpoint it belongs to, a
    /// machine ticket minted by this machine's node to any other machine, nothing to loopback. A
    /// mint that fails is an ADVISORY and the connection goes on unauthenticated -- the node then
    /// answers as it does to a machine it does not know, and the advisory says why.
    /// @param endpoint Where to dial.
    /// @param timeouts How long to wait.
    /// @param credentials What each endpoint is shown.
    /// @param dial How a connection is opened, the mint's included.
    /// @return The open connection, or why there is none.
    [[nodiscard]] static std::expected<std::unique_ptr<NodeExchange>, ExchangeError> Open(Endpoint const& endpoint,
                                                                                          DialTimeouts timeouts,
                                                                                          NodeCredentials const& credentials,
                                                                                          SocketDial const& dial);

    /// `Open` over a TCP dial.
    /// @param endpoint Where to dial.
    /// @param timeouts How long to wait.
    /// @param credentials What each endpoint is shown.
    /// @return The open connection, or why there is none.
    [[nodiscard]] static std::expected<std::unique_ptr<NodeExchange>, ExchangeError> Open(
        Endpoint const& endpoint, DialTimeouts timeouts, NodeCredentials const& credentials);

    ~NodeExchange() override;
    NodeExchange(NodeExchange const&) = delete;
    NodeExchange(NodeExchange&&) = delete;
    NodeExchange& operator=(NodeExchange const&) = delete;
    NodeExchange& operator=(NodeExchange&&) = delete;

    [[nodiscard]] std::expected<NodeReply, ExchangeError> Send(std::span<std::byte const> request) override;

    [[nodiscard]] std::string_view Address() const override
    {
        return _endpoint;
    }

    /// Remarks gathered while opening the connection, for stderr.
    ///
    /// Non-empty when the credential was configured and not honoured, for the reason
    /// `SocketExchange::Advisories` is: the operator ASKED for authentication, so
    /// silently proceeding without it is the one outcome they cannot see.
    /// @return The remarks, in the order they were made.
    [[nodiscard]] std::span<std::string const> Advisories() const noexcept override;

    /// @copydoc INodeExchange::MissingTicket
    [[nodiscard]] std::optional<Cc::MintFailure> MissingTicket() const noexcept override
    {
        return _missingTicket;
    }

    /// Send one framed request and read nothing: the first half of a stream (#1399).
    ///
    /// `Send` reads to a TERMINAL status, which a `SUBSCRIBE` sends only when its stream ends -- so a
    /// stream is this, then `ReadFrame` once per frame.
    /// @param request The framed request.
    /// @return Nothing once it is sent, or why it could not be.
    [[nodiscard]] std::expected<void, ExchangeError> Post(std::span<std::byte const> request);

    /// Read exactly one frame of a STREAM, whatever its status.
    ///
    /// A failure is told in a stream's words -- `the stream was lost`, `the server closed the stream` -- and
    /// never `Send`'s: a subscription has no reply, so *closed without answering* after thirty pushes names
    /// a fault it did not have.
    /// @return The frame, or why there is none.
    [[nodiscard]] std::expected<NodeReply, ExchangeError> ReadFrame();

    /// Wait no longer than @p deadline for each read from now on.
    /// @param deadline The bound on one read.
    void SetReceiveDeadline(std::chrono::milliseconds deadline) noexcept;

    /// Half-close: say this end has finished sending.
    ///
    /// Safe while `ReadFrame` blocks on another thread -- it is one `shutdown(2)` on the descriptor,
    /// which a blocked receive does not share state with -- and it is the ordinary way a watcher leaves.
    void ShutdownWrite() noexcept;

  private:
    /// @param socket The connected socket.
    /// @param endpoint What to call this connection in a diagnostic.
    NodeExchange(std::unique_ptr<core::net::ISocket> socket, std::string endpoint) noexcept;

    /// What one read is of, for the words its failure is told in. Private to this class; never transmitted.
    enum class Reading : std::uint8_t
    {
        Reply,  ///< One exchange's answer, which `Send` reads.
        Stream, ///< One frame of a subscription, which `ReadFrame` reads.
    };

    /// Read exactly one frame.
    /// @param reading What the frame is part of.
    /// @return The frame, or why there is none.
    [[nodiscard]] std::expected<NodeReply, ExchangeError> ReadOne(Reading reading);

    /// Present @p request on @p exchange and hand the connection back when it may be used.
    /// @param exchange The open connection.
    /// @param request What to present.
    /// @return The connection, or why the credential stops it here.
    [[nodiscard]] static std::expected<std::unique_ptr<NodeExchange>, ExchangeError> Authenticated(
        std::unique_ptr<NodeExchange> exchange, CompileCacheWire::AuthRequest const& request);

    std::unique_ptr<core::net::ISocket> _socket;
    std::string _endpoint;
    /// Bytes read and not yet consumed by a reply; held across calls for the reason
    /// `SocketExchange::_pending` is.
    ///
    /// **Wiping storage, because a reply here can BE a credential**: `MINT-TICKET`'s `Ok` carries
    /// the ticket. A consumed frame is zeroed before the rest moves down over it, and the storage
    /// is zeroed when it is released -- a `std::string` did neither.
    SecureCharBuffer _pending;
    std::vector<std::string> _advisories;
    std::optional<Cc::MintFailure> _missingTicket; ///< Why no ticket was presented, when one was due.
};

/// The production `INodeDialer`: `NodeExchange::Open` with this invocation's timeouts and credentials.
class NodeDialer final: public INodeDialer
{
  public:
    /// @param timeouts How long a dial may take.
    /// @param credentials What each endpoint is shown; must outlive this. Held by reference, so the
    ///        secret is not copied once more -- and asked per dial, so a leader a redirect names is
    ///        shown a ticket naming it, never the token `--addr` was given.
    NodeDialer(DialTimeouts timeouts, NodeCredentials const& credentials) noexcept:
        _timeouts { timeouts },
        _credentials { credentials }
    {
    }

    /// @copydoc INodeDialer::Dial
    [[nodiscard]] std::expected<std::unique_ptr<INodeExchange>, ExchangeError> Dial(Endpoint const& endpoint) override
    {
        return NodeExchange::Open(endpoint, _timeouts, _credentials)
            .transform([](std::unique_ptr<NodeExchange> exchange) -> std::unique_ptr<INodeExchange> { return exchange; });
    }

  private:
    DialTimeouts _timeouts;
    NodeCredentials const& _credentials;
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
/// **It presents NO credential, and that is the design rather than an omission.** The admin
/// surface is plain HTTP on a port that can be `$FASTCACHE_ADMIN_ADDR` on any host or one
/// DISCOVERED on `--addr`'s host, and `--token-file`'s secret is never the credential there:
/// `/metrics` needs none, and a node's dashboard has its own (`--dashboard-token-file`). Sent
/// anyway, it crossed the network in the clear to whichever host the port was found on. A
/// credential this request ever needs is a SEPARATE one, configured for it by name.
///
/// @param endpoint Where to dial.
/// @param path The request target, e.g. `/metrics`.
/// @param timeouts How long to wait.
/// @param maxBodyBytes Cap on the response body; a larger one is a `Malformed` failure.
/// @return The response, or why there is none.
[[nodiscard]] std::expected<HttpResponse, ExchangeError> HttpGet(Endpoint const& endpoint,
                                                                 std::string_view path,
                                                                 DialTimeouts timeouts,
                                                                 std::size_t maxBodyBytes = 8U * 1024U * 1024U);

} // namespace FastCache::Cli
