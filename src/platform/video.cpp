#include <stackide/platform/video.hpp>

#include <stackide/base/log.hpp>

#include <SDL3/SDL.h>

#include <atomic>
#include <cstring>

namespace stackide::platform
{
    namespace
    {

        // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
        constinit std::atomic<std::uint64_t> g_gpu_errors{0};
        constinit std::atomic<bool> g_validation_missing{false};
        // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

        template <LogCategory kCategory>
        void log_sdl_message(SDL_LogPriority priority, const char* message)
        {
            if (priority >= SDL_LOG_PRIORITY_ERROR)
            {
                STACKIDE_LOG_ERROR(kCategory, "SDL: {}", message);
            }
            else if (priority == SDL_LOG_PRIORITY_WARN)
            {
                STACKIDE_LOG_WARN(kCategory, "SDL: {}", message);
            }
            else
            {
                STACKIDE_LOG_DEBUG(kCategory, "SDL: {}", message);
            }
        }

        // Sent from whichever thread SDL logged on, which the logger allows.
        void SDLCALL forward_sdl_log(void* /*userdata*/, int category, SDL_LogPriority priority, const char* message)
        {
            if (category != SDL_LOG_CATEGORY_GPU)
            {
                log_sdl_message<LogCategory::Platform>(priority, message);
                return;
            }

            if (priority >= SDL_LOG_PRIORITY_ERROR)
            {
                g_gpu_errors.fetch_add(1, std::memory_order_relaxed);
            }
            if (std::strstr(message, "Validation layers not found") != nullptr)
            {
                g_validation_missing.store(true, std::memory_order_relaxed);
            }
            log_sdl_message<LogCategory::Render>(priority, message);
        }

        bool select_backend(VideoBackend backend)
        {
            switch (backend)
            {
            case VideoBackend::Native:
#ifdef __linux__
                // Normal priority, so that SDL_VIDEODRIVER still wins.
                return SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
#else
                return true;
#endif
            case VideoBackend::Wayland:
                return SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "wayland", SDL_HINT_OVERRIDE);
            case VideoBackend::X11:
                return SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "x11", SDL_HINT_OVERRIDE);
            case VideoBackend::Offscreen:
                return SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);
            case VideoBackend::Dummy:
                return SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
            }
            return false;
        }

    } // namespace

    std::optional<Video> Video::start(VideoBackend backend)
    {
        g_gpu_errors.store(0, std::memory_order_relaxed);
        g_validation_missing.store(false, std::memory_order_relaxed);
        SDL_SetLogOutputFunction(forward_sdl_log, nullptr);
        // SDL's default hides GPU warnings, including the one about missing validation.
        SDL_SetLogPriority(SDL_LOG_CATEGORY_GPU, SDL_LOG_PRIORITY_INFO);

        if (!select_backend(backend))
        {
            STACKIDE_LOG_ERROR(LogCategory::Platform, "selecting a video backend failed: {}", SDL_GetError());
            return std::nullopt;
        }
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            STACKIDE_LOG_ERROR(LogCategory::Platform, "SDL_Init failed: {}", SDL_GetError());
            return std::nullopt;
        }
        return Video{};
    }

    Video::Video(Video&& other) noexcept : active_(other.active_) { other.active_ = false; }

    Video::~Video()
    {
        if (active_)
        {
            SDL_Quit();
        }
    }

    std::string_view Video::driver_name()
    {
        const char* name = SDL_GetCurrentVideoDriver();
        return name != nullptr ? std::string_view{name} : std::string_view{};
    }

    std::uint64_t gpu_error_count() { return g_gpu_errors.load(std::memory_order_relaxed); }

    bool gpu_validation_missing() { return g_validation_missing.load(std::memory_order_relaxed); }

} // namespace stackide::platform
