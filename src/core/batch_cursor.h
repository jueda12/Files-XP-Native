#pragma once

#include <algorithm>
#include <cstddef>

namespace filesxp::core
{
    struct IndexBatch final
    {
        std::size_t first{};
        std::size_t count{};
    };

    class BatchCursor final
    {
    public:
        [[nodiscard]] bool start(std::size_t total, std::size_t maximumTotal) noexcept
        {
            cancel();
            if (total == 0 || total > maximumTotal) return false;
            total_ = total;
            active_ = true;
            return true;
        }

        void cancel() noexcept
        {
            total_ = 0;
            next_ = 0;
            active_ = false;
        }

        [[nodiscard]] IndexBatch next(std::size_t maximum) noexcept
        {
            if (!active_ || maximum == 0) return {};
            const std::size_t first = next_;
            const std::size_t count = std::min(maximum, total_ - next_);
            next_ += count;
            if (next_ == total_) active_ = false;
            return IndexBatch{first, count};
        }

        [[nodiscard]] bool active() const noexcept { return active_; }
        [[nodiscard]] std::size_t processed() const noexcept { return next_; }
        [[nodiscard]] std::size_t total() const noexcept { return total_; }

    private:
        std::size_t total_{};
        std::size_t next_{};
        bool active_{};
    };
}
