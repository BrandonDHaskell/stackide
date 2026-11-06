#include <stackide/base/log.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <regex>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Sanitizers bring their own allocator and signal handling, which the allocation counter
// and the exit status checks below would conflict with.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define STACKIDE_TEST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define STACKIDE_TEST_SANITIZED 1
#endif
#endif
#ifndef STACKIDE_TEST_SANITIZED
#define STACKIDE_TEST_SANITIZED 0
#endif

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using stackide::LogCategory;
using stackide::LogConfig;
using stackide::LogLevel;
using stackide::LogSession;

namespace
{

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    [[maybe_unused]] thread_local std::uint64_t t_allocations = 0;

    class TempDir
    {
    public:
        TempDir()
        {
            std::string pattern = (fs::temp_directory_path() / "stackide-log-XXXXXX").string();
            REQUIRE(::mkdtemp(pattern.data()) != nullptr);
            path_ = pattern;
        }
        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;
        ~TempDir()
        {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
        const fs::path& path() const { return path_; }

    private:
        fs::path path_;
    };

    LogConfig quiet_config(const fs::path& directory)
    {
        LogConfig config;
        config.directory = directory;
        config.to_stderr = false;
        return config;
    }

    std::string read_file(const fs::path& path)
    {
        const std::ifstream file{path};
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    std::size_t count_of(const std::string& text, std::string_view needle)
    {
        std::size_t count = 0;
        for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
        {
            ++count;
        }
        return count;
    }

} // namespace

#if !STACKIDE_TEST_SANITIZED
// Counts this thread's allocations, so that the writer's don't affect the test below.
// Not inlined because GCC otherwise reports a free() of new'd memory, a false positive.
[[gnu::noinline]] void* operator new(std::size_t size)
{
    ++t_allocations;
    void* memory = std::malloc(size != 0 ? size : 1);
    if (memory == nullptr)
    {
        throw std::bad_alloc{};
    }
    return memory;
}
[[gnu::noinline]] void* operator new[](std::size_t size) { return ::operator new(size); }
[[gnu::noinline]] void operator delete(void* memory) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete[](void* memory) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
#endif

TEST_CASE("a record is written with its arguments, level, category and location")
{
    const TempDir dir;
    {
        const LogSession session{quiet_config(dir.path())};
        const std::string owned = "owned";
        const char* pointer = "pointer";
        const char* null_text = nullptr;
        STACKIDE_LOG_WARN(LogCategory::Render, "n={} x={:.2f} {} {} {} {} {:02X}", 42, 1.5, owned, pointer, "literal",
                          null_text, std::uint8_t{0xAB});
    }

    const auto text = read_file(dir.path() / "stackide.log");
    CHECK_THAT(text, ContainsSubstring("n=42 x=1.50 owned pointer literal (null) AB\n"));
    CHECK_THAT(text, ContainsSubstring("WARN"));
    CHECK_THAT(text, ContainsSubstring("render"));
    CHECK_THAT(text, ContainsSubstring("test_log.cpp:"));
    CHECK(std::regex_search(text, std::regex{R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6}Z )"}));
}

TEST_CASE("a call with no arguments works")
{
    const TempDir dir;
    {
        const LogSession session{quiet_config(dir.path())};
        STACKIDE_LOG_ERROR(LogCategory::General, "plain message");
    }
    CHECK_THAT(read_file(dir.path() / "stackide.log"), ContainsSubstring("plain message\n"));
}

TEST_CASE("levels are filtered at runtime, per category")
{
    const TempDir dir;
    {
        auto config = quiet_config(dir.path());
        config.level = LogLevel::Warn;
        const LogSession session{config};

        STACKIDE_LOG_WARN(LogCategory::Core, "kept warn");
        STACKIDE_LOG_INFO(LogCategory::Core, "hidden info");

        stackide::log_set_level(LogCategory::Core, LogLevel::Info);
        STACKIDE_LOG_INFO(LogCategory::Core, "kept info");
        STACKIDE_LOG_INFO(LogCategory::Render, "hidden other category");
    }
    const auto text = read_file(dir.path() / "stackide.log");
    CHECK_THAT(text, ContainsSubstring("kept warn"));
    CHECK_THAT(text, ContainsSubstring("kept info"));
    CHECK_FALSE(text.contains("hidden"));
}

TEST_CASE("a filtered call doesn't evaluate its arguments")
{
    const TempDir dir;
    const LogSession session{quiet_config(dir.path())};
    int evaluations = 0;
    const auto argument = [&] { return ++evaluations; };
    STACKIDE_LOG_DEBUG(LogCategory::General, "{}", argument());
    CHECK(evaluations == 0);
}

TEST_CASE("a call outside a session is ignored")
{
    STACKIDE_LOG_ERROR(LogCategory::General, "nobody is listening");
    SUCCEED();
}

TEST_CASE("a long string is truncated without corrupting the record")
{
    const TempDir dir;
    {
        const LogSession session{quiet_config(dir.path())};
        const std::string huge(10'000, 'x');
        STACKIDE_LOG_INFO(LogCategory::General, "{} then {}", huge, 7);
        STACKIDE_LOG_INFO(LogCategory::General, "next record");
    }
    const auto text = read_file(dir.path() / "stackide.log");
    CHECK_THAT(text, ContainsSubstring("x then 7\n"));
    CHECK_THAT(text, ContainsSubstring("next record\n"));
    CHECK(count_of(text, "x") < 300);
}

TEST_CASE("a log call doesn't allocate after the thread's first")
{
#if STACKIDE_TEST_SANITIZED
    SKIP("allocations aren't counted under a sanitizer");
#endif
    const TempDir dir;
    const LogSession session{quiet_config(dir.path())};
    STACKIDE_LOG_INFO(LogCategory::General, "first call sets up this thread's ring");

    const std::string text = "a string long enough that it can't use small buffer optimization";
    const auto before = t_allocations;
    for (int i = 0; i < 10'000; ++i)
    {
        STACKIDE_LOG_INFO(LogCategory::General, "{} {} {}", i, 2.5, text);
    }
    CHECK(t_allocations == before);
}

TEST_CASE("a full ring drops the oldest records and counts them")
{
    const TempDir dir;
    {
        auto config = quiet_config(dir.path());
        config.ring_slots = 8;
        config.start_writer = false;
        const LogSession session{config};
        for (int i = 0; i < 20; ++i)
        {
            STACKIDE_LOG_INFO(LogCategory::General, "record {}.", i);
        }
    }
    const auto text = read_file(dir.path() / "stackide.log");
    CHECK_FALSE(text.contains("record 11."));
    CHECK_THAT(text, ContainsSubstring("record 12."));
    CHECK_THAT(text, ContainsSubstring("record 19."));
    CHECK_THAT(text, ContainsSubstring("dropped 12 record(s)"));
    CHECK(stackide::log_dropped_total() == 12);
}

TEST_CASE("records survive the exit of the thread that logged them")
{
    const TempDir dir;
    {
        const LogSession session{quiet_config(dir.path())};
        std::thread{[] { STACKIDE_LOG_INFO(LogCategory::General, "from a finished thread"); }}.join();
    }
    CHECK_THAT(read_file(dir.path() / "stackide.log"), ContainsSubstring("from a finished thread"));
}

TEST_CASE("records from many short-lived threads all arrive")
{
    const TempDir dir;
    {
        const LogSession session{quiet_config(dir.path())};
        for (int i = 0; i < 5; ++i)
        {
            std::thread{[i] { STACKIDE_LOG_INFO(LogCategory::General, "churn {}", i); }}.join();
            stackide::log_flush();
            stackide::log_flush();
        }
    }
    const auto text = read_file(dir.path() / "stackide.log");
    for (int i = 0; i < 5; ++i)
    {
        CHECK_THAT(text, ContainsSubstring(std::format("churn {}", i)));
    }
}

TEST_CASE("log_flush makes earlier records visible")
{
    const TempDir dir;
    const LogSession session{quiet_config(dir.path())};
    STACKIDE_LOG_INFO(LogCategory::General, "flushed record");
    stackide::log_flush();
    CHECK_THAT(read_file(dir.path() / "stackide.log"), ContainsSubstring("flushed record"));
}

TEST_CASE("files rotate by size and keep the last N")
{
    const TempDir dir;
    {
        auto config = quiet_config(dir.path());
        config.max_file_bytes = 2000;
        config.max_rotated_files = 2;
        const LogSession session{config};
        for (int i = 0; i < 400; ++i)
        {
            STACKIDE_LOG_INFO(LogCategory::General, "rotation record {}.", i);
            stackide::log_flush();
        }
    }
    CHECK(fs::exists(dir.path() / "stackide.log"));
    CHECK(fs::exists(dir.path() / "stackide.1.log"));
    CHECK(fs::exists(dir.path() / "stackide.2.log"));
    CHECK_FALSE(fs::exists(dir.path() / "stackide.3.log"));
    CHECK(fs::file_size(dir.path() / "stackide.1.log") <= 2000);
    CHECK_THAT(read_file(dir.path() / "stackide.log"), ContainsSubstring("rotation record 399."));
}

TEST_CASE("the history holds recent output, whole lines only, within its bound")
{
    const TempDir dir;
    auto config = quiet_config(dir.path());
    config.history_bytes = 1000;
    const LogSession session{config};
    for (int i = 0; i < 100; ++i)
    {
        STACKIDE_LOG_INFO(LogCategory::General, "history record {}.", i);
        stackide::log_flush();
    }
    const auto history = stackide::log_recent_history();
    CHECK(history.size() <= 1000);
    CHECK_THAT(history, ContainsSubstring("history record 99."));
    CHECK_FALSE(history.contains("history record 0."));
    CHECK(std::regex_search(history, std::regex{R"(^\d{4}-)"}));
}

// Races the reader against overwrites on purpose: small rings, nonstop producers, and
// forced drains, so that records are constantly overwritten mid-copy. Each record's
// fields are related, so a torn read that got past the sequence check shows up as a
// mismatch. Without the forced drains the writer sleeps through nearly all of the race.
TEST_CASE("concurrent producers never produce a torn record")
{
    const TempDir dir;
    constexpr int kThreads = 8;
    // Counted in drains rather than time, so that a slow machine does the same work.
    constexpr int kDrains = 200;
    {
        auto config = quiet_config(dir.path());
        config.ring_slots = 16;
        config.max_file_bytes = std::uint64_t{1} << 40U;
        config.history_bytes = 0;
        const LogSession session{config};

        std::vector<std::jthread> producers;
        for (int t = 0; t < kThreads; ++t)
        {
            producers.emplace_back(
                [t](const std::stop_token& stop)
                {
                    for (std::uint64_t i = 0; !stop.stop_requested(); ++i)
                    {
                        const std::string tag(1 + (i % 40), static_cast<char>('a' + (i % 26)));
                        STACKIDE_LOG_INFO(LogCategory::Core, "p={} i={} twice={} size={} tag={}|", t, i, i * 2,
                                          tag.size(), tag);
                    }
                });
        }

        for (int i = 0; i < kDrains; ++i)
        {
            stackide::log_flush();
        }
    }

    std::ifstream file{dir.path() / "stackide.log"};
    std::array<std::uint64_t, kThreads> last_index{};
    std::array<bool, kThreads> seen{};
    std::size_t records = 0;
    std::size_t drop_notices = 0;
    for (std::string line; std::getline(file, line);)
    {
        if (line.starts_with("log: dropped"))
        {
            ++drop_notices;
            continue;
        }
        const auto start = line.find("p=");
        REQUIRE(start != std::string::npos);

        int producer = -1;
        unsigned long long index = 0;
        unsigned long long twice = 0;
        std::size_t size = 0;
        std::array<char, 64> tag{};
        // NOLINTNEXTLINE(cert-err34-c)
        const int fields = std::sscanf(line.c_str() + start, "p=%d i=%llu twice=%llu size=%zu tag=%63[a-z]|", &producer,
                                       &index, &twice, &size, tag.data());
        const std::string_view tag_text{tag.data()};
        const bool consistent = fields == 5 && producer >= 0 && producer < kThreads && twice == index * 2 &&
            size == tag_text.size() && size == 1 + (index % 40) &&
            tag_text.find_first_not_of(static_cast<char>('a' + (index % 26))) == std::string_view::npos;
        if (!consistent)
        {
            FAIL("torn record: " << line);
        }

        const auto slot = static_cast<std::size_t>(producer);
        if (seen.at(slot) && index <= last_index.at(slot))
        {
            FAIL("out of order record: " << line);
        }
        seen.at(slot) = true;
        last_index.at(slot) = index;
        ++records;
    }

    INFO("records checked: " << records << ", drop notices: " << drop_notices);
    CHECK(records >= kDrains);
    CHECK(drop_notices > 0);
    CHECK(std::ranges::all_of(seen, [](bool value) { return value; }));
}

namespace
{

    void check_crash_flush(int signal_number)
    {
        const TempDir dir;
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0)
        {
            {
                auto config = quiet_config(dir.path());
                // Records stay queued, so only the crash path can write this one out.
                config.start_writer = false;
                const LogSession session{config};
                STACKIDE_LOG_ERROR(LogCategory::Core, "last words {}", 42);
                std::raise(signal_number);
            }
            std::_Exit(0);
        }

        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        const auto text = read_file(dir.path() / "stackide.log");
        CHECK_THAT(text, ContainsSubstring("last words 42\n"));
        CHECK_THAT(text, ContainsSubstring(std::format("flushed after fatal signal {}", signal_number)));
        // The original cause of death has to survive, or core dumps and parents are misled.
        REQUIRE(WIFSIGNALED(status));
        CHECK(WTERMSIG(status) == signal_number);
    }

} // namespace

TEST_CASE("a fatal signal writes queued records and still kills the process")
{
#if STACKIDE_TEST_SANITIZED
    SKIP("the sanitizer's own signal handling replaces the exit status");
#endif
    SECTION("SIGSEGV") { check_crash_flush(SIGSEGV); }
    SECTION("SIGABRT") { check_crash_flush(SIGABRT); }
}

TEST_CASE("the environment selects the directory and level")
{
    ::setenv("STACKIDE_LOG_DIR", "/tmp/stackide-env-test", 1);
    ::setenv("STACKIDE_LOG", "trace", 1);
    const auto config = stackide::log_config_from_environment();
    ::unsetenv("STACKIDE_LOG_DIR");
    ::unsetenv("STACKIDE_LOG");
    CHECK(config.directory == fs::path{"/tmp/stackide-env-test"});
    CHECK(config.level == LogLevel::Trace);
}
