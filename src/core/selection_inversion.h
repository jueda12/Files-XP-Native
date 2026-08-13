#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace filesxp::core
{
    inline constexpr int maxSelectionInversionItems = 1000000;

    struct SelectionChange final
    {
        int index{};
        bool select{};
    };

    class SelectionInversionCursor final
    {
    public:
        [[nodiscard]] bool start(int itemCount, std::vector<int> selected)
        {
            cancel();
            if (itemCount < 0 || itemCount > maxSelectionInversionItems) return false;
            std::sort(selected.begin(), selected.end());
            selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
            if (!selected.empty() && (selected.front() < 0 || selected.back() >= itemCount))
                return false;
            itemCount_ = itemCount;
            selected_ = std::move(selected);
            active_ = itemCount_ != 0;
            return true;
        }

        [[nodiscard]] bool beginSnapshot(int itemCount)
        {
            cancel();
            if (itemCount < 0 || itemCount > maxSelectionInversionItems) return false;
            itemCount_ = itemCount;
            snapshotActive_ = true;
            return true;
        }

        [[nodiscard]] bool addSelected(int index)
        {
            if (!snapshotActive_ || index < 0 || index >= itemCount_ ||
                (!selected_.empty() && index <= selected_.back()))
                return false;
            selected_.push_back(index);
            return true;
        }

        [[nodiscard]] bool finishSnapshot() noexcept
        {
            if (!snapshotActive_) return false;
            snapshotActive_ = false;
            active_ = itemCount_ != 0;
            return true;
        }

        void cancel() noexcept
        {
            selected_.clear();
            itemCount_ = 0;
            index_ = 0;
            selectedPosition_ = 0;
            active_ = false;
            snapshotActive_ = false;
        }

        [[nodiscard]] std::vector<SelectionChange> next(std::size_t maximum)
        {
            std::vector<SelectionChange> result;
            if (!active_ || maximum == 0) return result;
            result.reserve(std::min(maximum, static_cast<std::size_t>(itemCount_ - index_)));
            while (index_ < itemCount_ && result.size() < maximum)
            {
                const bool wasSelected = selectedPosition_ < selected_.size() &&
                    selected_[selectedPosition_] == index_;
                result.push_back(SelectionChange{index_, !wasSelected});
                if (wasSelected) ++selectedPosition_;
                ++index_;
            }
            if (index_ == itemCount_) active_ = false;
            return result;
        }

        [[nodiscard]] bool active() const noexcept { return active_; }
        [[nodiscard]] bool snapshotActive() const noexcept { return snapshotActive_; }

    private:
        std::vector<int> selected_;
        int itemCount_{};
        int index_{};
        std::size_t selectedPosition_{};
        bool active_{};
        bool snapshotActive_{};
    };
}
