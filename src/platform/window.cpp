#include <stackide/platform/window.hpp>

#include <stackide/base/log.hpp>

#include <SDL3/SDL.h>

namespace stackide::platform
{

    std::optional<Window> Window::create(const std::string& title, int width, int height)
    {
        // Without high pixel density, a scaled display would stretch a lower resolution
        // surface, and every later text pixel would be blurry.
        constexpr SDL_WindowFlags kFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        SDL_Window* window = SDL_CreateWindow(title.c_str(), width, height, kFlags);
        if (window == nullptr)
        {
            STACKIDE_LOG_ERROR(LogCategory::Platform, "SDL_CreateWindow failed: {}", SDL_GetError());
            return std::nullopt;
        }
        return Window{window};
    }

    void Window::Destroy::operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }

    PixelSize Window::size_in_pixels() const
    {
        PixelSize size;
        SDL_GetWindowSizeInPixels(window_.get(), &size.width, &size.height);
        return size;
    }

    float Window::display_scale() const { return SDL_GetWindowDisplayScale(window_.get()); }

    float Window::refresh_hz() const
    {
        if (const SDL_DisplayID display = SDL_GetDisplayForWindow(window_.get()); display != 0)
        {
            if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display); mode != nullptr)
            {
                return mode->refresh_rate;
            }
        }
        return 0.0F;
    }

    void Window::set_size(int width, int height) { SDL_SetWindowSize(window_.get(), width, height); }

    SDL_Window* Window::sdl_handle() const { return window_.get(); }

} // namespace stackide::platform
