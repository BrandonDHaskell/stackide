#include <stackide/base/log.hpp>

#include <stackide/base/constants.hpp>

#include <fcntl.h>
#include <signal.h> // NOLINT(modernize-deprecated-headers): sigaction is POSIX, not <csignal>
#include <sys/stat.h>
#include <time.h> // NOLINT(modernize-deprecated-headers): nanosleep is POSIX, not <ctime>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <print>
#include <stop_token>
#include <system_error>
#include <thread>
#include <vector>

namespace stackide
{
    // Every index below is masked to a ring's size or bounded by a fixed layout, and
    // these paths can't afford a throwing check: the writer's runs on every log call, and
    // the reader's runs inside a signal handler.
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
    namespace
    {

        using detail::kLogPayloadBytes;
        using detail::LogDecoder;
        using detail::LogSite;

        constexpr std::size_t kLineBytes = 1024;
        constexpr std::size_t kSlotBytes = 256;
        constexpr std::size_t kWordBytes = sizeof(std::uint64_t);
        constexpr std::size_t kHeaderWords = 4; // timestamp, site, decoder, payload size
        constexpr std::size_t kSlotWords = kHeaderWords + (kLogPayloadBytes / kWordBytes);
        constexpr auto kMinDrainInterval = std::chrono::milliseconds{10};
        constexpr auto kMaxDrainInterval = std::chrono::milliseconds{200};
        constexpr unsigned kCrashFlushSeconds = 3;
        constexpr int kCrashWaitForWriterMs = 200;
        constexpr std::array kCrashSignals{SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGHUP};

        // A sequence lock over atomic words: a record's sequence is odd while it is being
        // written, and the reader discards any record whose sequence changed during the
        // copy. Every word is atomic so that an overwrite during a read is well-defined
        // rather than a data race. Acquire and release on the words stand in for fences,
        // which ThreadSanitizer doesn't model.
        struct Slot
        {
            std::atomic<std::uint64_t> sequence;
            std::array<std::atomic<std::uint64_t>, kSlotWords> words;
        };
        static_assert(sizeof(Slot) == kSlotBytes);

        struct Record
        {
            std::uint64_t timestamp_ns = 0;
            const LogSite* site = nullptr;
            LogDecoder decoder = nullptr;
            std::uint32_t thread_label = 0;
            std::size_t payload_size = 0;
            std::array<std::uint64_t, kLogPayloadBytes / kWordBytes> payload{};
        };

        // Who holds a ring. Compare-and-swap moves it, because a writer that saw a ring
        // as retired must not free it after a new thread has already claimed it.
        enum class RingOwner : std::uint8_t
        {
            Thread,
            Retired,
            Free,
        };

        struct Ring
        {
            explicit Ring(std::size_t slot_count) : slots(slot_count), mask(slot_count - 1), capacity(slot_count) {}

            std::vector<Slot> slots;
            std::size_t mask;
            std::uint64_t capacity;
            Ring* next = nullptr;

            std::atomic<RingOwner> owner{RingOwner::Thread};
            std::atomic<std::uint32_t> thread_label{0};

            alignas(kCacheLineBytes) std::atomic<std::uint64_t> write_index{0};
            alignas(kCacheLineBytes) std::uint64_t read_index = 0;
        };

        std::uint64_t now_ns() noexcept
        {
            const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
        }

        // Takes the next live record, returning false once the ring is empty. Lost
        // records are added to dropped. Only one thread may read a ring at a time.
        bool read_record(Ring& ring, Record& out, std::uint64_t& dropped) noexcept
        {
            while (true)
            {
                const std::uint64_t written = ring.write_index.load(std::memory_order_acquire);
                if (ring.read_index >= written)
                {
                    return false;
                }
                if (written - ring.read_index > ring.capacity)
                {
                    dropped += written - ring.read_index - ring.capacity;
                    ring.read_index = written - ring.capacity;
                }

                const std::uint64_t index = ring.read_index++;
                Slot& slot = ring.slots[index & ring.mask];
                const std::uint64_t expected = (index * 2) + 2;
                if (slot.sequence.load(std::memory_order_acquire) != expected)
                {
                    ++dropped;
                    continue;
                }

                const auto timestamp = slot.words[0].load(std::memory_order_acquire);
                const auto site = slot.words[1].load(std::memory_order_acquire);
                const auto decoder = slot.words[2].load(std::memory_order_acquire);
                const std::size_t size = slot.words[3].load(std::memory_order_acquire);
                // A torn size is only ever used to bound this copy, and is clamped first.
                const auto words = std::min((size + kWordBytes - 1) / kWordBytes, out.payload.size());
                for (std::size_t i = 0; i < words; ++i)
                {
                    out.payload[i] = slot.words[kHeaderWords + i].load(std::memory_order_acquire);
                }
                if (slot.sequence.load(std::memory_order_relaxed) != expected)
                {
                    ++dropped;
                    continue;
                }

                out.timestamp_ns = timestamp;
                out.site = std::bit_cast<const LogSite*>(site);
                out.decoder = std::bit_cast<LogDecoder>(decoder);
                out.payload_size = size;
                out.thread_label = ring.thread_label.load(std::memory_order_relaxed);
                return true;
            }
        }

        constexpr std::string_view level_name(LogLevel level) noexcept
        {
            switch (level)
            {
            case LogLevel::Error:
                return "ERROR";
            case LogLevel::Warn:
                return "WARN";
            case LogLevel::Info:
                return "INFO";
            case LogLevel::Debug:
                return "DEBUG";
            case LogLevel::Trace:
                return "TRACE";
            }
            return "?";
        }

        constexpr std::string_view category_name(LogCategory category) noexcept
        {
            switch (category)
            {
            case LogCategory::General:
                return "general";
            case LogCategory::Platform:
                return "platform";
            case LogCategory::Render:
                return "render";
            case LogCategory::Core:
                return "core";
            case LogCategory::Profile:
                return "profile";
            }
            return "?";
        }

        std::string_view base_name(std::string_view path) noexcept
        {
            if (const auto slash = path.rfind('/'); slash != std::string_view::npos)
            {
                path.remove_prefix(slash + 1);
            }
            return path;
        }

        void write_fully(int fd, std::string_view text) noexcept
        {
            while (!text.empty())
            {
                const auto written = ::write(fd, text.data(), text.size());
                if (written <= 0)
                {
                    return;
                }
                text.remove_prefix(static_cast<std::size_t>(written));
            }
        }

        struct State
        {
            explicit State(LogConfig config_in) : config(std::move(config_in)) { history.resize(config.history_bytes); }

            LogConfig config;
            std::uint64_t wall_base_ns = 0;
            std::uint64_t steady_base_ns = 0;
            std::uint64_t generation = 0;

            std::mutex registry_mutex;
            std::atomic<Ring*> rings{nullptr};
            std::uint32_t next_thread_label = 1;

            // Set while the writer holds drained records it hasn't written yet, so the
            // crash handler can wait for them rather than losing them.
            std::atomic<bool> writer_busy{false};

            std::atomic<int> file_fd{-1};
            std::uint64_t file_bytes = 0;
            std::string pending;

            std::mutex history_mutex;
            std::string history;
            std::size_t history_end = 0;
            bool history_wrapped = false;

            std::mutex flush_mutex;
            std::condition_variable_any flush_cv;
            std::uint64_t flush_requested = 0;
            std::uint64_t flush_completed = 0;

            std::jthread writer;
            std::array<struct sigaction, kCrashSignals.size()> previous_handlers{};
        };

        // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
        constinit std::atomic<State*> g_state{nullptr};
        constinit std::atomic<std::uint64_t> g_generation{0};
        constinit std::atomic<std::uint64_t> g_dropped{0};
        constinit std::atomic_flag g_crashing;
        constinit thread_local bool t_is_writer = false;
        // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

        // Marks this thread's ring reusable once the writer has drained it. Checks the
        // generation because the thread may outlive the session that owned the ring.
        struct ThreadRing
        {
            ThreadRing() = default;
            ThreadRing(const ThreadRing&) = delete;
            ThreadRing& operator=(const ThreadRing&) = delete;
            ThreadRing(ThreadRing&&) = delete;
            ThreadRing& operator=(ThreadRing&&) = delete;

            ~ThreadRing()
            {
                if (ring != nullptr && generation == g_generation.load(std::memory_order_acquire))
                {
                    ring->owner.store(RingOwner::Retired, std::memory_order_release);
                }
            }

            Ring* ring = nullptr;
            std::uint64_t generation = 0;
        };

        // The only allocation on the logging path, and the only lock: a thread's first
        // call in a session. Reuses a finished thread's ring, so that the total stays
        // bounded by the peak thread count however many threads come and go.
        Ring* acquire_ring(State& state)
        {
            const std::scoped_lock lock{state.registry_mutex};
            const std::uint32_t label = state.next_thread_label++;
            for (Ring* ring = state.rings.load(std::memory_order_relaxed); ring != nullptr; ring = ring->next)
            {
                RingOwner expected = RingOwner::Free;
                if (ring->owner.compare_exchange_strong(expected, RingOwner::Thread, std::memory_order_acq_rel))
                {
                    ring->thread_label.store(label, std::memory_order_relaxed);
                    return ring;
                }
            }
            auto ring = std::make_unique<Ring>(state.config.ring_slots);
            ring->thread_label.store(label, std::memory_order_relaxed);
            ring->next = state.rings.load(std::memory_order_relaxed);
            state.rings.store(ring.get(), std::memory_order_release);
            return ring.release(); // freed by ~LogSession
        }

        // Formats one record's complete line into a stack buffer without allocating.
        std::size_t format_line(const State& state, const Record& record, std::array<char, kLineBytes>& line) noexcept
        {
            using namespace std::chrono;
            const std::uint64_t wall_ns = state.wall_base_ns + (record.timestamp_ns - state.steady_base_ns);
            const sys_time<microseconds> wall{duration_cast<microseconds>(nanoseconds{wall_ns})};
            const auto day = floor<days>(wall);
            const year_month_day date{day};
            const hh_mm_ss clock{wall - day};

            // One byte is held back for the newline.
            const std::size_t cap = line.size() - 1;
            std::size_t used = 0;
            try
            {
                const auto prefix = std::format_to_n(
                    line.data(), static_cast<std::ptrdiff_t>(cap),
                    "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:06}Z {:<5} {:<8} t{} {}:{} ", static_cast<int>(date.year()),
                    static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()), clock.hours().count(),
                    clock.minutes().count(), clock.seconds().count(), clock.subseconds().count(),
                    level_name(record.site->level), category_name(record.site->category), record.thread_label,
                    base_name(record.site->file), record.site->line);
                used = static_cast<std::size_t>(prefix.out - line.data());
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                const auto* payload = reinterpret_cast<const std::byte*>(record.payload.data());
                used += record.decoder(*record.site, payload, line.data() + used, cap - used);
            }
            catch (...)
            {
                // Keeps whatever was formatted: losing the rest of one line is better than
                // losing the logger.
                used = std::min(used, cap);
            }
            line[used] = '\n';
            return used + 1;
        }

        void append_history(State& state, std::string_view text)
        {
            if (state.history.empty())
            {
                return;
            }
            const std::scoped_lock lock{state.history_mutex};
            if (text.size() > state.history.size())
            {
                text = text.substr(text.size() - state.history.size());
            }
            while (!text.empty())
            {
                const auto chunk = std::min(text.size(), state.history.size() - state.history_end);
                std::ranges::copy(text.substr(0, chunk),
                                  state.history.begin() + static_cast<std::ptrdiff_t>(state.history_end));
                text.remove_prefix(chunk);
                state.history_end += chunk;
                if (state.history_end == state.history.size())
                {
                    state.history_end = 0;
                    state.history_wrapped = true;
                }
            }
        }

        std::filesystem::path log_file_path(const State& state, int rotation)
        {
            if (rotation == 0)
            {
                return state.config.directory / "stackide.log";
            }
            return state.config.directory / std::format("stackide.{}.log", rotation);
        }

        void open_log_file(State& state)
        {
            const auto path = log_file_path(state, 0);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open(2) is variadic
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
            state.file_bytes = 0;
            if (fd >= 0)
            {
                struct stat info{};
                if (::fstat(fd, &info) == 0)
                {
                    state.file_bytes = static_cast<std::uint64_t>(info.st_size);
                }
            }
            state.file_fd.store(fd, std::memory_order_release);
        }

        void rotate_log_file(State& state)
        {
            const int old_fd = state.file_fd.exchange(-1, std::memory_order_acq_rel);
            if (old_fd >= 0)
            {
                ::close(old_fd);
            }
            std::error_code ignored;
            std::filesystem::remove(log_file_path(state, state.config.max_rotated_files), ignored);
            for (int i = state.config.max_rotated_files - 1; i >= 0; --i)
            {
                std::filesystem::rename(log_file_path(state, i), log_file_path(state, i + 1), ignored);
            }
            if (state.config.max_rotated_files <= 0)
            {
                std::filesystem::remove(log_file_path(state, 0), ignored);
            }
            open_log_file(state);
        }

        void flush_pending(State& state)
        {
            if (state.pending.empty())
            {
                return;
            }
            const int fd = state.file_fd.load(std::memory_order_acquire);
            if (fd >= 0)
            {
                write_fully(fd, state.pending);
                state.file_bytes += state.pending.size();
            }
            if (state.config.to_stderr)
            {
                write_fully(STDERR_FILENO, state.pending);
            }
            append_history(state, state.pending);
            state.pending.clear();
        }

        void emit_line(State& state, std::string_view line)
        {
            if (state.file_fd.load(std::memory_order_relaxed) >= 0 &&
                state.file_bytes + state.pending.size() + line.size() > state.config.max_file_bytes)
            {
                flush_pending(state);
                rotate_log_file(state);
            }
            state.pending.append(line);
        }

        // Writes out everything queued, returning whether there was anything.
        bool drain(State& state, std::vector<Record>& batch)
        {
            state.writer_busy.store(true, std::memory_order_seq_cst);

            std::uint64_t dropped = 0;
            batch.clear();
            for (Ring* ring = state.rings.load(std::memory_order_acquire); ring != nullptr; ring = ring->next)
            {
                // Read before draining, so that everything the thread logged gets drained
                // before the ring can be given to another thread.
                const bool retired = ring->owner.load(std::memory_order_acquire) == RingOwner::Retired;
                Record record;
                while (read_record(*ring, record, dropped))
                {
                    batch.push_back(record);
                }
                if (retired)
                {
                    RingOwner expected = RingOwner::Retired;
                    ring->owner.compare_exchange_strong(expected, RingOwner::Free, std::memory_order_acq_rel);
                }
            }

            // Sorted within a batch only: rings are drained in turn, so lines from
            // different threads can be slightly out of order across batches.
            std::ranges::stable_sort(batch, {}, &Record::timestamp_ns);

            std::array<char, kLineBytes> line{};
            for (const Record& record : batch)
            {
                const auto size = format_line(state, record, line);
                emit_line(state, std::string_view{line.data(), size});
            }

            if (dropped > 0)
            {
                g_dropped.fetch_add(dropped, std::memory_order_relaxed);
                emit_line(state,
                          std::format("log: dropped {} record(s), {} in total\n", dropped,
                                      g_dropped.load(std::memory_order_relaxed)));
            }

            flush_pending(state);
            state.writer_busy.store(false, std::memory_order_seq_cst);
            return !batch.empty() || dropped > 0;
        }

        void writer_loop(const std::stop_token& stop, State& state)
        {
            t_is_writer = true;
            std::vector<Record> batch;
            auto interval = kMinDrainInterval;

            while (!stop.stop_requested())
            {
                std::uint64_t requested = 0;
                {
                    const std::scoped_lock lock{state.flush_mutex};
                    requested = state.flush_requested;
                }

                const bool had_work = drain(state, batch);
                // Backs off so that an idle editor isn't kept awake by its own logger.
                interval = had_work ? kMinDrainInterval : std::min(interval * 2, kMaxDrainInterval);

                std::unique_lock lock{state.flush_mutex};
                state.flush_completed = requested;
                state.flush_cv.notify_all();
                state.flush_cv.wait_for(lock, stop, interval,
                                        [&] { return state.flush_requested != state.flush_completed; });
            }

            drain(state, batch);
        }

        // Async-signal context. Formatting avoids the heap, so this normally completes
        // even if the crash was inside malloc. If it hangs anyway, the alarm ends the
        // process, since the default action for SIGALRM is to terminate.
        void crash_flush(State& state, int signal_number) noexcept
        {
            ::alarm(kCrashFlushSeconds);

            // The writer may hold drained records, so give it time to finish. Not if this
            // is the writer: it crashed mid-batch and that batch is lost.
            if (!t_is_writer)
            {
                const timespec one_ms{.tv_sec = 0, .tv_nsec = 1'000'000};
                for (int i = 0; i < kCrashWaitForWriterMs && state.writer_busy.load(std::memory_order_seq_cst); ++i)
                {
                    ::nanosleep(&one_ms, nullptr);
                }
            }

            const int fd = state.file_fd.load(std::memory_order_acquire);
            const auto output = [&](std::string_view text)
            {
                if (fd >= 0)
                {
                    write_fully(fd, text);
                }
                if (state.config.to_stderr)
                {
                    write_fully(STDERR_FILENO, text);
                }
            };

            std::uint64_t dropped = 0;
            std::array<char, kLineBytes> line{};
            for (Ring* ring = state.rings.load(std::memory_order_acquire); ring != nullptr; ring = ring->next)
            {
                Record record;
                while (read_record(*ring, record, dropped))
                {
                    output(std::string_view{line.data(), format_line(state, record, line)});
                }
            }

            try
            {
                const auto notice = std::format_to_n(line.data(), static_cast<std::ptrdiff_t>(line.size()),
                                                     "log: flushed after fatal signal {}\n", signal_number);
                output(std::string_view{line.data(), static_cast<std::size_t>(notice.out - line.data())});
            }
            catch (...)
            {
                output("log: flushed after a fatal signal\n");
            }
        }

        void crash_handler(int signal_number)
        {
            if (!g_crashing.test_and_set())
            {
                State* state = g_state.load(std::memory_order_acquire);
                if (state != nullptr)
                {
                    crash_flush(*state, signal_number);
                    // Restored so that a sanitizer's report still runs, or the signal's
                    // default action ends the process with the right status.
                    for (std::size_t i = 0; i < kCrashSignals.size(); ++i)
                    {
                        if (kCrashSignals.at(i) == signal_number)
                        {
                            ::sigaction(signal_number, &state->previous_handlers.at(i), nullptr);
                        }
                    }
                }
            }
            else
            {
                ::signal(signal_number, SIG_DFL);
            }
            ::raise(signal_number);
        }

        void install_crash_handlers(State& state)
        {
            struct sigaction action{};
            action.sa_handler = crash_handler;
            action.sa_flags = SA_NODEFER;
            // Unqualified, and with the result discarded, because macOS defines this as a macro.
            static_cast<void>(sigemptyset(&action.sa_mask));
            for (std::size_t i = 0; i < kCrashSignals.size(); ++i)
            {
                ::sigaction(kCrashSignals.at(i), &action, &state.previous_handlers.at(i));
            }
        }

        void restore_crash_handlers(const State& state)
        {
            for (std::size_t i = 0; i < kCrashSignals.size(); ++i)
            {
                ::sigaction(kCrashSignals.at(i), &state.previous_handlers.at(i), nullptr);
            }
        }

        void set_all_thresholds(LogLevel level) noexcept
        {
            for (auto& threshold : detail::g_log_thresholds)
            {
                threshold.store(static_cast<std::uint8_t>(static_cast<std::uint8_t>(level) + 1),
                                std::memory_order_relaxed);
            }
        }

    } // namespace
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)

    std::filesystem::path log_default_directory()
    {
#ifdef __APPLE__
        const char* home = std::getenv("HOME");
        return home != nullptr ? std::filesystem::path{home} / "Library/Logs/stackide" : std::filesystem::path{};
#else
        if (const char* state_home = std::getenv("XDG_STATE_HOME"); state_home != nullptr && *state_home != '\0')
        {
            return std::filesystem::path{state_home} / "stackide/logs";
        }
        const char* home = std::getenv("HOME");
        return home != nullptr ? std::filesystem::path{home} / ".local/state/stackide/logs" : std::filesystem::path{};
#endif
    }

    LogConfig log_config_from_environment()
    {
        LogConfig config;
        const char* directory = std::getenv("STACKIDE_LOG_DIR");
        config.directory = directory != nullptr ? std::filesystem::path{directory} : log_default_directory();

        if (const char* level = std::getenv("STACKIDE_LOG"); level != nullptr)
        {
            const std::string_view name{level};
            if (name == "error")
            {
                config.level = LogLevel::Error;
            }
            else if (name == "warn")
            {
                config.level = LogLevel::Warn;
            }
            else if (name == "info")
            {
                config.level = LogLevel::Info;
            }
            else if (name == "debug")
            {
                config.level = LogLevel::Debug;
            }
            else if (name == "trace")
            {
                config.level = LogLevel::Trace;
            }
            else
            {
                std::println(stderr, "ignoring STACKIDE_LOG={}: expected error, warn, info, debug or trace", name);
            }
        }
        return config;
    }

    LogSession::LogSession(const LogConfig& config)
    {
        auto state = std::make_unique<State>(config);
        const auto wall = std::chrono::system_clock::now().time_since_epoch();
        state->wall_base_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(wall).count());
        state->steady_base_ns = now_ns();
        state->generation = g_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        g_dropped.store(0, std::memory_order_relaxed);
        g_crashing.clear();

        if (!config.directory.empty())
        {
            std::error_code error;
            std::filesystem::create_directories(config.directory, error);
            open_log_file(*state);
            if (state->file_fd.load(std::memory_order_relaxed) < 0)
            {
                // A read-only home shouldn't stop the editor, and stderr is still live.
                std::println(stderr, "log: can't write to {}, using stderr only", config.directory.string());
            }
        }

        install_crash_handlers(*state);
        State* raw = state.release();
        g_state.store(raw, std::memory_order_release);
        set_all_thresholds(config.level);

        if (config.start_writer)
        {
            raw->writer = std::jthread{[raw](const std::stop_token& stop) { writer_loop(stop, *raw); }};
        }
    }

    // The joins below can only throw on a logic error, where terminating is correct.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    LogSession::~LogSession()
    {
        // Disable first, so that a call that slipped through can't land after the drain.
        for (auto& threshold : detail::g_log_thresholds)
        {
            threshold.store(0, std::memory_order_relaxed);
        }

        std::unique_ptr<State> state{g_state.load(std::memory_order_acquire)};
        if (state->writer.joinable())
        {
            state->writer.request_stop();
            state->writer.join();
        }
        else
        {
            std::vector<Record> batch;
            drain(*state, batch);
        }

        restore_crash_handlers(*state);
        g_state.store(nullptr, std::memory_order_release);
        // Invalidates every thread's cached ring before the rings are freed.
        g_generation.fetch_add(1, std::memory_order_acq_rel);

        Ring* ring = state->rings.load(std::memory_order_relaxed);
        while (ring != nullptr)
        {
            Ring* next = ring->next;
            delete ring; // NOLINT(cppcoreguidelines-owning-memory): allocated in acquire_ring
            ring = next;
        }
        const int fd = state->file_fd.load(std::memory_order_relaxed);
        if (fd >= 0)
        {
            ::close(fd);
        }
    }

    void log_set_level(LogLevel level) { set_all_thresholds(level); }

    void log_set_level(LogCategory category, LogLevel level)
    {
        detail::g_log_thresholds.at(static_cast<std::size_t>(category))
            .store(static_cast<std::uint8_t>(static_cast<std::uint8_t>(level) + 1), std::memory_order_relaxed);
    }

    void log_flush()
    {
        State* state = g_state.load(std::memory_order_acquire);
        if (state == nullptr || !state->writer.joinable())
        {
            return;
        }
        std::unique_lock lock{state->flush_mutex};
        const std::uint64_t ticket = ++state->flush_requested;
        state->flush_cv.notify_all();
        state->flush_cv.wait(lock, [&] { return state->flush_completed >= ticket; });
    }

    std::uint64_t log_dropped_total() { return g_dropped.load(std::memory_order_relaxed); }

    std::string log_recent_history()
    {
        State* state = g_state.load(std::memory_order_acquire);
        if (state == nullptr || state->history.empty())
        {
            return {};
        }
        const std::scoped_lock lock{state->history_mutex};
        if (!state->history_wrapped)
        {
            return state->history.substr(0, state->history_end);
        }
        std::string text = state->history.substr(state->history_end) + state->history.substr(0, state->history_end);
        // After a wrap the first line is usually partial.
        const auto first_newline = text.find('\n');
        return first_newline == std::string::npos ? text : text.substr(first_newline + 1);
    }

    namespace detail
    {

        // See the note on index bounds at the top of this file.
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)
        void log_commit(const LogSite& site, LogDecoder decoder, std::byte* payload, std::size_t size) noexcept
        {
            // Holds a cached pointer, checked against the generation, so that the common
            // case takes no lock and no state lookup beyond two loads.
            static constinit thread_local ThreadRing t_ring;
            const std::uint64_t generation = g_generation.load(std::memory_order_acquire);
            if (t_ring.ring == nullptr || t_ring.generation != generation) [[unlikely]]
            {
                State* state = g_state.load(std::memory_order_acquire);
                if (state == nullptr)
                {
                    return;
                }
                try
                {
                    t_ring.ring = acquire_ring(*state);
                    t_ring.generation = generation;
                }
                catch (...)
                {
                    t_ring.ring = nullptr;
                    return; // out of memory while setting up: this record is lost
                }
            }

            const auto timestamp = now_ns();
            Ring& ring = *t_ring.ring;
            const std::uint64_t index = ring.write_index.load(std::memory_order_relaxed);
            Slot& slot = ring.slots[index & ring.mask];

            // The word stores use release, so a reader that sees any new word also sees
            // the odd sequence stored before it.
            slot.sequence.store((index * 2) + 1, std::memory_order_relaxed);
            slot.words[0].store(timestamp, std::memory_order_release);
            slot.words[1].store(std::bit_cast<std::uint64_t>(&site), std::memory_order_release);
            slot.words[2].store(std::bit_cast<std::uint64_t>(decoder), std::memory_order_release);
            slot.words[3].store(size, std::memory_order_release);

            // Pads to a whole word, so that the copy below never reads unset bytes.
            const std::size_t padded = (size + kWordBytes - 1) & ~(kWordBytes - 1);
            std::memset(payload + size, 0, padded - size);
            for (std::size_t i = 0; i < padded / kWordBytes; ++i)
            {
                std::uint64_t word = 0;
                std::memcpy(&word, payload + (i * kWordBytes), kWordBytes);
                slot.words[kHeaderWords + i].store(word, std::memory_order_release);
            }

            slot.sequence.store((index * 2) + 2, std::memory_order_release);
            ring.write_index.store(index + 1, std::memory_order_release);
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,cppcoreguidelines-pro-bounds-constant-array-index)

    } // namespace detail
} // namespace stackide
