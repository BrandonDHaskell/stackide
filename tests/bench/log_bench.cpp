// Measures the producer's cost per log call with the writer thread live, and prints
// the median across batches. --assert-under-ns makes it a pass/fail gate.

#include <stackide/base/log.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <print>
#include <string_view>
#include <vector>

namespace
{

    constexpr int kBatches = 101;
    constexpr int kCallsPerBatch = 100'000;

} // namespace

int main(int argc, char** argv)
{
    double limit_ns = 0.0;
    if (argc == 3 && std::string_view{argv[1]} == "--assert-under-ns")
    {
        const std::string_view text{argv[2]};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), limit_ns);
        if (result.ec != std::errc{} || limit_ns <= 0.0)
        {
            std::println(stderr, "invalid value for --assert-under-ns: {}", text);
            return EXIT_FAILURE;
        }
    }
    else if (argc != 1)
    {
        std::println(stderr, "usage: stackide_log_bench [--assert-under-ns N]");
        return EXIT_FAILURE;
    }

    double median_ns = 0.0;
    std::uint64_t dropped = 0;
    {
        stackide::LogConfig config;
        config.to_stderr = false;
        config.history_bytes = 0;
        const stackide::LogSession session{config};

        std::vector<double> batches;
        batches.reserve(kBatches);
        const std::string_view name{"swapchain"};
        for (int batch = 0; batch < kBatches; ++batch)
        {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < kCallsPerBatch; ++i)
            {
                STACKIDE_LOG_INFO(stackide::LogCategory::Render, "frame {} took {} ms in {}", i, 1.5, name);
            }
            const std::chrono::duration<double, std::nano> elapsed = std::chrono::steady_clock::now() - start;
            batches.push_back(elapsed.count() / kCallsPerBatch);
        }
        std::ranges::sort(batches);
        median_ns = batches.at(batches.size() / 2);
        stackide::log_flush();
        dropped = stackide::log_dropped_total();
    }

    std::println("log call: {:.1f} ns median, {} record(s) dropped by the writer", median_ns, dropped);
    if (limit_ns > 0.0 && median_ns > limit_ns)
    {
        std::println(stderr, "log call {:.1f} ns exceeds gate {:.1f} ns", median_ns, limit_ns);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
