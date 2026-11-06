#pragma once

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace stackide
{

    // A fixed-size FIFO between threads. Producers block when it's full, which is how a
    // fast worker gets slowed down, and consumers block when it's empty.
    //
    // The main thread must never wait on a lock a worker holds, because that would spend
    // the frame budget. So the try_ functions never wait at all, not even for the lock:
    // they fail if another thread holds it right now. A failed try_pop doesn't mean the
    // queue is empty, so check maybe_has_items() before sleeping.
    template <std::movable T>
    class BoundedQueue
    {
    public:
        // on_wake runs when items may have become available, at most once between calls
        // to clear_wake(), on the producer's thread and outside the lock. Coalescing
        // keeps a flood of items from flooding whatever mechanism wakes the consumer.
        explicit BoundedQueue(std::size_t capacity, std::function<void()> on_wake = {}) :
            items_(capacity), on_wake_(std::move(on_wake))
        {
        }

        // Blocks while full. Returns false if stop was requested first, dropping value.
        bool push(T value, const std::stop_token& stop)
        {
            {
                std::unique_lock lock{mutex_};
                if (!not_full_.wait(lock, stop, [this] { return count_ < items_.size(); }))
                {
                    return false;
                }
                insert(std::move(value));
            }
            not_empty_.notify_one();
            wake();
            return true;
        }

        // Fails if the queue is full or the lock is busy, leaving value untouched.
        [[nodiscard]] bool try_push(T&& value)
        {
            {
                const std::unique_lock lock{mutex_, std::try_to_lock};
                if (!lock.owns_lock() || count_ == items_.size())
                {
                    return false;
                }
                insert(std::move(value));
            }
            not_empty_.notify_one();
            wake();
            return true;
        }

        // Empty if the queue is empty or the lock is busy.
        [[nodiscard]] std::optional<T> try_pop()
        {
            std::optional<T> value;
            {
                const std::unique_lock lock{mutex_, std::try_to_lock};
                if (!lock.owns_lock() || count_ == 0)
                {
                    return value;
                }
                value = remove();
            }
            not_full_.notify_one();
            return value;
        }

        // Blocks while empty. Empty if stop was requested first.
        [[nodiscard]] std::optional<T> pop(const std::stop_token& stop)
        {
            std::optional<T> value;
            {
                std::unique_lock lock{mutex_};
                if (!not_empty_.wait(lock, stop, [this] { return count_ > 0; }))
                {
                    return value;
                }
                value = remove();
            }
            not_full_.notify_one();
            return value;
        }

        // Call before draining, so that a push during the drain wakes the consumer again.
        void clear_wake() noexcept { wake_pending_.store(false, std::memory_order_seq_cst); }

        // Taken without the lock. Items pushed before this call are always seen.
        [[nodiscard]] bool maybe_has_items() const noexcept
        {
            return approximate_count_.load(std::memory_order_seq_cst) > 0;
        }

    private:
        void insert(T&& value)
        {
            items_.at((head_ + count_) % items_.size()).emplace(std::move(value));
            ++count_;
            approximate_count_.store(count_, std::memory_order_seq_cst);
        }

        T remove()
        {
            // Every slot from head_ for count_ slots holds a value, and count_ is checked by callers.
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
            T value = std::move(*items_.at(head_));
            items_.at(head_).reset();
            head_ = (head_ + 1) % items_.size();
            --count_;
            approximate_count_.store(count_, std::memory_order_seq_cst);
            return value;
        }

        void wake()
        {
            if (on_wake_ && !wake_pending_.exchange(true, std::memory_order_seq_cst))
            {
                on_wake_();
            }
        }

        std::mutex mutex_;
        std::condition_variable_any not_empty_;
        std::condition_variable_any not_full_;
        std::vector<std::optional<T>> items_;
        std::size_t head_ = 0;
        std::size_t count_ = 0;
        std::atomic<std::size_t> approximate_count_{0};
        std::atomic<bool> wake_pending_{false};
        std::function<void()> on_wake_;
    };

} // namespace stackide
