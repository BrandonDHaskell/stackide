#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace stackide::platform
{

    enum class VideoBackend : std::uint8_t
    {
        Native, // Wayland, then X11; SDL_VIDEODRIVER can override
        Wayland,
        X11,
        Offscreen, // no display server, but can still create a GPU device
        Dummy, // no display server and no GPU device
    };

    // SDL's video subsystem, shut down on destruction. Must stay on the main thread,
    // along with windows and events, which macOS requires.
    class Video
    {
    public:
        // Empty on failure, having logged why.
        static std::optional<Video> start(VideoBackend backend);

        Video(const Video&) = delete;
        Video& operator=(const Video&) = delete;
        Video(Video&& other) noexcept;
        Video& operator=(Video&&) = delete;
        ~Video();

        [[nodiscard]] static std::string_view driver_name();

    private:
        Video() = default;
        bool active_ = true;
    };

    // SDL's event type for wakeups, valid while a Video exists.
    std::uint32_t wakeup_event_type();

    // Errors SDL's GPU code has logged, including validation layer errors.
    std::uint64_t gpu_error_count();

    // Whether a debug GPU device was requested on a system without validation layers,
    // which means a debug run has validated nothing.
    bool gpu_validation_missing();

} // namespace stackide::platform
