// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Auth/AuthPolicy.hpp>

namespace FastCache
{

/// Per-server, immutable context handed to every protocol handler's command
/// loop. Bundles the optional collaborators a connection needs beyond its
/// socket and the shared cache engine.
///
/// Nullable members mean "feature off": a null `auth` means authentication is
/// disabled and every command is served without a credential check. The
/// referenced objects are owned by the daemon body and outlive every
/// connection, so handlers and connections hold borrowed pointers only — the
/// struct itself is a cheap value, copied by reference-sized members.
struct SessionContext
{
    /// Authentication policy, or nullptr when auth is disabled.
    AuthPolicy const* auth { nullptr };
};

} // namespace FastCache
