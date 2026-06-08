// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/TlsContext.hpp>

#include <array>
#include <string>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace FastCache
{

namespace
{

    /// Drain one entry from the OpenSSL error queue (if any) into a NetError.
    /// @param context Human-readable description of what was being attempted.
    [[nodiscard]] NetError SslError(std::string context)
    {
        unsigned long const code = ERR_get_error();
        if (code != 0)
        {
            std::array<char, 256> buffer {};
            ERR_error_string_n(code, buffer.data(), buffer.size());
            context += ": ";
            context += buffer.data();
        }
        return NetError { .code = NetErrorCode::SystemError,
                          .systemCode = static_cast<int>(code),
                          .context = std::move(context) };
    }

} // namespace

std::expected<std::unique_ptr<TlsContext>, NetError> TlsContext::Create(std::string_view certPath, std::string_view keyPath)
{
    // Clear any stale entries left on this thread's OpenSSL error queue so a
    // failure below is reported with its own error, not an earlier unrelated one
    // (SslError drains the queue via ERR_get_error, which is FIFO).
    ERR_clear_error();
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr)
        return std::unexpected(SslError("SSL_CTX_new failed"));

    // Refuse legacy protocol versions: TLS 1.2 is the floor.
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    std::string const cert { certPath };
    std::string const key { keyPath };

    if (SSL_CTX_use_certificate_chain_file(ctx, cert.c_str()) != 1)
    {
        auto error = SslError("loading certificate '" + cert + "'");
        SSL_CTX_free(ctx);
        return std::unexpected(std::move(error));
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key.c_str(), SSL_FILETYPE_PEM) != 1)
    {
        auto error = SslError("loading private key '" + key + "'");
        SSL_CTX_free(ctx);
        return std::unexpected(std::move(error));
    }
    if (SSL_CTX_check_private_key(ctx) != 1)
    {
        auto error = SslError("private key does not match certificate");
        SSL_CTX_free(ctx);
        return std::unexpected(std::move(error));
    }

    return std::unique_ptr<TlsContext>(new TlsContext(ctx));
}

TlsContext::~TlsContext()
{
    if (_ctx != nullptr)
        SSL_CTX_free(_ctx);
}

} // namespace FastCache
