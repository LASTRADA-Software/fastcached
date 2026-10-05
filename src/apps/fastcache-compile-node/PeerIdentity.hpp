// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/ProvenIdentity.hpp>

namespace FastCache::Node
{

/// Who is at the other end of a connection, as an admission policy sees it.
///
/// Admission used to be decided from the peer's ADDRESS alone, which is a stand-in for *this is one
/// of our nodes* and stops being one the moment an address is not stable
/// ([#1428](https://github.com/LASTRADA-Software/fastcached/issues/1428),
/// [#178](https://github.com/LASTRADA-Software/fastcached/issues/178) item 1). A machine that is not
/// this one is now admitted by what its connection PROVED or presented -- a session proof or a
/// verified ticket -- so the facts travel together, as `ConnectionFacts`, and a policy cannot read
/// one and forget another. An alias of the one struct the live-stream path holds too.
using PeerIdentity = FastCache::ConnectionFacts;

} // namespace FastCache::Node
