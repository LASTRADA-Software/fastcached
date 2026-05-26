// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/DaemonControls.hpp>

namespace FastCache
{

DaemonControls& DaemonControls::Instance() noexcept
{
    static DaemonControls instance;
    return instance;
}

} // namespace FastCache
