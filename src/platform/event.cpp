#include <stackide/platform/event.hpp>

#include <stackide/platform/video.hpp>

#include <SDL3/SDL.h>

namespace stackide::platform
{

    std::uint64_t now_ns() { return SDL_GetTicksNS(); }

    void post_test_input()
    {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        SDL_PushEvent(&event);
    }

    bool poll_event(Event& out)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            out.timestamp_ns = event.common.timestamp;
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
                out.type = EventType::Quit;
                return true;
            case SDL_EVENT_KEY_DOWN:
                // A fixed key until the Phase 1 keybinding system exists.
                if (event.key.key == SDLK_ESCAPE)
                {
                    out.type = EventType::Quit;
                    return true;
                }
                if (event.key.key == SDLK_F12)
                {
                    out.type = EventType::ToggleFrameGraph;
                    return true;
                }
                out.type = EventType::Input;
                return true;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_WHEEL:
                out.type = EventType::Input;
                return true;
            case SDL_EVENT_WINDOW_EXPOSED:
                out.type = EventType::Exposed;
                return true;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                out.type = EventType::WindowResized;
                out.size = PixelSize{.width = event.window.data1, .height = event.window.data2};
                return true;
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                out.type = EventType::ScaleChanged;
                return true;
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
                out.type = EventType::DisplayChanged;
                return true;
            default:
                if (event.type == wakeup_event_type())
                {
                    out.type = EventType::Wakeup;
                    return true;
                }
                break;
            }
        }
        return false;
    }

} // namespace stackide::platform
