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
    };

    struct Event
    {
        EventType type = EventType::Quit;
        std::uint64_t timestamp_ns = 0;
        PixelSize size;
    };

    // Takes the next event STACK cares about, returning false once none are pending.
    bool poll_event(Event& out);

} // namespace stackide::platform
