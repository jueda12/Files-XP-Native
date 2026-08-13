#pragma once

#include "../core/session_codec.h"

namespace filesxp::app
{
    class SessionStore final
    {
    public:
        [[nodiscard]] static core::SessionSnapshot load() noexcept;
        static void save(const core::SessionSnapshot& snapshot) noexcept;
    };
}
