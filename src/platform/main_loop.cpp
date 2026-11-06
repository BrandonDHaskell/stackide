#include <stackide/platform/main_loop.hpp>

#include <stackide/platform/video.hpp>

#include <SDL3/SDL.h>

namespace stackide::platform
{

    void post_wakeup()
    {
        SDL_Event event{};
        event.type = wakeup_event_type();
        SDL_PushEvent(&event);
    }

    void MainLoop::wait(int timeout_ms)
    {
        if (!messages_->maybe_has_items())
        {
            SDL_WaitEventTimeout(nullptr, timeout_ms);
        }
    }

} // namespace stackide::platform
