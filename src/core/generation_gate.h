#pragma once

#include <atomic>
#include <cstdint>

namespace filesxp::core
{
    class GenerationGate final
    {
    public:
        using value_type = std::uint64_t;

        [[nodiscard]] value_type next() noexcept
        {
            return current_.fetch_add(1, std::memory_order_acq_rel) + 1;
        }

        [[nodiscard]] value_type current() const noexcept
        {
            return current_.load(std::memory_order_acquire);
        }

        [[nodiscard]] bool accepts(value_type candidate) const noexcept
        {
            return candidate == current();
        }

    private:
        std::atomic<value_type> current_{0};
    };
}

