#pragma once

#include <stackide/platform/window.hpp>

#include <cstdint>

namespace stackide::platform
{

    enum class EventType : std::uint8_t
    {
        Quit,
        WindowResized, // size holds the new size in physical pixels
        ScaleChanged, // the window's display scale changed
        DisplayChanged, // the window moved to another display, or that display's mode changed
        Exposed, // the window's contents need drawing again
        Input, // a key or button press, or a wheel movement
        Wakeup, // another thread has something for the main loop
    };

    struct Event
    {
        EventType type = EventType::Quit;
        std::uint64_t timestamp_ns = 0;
        PixelSize size;
    };

    // The clock Event::timestamp_ns uses.
    std::uint64_t now_ns();

    // Queues a synthetic input event, which SDL timestamps as it's queued. For measuring
    // input to paint in tests. Safe from any thread.
    void post_test_input();

    // Takes the next event STACK cares about, returning false once none are pending.
    bool poll_event(Event& out);

} // namespace stackide::platform
