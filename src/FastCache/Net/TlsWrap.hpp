// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/ISocket.hpp>

#include <memory>
#include <utility>

#if defined(FC_TLS_ENABLED)
    #include <FastCache/Net/TlsSocket.hpp> // pulls in TlsContext + TlsSocket
#endif

namespace FastCache
{

// Forward declaration so the signature is identical whether or not TLS is built
// in (when FC_TLS_ENABLED, TlsSocket.hpp above provides the full definition).
class TlsContext;

/// Wrap a freshly-accepted socket in server-side TLS when a context is
/// configured; a no-op (returns the socket unchanged) in plaintext mode and in
/// builds compiled without TLS support. Centralises the single point where an
/// accepted socket becomes a TLS socket, so every accept path (Server and the
/// Windows IOCP hand-off) shares one implementation instead of re-deriving the
/// `#if FC_TLS_ENABLED` dance.
/// @param socket Owned transport just returned by accept().
/// @param tls Server TLS context, or nullptr for plaintext.
/// @return The socket, wrapped in a TlsSocket when TLS is active.
[[nodiscard]] inline std::unique_ptr<ISocket> WrapTls(std::unique_ptr<ISocket> socket, [[maybe_unused]] TlsContext* tls)
{
#if defined(FC_TLS_ENABLED)
    if (tls != nullptr)
        return std::make_unique<TlsSocket>(std::move(socket), *tls);
#endif
    return socket;
}

} // namespace FastCache
