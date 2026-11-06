#pragma once

#include <memory>
#include <optional>
#include <string>

struct SDL_Window;

namespace stackide::platform
{

    struct PixelSize
    {
        int width = 0;
        int height = 0;
    };

    // Rendering uses physical pixels. Layout uses logical units, which are physical
    // pixels divided by display_scale().
    class Window
    {
    public:
        // Empty on failure, having logged why. Sizes are in logical units.
        static std::optional<Window> create(const std::string& title, int width, int height);

        [[nodiscard]] PixelSize size_in_pixels() const;
        [[nodiscard]] float display_scale() const;
        // Of the display the window is on now, or 0 if unknown.
        [[nodiscard]] float refresh_hz() const;
        void set_size(int width, int height);

        // For the renderer only: see sdl_handles.hpp.
        [[nodiscard]] SDL_Window* sdl_handle() const;

    private:
        struct Destroy
        {
            void operator()(SDL_Window* window) const;
        };

        explicit Window(SDL_Window* window) : window_(window) {}

        std::unique_ptr<SDL_Window, Destroy> window_;
    };

} // namespace stackide::platform
