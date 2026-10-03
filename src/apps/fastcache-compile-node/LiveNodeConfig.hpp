// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeReload.hpp"

/// @file LiveNodeConfig.hpp
/// Where a component that judges this node's configuration reads the one in force NOW.

namespace FastCache::Node
{

/// The configuration this node runs by now, read at the moment it is needed.
///
/// A seam because several of the flags a judgement reads are `Reloadable::Yes` (`--advertise`,
/// `--fleet-member`, `--fleet-open`): a copy taken when a component was built goes on describing the
/// flags the body started with after an accepted reload has changed them. Read where it is used, as
/// an outbound credential is (`ICredentialSource`).
class INodeConfigSource
{
  public:
    INodeConfigSource() = default;
    INodeConfigSource(INodeConfigSource const&) = delete;
    INodeConfigSource(INodeConfigSource&&) = delete;
    INodeConfigSource& operator=(INodeConfigSource const&) = delete;
    INodeConfigSource& operator=(INodeConfigSource&&) = delete;
    virtual ~INodeConfigSource() = default;

    /// @return The configuration in force now: a copy, so a reload cannot change it under the caller.
    [[nodiscard]] virtual NodeConfig Current() const = 0;
};

/// Production's: the reloader's live snapshot when this node has a configuration file, and the
/// body's own configuration when it has none -- a process with no file has no second moment.
///
/// The reloader's snapshot is the body's equal at every moment that matters: a reload candidate is
/// shaped by the formation record kept now (`ReloadCandidateReader`), and a reform publishes the
/// configuration it adopted (`ReloaderPublisher`), so both describe the mode the node is in.
class LiveNodeConfig final: public INodeConfigSource
{
  public:
    /// @param body The configuration the body's record shaped; must outlive this.
    /// @param reloader Where the live configuration lives, or null when this node has no file.
    ///        Borrowed; must outlive this.
    LiveNodeConfig(NodeConfig const& body, NodeReloader const* reloader) noexcept:
        _body { body },
        _reloader { reloader }
    {
    }

    /// @copydoc INodeConfigSource::Current
    [[nodiscard]] NodeConfig Current() const override
    {
        if (_reloader == nullptr)
            return _body;
        return *_reloader->Current();
    }

  private:
    NodeConfig const& _body;       ///< In force while no reloader exists.
    NodeReloader const* _reloader; ///< The live snapshot's owner, or null.
};

} // namespace FastCache::Node
