// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliEndpoint.hpp"
#include "MemcachedClient.hpp"
#include "NodeClient.hpp"
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
class NodeExchange final: public INodeExchange
{
  public:
    /// Dial @p endpoint and present @p credential.
    /// @param endpoint Where to dial.
    /// @param timeouts How long to wait.
    /// @param credential What to present; nothing is presented when unconfigured.
    /// @return The open connection, or why there is none.
    [[nodiscard]] static std::expected<std::unique_ptr<NodeExchange>, ExchangeError> Open(Endpoint const& endpoint,
                                                                                          DialTimeouts timeouts,
                                                                                          Credential const& credential);

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
    [[nodiscard]] std::span<std::string const> Advisories() const noexcept;

  private:
    /// @param socket The connected socket.
    /// @param endpoint What to call this connection in a diagnostic.
    NodeExchange(std::unique_ptr<ISocket> socket, std::string endpoint) noexcept;

    /// Read exactly one framed reply, whatever its status.
    /// @return The reply, or why there is none.
    [[nodiscard]] std::expected<NodeReply, ExchangeError> ReadFrame();

    std::unique_ptr<ISocket> _socket;
    std::string _endpoint;
    /// Bytes read and not yet consumed by a reply; held across calls for the reason
    /// `SocketExchange::_pending` is.
    std::string _pending;
    std::vector<std::string> _advisories;
};

/// One HTTP response.
struct HttpResponse
{
    int status { 0 };    ///< The status line's code.
    std::string body {}; ///< Everything after the head.
};

/// Which way an admin fetch produced no document.
///
/// TWO states rather than one message, because an operator does different things with
/// them: nothing was reached, so go and look at a listener; or the surface answered
/// and declined, so go and read what it said. Collapsed into one string, a verb has to
/// pick an exit code and is wrong about half the traffic -- the same shape as an
/// absence counted as a failed attempt.
enum class AdminFailure : std::uint8_t
{
    Unreachable, ///< Nowhere to ask, or nothing answered.
    Refused,     ///< The surface answered and declined; the detail carries its words.
    Last,        ///< Not a failure: the length of a table keyed by one.
};

/// Why an admin fetch produced no document.
struct AdminError
{
    AdminFailure kind { AdminFailure::Unreachable }; ///< Which way it failed.
    std::string detail;                              ///< What to tell the operator.
};

/// Fetch one document from the endpoint's own admin surface.
///
/// The seam a verb reaches the admin surface through, so that WHERE that surface is
/// stays one decision. Finding it is not one question but four -- did the operator
/// override it, did the node report one at all, is it TLS this client cannot speak,
/// and is the port the one already being talked 0xFC to -- each with its own named
/// refusal, and each wrong in a way that reads as the surface being down. A verb that
/// resolved the address itself would be a second copy of all four.
///
/// Injected rather than reached for, like every other collaborator here: a verb that
/// opened its own socket could not be tested without one.
class IAdminDocument
{
  public:
    IAdminDocument() = default;
    IAdminDocument(IAdminDocument const&) = delete;
    IAdminDocument(IAdminDocument&&) = delete;
    IAdminDocument& operator=(IAdminDocument const&) = delete;
    IAdminDocument& operator=(IAdminDocument&&) = delete;
    virtual ~IAdminDocument() = default;

    /// Fetch @p path from the admin surface.
    /// @param path An absolute path, query string included.
    /// @return The body, or what went wrong and which KIND of wrong it was.
    [[nodiscard]] virtual std::expected<std::string, AdminError> FetchAdmin(std::string_view path) = 0;
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
