// SPDX-License-Identifier: Apache-2.0
#include "TerminalCapabilities.hpp"

namespace FastCache::Cli
{

RenderRung ChooseRenderRung(TerminalCapabilities const& capabilities) noexcept
{
    if (capabilities.sixel == SixelAnswer::Advertised && capabilities.cellPixels.has_value())
        return RenderRung::Sixel;
    if (capabilities.encoding == TerminalTextEncoding::Utf8)
        return RenderRung::Unicode;
    return RenderRung::Ascii;
}

} // namespace FastCache::Cli
