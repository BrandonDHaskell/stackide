#pragma once

// Logging cheap enough to leave on in the frame loop. A log call copies its arguments
// into a per-thread ring and returns; one writer thread formats and outputs them.
// A call never blocks and, after a thread's first call, never allocates. When a ring
// is full the oldest record is dropped and counted.
//
//     STACKIDE_LOG_INFO(LogCategory::Render, "gpu driver: {}", name);
//
// Arguments may be arithmetic types, pointers, or anything convertible to
// std::string_view. Strings are copied and may be truncated to fit a record.

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

// How many levels survive compilation: 0 removes every call, 5 keeps them all. A
// removed call's arguments are not evaluated.
// NOLINTBEGIN(cppcoreguidelines-macro-usage): must be settable per translation unit
#ifndef STACKIDE_LOG_COMPILE_LEVEL
#ifdef NDEBUG
#define STACKIDE_LOG_COMPILE_LEVEL 3
#else
#define STACKIDE_LOG_COMPILE_LEVEL 5
#endif
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

namespace stackide
{

    enum class LogLevel : std::uint8_t
    {
        Error,
        Warn,
        Info,
        Debug,
        Trace,
    };

    enum class LogCategory : std::uint8_t
    {
        General,
        Platform,
        Render,
        Core,
        Profile,
    };

    inline constexpr std::size_t kLogCategoryCount = 5;

    inline constexpr std::uint64_t kLogDefaultMaxFileBytes = std::uint64_t{16} << 20U;
    inline constexpr int kLogDefaultMaxRotatedFiles = 5;
    inline constexpr std::size_t kLogDefaultRingSlots = 1024;
    inline constexpr std::size_t kLogDefaultHistoryBytes = std::size_t{64} << 10U;

    struct LogConfig
    {
        std::filesystem::path directory; // empty disables file output
        bool to_stderr = true;
        LogLevel level = LogLevel::Info;
        std::uint64_t max_file_bytes = kLogDefaultMaxFileBytes;
        int max_rotated_files = kLogDefaultMaxRotatedFiles;
        std::size_t ring_slots = kLogDefaultRingSlots; // per thread, must be a power of two
        std::size_t history_bytes = kLogDefaultHistoryBytes;
        bool start_writer = true; // false holds records until shutdown, for tests
    };

    // ~/.local/state/stackide/logs on Linux (honoring XDG_STATE_HOME) and
    // ~/Library/Logs/stackide on macOS. Empty if HOME is unset.
    std::filesystem::path log_default_directory();

    // The defaults, with STACKIDE_LOG_DIR and STACKIDE_LOG (a level name) applied.
    LogConfig log_config_from_environment();

    // Starts logging until destroyed, then writes out everything still queued. Stop any
    // threads that log before destroying this. Only one may exist at a time.
    class LogSession
    {
    public:
        explicit LogSession(const LogConfig& config);
        LogSession(const LogSession&) = delete;
        LogSession& operator=(const LogSession&) = delete;
        LogSession(LogSession&&) = delete;
        LogSession& operator=(LogSession&&) = delete;
        ~LogSession();
    };

    void log_set_level(LogLevel level);
    void log_set_level(LogCategory category, LogLevel level);

    // Blocks until every record logged before this call is written. Not for hot paths.
    void log_flush();

    // Records lost to overwriting since the session started. Readable after it ends.
    std::uint64_t log_dropped_total();

    // The most recently written output, for attaching to crash reports. Empty outside
    // a session.
    std::string log_recent_history();

    namespace detail
    {

        // Number of levels enabled per category. Zero until a session starts, so a
        // call outside one costs only this check.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        inline constinit std::array<std::atomic<std::uint8_t>, kLogCategoryCount> g_log_thresholds{};

        struct LogSite
        {
            LogLevel level;
            LogCategory category;
            const char* file;
            int line;
            const char* format;
        };

        // Formats a record's payload into out, returning the number of chars written.
        using LogDecoder = std::size_t (*)(const LogSite& site, const std::byte* payload, char* out, std::size_t cap);

        // 256-byte slots, less the sequence word and the record header.
        inline constexpr std::size_t kLogPayloadBytes = 216;

        void log_commit(const LogSite& site, LogDecoder decoder, std::byte* payload, std::size_t size) noexcept;

        template <class T>
        concept LogStringArg = std::convertible_to<const T&, std::string_view>;

        template <class T>
        struct LogWire
        {
            static_assert(std::is_arithmetic_v<T> || std::is_pointer_v<T>,
                          "log arguments must be arithmetic, pointers, or string-like; cast enums to an integer");
            using type = std::conditional_t<std::is_pointer_v<T>, const void*, T>;
        };

        template <LogStringArg T>
        struct LogWire<T>
        {
            using type = std::string_view;
        };

        template <class T>
        using LogWireT = LogWire<T>::type;

        template <class W>
        inline constexpr std::size_t kLogWireFixedSize = sizeof(W);
        template <>
        inline constexpr std::size_t kLogWireFixedSize<std::string_view> = sizeof(std::uint32_t);

        template <class T>
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): only called from log_write
        void log_encode(std::byte* buffer, std::size_t& pos, std::size_t& string_budget, const T& value) noexcept
        {
            if constexpr (LogStringArg<T>)
            {
                std::string_view text;
                if constexpr (std::is_pointer_v<T>)
                {
                    text = value != nullptr ? std::string_view{value} : std::string_view{"(null)"};
                }
                else
                {
                    text = value;
                }
                const auto size = text.size() < string_budget ? text.size() : string_budget;
                const auto wire_size = static_cast<std::uint32_t>(size);
                std::memcpy(buffer + pos, &wire_size, sizeof(wire_size));
                pos += sizeof(wire_size);
                std::memcpy(buffer + pos, text.data(), size);
                pos += size;
                string_budget -= size;
            }
            else
            {
                const LogWireT<T> wire = value;
                std::memcpy(buffer + pos, &wire, sizeof(wire));
                pos += sizeof(wire);
            }
        }

        template <class W>
        W log_read(const std::byte*& cursor) noexcept
        {
            if constexpr (std::same_as<W, std::string_view>)
            {
                std::uint32_t size = 0;
                std::memcpy(&size, cursor, sizeof(size));
                cursor += sizeof(size);
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                const std::string_view text{reinterpret_cast<const char*>(cursor), size};
                cursor += size;
                return text;
            }
            else
            {
                W value{};
                std::memcpy(&value, cursor, sizeof(value));
                cursor += sizeof(value);
                return value;
            }
        }

        // Writes into a fixed buffer and silently discards anything past its end.
        struct LogBoundedOutput
        {
            using difference_type = std::ptrdiff_t;

            char* cursor = nullptr;
            char* end = nullptr;

            LogBoundedOutput& operator*() noexcept { return *this; }
            LogBoundedOutput& operator=(char value) noexcept
            {
                if (cursor != end)
                {
                    *cursor++ = value;
                }
                return *this;
            }
            LogBoundedOutput& operator++() noexcept { return *this; }
            LogBoundedOutput operator++(int) noexcept { return *this; }
        };

        // The format string was checked at compile time against these types by
        // log_write, so the runtime form can't fail on a mismatch. Writes to a caller's
        // stack buffer, which is what lets the crash handler use this without allocating.
        template <class... Wire>
        std::size_t log_decode(const LogSite& site, const std::byte* payload, char* out, std::size_t cap)
        {
            [[maybe_unused]] const std::byte* cursor = payload;
            // Braces guarantee left-to-right evaluation.
            const std::tuple<Wire...> values{log_read<Wire>(cursor)...};
            return std::apply(
                [&](const Wire&... args)
                {
                    const auto result = std::vformat_to(LogBoundedOutput{.cursor = out, .end = out + cap}, site.format,
                                                        std::make_format_args(args...));
                    return static_cast<std::size_t>(result.cursor - out);
                },
                values);
        }

        template <class... Args>
        void log_write(const LogSite& site, std::format_string<LogWireT<Args>...> /*checked_format*/,
                       const Args&... args) noexcept
        {
            static constexpr auto kFixedSize = (std::size_t{0} + ... + kLogWireFixedSize<LogWireT<Args>>);
            static_assert(kFixedSize <= kLogPayloadBytes, "too many log arguments to fit in one record");

            // Left uninitialized: only the bytes written below are ever read.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
            std::array<std::byte, kLogPayloadBytes> buffer;
            std::size_t pos = 0;
            [[maybe_unused]] std::size_t string_budget = kLogPayloadBytes - kFixedSize;
            (log_encode(buffer.data(), pos, string_budget, args), ...);
            log_commit(site, &log_decode<LogWireT<Args>...>, buffer.data(), pos);
        }

    } // namespace detail

    inline bool log_enabled(LogLevel level, LogCategory category) noexcept
    {
        // The enum bounds the index, and this runs on every log call.
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
        return static_cast<std::uint8_t>(level) <
            detail::g_log_thresholds[static_cast<std::size_t>(category)].load(std::memory_order_relaxed);
        // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
    }

} // namespace stackide

// NOLINTBEGIN(cppcoreguidelines-macro-usage,cppcoreguidelines-avoid-do-while,bugprone-macro-parentheses)
#define STACKIDE_DETAIL_LOG(level_, category_, format_, ...)                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        if constexpr (static_cast<int>(level_) < STACKIDE_LOG_COMPILE_LEVEL)                                           \
        {                                                                                                              \
            if (::stackide::log_enabled(level_, category_))                                                            \
            {                                                                                                          \
                static constexpr ::stackide::detail::LogSite stackide_log_site{level_, category_, __FILE__, __LINE__,  \
                                                                               format_};                               \
                ::stackide::detail::log_write(stackide_log_site, format_ __VA_OPT__(, ) __VA_ARGS__);                  \
            }                                                                                                          \
        }                                                                                                              \
    }                                                                                                                  \
    while (false)

#define STACKIDE_LOG_ERROR(category_, format_, ...)                                                                    \
    STACKIDE_DETAIL_LOG(::stackide::LogLevel::Error, category_, format_ __VA_OPT__(, ) __VA_ARGS__)
#define STACKIDE_LOG_WARN(category_, format_, ...)                                                                     \
    STACKIDE_DETAIL_LOG(::stackide::LogLevel::Warn, category_, format_ __VA_OPT__(, ) __VA_ARGS__)
#define STACKIDE_LOG_INFO(category_, format_, ...)                                                                     \
    STACKIDE_DETAIL_LOG(::stackide::LogLevel::Info, category_, format_ __VA_OPT__(, ) __VA_ARGS__)
#define STACKIDE_LOG_DEBUG(category_, format_, ...)                                                                    \
    STACKIDE_DETAIL_LOG(::stackide::LogLevel::Debug, category_, format_ __VA_OPT__(, ) __VA_ARGS__)
#define STACKIDE_LOG_TRACE(category_, format_, ...)                                                                    \
    STACKIDE_DETAIL_LOG(::stackide::LogLevel::Trace, category_, format_ __VA_OPT__(, ) __VA_ARGS__)
// NOLINTEND(cppcoreguidelines-macro-usage,cppcoreguidelines-avoid-do-while,bugprone-macro-parentheses)
