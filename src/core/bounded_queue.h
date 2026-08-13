#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace filesxp::core
{
    template <typename T>
    class BoundedQueue final
    {
    public:
        explicit BoundedQueue(std::size_t capacity)
            : capacity_(capacity == 0 ? 1 : capacity)
        {
        }

        BoundedQueue(const BoundedQueue&) = delete;
        BoundedQueue& operator=(const BoundedQueue&) = delete;

        [[nodiscard]] bool try_push(T value)
        {
            std::scoped_lock lock(mutex_);
            if (queue_.size() >= capacity_)
            {
                return false;
            }

            queue_.push_back(std::move(value));
            return true;
        }

        [[nodiscard]] std::optional<T> try_pop()
        {
            std::scoped_lock lock(mutex_);
            if (queue_.empty())
            {
                return std::nullopt;
            }

            auto value = std::move(queue_.front());
            queue_.pop_front();
            return value;
        }

        [[nodiscard]] std::vector<T> drain()
        {
            std::scoped_lock lock(mutex_);
            std::vector<T> values;
            values.reserve(queue_.size());
            while (!queue_.empty())
            {
                values.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
            return values;
        }

        void clear()
        {
            std::scoped_lock lock(mutex_);
            queue_.clear();
        }

        [[nodiscard]] std::size_t size() const
        {
            std::scoped_lock lock(mutex_);
            return queue_.size();
        }

        [[nodiscard]] std::size_t capacity() const noexcept
        {
            return capacity_;
        }

    private:
        const std::size_t capacity_;
        mutable std::mutex mutex_;
        std::deque<T> queue_;
    };
}

