#pragma once

#include <optional>
#include <string>
#include <utility>

namespace filesxp::core
{
    struct PreviewRequest final
    {
        bool switchSelection{};
        bool closing{};
        std::wstring path;

        [[nodiscard]] bool operator==(const PreviewRequest&) const = default;
    };

    struct PreviewCompletion final
    {
        bool recognized{};
        bool switchSelection{};
        bool closing{};
        bool launchNext{};
    };

    class PreviewQueue final
    {
    public:
        [[nodiscard]] bool submit(PreviewRequest request)
        {
            if (request.path.empty()) return false;
            if (current_.has_value())
            {
                // ponytail: Selection churn retains one latest request, never an unbounded queue.
                pending_ = std::move(request);
                return false;
            }
            current_ = std::move(request);
            return true;
        }

        [[nodiscard]] const PreviewRequest* current() const noexcept
        {
            return current_.has_value() ? &*current_ : nullptr;
        }

        [[nodiscard]] bool hasPending() const noexcept
        {
            return pending_.has_value();
        }

        [[nodiscard]] PreviewCompletion complete() noexcept
        {
            if (!current_.has_value()) return {};
            PreviewCompletion completion{
                true, current_->switchSelection, current_->closing, pending_.has_value()};
            if (pending_.has_value())
            {
                current_ = std::move(pending_);
                pending_.reset();
            }
            else
            {
                current_.reset();
            }
            return completion;
        }

        void clear() noexcept
        {
            current_.reset();
            pending_.reset();
        }

    private:
        std::optional<PreviewRequest> current_;
        std::optional<PreviewRequest> pending_;
    };
}
