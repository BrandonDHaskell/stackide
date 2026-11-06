#include <stackide/platform/event.hpp>

#include <SDL3/SDL.h>

namespace stackide::platform
{

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
                break;
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
                break;
            }
        }
        return false;
    }

} // namespace stackide::platform
