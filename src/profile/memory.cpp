#include <stackide/profile/memory.hpp>

#include <stackide/base/log.hpp>
#include <stackide/profile/profile.hpp>

#include <sys/resource.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#else
#include <fcntl.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <utility>

namespace stackide
{

    std::uint64_t current_rss_bytes() noexcept
    {
#ifdef __APPLE__
        mach_task_basic_info_data_t info{};
        mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the Mach API's own form
        if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
            KERN_SUCCESS)
        {
            return 0;
        }
        return info.resident_size;
#else
        // Raw reads, since this runs once a second for the life of the program and allocating
        // to measure memory would be a poor look. statm holds pages: total, then resident.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic
        const int fd = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
        {
            return 0;
        }
        constexpr std::size_t kBufferBytes = 128;
        std::array<char, kBufferBytes> buffer{};
        const auto size = ::read(fd, buffer.data(), buffer.size());
        ::close(fd);
        if (size <= 0)
        {
            return 0;
        }

        std::string_view text{buffer.data(), static_cast<std::size_t>(size)};
        const auto space = text.find(' ');
        if (space == std::string_view::npos)
        {
            return 0;
        }
        text.remove_prefix(space + 1);
        std::uint64_t pages = 0;
        std::from_chars(text.data(), text.data() + text.size(), pages);
        return pages * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
    }

    std::uint64_t peak_rss_bytes() noexcept
    {
        struct rusage usage{};
        if (::getrusage(RUSAGE_SELF, &usage) != 0)
        {
            return current_rss_bytes();
        }
#ifdef __APPLE__
        const auto peak = static_cast<std::uint64_t>(usage.ru_maxrss); // bytes on macOS
#else
        constexpr std::uint64_t kBytesPerKilobyte = 1024;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): how glibc declares this field
        const auto peak = static_cast<std::uint64_t>(usage.ru_maxrss) * kBytesPerKilobyte; // kilobytes on Linux
#endif
        // The kernel updates this counter lazily, so it can briefly trail the current figure.
        return std::max(peak, current_rss_bytes());
    }

    MemorySampler::MemorySampler(std::chrono::milliseconds interval, std::function<void(std::uint64_t)> on_sample) :
        thread_(
            [interval, on_sample = std::move(on_sample)](const std::stop_token& stop)
            {
                STACKIDE_THREAD_NAME("memory sampler");
                std::mutex mutex;
                std::condition_variable_any wake;
                bool warned = false;
                while (!stop.stop_requested())
                {
                    const std::uint64_t rss = current_rss_bytes();
                    STACKIDE_PLOT("resident memory (bytes)", rss);
                    STACKIDE_PLOT("dropped log records", log_dropped_total());
                    if (rss > kEditorMemoryLimitBytes && !warned)
                    {
                        STACKIDE_LOG_WARN(LogCategory::Profile, "resident memory {} bytes passed the {} byte target",
                                          rss, kEditorMemoryLimitBytes);
                        warned = true;
                    }
                    if (on_sample)
                    {
                        on_sample(rss);
                    }

                    std::unique_lock lock{mutex};
                    wake.wait_for(lock, stop, interval, [] { return false; });
                }
            })
    {
    }

} // namespace stackide
