#pragma once

namespace filesxp::core
{
    class CoalescingGate final
    {
    public:
        [[nodiscard]] bool request() noexcept
        {
            if (pending_) return false;
            pending_ = true;
            return true;
        }

        void consume() noexcept { pending_ = false; }
        void reset() noexcept { pending_ = false; }
        [[nodiscard]] bool pending() const noexcept { return pending_; }

    private:
        bool pending_{};
    };
}
