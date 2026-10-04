// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>

#include <span>

#include <tests/RaftPeerKeyFakes.hpp>

namespace FastCache::Testing
{

/// The identity key a test WORKER signs its COMPILE replies under (`SealCompileReply`, W-4),
/// and so the key a test GRANT names for it.
///
/// One key for every test worker, shared, for `MembershipFakes.hpp`'s reason: a case that built
/// its worker with one key and its grant with another would be a case about a forged reply whether
/// it meant to be or not. A case ABOUT a forgery names a second machine's key on purpose.
/// @return The key pair; static, so it outlives every protocol handed it.
[[nodiscard]] inline Ed25519KeyPair const& TestWorkerKey()
{
    static Ed25519KeyPair const key = TestKeyPair("test-worker");
    return key;
}

/// @return `TestWorkerKey()`'s public half, as the bytes a grant's worker-key field carries.
[[nodiscard]] inline std::span<std::byte const> TestWorkerPublicKey()
{
    return std::span<std::byte const> { TestWorkerKey().PublicKey() };
}

} // namespace FastCache::Testing
