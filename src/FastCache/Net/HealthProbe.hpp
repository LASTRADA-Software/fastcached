// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace FastCache
{

/// Connect to `host:port`, issue `GET <path> HTTP/1.0`, and report whether the
/// response status line is `200`. Blocking and best-effort (never throws);
/// intended for the `--healthcheck` entrypoint and container HEALTHCHECKs, so a
/// self-contained binary can probe its own `/healthz` without `curl`/`wget`.
/// @param host Target host (typically "127.0.0.1").
/// @param port Target TCP port (the admin/metrics port).
/// @param path Request path (typically "/healthz").
/// @return True iff a connection succeeded and the server answered 200.
[[nodiscard]] bool HttpHealthProbe(std::string_view host, std::uint16_t port, std::string_view path) noexcept;

} // namespace FastCache
