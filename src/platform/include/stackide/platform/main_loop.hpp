#pragma once

#include <stackide/base/bounded_queue.hpp>
#include <stackide/core/messages.hpp>
#include <stackide/platform/event.hpp>

#include <cstddef>
#include <optional>
#include <utility>

namespace stackide::platform
{

    // Wakes a thread blocked in MainLoop::wait(). Safe from any thread.
    void post_wakeup();

    // One place for OS events and messages from the core, on the main thread.
    //
    // Ordering, within one pump():
    //   1. All pending OS events, in the order the OS delivered them.
    //   2. Then messages from the core, in the order they were queued.
    //
    // Input goes first so that a keystroke or a quit is never held up behind worker
    // updates. There is no ordering between the two sources beyond that, since they have
    // no shared clock: a message can reflect a state older than an event handled before
    // it, so message handlers mustn't assume otherwise.
    //
    // At most kMaxMessagesPerPump messages run per pump, so that a flood can't consume a
    // frame. The rest wait for the next one, and wait() won't sleep while any remain.
    class MainLoop
    {
    public:
        static constexpr std::size_t kMaxMessagesPerPump = 256;

        explicit MainLoop(BoundedQueue<CoreToUi>& messages) noexcept : messages_(&messages) {}

        template <class OnEvent, class OnMessage>
        void pump(OnEvent on_event, OnMessage on_message)
        {
            Event event;
            while (poll_event(event))
            {
                on_event(event);
            }

            // Before draining, so that a message arriving during the drain wakes us again.
            messages_->clear_wake();
            for (std::size_t i = 0; i < kMaxMessagesPerPump; ++i)
            {
                std::optional<CoreToUi> message = messages_->try_pop();
                if (!message)
                {
                    break;
                }
                on_message(std::move(*message));
            }
        }

        // Sleeps until an OS event or a wakeup arrives, or timeout_ms passes.
        void wait(int timeout_ms);

    private:
        BoundedQueue<CoreToUi>* messages_;
    };

} // namespace stackide::platform
